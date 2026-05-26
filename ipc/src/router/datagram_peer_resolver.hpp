#pragma once

#include "ipc/datagram.hpp"
#include "ipc/transport.hpp"
#include "router/peer_table.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <type_traits>

template<DatagramTransport Transport>
inline uint8_t peer_id_from_recv(
    const RouterTopology& topo,
    const typename Transport::RecvResult& recv) {
    if constexpr (std::is_same_v<Transport, Uds>) {
        const PeerEntry* entry = peer_find(topo, [&](const PeerEntry& e) {
            return e.local.kind == PeerAddressKind::UdsPath
                && std::strcmp(e.local.u.uds_path, recv.from.sun_path) == 0;
        });
        return entry ? entry->id : kEndpointInvalid;
    } else if constexpr (std::is_same_v<Transport, Udp>) {
        const uint16_t port = ntohs(recv.from.sin_port);
        const PeerEntry* entry = peer_find(topo, [&](const PeerEntry& e) {
            return e.local.kind == PeerAddressKind::UdpEndpoint
                && e.local.u.udp.port == port;
        });
        return entry ? entry->id : kEndpointInvalid;
    } else {
        return kEndpointInvalid;
    }
}
