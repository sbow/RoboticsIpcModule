#pragma once

// Phase H — mixed-transport router (ADR 0014).
//
// The templated RouterServer<Link> serves exactly one transport per instance:
// a profile that mixes SHM, UDS and UDP peers cannot be served by a single
// RouterServer because the link type is fixed at compile time. MixedRouterServer
// lifts that constraint for the single-host case. It holds an std::optional link
// per transport actually present in the topology and drives them from one
// cooperative, single-threaded, non-blocking poll loop:
//
//   * Receive is split from send (DatagramRouterLink/ShmRouterLink expose
//     try_receive() + send_to_peer()), so a frame that arrives on one transport
//     can be delivered to a peer on a different transport — a single in-process
//     hop, no bridge process.
//   * Datagram links poll non-blocking (MSG_DONTWAIT) so no transport starves
//     another. The SHM hot path is unchanged (it always spin-polled). The cost
//     is a bounded idle-pickup latency on datagram ingress (<= idle_sleep_us,
//     ADR 0007); under load the loop never sleeps. The parked eventfd doorbell
//     (ADR 0007) is the documented path to erase even that.
//   * Egress is dispatched by a peer-id -> transport map so route rules stay
//     fully transport-agnostic (Phase G per-topic rules compose unchanged —
//     RouteTargets are just peer ids).
//
// Cross-host federation (UDP gateways between routers) is explicitly out of
// scope and parked as a follow-on; see plans/H-mixed-transport-router.md.

#include "ipc/datagram.hpp"
#include "router/lifecycle.hpp"
#include "router/link.hpp"
#include "router/node.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>

// True when the topology's peers span more than one transport kind, i.e. the
// profile needs MixedRouterServer rather than a single-transport RouterServer.
inline bool topology_is_mixed(const RouterTopology& topo) {
    bool shm = false, uds = false, udp = false;
    for (size_t i = 0; i < topo.peer_count; ++i) {
        switch (topo.peers[i].local.kind) {
            case PeerAddressKind::ShmRing:     shm = true; break;
            case PeerAddressKind::UdsPath:     uds = true; break;
            case PeerAddressKind::UdpEndpoint: udp = true; break;
        }
    }
    return (static_cast<int>(shm) + static_cast<int>(uds)
            + static_cast<int>(udp)) > 1;
}

class MixedRouterServer {
public:
    explicit MixedRouterServer(const RouterTopology& topo) : topo_(topo) {
        peer_known_.fill(false);
        peer_transport_.fill(PeerAddressKind::ShmRing);
        for (size_t i = 0; i < topo.peer_count; ++i) {
            const PeerEntry& p = topo.peers[i];
            peer_known_[p.id]    = true;
            peer_transport_[p.id] = p.local.kind;
            switch (p.local.kind) {
                case PeerAddressKind::ShmRing:     has_shm_ = true; break;
                case PeerAddressKind::UdsPath:     has_uds_ = true; break;
                case PeerAddressKind::UdpEndpoint: has_udp_ = true; break;
            }
        }
        if (has_shm_) {
            shm_.emplace(ShmRouterLink::server(topo));
        }
        if (has_uds_) {
            uds_.emplace(DatagramRouterLink<Uds>::server(topo));
        }
        if (has_udp_) {
            udp_.emplace(DatagramRouterLink<Udp>::server(topo));
        }
    }

    // Bind every present link. SHM derives its per-peer rings from the topology
    // (no listen address). Each datagram link binds the matching per-transport
    // router listen endpoint, which the loader validated is present.
    void bind_router() {
        if (shm_) {
            shm_->bind_router(ShmSpsc::BindParams{});
        }
        if (uds_) {
            if (!topo_.has_listen_uds) {
                throw std::runtime_error(
                    "mixed router: uds peers present but no uds router listen");
            }
            uds_->bind_router(
                Uds::BindParams{.path = topo_.listen_uds.u.uds_path});
        }
        if (udp_) {
            if (!topo_.has_listen_udp) {
                throw std::runtime_error(
                    "mixed router: udp peers present but no udp router listen");
            }
            udp_->bind_router(Udp::BindParams{.port = topo_.listen_udp.u.udp.port});
        }
    }

