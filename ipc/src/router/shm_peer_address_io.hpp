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

inline void send_shm_buffer(IpcEndpoint<ShmSpsc>& endpoint, const Buffer& payload) {
    ShmSpsc::SendParams params{.payload = payload};
    endpoint.send(params, payload);
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
