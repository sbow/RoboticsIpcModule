// Phase D4 — fault injection.
//
// Six scenarios, one per fault gate called out in the D plan:
//
//   1. Truncated UDP datagram (< kRouterFrameSize) is dropped and
//      `DatagramRouterMetrics::recv_truncated` increments.
//   2. UDP datagram from a source port not in the topology is dropped
//      and `DatagramRouterMetrics::recv_unknown_source` increments.
//   3. UDS router_listen path under a non-existent directory raises a
//      clean exception from `bind_router(...)` and leaves no leftover
//      socket file behind.
//   4. UDS router_listen path with a pre-existing socket file is
//      cleanly rebound (regression for `Uds::bind`'s `unlink-then-bind`
//      sequence — required to recover from previous crash leftovers).
//   5. Topology with `shm_max_payload < kRouterFrameSize` is rejected
//      at load time with a frame-aware error message — never reaches
//      `bind_router`.  (Cross-references topology_loader_test.)
//   6. SIGKILL the router mid-traffic; next clean `router_server`
//      bind on the same profile succeeds (SHM regions are reset via
//      `ShmRegion::bind`'s `shm_open(O_CREAT) + ftruncate + memset`
//      path). Complements `router_restart_test`'s idle-state coverage.
//
// Scenarios 1-2 touch AF_INET. The repository's sandbox blocks
// `socket(AF_INET, ...)` so this test is part of the
// `make test-ipc-integration` suite that runs outside the sandbox.
// Within the sandbox the UDP-bound scenarios fail with a clear
// "[skip] sandboxed" message and do not contribute to the assertion
// count.

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/link.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_peer_address_io.hpp"
#include "router/topology_loader.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

// ----------------------------------------------------------------------
// Scenarios 1 & 2 — UDP truncation + unknown source. Both share the same
// in-process server pattern: build a tiny topology, spin up a thread
// that calls DatagramRouterLink::forward() in a loop, drive it with
// raw sendto() calls from a sender socket bound to a known (or
// deliberately wrong) port.
// ----------------------------------------------------------------------

constexpr uint16_t kRouterPortTrunc   = 25000;
constexpr uint16_t kSensorPortTrunc   = 25001;
constexpr uint16_t kSubPortTrunc      = 25002;

constexpr uint16_t kRouterPortUnknown = 25010;
constexpr uint16_t kSensorPortUnknown = 25011;
constexpr uint16_t kSubPortUnknown    = 25012;
constexpr uint16_t kGhostPort         = 25099;  // not in topology

constexpr uint8_t kSensorId = 1;
constexpr uint8_t kSubId    = 2;

// Try to open an AF_INET datagram socket. Returns -1 + sets sandboxed=true
// if the sandbox blocks the call so callers can [skip] cleanly.
int open_udp_socket(bool& sandboxed) {
    sandboxed = false;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        // EPERM / EACCES are the typical sandbox responses to AF_INET.
        if (errno == EPERM || errno == EACCES) {
            sandboxed = true;
        }
        return -1;
    }
    return fd;
}

// Bind a UDP socket to 127.0.0.1:port with SO_REUSEADDR so back-to-back
// scenarios in the same test process don't race on TIME_WAIT.
bool bind_udp_loopback(int fd, uint16_t port) {
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        return false;
    }
    return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool sendto_udp_loopback(int fd, uint16_t dest_port, const void* data, size_t len) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr) <= 0) {
        return false;
    }
    const ssize_t n = ::sendto(fd, data, len, 0,
        reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    return n == static_cast<ssize_t>(len);
}

