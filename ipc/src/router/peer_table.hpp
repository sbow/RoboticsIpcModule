#pragma once

#include "router/frame.hpp"

#include <cstdint>
#include <cstring>
#include <stddef.h>

enum class PeerAddressKind {
    UdsPath,
    UdpEndpoint,
    ShmRing,
};

struct PeerAddress {
    PeerAddressKind kind = PeerAddressKind::UdsPath;

    union {
        const char* uds_path;
        struct {
            const char* host;
            uint16_t port;
        } udp;
        const char* shm_name;
    } u{};
};

constexpr PeerAddress peer_uds(const char* path) {
    PeerAddress addr;
    addr.kind = PeerAddressKind::UdsPath;
    addr.u.uds_path = path;
    return addr;
}

constexpr PeerAddress peer_udp(const char* host, uint16_t port) {
    PeerAddress addr;
    addr.kind = PeerAddressKind::UdpEndpoint;
    addr.u.udp.host = host;
    addr.u.udp.port = port;
    return addr;
}

constexpr PeerAddress peer_shm(const char* name) {
    PeerAddress addr;
    addr.kind = PeerAddressKind::ShmRing;
    addr.u.shm_name = name;
    return addr;
}

struct PeerEntry {
    uint8_t id;
    const char* name;
    PeerAddress local;
};

struct RouterTopology {
    const PeerEntry* peers;
    size_t peer_count;
    PeerAddress router_listen;
};

template<typename Predicate>
inline const PeerEntry* peer_find(const RouterTopology& topo, Predicate pred) {
    for (size_t i = 0; i < topo.peer_count; ++i) {
        if (pred(topo.peers[i])) {
            return &topo.peers[i];
        }
    }
    return nullptr;
}

inline const PeerEntry* peer_by_id(const RouterTopology& topo, uint8_t id) {
    return peer_find(topo, [id](const PeerEntry& e) { return e.id == id; });
}

inline const PeerEntry* peer_by_name(const RouterTopology& topo, const char* name) {
    return peer_find(topo, [name](const PeerEntry& e) {
        return std::strcmp(name, e.name) == 0;
    });
}

inline const char* peer_display_name(const RouterTopology& topo, uint8_t id) {
    if (id == kEndpointServer) {
        return "server";
    }
    const PeerEntry* entry = peer_by_id(topo, id);
    return entry ? entry->name : "invalid";
}
