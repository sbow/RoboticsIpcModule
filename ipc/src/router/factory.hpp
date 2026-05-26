#pragma once

#include "ipc/datagram.hpp"
#include "ipc/shm_spsc.hpp"
#include "router/node.hpp"
#include "router/peer_address_io.hpp"
#include "router/shm_peer_address_io.hpp"
#include "router/transport_kind.hpp"

#include <cstring>

template<typename Fn>
inline int dispatch_transport_kind(const char* transport, Fn&& fn) {
    if (std::strcmp(transport, "uds") == 0) {
        return fn.template operator()<Uds>();
    }
    if (std::strcmp(transport, "udp") == 0) {
        return fn.template operator()<Udp>();
    }
    if (std::strcmp(transport, "shm") == 0) {
        return fn.template operator()<ShmSpsc>();
    }
    return -1;
}

template<typename Fn>
inline int dispatch_router_kind(TransportKind kind, Fn&& fn) {
    switch (kind) {
        case TransportKind::Uds:
            return fn.template operator()<Uds>();
        case TransportKind::Udp:
            return fn.template operator()<Udp>();
        case TransportKind::Shm:
            return fn.template operator()<ShmSpsc>();
    }
    return -1;
}

template<DatagramTransport Transport>
inline DatagramRouterServer<Transport> make_datagram_router_server(
    const RouterTopology& topo) {
    return DatagramRouterServer<Transport>(topo);
}

template<DatagramTransport Transport>
inline DatagramRouterClient<Transport> make_datagram_router_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return DatagramRouterClient<Transport>(topo, peer_id);
}

inline ShmRouterServer make_shm_router_server(const RouterTopology& topo) {
    return ShmRouterServer(topo);
}

inline ShmRouterClient make_shm_router_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return ShmRouterClient(topo, peer_id);
}

template<DatagramTransport Transport>
inline void bind_datagram_router_listen(
    DatagramRouterServer<Transport>& server,
    const RouterTopology& topo) {
    server.bind_router(router_listen_bind_params<Transport>(topo));
}

inline void bind_shm_router_listen(
    ShmRouterServer& server,
    const RouterTopology& topo) {
    server.bind_router(router_listen_bind_params_shm(topo));
}

template<typename Fn>
inline void with_datagram_transport(TransportKind kind, Fn&& fn) {
    switch (kind) {
        case TransportKind::Uds:
            fn.template operator()<Uds>();
            break;
        case TransportKind::Udp:
            fn.template operator()<Udp>();
            break;
        default:
            break;
    }
}

// Backward-compatible names (datagram only).
template<DatagramTransport Transport>
inline DatagramRouterServer<Transport> make_router_server(const RouterTopology& topo) {
    return make_datagram_router_server<Transport>(topo);
}

template<DatagramTransport Transport>
inline DatagramRouterClient<Transport> make_router_client(
    const RouterTopology& topo,
    uint8_t peer_id) {
    return make_datagram_router_client<Transport>(topo, peer_id);
}

template<DatagramTransport Transport>
inline void bind_router_listen(
    DatagramRouterServer<Transport>& server,
    const RouterTopology& topo) {
    bind_datagram_router_listen(server, topo);
}
