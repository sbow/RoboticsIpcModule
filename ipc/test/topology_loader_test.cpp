// Unit tests for ipc/src/router/topology_loader.hpp.
//
// Plain-C++ assertions; no test framework dep so the binary stays drop-in
// header-only. Run via `make test-ipc-unit` (also builds last_value_cache_test).

#include "router/topology_loader.hpp"
#include "router/sideband.hpp"
#include "ipc/shm_spsc.hpp"
#include "router/frame.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int g_failed = 0;
int g_total  = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
                      << " EXPECT(" #cond ")\n";                            \
        }                                                                   \
    } while (0)

#define EXPECT_EQ(a, b)                                                     \
    do {                                                                    \
        ++g_total;                                                          \
        const auto _a = (a);                                                \
        const auto _b = (b);                                                \
        if (!(_a == _b)) {                                                  \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
                      << " EXPECT_EQ(" #a ", " #b ") -> "                   \
                      << _a << " != " << _b << "\n";                        \
        }                                                                   \
    } while (0)

#define EXPECT_STREQ(a, b)                                                  \
    do {                                                                    \
        ++g_total;                                                          \
        const char* _a = (a);                                               \
        const char* _b = (b);                                               \
        if (_a == nullptr || _b == nullptr || std::strcmp(_a, _b) != 0) {   \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__             \
                      << " EXPECT_STREQ(" #a ", " #b ") -> '"               \
                      << (_a ? _a : "<null>") << "' != '"                   \
                      << (_b ? _b : "<null>") << "'\n";                     \
        }                                                                   \
    } while (0)

void expect_load_error(std::string_view toml, const char* substr) {
    ++g_total;
    try {
        load_topology_from_toml_string(toml);
        std::cerr << "FAIL expected TopologyLoadError containing '" << substr
                  << "', loader returned success\n";
        ++g_failed;
    } catch (const TopologyLoadError& e) {
        if (std::string(e.what()).find(substr) == std::string::npos) {
            std::cerr << "FAIL TopologyLoadError text '" << e.what()
                      << "' did not contain '" << substr << "'\n";
            ++g_failed;
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL expected TopologyLoadError, got std::exception: "
                  << e.what() << "\n";
        ++g_failed;
    }
}

constexpr const char* kValidUds = R"(
[router]
listen = "uds:/tmp/rim_router.sock"

[[peers]]
id    = 1
name  = "sensor"
local = "uds:/tmp/rim_router_a.sock"

[[peers]]
id    = 2
name  = "controller"
local = "uds:/tmp/rim_router_b.sock"

[[peers]]
id    = 3
name  = "recorder"
local = "uds:/tmp/rim_router_c.sock"

[[routes]]
source = 1
dest   = [2, 3]

[[routes]]
source = 2
dest   = [3]
)";

constexpr const char* kValidShmWithSideband = R"(
[router]
listen = "shm:/router_a"

[[peers]]
id    = 1
name  = "sensor"
local = "shm:/router_sensor"

[[peers]]
id    = 2
name  = "controller"
local = "shm:/router_controller"

  [[peers.sideband]]
  class             = "vision_nv12"
  name              = "/robot_vision_nv12"
  max_payload_bytes = 8388608

  [[peers.sideband]]
  class             = "ml_tensor_in"
  name              = "/robot_ml_tensor_in"
  max_payload_bytes = 16777216

[[peers]]
id    = 3
name  = "recorder"
local = "shm:/router_recorder"

[[routes]]
source = 1
dest   = [2, 3]
)";

constexpr const char* kValidUdp = R"(
[router]
listen = "udp:127.0.0.1:19100"

[[peers]]
id    = 1
name  = "sensor"
local = "udp:127.0.0.1:19101"

[[peers]]
id    = 2
name  = "controller"
local = "udp:127.0.0.1:19102"

[[routes]]
source = 1
dest   = [2]
)";

