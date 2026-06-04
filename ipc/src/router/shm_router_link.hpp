#pragma once

#include "ipc/app_shutdown.hpp"
#include "ipc/endpoint.hpp"
#include "ipc/shm_spsc.hpp"
#include "router/frame.hpp"
#include "router/metrics.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_peer_address_io.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// One SPSC region per peer: router creates each ring; clients join their own ring.
class ShmRouterLink {
public:
    using BindParams = ShmSpsc::BindParams;

    ShmRouterLink(ShmRouterLink&&) noexcept = default;
    ShmRouterLink& operator=(ShmRouterLink&&) noexcept = default;
    ShmRouterLink(const ShmRouterLink&) = delete;
    ShmRouterLink& operator=(const ShmRouterLink&) = delete;

    static ShmRouterLink server(const RouterTopology& topo) {
        return ShmRouterLink(topo, true, kEndpointInvalid);
    }

    static ShmRouterLink client(const RouterTopology& topo, uint8_t peer_id) {
        ShmRouterLink link(topo, false, peer_id);
        link.bind_peer();
        return link;
    }

    void bind_router(const BindParams& params) {
        if (!is_server_) {
            throw std::runtime_error("bind_router on client link");
        }
        (void)params;
        peer_channels_.clear();
        peer_channels_.reserve(topo_.peer_count);

        for (size_t i = 0; i < topo_.peer_count; ++i) {
            const PeerEntry& entry = topo_.peers[i];
            if (entry.local.kind != PeerAddressKind::ShmRing) {
                continue;
            }
            IpcEndpoint<ShmSpsc> endpoint;
            bind_shm_endpoint(endpoint, entry, true);  // ADR 0009 per-peer sizing
            peer_channels_.emplace_back(entry.id, std::move(endpoint));
        }

        if (peer_channels_.empty()) {
            throw std::runtime_error("shm router: no shm peers in topology");
        }
    }

    void set_recv_timeout_ms(int) {
        // SHM router polls with try_recv; no socket timeout.
    }

    void set_recv_blocking() {}

    // C7 (ADR 0016) — priority-aware drop-lowest-first under backpressure.
    // `floor` is a minimum priority (0..7). While a destination ring is
    // congested (its previous send hit a full ring), incoming frames whose
    // 3-bit priority is below the floor are shed *before* attempting a send,
    // reserving the ring's drain bandwidth for higher-priority traffic. SPSC
    // is preserved — nothing already enqueued is evicted; we only decline to
    // admit low-priority frames while the ring is backed up. floor == 0 (the
    // default) disables the policy, so behavior is byte-for-byte the legacy
    // unconditional drop-on-full. Congestion clears on the next successful send.
    void set_priority_drop_floor(uint8_t floor) { priority_drop_floor_ = floor; }
    uint8_t priority_drop_floor() const noexcept { return priority_drop_floor_; }

