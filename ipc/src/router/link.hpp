#pragma once

#include "ipc/app_shutdown.hpp"
#include "ipc/endpoint.hpp"
#include "router/datagram_peer_resolver.hpp"
#include "router/frame.hpp"
#include "router/metrics.hpp"
#include "router/peer_address_io.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

inline std::string router_route_line(
    const RouterTopology& topo,
    uint8_t source,
    uint8_t dest,
    const RouterFrame& frame) {
    return "router: " + std::string(peer_display_name(topo, source))
        + " -> " + peer_display_name(topo, dest)
        + " payload=" + frame.payload();
}

template<DatagramTransport Transport>
class DatagramRouterLink {
public:
    using BindParams = typename Transport::BindParams;

    DatagramRouterLink(DatagramRouterLink&&) noexcept = default;
    DatagramRouterLink& operator=(DatagramRouterLink&&) noexcept = default;
    DatagramRouterLink(const DatagramRouterLink&) = delete;
    DatagramRouterLink& operator=(const DatagramRouterLink&) = delete;

    static DatagramRouterLink server(const RouterTopology& topo) {
        return DatagramRouterLink(topo, true, kEndpointInvalid);
    }

    static DatagramRouterLink client(const RouterTopology& topo, uint8_t peer_id) {
        DatagramRouterLink link(topo, false, peer_id);
        link.bind_peer();
        return link;
    }

    void bind_router(const typename Transport::BindParams& params) {
        if (!is_server_) {
            throw std::runtime_error("bind_router on client link");
        }
        endpoint_.bind(params);
    }

    void set_recv_timeout_ms(int ms) {
        endpoint_.set_recv_timeout_ms(ms);
    }

    void set_recv_blocking() {
        endpoint_.set_recv_blocking();
    }