void test_valid_uds() {
    LoadedTopology topo = load_topology_from_toml_string(kValidUds);
    const RouterTopology view = topo.view();

    EXPECT_EQ(view.peer_count, static_cast<std::size_t>(3));
    EXPECT(view.router_listen.kind == PeerAddressKind::UdsPath);
    EXPECT_STREQ(view.router_listen.u.uds_path,
                 "/tmp/rim_router.sock");

    const PeerEntry* sensor = peer_by_id(view, 1);
    EXPECT(sensor != nullptr);
    if (sensor) {
        EXPECT_STREQ(sensor->name, "sensor");
        EXPECT(sensor->local.kind == PeerAddressKind::UdsPath);
        EXPECT_STREQ(sensor->local.u.uds_path,
                     "/tmp/rim_router_a.sock");
    }

    EXPECT_EQ(topo.route_count(), static_cast<std::size_t>(2));
    const RouteRule* rules = topo.routes();
    EXPECT_EQ(static_cast<int>(rules[0].source), 1);
    EXPECT_EQ(static_cast<int>(rules[0].dest0),  2);
    EXPECT_EQ(static_cast<int>(rules[0].dest1),  3);
    EXPECT_EQ(static_cast<int>(rules[1].source), 2);
    EXPECT_EQ(static_cast<int>(rules[1].dest0),  3);
    EXPECT_EQ(static_cast<int>(rules[1].dest1),  0);
}

void test_valid_shm_with_sideband() {
    LoadedTopology topo = load_topology_from_toml_string(kValidShmWithSideband);
    const RouterTopology view = topo.view();

    EXPECT(view.router_listen.kind == PeerAddressKind::ShmRing);
    EXPECT_STREQ(view.router_listen.u.shm_name, "/router_a");

    std::size_t controller_sb_count = 0;
    const SidebandRegion* controller_sb =
        topo.sidebands_for(2, controller_sb_count);
    EXPECT_EQ(controller_sb_count, static_cast<std::size_t>(2));
    if (controller_sb_count == 2) {
        EXPECT_STREQ(controller_sb[0].name, "/robot_vision_nv12");
        EXPECT_EQ(controller_sb[0].max_payload_bytes,
                  static_cast<std::size_t>(8388608));
        EXPECT_EQ(controller_sb[0].version, kSidebandVersion);
        EXPECT_STREQ(controller_sb[1].name, "/robot_ml_tensor_in");
        EXPECT_EQ(controller_sb[1].max_payload_bytes,
                  static_cast<std::size_t>(16777216));
    }

    // Peers without sideband entries report count 0.
    std::size_t sensor_sb_count = 99;
    const SidebandRegion* sensor_sb = topo.sidebands_for(1, sensor_sb_count);
    EXPECT_EQ(sensor_sb_count, static_cast<std::size_t>(0));
    EXPECT(sensor_sb == nullptr);
}

void test_valid_udp() {
    LoadedTopology topo = load_topology_from_toml_string(kValidUdp);
    const RouterTopology view = topo.view();
    EXPECT(view.router_listen.kind == PeerAddressKind::UdpEndpoint);
    EXPECT_STREQ(view.router_listen.u.udp.host, "127.0.0.1");
    EXPECT_EQ(static_cast<int>(view.router_listen.u.udp.port), 19100);

    const PeerEntry* sensor = peer_by_id(view, 1);
    EXPECT(sensor != nullptr);
    if (sensor) {
        EXPECT(sensor->local.kind == PeerAddressKind::UdpEndpoint);
        EXPECT_EQ(static_cast<int>(sensor->local.u.udp.port), 19101);
    }
}

void test_load_from_file() {
    // x86_dev.toml is the Phase F F1+F2 7-peer template: peers 1/2/3 (demo)
    // plus 4 vision_capture, 5 ml_inference, 7 python_tooling (Phase F F2),
    // 8 dashboard_feed. Routes: 1→[2,3], 2→[3,7] (F2 tap), 4→[5,3], 5→[2,3],
    // 7→[3]. Dashboard (8) has no inbound route within the 2-dest-per-source
    // cap (parked C5). Peer 6 (mavlink_gateway) is reserved for F4.
    LoadedTopology topo = load_topology_from_toml_file(
        "config/profiles/x86_dev.toml");
    const RouterTopology view = topo.view();
    EXPECT_EQ(view.peer_count, static_cast<std::size_t>(7));
    EXPECT(view.router_listen.kind == PeerAddressKind::UdsPath);
    EXPECT_EQ(topo.route_count(), static_cast<std::size_t>(5));

    // 8-peer catalog stable IDs: peer 6 (mavlink_gateway) is reserved for F4.
    EXPECT(peer_by_id(view, 1) != nullptr);                  // sensor
    EXPECT(peer_by_id(view, 2) != nullptr);                  // controller
    EXPECT(peer_by_id(view, 3) != nullptr);                  // recorder
    EXPECT(peer_by_id(view, 4) != nullptr);                  // vision_capture
    EXPECT(peer_by_id(view, 5) != nullptr);                  // ml_inference
    EXPECT(peer_by_id(view, 6) == nullptr);                  // reserved (F4)
    EXPECT(peer_by_id(view, 7) != nullptr);                  // python_tooling (F2)
    EXPECT(peer_by_id(view, 8) != nullptr);                  // dashboard_feed
}

