#pragma once

#include "ipc/app_shutdown.hpp"
#include "ipc/endpoint.hpp"
#include "router/datagram_peer_resolver.hpp"
#include "router/frame.hpp"
#include "router/peer_address_io.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"

#include <cstdint>
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

        if (buf.size < kRouterFrameSize) {
            return {};
        }

        const uint8_t source = peer_id_from_recv<Transport>(topo_, recv);
        if (source == kEndpointInvalid) {
            return {};
        }

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

    void send_to_peer(uint8_t dest, const Buffer& payload) {
        const PeerEntry* entry = peer_by_id(topo_, dest);
        if (!entry) {
            return;
        }
        send_buffer_to(entry->local, payload);
    }

    void send_buffer_to(const PeerAddress& addr, const Buffer& payload) {
        send_datagram_to_address(endpoint_.fd(), addr, payload);
    }

    const RouterTopology& topo_;
    bool is_server_;
    uint8_t peer_id_;
    IpcEndpoint<Transport> endpoint_;
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
