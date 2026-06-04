#include "router_app.h"
#include "router_client_config.h"
#include "router/factory.hpp"
#include "router/mixed_router_server.hpp"
#include "router/timestamp.hpp"
#include "router/topology_loader.hpp"
#include "router_protocol.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace {

// Frame timestamp clock: CLOCK_MONOTONIC_RAW via router_now_ns() from
// router/timestamp.hpp. See docs/adr/0010-router-timestamp-clock.md.

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " uds [router_path]\n"
              << "       " << prog << " udp [port]\n"
              << "       " << prog << " shm\n"
              << "       " << prog << " --config <profile.toml>\n";
}

void demo_stderr_logger(int level, const char* msg, std::size_t len) {
    // B3 reference logger: prefix by level, write to stderr.
    const char* tag = (level == ROUTER_LOG_ERR)  ? "[err]  "
                    : (level == ROUTER_LOG_WARN) ? "[warn] "
                                                 : "[info] ";
    std::string out;
    out.reserve(7 + len + 1);
    out.append(tag);
    out.append(msg, len);
    out.push_back('\n');
    (void)!::write(STDERR_FILENO, out.data(), out.size());
}

RouterRunOptions run_options() {
    RouterRunOptions opts;
    opts.poll_timeout_ms = 200;
    if (router_test_mode()) {
        opts.idle_exit_ms = 1500;
    }
    return opts;
}

// C7 (ADR 0016) — opt-in production hardening, driven by environment so the
// default build is unchanged. A systemd unit sets these alongside
// MemoryLock=infinity / CPUAffinity= (see rim-router.service).
//   RIM_MLOCK (set, any value) → mlockall(MCL_CURRENT|MCL_FUTURE)
//   RIM_CPU=<n>                → pin the router thread to core n
void apply_rt_hardening() {
    if (std::getenv("RIM_MLOCK") != nullptr) {
        if (router_lock_memory()) {
            router_log(ROUTER_LOG_INFO, "mlockall: locked current + future pages");
        } else {
            router_log(ROUTER_LOG_WARN,
                       "mlockall failed (need CAP_IPC_LOCK / MemoryLock=infinity?)");
        }
    }
    if (const char* cpu = std::getenv("RIM_CPU")) {
        const int core = std::atoi(cpu);
        if (router_pin_to_core(core)) {
            router_log(ROUTER_LOG_INFO,
                       std::string("pinned to core ") + std::to_string(core));
        } else {
            router_log(ROUTER_LOG_WARN,
                       std::string("pin to core ") + cpu + " failed");
        }
    }
}

// C7 — priority-aware drop-lowest-first floor (SHM links only). 0 = disabled.
// RIM_PRIORITY_DROP_FLOOR=<0..7>.
uint8_t priority_drop_floor_from_env() {
    const char* v = std::getenv("RIM_PRIORITY_DROP_FLOOR");
    if (v == nullptr) {
        return 0;
    }
    const int floor = std::atoi(v);
    if (floor <= 0) return 0;
    if (floor > 7) return 7;
    return static_cast<uint8_t>(floor);
}

template<typename Server>
void run_forward_loop(Server& server,
                      const RouterTopology& topo,
                      const RouteRule* rules,
                      std::size_t rule_count) {
    // C6: every server runner above binds its endpoint(s) before calling here,
    // so this is the single point where the router is "ready". Tell systemd so
    // units gated `After=rim-router.service` are released only now, not at
    // exec() time (no-op when not under Type=notify). See ADR 0015.
    if (router_notify_ready()) {
        router_log(ROUTER_LOG_INFO, "sd_notify READY=1 (endpoints bound)");
    }

    server.run(
        rules,
        rule_count,
        router_now_ns,
        [&](uint8_t source, uint8_t dest, const RouterFrame& frame) {
            router_log(ROUTER_LOG_INFO,
                       router_route_line(topo, source, dest, frame));
        },
        run_options());

    // The forward loop has unwound (stop requested or idle-exit). Mark the
    // shutdown window so systemd sees a clean stop rather than a crash.
    router_notify_stopping();
}

template<DatagramTransport Transport>
void run_datagram_router(const RouterTopology& topo,
                         const RouteRule* rules,
                         std::size_t rule_count) {
    auto server = make_datagram_router_server<Transport>(topo);
    bind_datagram_router_listen(server, topo);
    router_log(ROUTER_LOG_INFO,
               std::string("router listening (")
               + peer_display_name(topo, kEndpointServer) + ")");
    run_forward_loop(server, topo, rules, rule_count);
}