// ADR 0009 — per-peer SHM ring sizing.

constexpr const char* kShmRingSizingExplicit = R"(
[router]
listen = "shm:/router_a"

[[peers]]
id              = 1
name            = "sensor"
local           = "shm:/router_sensor"
shm_slot_count  = 256
shm_max_payload = 64

[[peers]]
id              = 2
name            = "controller"
local           = "shm:/router_controller"

[[peers]]
id              = 3
name            = "recorder"
local           = "shm:/router_recorder"
shm_slot_count  = 1024
shm_max_payload = 128
)";

void test_shm_ring_sizing_defaults_when_absent() {
    // Peer with no shm_slot_count / shm_max_payload keeps the sentinel zero
    // values; bind helpers interpret zero as "use ShmSpsc::BindParams default."
    LoadedTopology topo = load_topology_from_toml_string(kShmRingSizingExplicit);
    const RouterTopology view = topo.view();
    const PeerEntry* controller = peer_by_id(view, 2);
    EXPECT(controller != nullptr);
    if (controller) {
        EXPECT_EQ(controller->shm_slot_count, static_cast<uint32_t>(0));
        EXPECT_EQ(controller->shm_max_payload, static_cast<uint32_t>(0));
    }
}

void test_shm_ring_sizing_overrides_parsed() {
    LoadedTopology topo = load_topology_from_toml_string(kShmRingSizingExplicit);
    const RouterTopology view = topo.view();

    const PeerEntry* sensor = peer_by_id(view, 1);
    EXPECT(sensor != nullptr);
    if (sensor) {
        EXPECT_EQ(sensor->shm_slot_count, static_cast<uint32_t>(256));
        EXPECT_EQ(sensor->shm_max_payload, static_cast<uint32_t>(64));
    }

    const PeerEntry* recorder = peer_by_id(view, 3);
    EXPECT(recorder != nullptr);
    if (recorder) {
        EXPECT_EQ(recorder->shm_slot_count, static_cast<uint32_t>(1024));
        EXPECT_EQ(recorder->shm_max_payload, static_cast<uint32_t>(128));
    }
}

void test_shm_ring_sizing_cache_footprint_ratio() {
    // Quantitative claim from ADR 0009: a router-frame ring sized at
    // 256 slots × 64 B is ~15× smaller than the legacy 256 × 1024 default.
    const size_t legacy_bytes  = shm_region_size(256, 1024);
    const size_t sized_bytes   = shm_region_size(256,   64);
    EXPECT_EQ(legacy_bytes, static_cast<size_t>(64 + 2 * 256 * 1028));
    EXPECT_EQ(sized_bytes,  static_cast<size_t>(64 + 2 * 256 *   68));
    // Ratio comfortably > 14 (exact value ~15.1).
    EXPECT(legacy_bytes > sized_bytes * 14);
}

void test_shm_ring_sizing_validation_errors() {
    // shm_max_payload below kRouterFrameSize is rejected with a frame-aware
    // error message.
    expect_load_error(R"(
[router]
listen = "shm:/router_a"
[[peers]]
id              = 1
name            = "a"
local           = "shm:/router_a_peer"
shm_max_payload = 32
)", "< kRouterFrameSize");

    // shm_slot_count outside 1..2^20 is rejected.
    expect_load_error(R"(
[router]
listen = "shm:/router_a"
[[peers]]
id              = 1
name            = "a"
local           = "shm:/router_a_peer"
shm_slot_count  = 2000000
)", "shm_slot_count");

    // shm_max_payload way over the cap (set to 1 GiB) is rejected.
    expect_load_error(R"(
[router]
listen = "shm:/router_a"
[[peers]]
id              = 1
name            = "a"
local           = "shm:/router_a_peer"
shm_max_payload = 1073741824
)", "shm_max_payload");

    // shm_* fields on a non-SHM peer are a clear configuration error.
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id              = 1
name            = "a"
local           = "uds:/tmp/a.sock"
shm_max_payload = 64
)", "local address is not shm:");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id              = 1
name            = "a"
local           = "udp:127.0.0.1:5000"
shm_slot_count  = 256
)", "local address is not shm:");
}

