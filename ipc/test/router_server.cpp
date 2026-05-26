#include "router_app.h"
#include "router_client_config.h"
#include "router/factory.hpp"
#include "router_protocol.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <type_traits>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point kStartTime = Clock::now();

uint64_t ns_since_start() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - kStartTime).count());
}

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " uds [router_path]\n"
              << "       " << prog << " udp [port]\n"
              << "       " << prog << " shm\n";
}

RouterRunOptions run_options() {
    RouterRunOptions opts;
    opts.poll_timeout_ms = 200;
    if (router_test_mode()) {
        opts.idle_exit_ms = 1500;
    }
    return opts;
}

template<typename Server>
void run_forward_loop(Server& server, const RouterTopology& topo) {
    const RouteRule* rules = kDemoRouteRules;
    const size_t rule_count = sizeof(kDemoRouteRules) / sizeof(kDemoRouteRules[0]);

    server.run(
        rules,
        rule_count,
        ns_since_start,
        [&](uint8_t source, uint8_t dest, const RouterFrame& frame) {
            router_log(router_route_line(topo, source, dest, frame));
        },
        run_options());
}

template<DatagramTransport Transport>
void run_datagram_router(const RouterTopology& topo) {
    auto server = make_datagram_router_server<Transport>(topo);
    bind_datagram_router_listen(server, topo);
    std::cerr << "router listening ("
              << peer_display_name(topo, kEndpointServer) << ")\n";
    run_forward_loop(server, topo);
}

void run_shm_router(const RouterTopology& topo) {
    auto server = make_shm_router_server(topo);
    bind_shm_router_listen(server, topo);
    std::cerr << "SHM router on shared-memory rings\n";
    run_forward_loop(server, topo);
}

struct ServerRunner {
    int argc;
    char** argv;

    template<typename Transport>
    int operator()() const {
        if constexpr (std::is_same_v<Transport, ShmSpsc>) {
            const RouterTopology& topo = demo_topology(TransportKind::Shm);
            run_shm_router(topo);
            return 0;
        } else {
            const TransportKind kind =
                std::is_same_v<Transport, Udp> ? TransportKind::Udp : TransportKind::Uds;
            const RouterTopology& topo = demo_topology(kind);

            if constexpr (std::is_same_v<Transport, Uds>) {
                const std::string path = (argc >= 3) ? argv[2] : kRouterUdsPath;
                if (path != kRouterUdsPath) {
                    ::unlink(path.c_str());
                }
                std::cerr << "UDS router on " << path << '\n';
            } else {
                const uint16_t port = (argc >= 3)
                    ? static_cast<uint16_t>(std::stoi(argv[2]))
                    : kRouterUdpPort;
                std::cerr << "UDP router on port " << port << '\n';
                (void)port;
            }

            run_datagram_router<Transport>(topo);
            return 0;
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    install_router_stop_handlers();

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    try {
        const int rc = dispatch_transport_kind(
            argv[1], ServerRunner{argc, argv});
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
