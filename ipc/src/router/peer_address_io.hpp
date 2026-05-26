#pragma once

#include "ipc/datagram.hpp"
#include "ipc/endpoint.hpp"
#include "ipc/transport.hpp"
#include "router/peer_table.hpp"

#include <stdexcept>
#include <type_traits>
#include <unistd.h>

template<DatagramTransport Transport>
inline void bind_datagram_endpoint(
    IpcEndpoint<Transport>& endpoint,
    const PeerAddress& addr) {
    if constexpr (std::is_same_v<Transport, Uds>) {
        if (addr.kind != PeerAddressKind::UdsPath) {
            throw std::runtime_error("uds endpoint expected uds peer address");
        }
        endpoint.bind(typename Uds::BindParams{.path = addr.u.uds_path});
    } else if constexpr (std::is_same_v<Transport, Udp>) {
        if (addr.kind != PeerAddressKind::UdpEndpoint) {
            throw std::runtime_error("udp endpoint expected udp peer address");
        }
        endpoint.bind(typename Udp::BindParams{.port = addr.u.udp.port});
    } else {
        throw std::runtime_error("unsupported datagram transport for bind");
    }
}

inline void send_datagram_to_address(
    int fd,
    const PeerAddress& addr,
    const Buffer& payload) {
    switch (addr.kind) {
        case PeerAddressKind::UdsPath:
            Uds::send_to(fd, addr.u.uds_path, payload);
            break;
        case PeerAddressKind::UdpEndpoint:
            Udp::send_to(fd, addr.u.udp.host, addr.u.udp.port, payload);
            break;
        default:
            throw std::runtime_error("unsupported peer address kind for datagram send");
    }
}

template<DatagramTransport Transport>
inline typename Transport::BindParams router_listen_bind_params(
    const RouterTopology& topo) {
    const PeerAddress& listen = topo.router_listen;
    if constexpr (std::is_same_v<Transport, Uds>) {
        if (listen.kind != PeerAddressKind::UdsPath) {
            throw std::runtime_error("uds topology expected uds router listen address");
        }
        return typename Uds::BindParams{.path = listen.u.uds_path};
    } else if constexpr (std::is_same_v<Transport, Udp>) {
        if (listen.kind != PeerAddressKind::UdpEndpoint) {
            throw std::runtime_error("udp topology expected udp router listen address");
        }
        return typename Udp::BindParams{.port = listen.u.udp.port};
    } else {
        throw std::runtime_error("unsupported datagram transport for router bind");
    }
}