void test_shm_ring_sizing_jetson_profile_demonstrates_recommended_values() {
    // The shipped jetson_prod.toml must demonstrate the ADR 0009 sizing on
    // every peer; this guards against future profile edits that accidentally
    // regress the cache-friendly defaults. Phase F F1 expanded the profile
    // to 6 peers; F2 added peer 7 python_tooling. All 7 stay on SHM until
    // the parked C11 mixed-transport gap is closed (the Python ctypes bridge
    // in examples/bridges/python_peer/ is UDS-only today, so on Jetson it
    // would need a separate UDS bridge daemon).
    LoadedTopology topo = load_topology_from_toml_file(
        "config/profiles/jetson_prod.toml");
    const RouterTopology view = topo.view();
    EXPECT_EQ(view.peer_count, static_cast<std::size_t>(7));
    for (size_t i = 0; i < view.peer_count; ++i) {
        const PeerEntry& p = view.peers[i];
        EXPECT(p.local.kind == PeerAddressKind::ShmRing);
        EXPECT_EQ(p.shm_slot_count,  static_cast<uint32_t>(256));
        EXPECT_EQ(p.shm_max_payload, static_cast<uint32_t>(kRouterFrameSize));
    }
}

void test_move_preserves_interned_pointers() {
    // After moving a LoadedTopology, the const char* pointers it handed out
    // through view() must still resolve. (Documented Linux libstdc++/libc++
    // contract; this test guards against regressions on the platforms we
    // actually support.)
    LoadedTopology topo = load_topology_from_toml_string(kValidUds);
    const char* sensor_name_before;
    {
        const PeerEntry* sensor = peer_by_id(topo.view(), 1);
        sensor_name_before = sensor ? sensor->name : nullptr;
    }
    LoadedTopology moved = std::move(topo);
    const PeerEntry* sensor_after = peer_by_id(moved.view(), 1);
    EXPECT(sensor_after != nullptr);
    if (sensor_after) {
        EXPECT_STREQ(sensor_after->name, "sensor");
        EXPECT(sensor_after->name == sensor_name_before);
    }
}

void test_errors() {
    expect_load_error("", "missing [router] section");

    expect_load_error(R"(
[router]
listen = "udp:127.0.0.1:99999"
[[peers]]
id = 1
name = "a"
local = "udp:127.0.0.1:1"
)", "udp port out of range");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
)", "missing or empty [[peers]]");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[peers]]
id = 1
name = "b"
local = "uds:/tmp/b.sock"
)", "duplicate peer id");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 0
name = "a"
local = "uds:/tmp/a.sock"
)", "out of range 1..254");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "smtp:/tmp/a.sock"
)", "unknown address scheme");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "shm:no_leading_slash"
)", "shm address must start with");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[routes]]
source = 1
dest = [99]
)", "does not match any peer");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[routes]]
source = 1
dest = []
)", "must be an array of 1 or 2");

    // Self-routing rejection (Phase D1 / routing test).
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[routes]]
source = 1
dest = [1]
)", "self-routing rejected");

    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[peers]]
id = 2
name = "b"
local = "uds:/tmp/b.sock"
[[routes]]
source = 1
dest = [2, 1]
)", "self-routing rejected");
}

}  // namespace

int main() {
    test_valid_uds();
    test_valid_shm_with_sideband();
    test_valid_udp();
    test_load_from_file();
    test_move_preserves_interned_pointers();
    test_errors();

    // ADR 0009 — per-peer SHM ring sizing.
    test_shm_ring_sizing_defaults_when_absent();
    test_shm_ring_sizing_overrides_parsed();
    test_shm_ring_sizing_cache_footprint_ratio();
    test_shm_ring_sizing_validation_errors();
    test_shm_ring_sizing_jetson_profile_demonstrates_recommended_values();

    std::cout << "topology_loader_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