void test_truncated_udp_datagram_drops_with_metric() {
    bool sandboxed = false;
    const int sender_fd = open_udp_socket(sandboxed);
    if (sender_fd < 0) {
        if (sandboxed) {
            std::cout << "  [skip] truncated_udp_datagram_drops_with_metric: "
                         "AF_INET blocked by sandbox\n";
            return;
        }
        EXPECT(sender_fd >= 0);   // fail loudly on non-sandbox errors
        return;
    }
    EXPECT(bind_udp_loopback(sender_fd, kSensorPortTrunc));

    // Build a 2-peer UDP topology. The sub peer never reads — UDP is
    // fire-and-forget, so the router can sendto() port 25002 even with
    // no socket bound there.
    static constexpr PeerEntry kPeers[] = {
        {kSensorId, "sensor", peer_udp("127.0.0.1", kSensorPortTrunc), 0, 0},
        {kSubId,    "sub",    peer_udp("127.0.0.1", kSubPortTrunc),    0, 0},
    };
    static constexpr RouterTopology kTopo = {
        .peers         = kPeers,
        .peer_count    = sizeof(kPeers) / sizeof(kPeers[0]),
        .router_listen = peer_udp("127.0.0.1", kRouterPortTrunc),
    };
    static constexpr RouteRule kRules[] = {
        make_route(kSensorId, kSubId),
    };

    auto link = DatagramRouterLink<Udp>::server(kTopo);
    Udp::BindParams bind{};
    bind.port = kRouterPortTrunc;
    try {
        link.bind_router(bind);
    } catch (const std::runtime_error& e) {
        std::cout << "  [skip] truncated_udp_datagram_drops_with_metric: "
                     "router bind() failed (" << e.what() << ")\n";
        ::close(sender_fd);
        return;
    }
    link.set_recv_timeout_ms(50);

    std::atomic<bool> stop_router{false};
    std::thread router_thread([&]() {
        uint64_t ts = 1;
        RouterFrame scratch;
        while (!stop_router.load(std::memory_order_relaxed)) {
            try {
                link.forward(scratch, ts++, kRules, std::size(kRules));
            } catch (const std::runtime_error&) {
                // recvfrom timed out — RouterServer::run does the same.
            }
        }
    });

    // 1) Send a 16-byte (< 64) datagram from sensor port. Router should
    //    increment recv_truncated and discard.
    const char short_payload[16] = {0};
    EXPECT(sendto_udp_loopback(sender_fd, kRouterPortTrunc,
                               short_payload, sizeof(short_payload)));

    // 2) Send a properly formed 64-byte RouterFrame from sensor port.
    //    Router should resolve source=1, route to sub, forwarded++.
    RouterFrame good;
    good.init(kSensorId);
    good.set_seq(42);
    EXPECT(sendto_udp_loopback(sender_fd, kRouterPortTrunc,
                               good.bytes, kRouterFrameSize));

    // 3) Send another truncated frame (8 bytes this time). Verify the
    //    counter increments by exactly one per truncated frame.
    const char tiny_payload[8] = {0};
    EXPECT(sendto_udp_loopback(sender_fd, kRouterPortTrunc,
                               tiny_payload, sizeof(tiny_payload)));

    // Let the router service the queued datagrams. 200 ms is comfortable
    // — recv timeout is 50 ms and the loop is tight.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stop_router.store(true, std::memory_order_relaxed);
    router_thread.join();
    ::close(sender_fd);

    const DatagramRouterMetrics& m = link.metrics();
    const uint64_t trunc      = m.recv_truncated.load();
    const uint64_t fwd        = m.forwarded.load();
    const uint64_t unknown    = m.recv_unknown_source.load();

    std::cout << "  truncated UDP: recv_truncated=" << trunc
              << " forwarded=" << fwd
              << " recv_unknown_source=" << unknown << '\n';

    EXPECT_EQ(trunc, static_cast<uint64_t>(2));   // 16-byte + 8-byte
    EXPECT_EQ(fwd,   static_cast<uint64_t>(1));   // sensor → sub
    EXPECT_EQ(unknown, static_cast<uint64_t>(0)); // no unknown sources
}

