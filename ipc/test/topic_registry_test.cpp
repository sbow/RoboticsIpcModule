// Phase F C5 Scope B — declarative topic registry.
//
// Unit tests for router/topic_table.hpp (TopicEntry + topic_by_id /
// topic_by_name) and the [[topics]] section parsed by
// router/topology_loader.hpp. The router itself never consults the
// registry, so this test stays in-process: build a LoadedTopology from
// inline TOML, query the resulting view().

#include "router/topic_table.hpp"
#include "router/topology_loader.hpp"
#include "router/frame.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

int g_total  = 0;
int g_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
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
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
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
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT_STREQ(" #a ", " #b ") -> '"               \
                      << (_a ? _a : "<null>") << "' != '"                   \
                      << (_b ? _b : "<null>") << "'\n";                     \
        }                                                                   \
    } while (0)

void expect_load_error(std::string_view toml, const char* substr) {
    ++g_total;
    try {
        load_topology_from_toml_string(toml);
        std::cerr << "FAIL expected TopologyLoadError containing '"
                  << substr << "', loader returned success\n";
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

constexpr const char* kPeersOnly = R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
)";

constexpr const char* kThreeTopics = R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "sensor"
local = "uds:/tmp/sensor.sock"
[[peers]]
id = 4
name = "vision"
local = "uds:/tmp/vision.sock"

[[topics]]
id            = 100
name          = "imu_proprio"

[[topics]]
id            = 200
name          = "controller_command"
payload_class = "controller_command"

[[topics]]
id            = 300
name          = "vision_frame"
payload_class = "vision_nv12"
sideband_idx  = 0
)";

void test_absent_topics_section_yields_empty_registry() {
    // Most profiles will not (yet) define a [[topics]] section. The view
    // must still be valid and the count must be zero — not unspecified.
    LoadedTopology topo = load_topology_from_toml_string(kPeersOnly);
    const RouterTopology view = topo.view();
    EXPECT_EQ(view.topic_count, static_cast<std::size_t>(0));
    EXPECT_EQ(topo.topic_count(), static_cast<std::size_t>(0));

    // Lookups against an empty catalog return nullptr without crashing.
    EXPECT(topic_by_id(view, 100) == nullptr);
    EXPECT(topic_by_name(view, "anything") == nullptr);
    EXPECT(topic_by_name(view, nullptr) == nullptr);
}

void test_three_topics_round_trip() {
    LoadedTopology topo = load_topology_from_toml_string(kThreeTopics);
    const RouterTopology view = topo.view();
    EXPECT_EQ(view.topic_count, static_cast<std::size_t>(3));

    const TopicEntry* imu = topic_by_id(view, 100);
    EXPECT(imu != nullptr);
    if (imu) {
        EXPECT_EQ(static_cast<int>(imu->id), 100);
        EXPECT_STREQ(imu->name, "imu_proprio");
        // Omitted payload_class round-trips as nullptr (not "" or undefined).
        EXPECT(imu->payload_class == nullptr);
        // Omitted sideband_idx defaults to kSidebandIdxNone, matching the
        // RouterFrame default — so frame.sideband_idx() == imu->sideband_idx
        // is a meaningful "no-sideband" assertion on the consumer side.
        EXPECT_EQ(imu->sideband_idx, kSidebandIdxNone);
    }

    const TopicEntry* ctrl = topic_by_id(view, 200);
    EXPECT(ctrl != nullptr);
    if (ctrl) {
        EXPECT_STREQ(ctrl->name, "controller_command");
        EXPECT_STREQ(ctrl->payload_class, "controller_command");
        EXPECT_EQ(ctrl->sideband_idx, kSidebandIdxNone);
    }

    const TopicEntry* vis = topic_by_id(view, 300);
    EXPECT(vis != nullptr);
    if (vis) {
        EXPECT_STREQ(vis->name, "vision_frame");
        EXPECT_STREQ(vis->payload_class, "vision_nv12");
        EXPECT_EQ(static_cast<int>(vis->sideband_idx), 0);
    }
}