    ForwardResult forward(
        RouterFrame& frame,
        uint64_t timestamp_ns,
        const RouteRule* rules,
        size_t rule_count) {
        if (!is_server_) {
            throw std::runtime_error("forward on client link");
        }

        const uint8_t source = try_receive(frame, timestamp_ns);
        if (source == kEndpointInvalid) {
            metrics_->recv_empty.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        ForwardResult result;
        result.source = source;
        result.targets = route_targets_for(rules, rule_count, source,
                                            frame.topic_id());
        for (uint8_t dest : result.targets) {
            send_to_peer(dest, frame.read_only());
        }
        return result;
    }

    // Phase H — receive-and-resolve half of forward() with no egress, for the
    // mixed-transport router's cooperative poll (see link.hpp). Scans the
    // per-peer SPSC rings, returns the first ready frame's source peer id (or
    // kEndpointInvalid when every ring is empty) and stamps source + timestamp
    // into `frame`. Does NOT bump recv_empty — the mixed loop decides idleness
    // across all transports, so single-transport forward() owns that counter.
    uint8_t try_receive(RouterFrame& frame, uint64_t timestamp_ns) {
        for (const auto& channel : peer_channels_) {
            Buffer buf = frame.writable();
            ShmSpsc::RecvResult recv{};
            if (!ShmSpsc::try_recv(channel.endpoint.handle(), buf, recv)) {
                continue;
            }
            if (buf.size < kRouterFrameSize) {
                metrics_->recv_truncated.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const uint8_t source = channel.peer_id;
            frame.set_source(source);
            frame.set_timestamp_ns(timestamp_ns);
            return source;
        }
        return kEndpointInvalid;
    }

    // Phase H — egress half of forward(), made public so the mixed router can
    // deliver frames that arrived on a different transport into a SHM ring.
    // Drop-on-full per ADR 0006; C7 (ADR 0016) adds optional priority-aware
    // drop-lowest-first via set_priority_drop_floor (off by default).
    void send_to_peer(uint8_t dest, const Buffer& payload) {
        for (auto& channel : peer_channels_) {
            if (channel.peer_id == dest) {
                const uint8_t prio = priority_from_frame(payload);

                // C7 — while this ring is backed up, shed sub-floor frames
                // before touching the ring so high-priority frames win the
                // next freed slot. floor == 0 disables (legacy behavior).
                if (priority_drop_floor_ > 0 && channel.congested
                    && prio < priority_drop_floor_) {
                    record_drop(dest, prio);
                    return;
                }

                if (try_send_shm_buffer(channel.endpoint, payload)
                    == ShmSendResult::Ok) {
                    channel.congested = false;
                    metrics_->forwarded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    channel.congested = true;
                    record_drop(dest, prio);
                }
                return;
            }
        }
        throw std::runtime_error("shm router: no channel for peer id "
            + std::to_string(dest));
    }

    // Phase C3: live counters. Returned reference is stable for the lifetime
    // of this link (heap-allocated; moves transfer ownership).
    const ShmRouterMetrics& metrics() const noexcept { return *metrics_; }

    void send_to_router(const RouterFrame& frame) {
        if (is_server_) {
            throw std::runtime_error("send_to_router on server link");
        }
        send_shm_buffer(endpoint_, frame.read_only());
    }

    bool recv_message(RouterFrame& frame) {
        if (is_server_) {
            throw std::runtime_error("recv_message on server link");
        }
        Buffer buf = frame.writable();
        ShmSpsc::RecvResult recv{};
        if (!ShmSpsc::try_recv(endpoint_.handle(), buf, recv)) {
            return false;
        }
        return buf.size >= kRouterFrameSize;
    }

    bool recv_message_until(
        uint8_t wanted_source,
        RouterFrame& frame,
        volatile std::sig_atomic_t* stop = app_stop_flag(),
        int poll_timeout_ms = 200) {
        (void)poll_timeout_ms;
        while (!*stop) {
            if (recv_message(frame) && frame.source() == wanted_source) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

private:
    struct PeerChannel {
        uint8_t peer_id;
        IpcEndpoint<ShmSpsc> endpoint;
        // C7 — set when the last send to this ring hit a full ring; cleared on
        // the next successful send. Gates the priority-aware drop-lowest-first.
        bool congested = false;
    };

    // C7 — read the 3-bit priority out of a serialized RouterFrame buffer.
    // send_to_peer always receives full frame bytes (callers guarantee
    // size >= kRouterFrameSize before egress), so the flags byte is present.
    static uint8_t priority_from_frame(const Buffer& payload) {
        if (payload.size < kRouterFrameSize) {
            return 0;
        }
        const uint8_t flags =
            static_cast<const uint8_t*>(payload.data)[kRouterFlagsOffset];
        return static_cast<uint8_t>(
            (flags & kFlagPriorityMask) >> kFlagPriorityShift);
    }

    void record_drop(uint8_t dest, uint8_t prio) {
        metrics_->dropped_full.fetch_add(1, std::memory_order_relaxed);
        metrics_->dropped_full_per_peer[dest].fetch_add(
            1, std::memory_order_relaxed);
        metrics_->dropped_by_priority[prio].fetch_add(
            1, std::memory_order_relaxed);
    }

    ShmRouterLink(const RouterTopology& topo, bool is_server, uint8_t peer_id)
        : topo_(topo), is_server_(is_server), peer_id_(peer_id) {}

    void bind_peer() {
        const PeerEntry* entry = peer_by_id(topo_, peer_id_);
        if (!entry) {
            throw std::runtime_error("unknown peer id");
        }
        bind_shm_endpoint(endpoint_, *entry, false);  // ADR 0009 per-peer sizing
    }

    // Phase C1 drop-on-full (ADR 0006) and Phase D2a per-peer drop counters
    // now live in the public send_to_peer above (Phase H made it public so the
    // mixed-transport router can drive SHM egress).

    const RouterTopology& topo_;
    bool is_server_;
    uint8_t peer_id_;
    uint8_t priority_drop_floor_ = 0;  // C7 — 0 disables priority-aware drop
    IpcEndpoint<ShmSpsc> endpoint_;
    std::vector<PeerChannel> peer_channels_;
    std::unique_ptr<ShmRouterMetrics> metrics_ = std::make_unique<ShmRouterMetrics>();
};

inline ShmRouterLink make_shm_router_link_server(const RouterTopology& topo) {
    return ShmRouterLink::server(topo);
}

inline ShmRouterLink make_shm_router_link_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return ShmRouterLink::client(topo, peer_id);
}