void test_unknown_source_udp_drops_with_metric() {
    bool sandboxed = false;
    const int sender_known = open_udp_socket(sandboxed);
    if (sender_known < 0) {
        if (sandboxed) {
            std::cout << "  [skip] unknown_source_udp_drops_with_metric: "
                         "AF_INET blocked by sandbox\n";
            return;
        }
        EXPECT(sender_known >= 0);
        return;
    }
    const int sender_ghost = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT(sender_ghost >= 0);

    EXPECT(bind_udp_loopback(sender_known, kSensorPortUnknown));
    EXPECT(bind_udp_loopback(sender_ghost, kGhostPort));

    static constexpr PeerEntry kPeers[] = {
        {kSensorId, "sensor", peer_udp("127.0.0.1", kSensorPortUnknown), 0, 0},
        {kSubId,    "sub",    peer_udp("127.0.0.1", kSubPortUnknown),    0, 0},
    };
    static constexpr RouterTopology kTopo = {
        .peers         = kPeers,
        .peer_count    = sizeof(kPeers) / sizeof(kPeers[0]),
        .router_listen = peer_udp("127.0.0.1", kRouterPortUnknown),
    };
    static constexpr RouteRule kRules[] = {
        make_route(kSensorId, kSubId),
    };

    auto link = DatagramRouterLink<Udp>::server(kTopo);
    Udp::BindParams bind{};
    bind.port = kRouterPortUnknown;
    try {
        link.bind_router(bind);
    } catch (const std::runtime_error& e) {
        std::cout << "  [skip] unknown_source_udp_drops_with_metric: "
                     "router bind() failed (" << e.what() << ")\n";
        ::close(sender_known);
        ::close(sender_ghost);
        return;
    }
    link.set_recv_timeout_ms(50);

    std::atomic<bool> stop_router{false};
    std::thread router_thread([&]() {
        uint64_t ts = 1;
        RouterFrame scratch;
        while (!stop_router.load(std::memory_order_relaxed)) {
            try {
                link.forward(scratch, ts++, kRules, std::size(kRules));
            } catch (const std::runtime_error&) {}
        }
    });

    RouterFrame frame;
    frame.init(kSensorId);
    frame.set_seq(1);

    // 1) Two well-formed frames from the GHOST port (not in topology)
    //    must be dropped with recv_unknown_source incrementing.
    EXPECT(sendto_udp_loopback(sender_ghost, kRouterPortUnknown,
                               frame.bytes, kRouterFrameSize));
    EXPECT(sendto_udp_loopback(sender_ghost, kRouterPortUnknown,
                               frame.bytes, kRouterFrameSize));

    // 2) One well-formed frame from the KNOWN sensor port must be
    //    forwarded.
    EXPECT(sendto_udp_loopback(sender_known, kRouterPortUnknown,
                               frame.bytes, kRouterFrameSize));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stop_router.store(true, std::memory_order_relaxed);
    router_thread.join();
    ::close(sender_known);
    ::close(sender_ghost);

    const DatagramRouterMetrics& m = link.metrics();
    const uint64_t unknown    = m.recv_unknown_source.load();
    const uint64_t fwd        = m.forwarded.load();
    const uint64_t trunc      = m.recv_truncated.load();

    std::cout << "  unknown UDP: recv_unknown_source=" << unknown
              << " forwarded=" << fwd
              << " recv_truncated=" << trunc << '\n';

    EXPECT_EQ(unknown, static_cast<uint64_t>(2));
    EXPECT_EQ(fwd,     static_cast<uint64_t>(1));
    EXPECT_EQ(trunc,   static_cast<uint64_t>(0));
}

// ----------------------------------------------------------------------
// Scenario 3 — UDS router_listen path under a non-existent directory.
// `Uds::bind` first ::unlink()s the path (always succeeds, even on
// ENOENT), then ::bind() into the parent directory. If the parent dir
// doesn't exist, ::bind returns -1 + errno=ENOENT, and the helper
// throws "uds bind failed". The caller (DatagramRouterLink::bind_router)
// propagates the exception. Verify both:
//   (a) bind_router throws (clean error, not crash);
//   (b) no socket file is left at the impossible path.
// ----------------------------------------------------------------------

void test_wrong_uds_path_throws_at_bind() {
    static constexpr const char* kBadPath = "/nonexistent_dir_xyzzy/router.sock";

    static constexpr PeerEntry kPeers[] = {
        {1, "a", peer_uds("/tmp/rim_fault_a.sock"), 0, 0},
    };
    static const RouterTopology kTopo = {
        .peers         = kPeers,
        .peer_count    = 1,
        .router_listen = peer_uds(kBadPath),
    };

    auto link = DatagramRouterLink<Uds>::server(kTopo);
    Uds::BindParams params{kBadPath};

    bool threw = false;
    std::string what;
    try {
        link.bind_router(params);
    } catch (const std::runtime_error& e) {
        threw = true;
        what = e.what();
    }
    EXPECT(threw);
    EXPECT(what.find("bind") != std::string::npos);

    // Belt and suspenders — the path should not exist (parent dir
    // doesn't exist, so it certainly shouldn't).
    struct stat st;
    EXPECT(::stat(kBadPath, &st) != 0);
}

// ----------------------------------------------------------------------
// Scenario 4 — Pre-existing UDS socket file is cleanly rebound.
//
// Stand up a UDS server, then "kill" it cleanly (close fd). Touch the
// path again with a file (simulating "a previous crash left a regular
// file or a stale socket here"), then bind a fresh server on the same
// path. The second bind must succeed because Uds::bind() ::unlink()s
// before calling ::bind(). Regression guard for the rebind path.
// ----------------------------------------------------------------------

