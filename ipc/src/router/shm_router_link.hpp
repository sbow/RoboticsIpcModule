#pragma once

#include "ipc/app_shutdown.hpp"
#include "ipc/endpoint.hpp"
#include "ipc/shm_spsc.hpp"
#include "router/frame.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_peer_address_io.hpp"

#include <cstdint>
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
            bind_shm_endpoint(endpoint, entry.local, true);
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

    ForwardResult forward(
        RouterFrame& frame,
        uint64_t timestamp_ns,
        const RouteRule* rules,
        size_t rule_count) {
        if (!is_server_) {
            throw std::runtime_error("forward on client link");
        }

        for (const auto& channel : peer_channels_) {
            Buffer buf = frame.writable();
            ShmSpsc::RecvResult recv{};
            if (!ShmSpsc::try_recv(channel.endpoint.handle(), buf, recv)) {
                continue;
            }
            if (buf.size < kRouterFrameSize) {
                continue;
            }

            const uint8_t source = channel.peer_id;
            frame.set_source(source);
            frame.set_timestamp_ns(timestamp_ns);

            ForwardResult result;
            result.source = source;
            result.targets = route_targets_for(rules, rule_count, source);
            for (uint8_t dest : result.targets) {
                send_to_peer(dest, buf);
            }
            return result;
        }
        return {};
    }

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
    };

    ShmRouterLink(const RouterTopology& topo, bool is_server, uint8_t peer_id)
        : topo_(topo), is_server_(is_server), peer_id_(peer_id) {}

    void bind_peer() {
        const PeerEntry* entry = peer_by_id(topo_, peer_id_);
        if (!entry) {
            throw std::runtime_error("unknown peer id");
        }
        bind_shm_endpoint(endpoint_, entry->local, false);
    }

    void send_to_peer(uint8_t dest, const Buffer& payload) {
        for (auto& channel : peer_channels_) {
            if (channel.peer_id == dest) {
                send_shm_buffer(channel.endpoint, payload);
                return;
            }
        }
        throw std::runtime_error("shm router: no channel for peer id "
            + std::to_string(dest));
    }

    const RouterTopology& topo_;
    bool is_server_;
    uint8_t peer_id_;
    IpcEndpoint<ShmSpsc> endpoint_;
    std::vector<PeerChannel> peer_channels_;
};

inline ShmRouterLink make_shm_router_link_server(const RouterTopology& topo) {
    return ShmRouterLink::server(topo);
}

inline ShmRouterLink make_shm_router_link_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return ShmRouterLink::client(topo, peer_id);
}
