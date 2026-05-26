#pragma once

#include "ipc/endpoint.hpp"
#include "ipc/shm_spsc.hpp"
#include "router/peer_table.hpp"

#include <stdexcept>

inline void bind_shm_endpoint(
    IpcEndpoint<ShmSpsc>& endpoint,
    const PeerAddress& addr,
    bool create) {
    if (addr.kind != PeerAddressKind::ShmRing) {
        throw std::runtime_error("shm endpoint expected shm peer address");
    }
    endpoint.bind(ShmSpsc::BindParams{
        .name = addr.u.shm_name,
        .create = create,
    });
}

// ADR 0009 — bind a SHM endpoint using per-peer ring sizing overrides on
// PeerEntry. Zero-valued overrides fall back to ShmSpsc::BindParams defaults,
// preserving the behavior of compile-time topologies that don't set the
// optional fields. Used by ShmRouterLink for both the router's per-peer
// channels and the client's own ring.
inline void bind_shm_endpoint(
    IpcEndpoint<ShmSpsc>& endpoint,
    const PeerEntry& entry,
    bool create) {
    if (entry.local.kind != PeerAddressKind::ShmRing) {
        throw std::runtime_error("shm endpoint expected shm peer address");
    }
    ShmSpsc::BindParams params{
        .name   = entry.local.u.shm_name,
        .create = create,
    };
    if (entry.shm_slot_count != 0) {
        params.slot_count = entry.shm_slot_count;
    }
    if (entry.shm_max_payload != 0) {
        params.max_payload = entry.shm_max_payload;
    }
    endpoint.bind(params);
}

inline void send_shm_buffer(IpcEndpoint<ShmSpsc>& endpoint, const Buffer& payload) {
    ShmSpsc::SendParams params{.payload = payload};
    endpoint.send(params, payload);
}

// Phase C1: non-blocking publish helper for the router hot path.
// Returns ShmSendResult::Full when the destination ring has no free slot; the
// caller is expected to drop, retry, or report per ADR 0006.
inline ShmSendResult try_send_shm_buffer(
    IpcEndpoint<ShmSpsc>& endpoint, const Buffer& payload) {
    ShmSpsc::SendParams params{.payload = payload};
    return endpoint.try_send(params, payload);
}

// Validates topology; peer rings are opened in ShmRouterLink::bind_router.
// router_listen is not a separate SHM object in the per-peer-ring model.
inline ShmSpsc::BindParams router_listen_bind_params_shm(const RouterTopology& topo) {
    const PeerAddress& listen = topo.router_listen;
    if (listen.kind != PeerAddressKind::ShmRing) {
        throw std::runtime_error("shm topology expected shm router listen address");
    }
    return ShmSpsc::BindParams{.name = listen.u.shm_name, .create = false};
}