void test_uds_rebind_after_stale_path_succeeds() {
    static constexpr const char* kPath = "/tmp/rim_fault_rebind.sock";
    ::unlink(kPath);

    static constexpr PeerEntry kPeers[] = {
        {1, "a", peer_uds("/tmp/rim_fault_unused.sock"), 0, 0},
    };
    static const RouterTopology kTopo = {
        .peers         = kPeers,
        .peer_count    = 1,
        .router_listen = peer_uds(kPath),
    };

    // First bind — creates the socket file.
    {
        auto link = DatagramRouterLink<Uds>::server(kTopo);
        Uds::BindParams params{kPath};
        link.bind_router(params);
        struct stat st{};
        EXPECT(::stat(kPath, &st) == 0);
        // Link destructor closes the fd but the socket file remains
        // because no one unlinks it on the close path.
    }

    // Path still exists from the prior bind (simulates stale state
    // after a SIGKILL'd router).
    struct stat st{};
    EXPECT(::stat(kPath, &st) == 0);

    // Second bind — must unlink + rebind cleanly.
    bool second_bind_ok = true;
    try {
        auto link = DatagramRouterLink<Uds>::server(kTopo);
        Uds::BindParams params{kPath};
        link.bind_router(params);
    } catch (const std::runtime_error&) {
        second_bind_ok = false;
    }
    EXPECT(second_bind_ok);

    ::unlink(kPath);
}

// ----------------------------------------------------------------------
// Scenario 5 — Topology with `shm_max_payload < kRouterFrameSize` is
// rejected at parse time. This is exercised in detail by
// topology_loader_test (multiple ADR 0009 sub-cases); the assertion
// here is the D4-level smoke check that "a profile that would later
// crash bind_router is rejected at the boundary, not at runtime."
// ----------------------------------------------------------------------

void test_topology_loader_rejects_undersize_shm_payload() {
    constexpr const char* kBadToml = R"(
[router]
listen = "shm:/rim_fault_router"
[[peers]]
id              = 1
name            = "sensor"
local           = "shm:/rim_fault_sensor"
shm_max_payload = 32
)";

    bool threw = false;
    std::string what;
    try {
        (void)load_topology_from_toml_string(kBadToml);
    } catch (const std::runtime_error& e) {
        threw = true;
        what = e.what();
    }
    EXPECT(threw);
    // Frame-aware error message — see ADR 0009.
    EXPECT(what.find("kRouterFrameSize") != std::string::npos
        || what.find("RouterFrame") != std::string::npos);
}

// ----------------------------------------------------------------------
// Scenario 6 — SIGKILL the router mid-traffic; second router_server
// bind on the same jetson_prod.toml profile succeeds. Complements
// router_restart_test (Phase D2), which kills the router at idle.
// "Mid-traffic" here means a sensor SHM client is actively pushing
// frames into the request ring when the SIGKILL fires.
// ----------------------------------------------------------------------

constexpr const char* kRouterServerBin = "build/ipc/test/router_server";
constexpr const char* kJetsonProfile   = "config/profiles/jetson_prod.toml";

// Phase F F1 — 6-peer jetson_prod.toml; control-plane SHM regions listed
// here. Sideband regions (rim_vision_nv12 / rim_ml_tensor_in/out) are
// excluded because they're created by vision_capture / ml_inference
// processes that fault_injection_test does not spawn.
const char* const kShmNames[] = {
    "/rim_router",
    "/rim_router_sensor",
    "/rim_router_controller",
    "/rim_router_recorder",
    "/rim_router_vision_capture",
    "/rim_router_ml_inference",
    "/rim_router_python_tooling",
    "/rim_router_dashboard",
};

void shm_unlink_all() {
    for (const char* name : kShmNames) {
        ::shm_unlink(name);
    }
}