    ForwardResult forward(
        RouterFrame& frame,
        uint64_t timestamp_ns,
        const RouteRule* rules,
        size_t rule_count) {
        if (!is_server_) {
            throw std::runtime_error("forward on client link");
        }

        typename Transport::RecvResult recv{};
        Buffer buf = frame.writable();
        endpoint_.recv(buf, recv);

        // Phase D4 — truncated datagram fault gate. A peer that sent fewer
        // than kRouterFrameSize bytes (buggy client, partial send on a
        // lossy link, hostile traffic) is dropped and counted. Without
        // the counter this failure was invisible — see ADR 0006 update.
        if (buf.size < kRouterFrameSize) {
            metrics_->recv_truncated.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        const uint8_t source = peer_id_from_recv<Transport>(topo_, recv);
        if (source == kEndpointInvalid) {
            // Phase D4 — datagram arrived from a (host, port) / socket path
            // that is not in the topology. Almost always a peer pointed at
            // the wrong listen address; could also be spoofed traffic.
            // Frame discarded.
            metrics_->recv_unknown_source.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        frame.set_source(source);
        frame.set_timestamp_ns(timestamp_ns);

        ForwardResult result;
        result.source = source;
        result.targets = route_targets_for(rules, rule_count, source,
                                            frame.topic_id());
        for (uint8_t dest : result.targets) {
            send_to_peer(dest, buf);
        }
        return result;
    }

    // Phase H — receive-and-resolve half of forward(), with no egress. The
    // mixed-transport router (mixed_router_server.hpp) needs to poll many
    // links from one thread and dispatch egress across transports, so it
    // separates "pull the next frame and tell me who sent it" from "send".
    // Non-blocking (MSG_DONTWAIT) so one idle transport never starves another.
    // Returns the resolved source peer id, or kEndpointInvalid when no frame
    // was available, the frame was truncated, or the source was unknown.
    // Stamps source + timestamp into `frame` on success.
    uint8_t try_receive(RouterFrame& frame, uint64_t timestamp_ns) {
        typename Transport::RecvResult recv{};
        Buffer buf = frame.writable();
        if (!endpoint_.try_recv(buf, recv)) {
            return kEndpointInvalid;
        }
        if (buf.size < kRouterFrameSize) {
            metrics_->recv_truncated.fetch_add(1, std::memory_order_relaxed);
            return kEndpointInvalid;
        }
        const uint8_t source = peer_id_from_recv<Transport>(topo_, recv);
        if (source == kEndpointInvalid) {
            metrics_->recv_unknown_source.fetch_add(1, std::memory_order_relaxed);
            return kEndpointInvalid;
        }
        frame.set_source(source);
        frame.set_timestamp_ns(timestamp_ns);
        return source;
    }

    // Phase H — egress half of forward(), made public so the mixed router can
    // drive cross-transport delivery. Bumps `forwarded` on a successful send
    // (a dest that is not a peer is a no-op, matching route-validated input).
    void send_to_peer(uint8_t dest, const Buffer& payload) {
        const PeerEntry* entry = peer_by_id(topo_, dest);
        if (!entry) {
            return;
        }
        send_buffer_to(entry->local, payload);
        metrics_->forwarded.fetch_add(1, std::memory_order_relaxed);
    }

    // Phase D4 — read-only metrics handle. Lives for the link's lifetime
    // (heap-allocated via unique_ptr so std::atomic members don't make
    // the link non-movable; same trick as ShmRouterLink, Phase C3).
    const DatagramRouterMetrics& metrics() const noexcept { return *metrics_; }

    void send_to_router(const RouterFrame& frame) {
        if (is_server_) {
            throw std::runtime_error("send_to_router on server link");
        }
        send_buffer_to(topo_.router_listen, frame.read_only());
    }

    bool recv_message(RouterFrame& frame) {
        if (is_server_) {
            throw std::runtime_error("recv_message on server link");
        }
        Buffer buf = frame.writable();
        try {
            typename Transport::RecvResult recv{};
            endpoint_.recv(buf, recv);
            return buf.size >= kRouterFrameSize;
        } catch (const std::runtime_error&) {
            return false;
        }
    }

    bool recv_message_until(
        uint8_t wanted_source,
        RouterFrame& frame,
        volatile std::sig_atomic_t* stop = app_stop_flag(),
        int poll_timeout_ms = 200) {
        set_recv_timeout_ms(poll_timeout_ms);
        while (!*stop) {
            if (recv_message(frame) && frame.source() == wanted_source) {
                return true;
            }
        }
        return false;
    }

    int fd() const
        requires requires(const IpcEndpoint<Transport>& ep) { ep.fd(); }
    {
        return endpoint_.fd();
    }

private:
    DatagramRouterLink(const RouterTopology& topo, bool is_server, uint8_t peer_id)
        : topo_(topo), is_server_(is_server), peer_id_(peer_id) {}

    void bind_peer() {
        const PeerEntry* entry = peer_by_id(topo_, peer_id_);
        if (!entry) {
            throw std::runtime_error("unknown peer id");
        }
        bind_datagram_endpoint(endpoint_, entry->local);
    }

    void send_buffer_to(const PeerAddress& addr, const Buffer& payload) {
        send_datagram_to_address(endpoint_.fd(), addr, payload);
    }

    const RouterTopology& topo_;
    bool is_server_;
    uint8_t peer_id_;
    IpcEndpoint<Transport> endpoint_;
    // Heap-owned so the link stays movable (atomics are non-movable).
    std::unique_ptr<DatagramRouterMetrics> metrics_
        = std::make_unique<DatagramRouterMetrics>();
};

template<DatagramTransport Transport>
inline DatagramRouterLink<Transport> make_datagram_router_link_server(
    const RouterTopology& topo) {
    return DatagramRouterLink<Transport>::server(topo);
}

template<DatagramTransport Transport>
inline DatagramRouterLink<Transport> make_datagram_router_link_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return DatagramRouterLink<Transport>::client(topo, peer_id);
}

template<typename Link>
ForwardResult router_forward(
    Link& link,
    RouterFrame& frame,
    uint64_t timestamp_ns,
    const RouteRule* rules,
    size_t rule_count) {
    return link.forward(frame, timestamp_ns, rules, rule_count);
}
