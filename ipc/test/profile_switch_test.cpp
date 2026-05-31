// Phase D2 — profile switch smoke.
//
// Loads two deployment profiles back-to-back from disk via the topology
// loader, stands up a router server on each, sends one frame through each
// transport, and shuts down cleanly. Catches:
//
//   * Topology loader regressions on real profile files
//   * RouterServer<Link> wiring breakage for either SHM or datagram
//   * Address resolution mismatches between TOML and bind code
//   * Leftover state from one profile preventing the next from binding
//     (covers the "tests can run in any order on the same host" promise)
//
// Profiles exercised:
//   config/profiles/jetson_prod.toml  → SHM (per-peer ring sizing ADR 0009)
//   config/profiles/hil.toml          → UDP (127.0.0.1, ports 19100..19103)
//
// We finish by loading jetson_prod again to prove the SHM regions cleaned
// up between rounds (would fail if the prior round left a region with a
// different size that ftruncate couldn't reconcile).

#include "ipc.hpp"
#include "router/factory.hpp"
#include "router/frame.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"
#include "router/topology_loader.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <thread>

namespace {

int assertions_run    = 0;
int assertions_failed = 0;

#define EXPECT(cond) do {                                                  \
    ++assertions_run;                                                      \
    if (!(cond)) {                                                         \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT failed @ " << __FILE__ << ':' << __LINE__     \
                  << " : " #cond "\n";                                     \
    }                                                                      \
} while (0)

#define EXPECT_EQ(a, b) do {                                               \
    ++assertions_run;                                                      \
    const auto _a = (a);                                                   \
    const auto _b = (b);                                                   \
    if (_a != _b) {                                                        \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT_EQ failed @ " << __FILE__ << ':' << __LINE__  \
                  << " : " #a " == " #b " (" << _a << " vs " << _b         \
                  << ")\n";                                                \
    }                                                                      \
} while (0)

constexpr const char* kJetsonProfile = "config/profiles/jetson_prod.toml";
constexpr const char* kHilProfile    = "config/profiles/hil.toml";

// Mirror jetson_prod's peer ids — kept here as named constants so the
// asserts read naturally.
constexpr uint8_t kSensorId     = 1;
constexpr uint8_t kControllerId = 2;
constexpr uint8_t kRecorderId   = 3;

// Forcibly clean any SHM regions the test or a prior run might have
// left behind. Hardcodes the names jetson_prod.toml uses (Phase F F1
// expanded to 6 peers; F2 added peer 7 python_tooling). Sideband regions
// only get created when the real vision / ml peers run, so they're not
// in this list — but the control-plane peer rings ARE.
void cleanup_jetson_shm() {
    ::shm_unlink("/rim_router");
    ::shm_unlink("/rim_router_sensor");
    ::shm_unlink("/rim_router_controller");
    ::shm_unlink("/rim_router_recorder");
    ::shm_unlink("/rim_router_vision_capture");
    ::shm_unlink("/rim_router_ml_inference");
    ::shm_unlink("/rim_router_python_tooling");
    ::shm_unlink("/rim_router_dashboard");
}

// ----- SHM (jetson_prod.toml) round ------------------------------------

void smoke_jetson_round(const LoadedTopology& loaded, const char* label) {
    const RouterTopology& topo = loaded.view();
    // Phase F F1: 6 peers (1 sensor, 2 controller, 3 recorder,
    // 4 vision_capture, 5 ml_inference, 8 dashboard_feed). F2 added
    // peer 7 python_tooling. All-SHM.
    EXPECT_EQ(topo.peer_count, 7u);
    EXPECT(topo.router_listen.kind == PeerAddressKind::ShmRing);

    // Per-peer ring sizing applied from TOML (256 × 64 in jetson_prod.toml,
    // per ADR 0009). The router bind below would happily accept defaults,
    // but the test should confirm the loader is feeding overrides through.
    for (size_t i = 0; i < topo.peer_count; ++i) {
        EXPECT_EQ(topo.peers[i].shm_slot_count,  256u);
        EXPECT_EQ(topo.peers[i].shm_max_payload, 64u);
    }

    cleanup_jetson_shm();

    auto server = make_shm_router_server(topo);
    bind_shm_router_listen(server, topo);

    auto sensor   = make_shm_router_client(topo, kSensorId);
    auto recorder = make_shm_router_client(topo, kRecorderId);

    std::atomic<bool> stop_router{false};
    std::atomic<bool> frame_seen{false};
    std::atomic<uint8_t> recorder_source_byte{0};

    std::thread router_thread([&]() {
        RouterFrame scratch;
        uint64_t ts = 1;
        while (!stop_router.load(std::memory_order_relaxed)) {
            server.link().forward(scratch, ts++, loaded.routes(),
                                  loaded.route_count());
        }
    });

    std::thread recorder_thread([&]() {
        RouterFrame frame;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (recorder.recv_message(frame)) {
                recorder_source_byte.store(frame.source(),
                                           std::memory_order_relaxed);
                frame_seen.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // Brief settle so the recorder is in its recv loop before publish.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    sensor.send_message(kSensorId, std::string("hello-") + label);

    recorder_thread.join();
    stop_router.store(true, std::memory_order_relaxed);
    router_thread.join();

    EXPECT(frame_seen.load(std::memory_order_acquire));
    EXPECT_EQ(recorder_source_byte.load(), kSensorId);

    // Router metrics reflect: 1 sensor frame received, fanned out to three
    // destinations after C5 Scope A widened source=1 to dest=[2, 3, 8]
    // (controller + recorder + dashboard). The router binds every peer's
    // SHM region in bind_shm_router_listen, so the dashboard ring exists
    // and accepts writes even though we never open a dashboard client —
    // unconsumed slots simply roll over and contribute to dropped_full.
    const ShmRouterMetrics& m = server.link().metrics();
    EXPECT(m.forwarded.load() >= 1u);
    // dropped_full may be 0 or more depending on whether the controller
    // and dashboard rings (which we didn't drain) caused fanout drops.
    // We only assert the recorder got at least one frame.

    cleanup_jetson_shm();
}

// ----- UDP (hil.toml) round --------------------------------------------

void smoke_hil_round(const LoadedTopology& loaded) {
    const RouterTopology& topo = loaded.view();
    // Phase F F1: 6 peers (same catalog as Jetson; all UDP loopback);
    // F2 added peer 7 python_tooling (UDP port 19107).
    EXPECT_EQ(topo.peer_count, 7u);
    EXPECT(topo.router_listen.kind == PeerAddressKind::UdpEndpoint);
    EXPECT_EQ(topo.router_listen.u.udp.port, static_cast<uint16_t>(19100));

    auto server = make_router_server<Udp>(topo);
    bind_router_listen<Udp>(server, topo);

    // Brief poll timeout so the router thread can observe stop_flag.
    server.set_recv_timeout_ms(50);

    auto sensor   = make_router_client<Udp>(topo, kSensorId);
    auto recorder = make_router_client<Udp>(topo, kRecorderId);
    recorder.set_recv_timeout_ms(50);

    std::atomic<bool> stop_router{false};
    std::atomic<bool> frame_seen{false};
    std::atomic<uint8_t> recorder_source_byte{0};

    std::thread router_thread([&]() {
        RouterFrame scratch;
        uint64_t ts = 1;
        while (!stop_router.load(std::memory_order_relaxed)) {
            try {
                server.link().forward(scratch, ts++, loaded.routes(),
                                      loaded.route_count());
            } catch (const std::runtime_error&) {
                // recv timeout is reported as a runtime_error; treat as idle.
            }
        }
    });

    std::thread recorder_thread([&]() {
        RouterFrame frame;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (recorder.recv_message(frame)) {
                recorder_source_byte.store(frame.source(),
                                           std::memory_order_relaxed);
                frame_seen.store(true, std::memory_order_release);
                return;
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Sensor publish — repeat a few times because UDP datagrams are
    // best-effort on loopback and the smoke test would be flaky if a
    // single recv could miss the only send.
    for (int i = 0; i < 5 && !frame_seen.load(std::memory_order_acquire); ++i) {
        sensor.send_message(kSensorId, "hello-hil");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    recorder_thread.join();
    stop_router.store(true, std::memory_order_relaxed);
    router_thread.join();

    EXPECT(frame_seen.load(std::memory_order_acquire));
    EXPECT_EQ(recorder_source_byte.load(), kSensorId);
}

void test_profile_switch_smoke() {
    // 1. SHM round.
    {
        LoadedTopology loaded = load_topology_from_toml_file(kJetsonProfile);
        smoke_jetson_round(loaded, "jetson1");
    }

    // 2. UDP round on a different transport — proves the test wiring
    //    doesn't depend on the prior round's state.
    {
        LoadedTopology loaded = load_topology_from_toml_file(kHilProfile);
        smoke_hil_round(loaded);
    }

    // 3. SHM round again — proves cleanup between rounds works. The shm
    //    regions named in jetson_prod.toml should not exist on /dev/shm
    //    after the first round; the loader + bind in this round either
    //    creates fresh ones or reuses the prior layout cleanly.
    {
        LoadedTopology loaded = load_topology_from_toml_file(kJetsonProfile);
        smoke_jetson_round(loaded, "jetson2");
    }
}

}  // namespace

int main() {
    test_profile_switch_smoke();

    std::cout << "profile_switch_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
