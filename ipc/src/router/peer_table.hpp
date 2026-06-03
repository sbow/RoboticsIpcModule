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

    // ADR 0009 — per-peer SHM ring sizing. Only consulted when local.kind is
    // PeerAddressKind::ShmRing. Zero means "use ShmSpsc::BindParams defaults"
    // (256 slots × 1024 B payload). Non-zero values must satisfy the
    // topology-loader validation (slot_count <= 2^20; max_payload between
    // kRouterFrameSize and 256 MiB).
    uint32_t shm_slot_count  = 0;
    uint32_t shm_max_payload = 0;
};

// Forward declaration — TopicEntry lives in router/topic_table.hpp.
// RouterTopology only needs to hold a const-pointer + count for the
// (optional) topic registry, so it does not have to pull in that
// header. Consumers that actually look up topics (`topic_by_id` etc.)
// include topic_table.hpp explicitly.
struct TopicEntry;

struct RouterTopology {
    const PeerEntry* peers;
    size_t peer_count;
    PeerAddress router_listen;

    // Phase F C5 Scope B — optional declarative topic registry.
    // Default-empty so every existing compile-time RouterTopology
    // aggregate-init keeps working without modification. The router
    // does not consult this catalog today; bridges and tooling do.
    const TopicEntry* topics = nullptr;
    size_t topic_count = 0;

    // Phase H — additional per-datagram-transport router listen endpoints, used
    // only by the mixed-transport router (mixed_router_server.hpp). A mixed
    // profile binds one SHM ring set (derived from peers, needs no listen),
    // plus a UDS and/or UDP listen socket for its datagram peers. has_* == false
    // means this router does not serve that datagram transport; single-transport
    // and homogeneous profiles leave them unset and keep using router_listen.
    bool has_listen_uds = false;
    PeerAddress listen_uds{};
    bool has_listen_udp = false;
    PeerAddress listen_udp{};
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