void test_lookup_by_name() {
    LoadedTopology topo = load_topology_from_toml_string(kThreeTopics);
    const RouterTopology view = topo.view();

    const TopicEntry* imu = topic_by_name(view, "imu_proprio");
    EXPECT(imu != nullptr);
    if (imu) {
        EXPECT_EQ(static_cast<int>(imu->id), 100);
    }

    const TopicEntry* vis = topic_by_name(view, "vision_frame");
    EXPECT(vis != nullptr);
    if (vis) {
        EXPECT_EQ(static_cast<int>(vis->id), 300);
    }

    // Misses
    EXPECT(topic_by_name(view, "nope") == nullptr);
    EXPECT(topic_by_name(view, "")     == nullptr);
    EXPECT(topic_by_name(view, nullptr) == nullptr);
}

void test_id_zero_is_a_legal_topic_id() {
    // The TOML schema says id is a u16; 0 is a legal wire value (the
    // RouterFrame default). Loader must accept it.
    const char* toml = R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id   = 0
name = "default"
)";
    LoadedTopology topo = load_topology_from_toml_string(toml);
    const RouterTopology view = topo.view();
    EXPECT_EQ(view.topic_count, static_cast<std::size_t>(1));
    const TopicEntry* zero = topic_by_id(view, 0);
    EXPECT(zero != nullptr);
    if (zero) {
        EXPECT_STREQ(zero->name, "default");
    }
}

void test_move_preserves_interned_topic_pointers() {
    // Same contract as PeerEntry::name — moving a LoadedTopology must
    // keep TopicEntry::name and TopicEntry::payload_class pointers
    // resolvable, because we rely on deque/vector move semantics that
    // transfer (not reallocate) element storage.
    LoadedTopology topo = load_topology_from_toml_string(kThreeTopics);
    const char* vis_name_before = nullptr;
    const char* vis_class_before = nullptr;
    {
        const TopicEntry* vis = topic_by_id(topo.view(), 300);
        vis_name_before  = vis ? vis->name : nullptr;
        vis_class_before = vis ? vis->payload_class : nullptr;
    }

    LoadedTopology moved = std::move(topo);
    const TopicEntry* vis_after = topic_by_id(moved.view(), 300);
    EXPECT(vis_after != nullptr);
    if (vis_after) {
        EXPECT_STREQ(vis_after->name, "vision_frame");
        EXPECT_STREQ(vis_after->payload_class, "vision_nv12");
        EXPECT(vis_after->name          == vis_name_before);
        EXPECT(vis_after->payload_class == vis_class_before);
    }
}

void test_loader_rejects_missing_or_invalid_topics() {
    // Missing id
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
name = "x"
)", "missing integer 'id'");

    // Missing name
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id = 100
)", "missing string 'name'");

    // Empty name
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id   = 100
name = ""
)", "empty name");

    // id out of u16 range
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id   = 65536
name = "too_big"
)", "out of range 0..65535");

    // Duplicate id
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id   = 100
name = "first"
[[topics]]
id   = 100
name = "second"
)", "duplicate topic id");

    // Duplicate name
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id   = 100
name = "imu_proprio"
[[topics]]
id   = 200
name = "imu_proprio"
)", "duplicate topic name");

    // sideband_idx out of u16 range
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id            = 100
name          = "x"
sideband_idx  = -1
)", "sideband_idx -1 out of range");

    // Empty payload_class
    expect_load_error(R"(
[router]
listen = "uds:/tmp/r.sock"
[[peers]]
id = 1
name = "a"
local = "uds:/tmp/a.sock"
[[topics]]
id            = 100
name          = "x"
payload_class = ""
)", "empty payload_class");
}

}  // namespace

int main() {
    test_absent_topics_section_yields_empty_registry();
    test_three_topics_round_trip();
    test_lookup_by_name();
    test_id_zero_is_a_legal_topic_id();
    test_move_preserves_interned_topic_pointers();
    test_loader_rejects_missing_or_invalid_topics();

    std::cout << "topic_registry_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