    template<typename OnForward>
    void run(
        const RouteRule* rules,
        size_t rule_count,
        uint64_t (*now_ns)(),
        OnForward on_forward,
        RouterRunOptions opts = {},
        volatile std::sig_atomic_t* stop_flag = router_stop_flag()) {
        RouterFrame frame;
        auto last_activity = std::chrono::steady_clock::now();

        while (!*stop_flag) {
            const uint64_t ts = now_ns();
            bool did_work = false;
            if (shm_) {
                did_work |= poll_link(*shm_, frame, ts, rules, rule_count,
                                      on_forward);
            }
            if (uds_) {
                did_work |= poll_link(*uds_, frame, ts, rules, rule_count,
                                      on_forward);
            }
            if (udp_) {
                did_work |= poll_link(*udp_, frame, ts, rules, rule_count,
                                      on_forward);
            }

            if (did_work) {
                last_activity = std::chrono::steady_clock::now();
            } else if (opts.idle_exit_ms > 0
                && router_idle_expired(
                    last_activity,
                    std::chrono::milliseconds(opts.idle_exit_ms))) {
                return;
            } else if (opts.idle_sleep_us > 0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(opts.idle_sleep_us));
            } else {
                std::this_thread::yield();
            }
        }
    }

    // Metrics roll-up — aggregate counters across whichever links exist.
    uint64_t forwarded() const noexcept {
        uint64_t n = 0;
        if (shm_) n += shm_->metrics().forwarded.load(std::memory_order_relaxed);
        if (uds_) n += uds_->metrics().forwarded.load(std::memory_order_relaxed);
        if (udp_) n += udp_->metrics().forwarded.load(std::memory_order_relaxed);
        return n;
    }

    uint64_t dropped_full() const noexcept {
        // Only the SHM link can drop on a full ring (datagram egress has no
        // bounded queue the router owns).
        return shm_ ? shm_->metrics().dropped_full.load(std::memory_order_relaxed)
                    : 0;
    }

    bool has_shm() const noexcept { return has_shm_; }
    bool has_uds() const noexcept { return has_uds_; }
    bool has_udp() const noexcept { return has_udp_; }

private:
    template<typename Link, typename OnForward>
    bool poll_link(
        Link& link,
        RouterFrame& frame,
        uint64_t ts,
        const RouteRule* rules,
        size_t rule_count,
        OnForward& on_forward) {
        const uint8_t source = link.try_receive(frame, ts);
        if (source == kEndpointInvalid) {
            return false;
        }
        const RouteTargets targets = route_targets_for(
            rules, rule_count, source, frame.topic_id());
        const Buffer payload = frame.read_only();
        for (uint8_t dest : targets) {
            send_to_peer(dest, payload);
            on_forward(source, dest, frame);
        }
        return true;
    }

    // Dispatch egress to the link that owns the destination peer's transport.
    void send_to_peer(uint8_t dest, const Buffer& payload) {
        if (dest >= peer_known_.size() || !peer_known_[dest]) {
            return;
        }
        switch (peer_transport_[dest]) {
            case PeerAddressKind::ShmRing:
                if (shm_) shm_->send_to_peer(dest, payload);
                break;
            case PeerAddressKind::UdsPath:
                if (uds_) uds_->send_to_peer(dest, payload);
                break;
            case PeerAddressKind::UdpEndpoint:
                if (udp_) udp_->send_to_peer(dest, payload);
                break;
        }
    }

    const RouterTopology& topo_;
    std::optional<ShmRouterLink> shm_;
    std::optional<DatagramRouterLink<Uds>> uds_;
    std::optional<DatagramRouterLink<Udp>> udp_;
    bool has_shm_ = false;
    bool has_uds_ = false;
    bool has_udp_ = false;
    std::array<bool, 256> peer_known_{};
    std::array<PeerAddressKind, 256> peer_transport_{};
};

inline MixedRouterServer make_mixed_router_server(const RouterTopology& topo) {
    return MixedRouterServer(topo);
}
