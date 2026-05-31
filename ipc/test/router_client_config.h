#pragma once

#include "router/transport_kind.hpp"
#include "router_protocol.hpp"

#include <cstring>

constexpr uint8_t kEndpointSensor = 1;
constexpr uint8_t kEndpointController = 2;
constexpr uint8_t kEndpointRecorder = 3;

// Resource-name prefix: `rim` = RoboticsIpcModule. Used everywhere a
// /dev/shm name, UDS socket path, or systemd unit name needs to be
// namespaced to this module. Pre-rename: these paths used a `cpp_tricks`
// prefix from the vendored source tree (`sbow/cpp_tricks`); see
// LESSONS-LEARNED.md ("rim namespace") for the rename rationale.
constexpr const char* kRouterUdsPath = "/tmp/rim_router.sock";
constexpr const char* kSensorUdsPath = "/tmp/rim_router_a.sock";
constexpr const char* kControllerUdsPath = "/tmp/rim_router_b.sock";
constexpr const char* kRecorderUdsPath = "/tmp/rim_router_c.sock";

constexpr uint16_t kRouterUdpPort = 19100;
constexpr uint16_t kSensorUdpPort = 19101;
constexpr uint16_t kControllerUdpPort = 19102;
constexpr uint16_t kRecorderUdpPort = 19103;
constexpr const char* kRouterUdpHost = "127.0.0.1";

constexpr const char* kControllerLogPath = "/tmp/rim_router_b.log";
constexpr const char* kRecorderLogPath = "/tmp/rim_router_c.log";

constexpr const char* kRouterShmName = "/rim_router";
constexpr const char* kSensorShmName = "/rim_router_sensor";
constexpr const char* kControllerShmName = "/rim_router_controller";
constexpr const char* kRecorderShmName = "/rim_router_recorder";

constexpr PeerEntry kDemoPeers[] = {
    {kEndpointSensor, "sensor", peer_uds(kSensorUdsPath)},
    {kEndpointController, "controller", peer_uds(kControllerUdsPath)},
    {kEndpointRecorder, "recorder", peer_uds(kRecorderUdsPath)},
};

constexpr RouteRule kDemoRouteRules[] = {
    make_route(kEndpointSensor, kEndpointController, kEndpointRecorder),
    make_route(kEndpointController, kEndpointRecorder),
};

inline const RouterTopology& demo_topology_uds() {
    static const RouterTopology topo{
        kDemoPeers,
        sizeof(kDemoPeers) / sizeof(kDemoPeers[0]),
        peer_uds(kRouterUdsPath),
    };
    return topo;
}

inline const RouterTopology& demo_topology_udp() {
    static const PeerEntry peers[] = {
        {kEndpointSensor, "sensor", peer_udp(kRouterUdpHost, kSensorUdpPort)},
        {kEndpointController, "controller", peer_udp(kRouterUdpHost, kControllerUdpPort)},
        {kEndpointRecorder, "recorder", peer_udp(kRouterUdpHost, kRecorderUdpPort)},
    };
    static const RouterTopology topo{
        peers,
        sizeof(peers) / sizeof(peers[0]),
        peer_udp(kRouterUdpHost, kRouterUdpPort),
    };
    return topo;
}

inline const RouterTopology& demo_topology_shm() {
    static const PeerEntry peers[] = {
        {kEndpointSensor, "sensor", peer_shm(kSensorShmName)},
        {kEndpointController, "controller", peer_shm(kControllerShmName)},
        {kEndpointRecorder, "recorder", peer_shm(kRecorderShmName)},
    };
    static const RouterTopology topo{
        peers,
        sizeof(peers) / sizeof(peers[0]),
        peer_shm(kRouterShmName),
    };
    return topo;
}

inline const RouterTopology& demo_topology(TransportKind kind) {
    switch (kind) {
        case TransportKind::Udp:
            return demo_topology_udp();
        case TransportKind::Shm:
            return demo_topology_shm();
        case TransportKind::Uds:
        default:
            return demo_topology_uds();
    }
}

inline const RouterTopology& demo_topology() {
    return demo_topology_uds();
}

inline const char* demo_log_path(const char* role) {
    if (std::strcmp(role, "controller") == 0) {
        return kControllerLogPath;
    }
    if (std::strcmp(role, "recorder") == 0) {
        return kRecorderLogPath;
    }
    return nullptr;
}