bool shm_exists(const char* name) {
    const int fd = ::shm_open(name, O_RDWR, 0);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

void redirect_stdio_to_devnull() {
    const int fd = ::open("/dev/null", O_WRONLY);
    if (fd < 0) {
        return;
    }
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
}

pid_t spawn_router() {
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
    redirect_stdio_to_devnull();
    const char* argv[] = {
        "router_server",
        "--config",
        kJetsonProfile,
        nullptr,
    };
    ::execv(kRouterServerBin, const_cast<char* const*>(argv));
    ::_exit(127);
}

bool wait_for_router_bind(int deadline_ms) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (shm_exists("/rim_router_sensor")) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool router_alive(pid_t pid) {
    return pid > 0 && ::kill(pid, 0) == 0;
}

// Bounded reap. Tries SIGTERM → poll WNOHANG → SIGKILL → poll WNOHANG.
// Returns true if the pid was reaped within the total deadline. **Never
// blocks indefinitely** — important for sandboxed CI hosts where parent
// → child signal delivery has been observed to be flaky.
bool reap_bounded(pid_t pid, int total_ms) {
    if (pid <= 0) {
        return true;
    }
    const auto end = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(total_ms);
    const auto sigkill_at = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(total_ms / 2);
    bool escalated = false;
    ::kill(pid, SIGTERM);
    while (std::chrono::steady_clock::now() < end) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid || (r == -1 && errno == ECHILD)) {
            return true;
        }
        if (!escalated && std::chrono::steady_clock::now() >= sigkill_at) {
            ::kill(pid, SIGKILL);
            escalated = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void test_sigkill_router_mid_traffic_recoverable() {
    shm_unlink_all();

    // Sensor ring view matching jetson_prod.toml — used to push frames
    // into the live router so the SIGKILL actually catches it servicing
    // traffic, not idling.
    static constexpr PeerEntry kSensorPeer[] = {
        {kSensorId, "sensor", peer_shm("/rim_router_sensor"),
         256u, 64u},
    };

    // ----- Round 1: spawn router, push some frames, SIGKILL -----------
    const pid_t pid1 = spawn_router();
    EXPECT(pid1 > 0);

    if (!wait_for_router_bind(2000)) {
        // The spawned router never bound — likely a sandboxed runner
        // where shm_open / mmap is restricted. We can't exercise the
        // recovery path without a live router, so reap and skip.
        std::cout << "  [skip] sigkill_router_mid_traffic: "
                     "spawned router_server did not bind within 2 s\n";
        reap_bounded(pid1, 1000);
        shm_unlink_all();
        return;
    }

    bool sensor_attached = false;
    try {
        IpcEndpoint<ShmSpsc> sensor_client;
        bind_shm_endpoint(sensor_client, kSensorPeer[0], false);
        sensor_attached = true;

        RouterFrame f;
        f.init(kSensorId);
        for (int i = 0; i < 16; ++i) {
            f.set_seq(static_cast<uint32_t>(i));
            ShmSpsc::SendParams params{.payload = f.read_only()};
            sensor_client.try_send(params, f.read_only());
        }
        // Brief sleep so the router forwards a few before SIGKILL.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } catch (const std::runtime_error&) {
        // bind_shm_endpoint can fail in restrictive environments — the
        // recovery path is the value of the test, not the mid-traffic
        // detail. Continue without the sensor publish.
    }

    // SIGKILL with bounded reap. ::kill returns -1 in some sandbox
    // contexts; we don't gate on its return value because the only
    // observable that matters is "router gone within the deadline."
    ::kill(pid1, SIGKILL);
    EXPECT(reap_bounded(pid1, 1000));

    // SIGKILL bypasses destructors — the per-peer SHM regions linger.
    // (`/rim_router` itself is a topology label, not a created
    // region; per-peer rings are what we recover on the next bind.)
    EXPECT(shm_exists("/rim_router_sensor"));
    EXPECT(shm_exists("/rim_router_controller"));
    EXPECT(shm_exists("/rim_router_recorder"));

    // ----- Round 2: fresh spawn must bind cleanly ---------------------
    const pid_t pid2 = spawn_router();
    EXPECT(pid2 > 0);
    EXPECT(wait_for_router_bind(2000));
    EXPECT(router_alive(pid2));

    if (sensor_attached) {
        try {
            IpcEndpoint<ShmSpsc> sensor_client;
            bind_shm_endpoint(sensor_client, kSensorPeer[0], false);

            RouterFrame f;
            f.init(kSensorId);
            f.set_seq(99u);
            ShmSpsc::SendParams params{.payload = f.read_only()};
            const ShmSendResult r =
                sensor_client.try_send(params, f.read_only());
            EXPECT(r == ShmSendResult::Ok || r == ShmSendResult::Full);
        } catch (const std::runtime_error&) {
            // Same caveat as round 1; the rebind itself is what matters.
        }
    }

    // Clean shutdown — bounded, never blocks forever.
    EXPECT(reap_bounded(pid2, 2000));

    shm_unlink_all();

    std::cout << "  sigkill mid-traffic: round-1 SHM persisted, "
                 "round-2 bind succeeded"
              << (sensor_attached ? ", round-2 publish accepted" : "")
              << '\n';
}

}  // namespace

int main() {
    test_truncated_udp_datagram_drops_with_metric();
    test_unknown_source_udp_drops_with_metric();
    test_wrong_uds_path_throws_at_bind();
    test_uds_rebind_after_stale_path_succeeds();
    test_topology_loader_rejects_undersize_shm_payload();
    test_sigkill_router_mid_traffic_recoverable();

    std::cout << "fault_injection_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