void run_shm_router(const RouterTopology& topo,
                    const RouteRule* rules,
                    std::size_t rule_count) {
    auto server = make_shm_router_server(topo);
    bind_shm_router_listen(server, topo);
    if (const uint8_t floor = priority_drop_floor_from_env()) {
        server.link().set_priority_drop_floor(floor);
        router_log(ROUTER_LOG_INFO,
                   std::string("priority drop floor = ")
                   + std::to_string(floor) + " (drop-lowest-first on backpressure)");
    }
    router_log(ROUTER_LOG_INFO, "SHM router on shared-memory rings");
    run_forward_loop(server, topo, rules, rule_count);
}

// Phase H — mixed-transport router (ADR 0014). Selected when a profile's peers
// span more than one transport kind. Binds one link per present transport and
// drives them from a single cooperative non-blocking poll loop.
void run_mixed_router(const RouterTopology& topo,
                      const RouteRule* rules,
                      std::size_t rule_count) {
    auto server = make_mixed_router_server(topo);
    if (topo.has_listen_uds) {
        ::unlink(topo.listen_uds.u.uds_path);
    }
    server.bind_router();
    if (const uint8_t floor = priority_drop_floor_from_env()) {
        server.set_priority_drop_floor(floor);  // SHM link only; no-op otherwise
        router_log(ROUTER_LOG_INFO,
                   std::string("priority drop floor = ")
                   + std::to_string(floor) + " (drop-lowest-first on backpressure)");
    }
    std::string kinds;
    if (server.has_shm()) kinds += "SHM ";
    if (server.has_uds()) kinds += "UDS ";
    if (server.has_udp()) kinds += "UDP ";
    router_log(ROUTER_LOG_INFO,
               std::string("mixed-transport router serving: ") + kinds);
    run_forward_loop(server, topo, rules, rule_count);
}

// --config <path>: load TOML topology, route table comes from the file.
// Transport kind is derived from router_listen's address kind, unless the
// profile mixes transports — then the mixed-transport router is used.
int run_with_config(const std::string& path) {
    LoadedTopology loaded = load_topology_from_toml_file(path);
    const RouterTopology topo = loaded.view();
    const RouteRule* rules = loaded.routes();
    const std::size_t rule_count = loaded.route_count();

    if (rule_count == 0) {
        router_log(ROUTER_LOG_WARN,
                   std::string("config '") + path
                   + "' has no [[routes]]; router will receive but not forward");
    }

    if (topology_is_mixed(topo)) {
        run_mixed_router(topo, rules, rule_count);
        return 0;
    }

    switch (topo.router_listen.kind) {
        case PeerAddressKind::UdsPath:
            ::unlink(topo.router_listen.u.uds_path);
            router_log(ROUTER_LOG_INFO,
                       std::string("UDS router on ")
                       + topo.router_listen.u.uds_path);
            run_datagram_router<Uds>(topo, rules, rule_count);
            return 0;
        case PeerAddressKind::UdpEndpoint:
            router_log(ROUTER_LOG_INFO,
                       std::string("UDP router on ")
                       + topo.router_listen.u.udp.host + ":"
                       + std::to_string(topo.router_listen.u.udp.port));
            run_datagram_router<Udp>(topo, rules, rule_count);
            return 0;
        case PeerAddressKind::ShmRing:
            run_shm_router(topo, rules, rule_count);
            return 0;
    }
    router_log(ROUTER_LOG_ERR, "unknown router_listen kind");
    return 1;
}

struct ServerRunner {
    int argc;
    char** argv;

    template<typename Transport>
    int operator()() const {
        const std::size_t demo_rule_count =
            sizeof(kDemoRouteRules) / sizeof(kDemoRouteRules[0]);

        if constexpr (std::is_same_v<Transport, ShmSpsc>) {
            const RouterTopology& topo = demo_topology(TransportKind::Shm);
            run_shm_router(topo, kDemoRouteRules, demo_rule_count);
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
                router_log(ROUTER_LOG_INFO,
                           std::string("UDS router on ") + path);
            } else {
                const uint16_t port = (argc >= 3)
                    ? static_cast<uint16_t>(std::stoi(argv[2]))
                    : kRouterUdpPort;
                router_log(ROUTER_LOG_INFO,
                           std::string("UDP router on port ")
                           + std::to_string(port));
                (void)port;
            }

            run_datagram_router<Transport>(topo, kDemoRouteRules, demo_rule_count);
            return 0;
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    install_router_stop_handlers();
    router_set_log_fn(demo_stderr_logger);
    apply_rt_hardening();  // C7 — opt-in mlock / CPU pin (env-gated, no-op by default)

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    try {
        if (std::strcmp(argv[1], "--config") == 0) {
            if (argc < 3) {
                usage(argv[0]);
                return 1;
            }
            return run_with_config(argv[2]);
        }

        const int rc = dispatch_transport_kind(
            argv[1], ServerRunner{argc, argv});
        if (rc != 0) {
            usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        router_log(ROUTER_LOG_ERR, std::string("error: ") + e.what());
        return 1;
    }

    return 0;
}
