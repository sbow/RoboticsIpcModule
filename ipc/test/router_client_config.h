#pragma once

#include "router/transport_kind.hpp"
#include "router_protocol.hpp"

#include <cstring>

constexpr uint8_t kEndpointSensor = 1;
constexpr uint8_t kEndpointController = 2;
constexpr uint8_t kEndpointRecorder = 3;

constexpr const char* kRouterUdsPath = "/tmp/cpp_tricks_router.sock";
constexpr const char* kSensorUdsPath = "/tmp/cpp_tricks_router_a.sock";
constexpr const char* kControllerUdsPath = "/tmp/cpp_tricks_router_b.sock";
constexpr const char* kRecorderUdsPath = "/tmp/cpp_tricks_router_c.sock";

constexpr uint16_t kRouterUdpPort = 19100;
constexpr uint16_t kSensorUdpPort = 19101;
constexpr uint16_t kControllerUdpPort = 19102;
constexpr uint16_t kRecorderUdpPort = 19103;
constexpr const char* kRouterUdpHost = "127.0.0.1";

constexpr const char* kControllerLogPath = "/tmp/cpp_tricks_router_b.log";
constexpr const char* kRecorderLogPath = "/tmp/cpp_tricks_router_c.log";

constexpr const char* kRouterShmName = "/cpp_tricks_router";
constexpr const char* kSensorShmName = "/cpp_tricks_router_sensor";
constexpr const char* kControllerShmName = "/cpp_tricks_router_controller";
constexpr const char* kRecorderShmName = "/cpp_tricks_router_recorder";

constexpr PeerEntry kDemoPeers[] = {
    {kEndpointSensor, "sensor", peer_uds(kSensorUdsPath)},
    {kEndpointController, "controller", peer_uds(kControllerUdsPath)},
    {kEndpointRecorder, "recorder", peer_uds(kRecorderUdsPath)},
};

constexpr RouteRule kDemoRouteRules[] = {
    {kEndpointSensor, kEndpointController, kEndpointRecorder},
    {kEndpointController, kEndpointRecorder, 0},
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
