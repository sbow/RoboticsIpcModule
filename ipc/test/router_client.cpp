#include "router_app.h"
#include "router_client_config.h"
#include "router/factory.hpp"
#include "router_protocol.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

namespace {

constexpr int kRecvTimeoutMs = 200;
constexpr int kRecorderTestIdleExitMs = 500;
constexpr int kSensorMessages = 5;
constexpr int kControllerMessages = 3;
constexpr int kControllerListenMs = 2500;

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " sensor uds [router_path] [client_path]\n"
              << "       " << prog << " controller uds [router_path] [client_path] [log_path]\n"
              << "       " << prog << " recorder uds [client_path] [log_path]\n"
              << "       " << prog << " sensor udp [router_port] [client_port]\n"
              << "       " << prog << " controller udp [router_port] [client_port] [log_path]\n"
              << "       " << prog << " recorder udp [client_port] [log_path]\n"
              << "       " << prog << " sensor shm\n"
              << "       " << prog << " controller shm [log_path]\n"
              << "       " << prog << " recorder shm [log_path]\n";
}

void append_record(const std::string& log_path, const RouterFrame& frame) {
    std::ofstream out(log_path, std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open log: " + log_path);
    }
    out << static_cast<int>(frame.source()) << ','
        << frame.timestamp_ns() << ','
        << frame.payload() << '\n';
}

void log_received(
    const RouterTopology& topo,
    const char* role,
    const std::string& log_path,
    const RouterFrame& frame) {
    append_record(log_path, frame);
    router_log(std::string(role) + " recv: "
        + frame.describe(peer_display_name(topo, frame.source())));
}

void log_sent(
    const RouterTopology& topo,
    const char* role,
    uint8_t source_id,
    const std::string& payload) {
    router_log(std::string(role) + " send: source="
        + peer_display_name(topo, source_id) + " payload=" + payload);
}

std::string log_path_for_role(const char* role, int argc, char* argv[]) {
    if (std::strcmp(role, "controller") == 0) {
        if (argc >= 6) {
            return argv[5];
        }
        if (argc >= 4) {
            return argv[3];
        }
        return demo_log_path(role);
    }
    if (std::strcmp(role, "recorder") == 0) {
        if (argc >= 5) {
            return argv[4];
        }
        if (argc >= 4) {
            return argv[3];
        }
        return demo_log_path(role);
    }
    return {};
}

template<typename Client>
void run_sensor(Client& client, const RouterTopology& topo) {
    router_log("sensor: sending " + std::to_string(kSensorMessages) + " messages");

    for (int i = 0; i < kSensorMessages; ++i) {
        const std::string payload = "sensor-" + std::to_string(i);
        log_sent(topo, "sensor", kEndpointSensor, payload);
        client.send_message(kEndpointSensor, payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    router_log("sensor: done");
}

template<typename Client>
void run_controller(Client& client, const RouterTopology& topo, const std::string& log_path) {
    router_log("controller: listening (log " + log_path + ")");

    for (int i = 0; i < kControllerMessages; ++i) {
        router_log("controller: waiting for sensor packet...");
        RouterFrame frame;
        if (!client.recv_message_until(kEndpointSensor, frame)) {
            router_log("controller: stopped");
            return;
        }
        client.set_recv_timeout_ms(kRecvTimeoutMs);
        log_received(topo, "controller", log_path, frame);

        const std::string payload = "control-" + std::to_string(i);
        log_sent(topo, "controller", kEndpointController, payload);
        client.send_message(kEndpointController, payload);
    }

    client.set_recv_timeout_ms(kRecvTimeoutMs);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kControllerListenMs);
    RouterFrame frame;
    while (std::chrono::steady_clock::now() < deadline
        && !router_stop_requested()) {
        if (client.recv_message(frame)) {
            log_received(topo, "controller", log_path, frame);
        } else {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    router_log("controller: done");
}

template<typename Client>
void run_recorder(Client& client, const RouterTopology& topo, const std::string& log_path) {
    client.set_recv_timeout_ms(kRecvTimeoutMs);
    router_log("recorder: listening (log " + log_path + ")");

    RouterFrame frame;
    auto last_activity = std::chrono::steady_clock::now();
    while (!router_stop_requested()) {
        if (client.recv_message(frame)) {
            log_received(topo, "recorder", log_path, frame);
            last_activity = std::chrono::steady_clock::now();
        } else {
            if (router_test_mode()
                && router_idle_expired(last_activity,
                    std::chrono::milliseconds(kRecorderTestIdleExitMs))) {
                break;
            }
            std::this_thread::yield();
        }
    }
}

template<typename Client>
int dispatch_role_client(
    const char* role,
    int argc,
    char* argv[],
    const RouterTopology& topo,
    Client& client) {
    const PeerEntry* entry = peer_by_name(topo, role);
    if (!entry) {
        return 1;
    }

    switch (entry->id) {
        case kEndpointSensor:
            run_sensor(client, topo);
            return 0;
        case kEndpointController:
            run_controller(client, topo, log_path_for_role(role, argc, argv));
            return 0;
        case kEndpointRecorder:
            run_recorder(client, topo, log_path_for_role(role, argc, argv));
            return 0;
        default:
            return 1;
    }
}

template<DatagramTransport Transport>
int dispatch_role_datagram(const char* role, int argc, char* argv[]) {
    const TransportKind kind =
        std::is_same_v<Transport, Udp> ? TransportKind::Udp : TransportKind::Uds;
    auto client = make_datagram_router_client<Transport>(
        demo_topology(kind), peer_by_name(demo_topology(kind), role)->id);
    return dispatch_role_client(role, argc, argv, demo_topology(kind), client);
}

int dispatch_role_shm(const char* role, int argc, char* argv[]) {
    const RouterTopology& topo = demo_topology(TransportKind::Shm);
    const PeerEntry* entry = peer_by_name(topo, role);
    if (!entry) {
        return 1;
    }
    auto client = make_shm_router_client(topo, entry->id);
    return dispatch_role_client(role, argc, argv, topo, client);
}

struct RoleDispatcher {
    const char* role;
    int argc;
    char** argv;

    template<typename Transport>
    int operator()() const {
        if constexpr (std::is_same_v<Transport, ShmSpsc>) {
            return dispatch_role_shm(role, argc, argv);
        } else {
            return dispatch_role_datagram<Transport>(role, argc, argv);
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    install_router_stop_handlers();

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const char* role = argv[1];

    try {
        const int rc = dispatch_transport_kind(
            argv[2], RoleDispatcher{role, argc, argv});
        if (rc != 0) {
            usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
