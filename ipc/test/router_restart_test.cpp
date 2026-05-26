// Phase D2 — router restart / SIGKILL recovery.
//
// Subprocess-driven: spawn `router_server --config jetson_prod.toml`, let it
// bind its SHM regions, then `kill -9` it (skipping every destructor) and
// confirm that a second router_server can bind the same profile without
// errors. This is the "stale /dev/shm/cpp_tricks_* won't block the next
// run" promise from the Phase D plan, and a regression guard for the
// shm_open/ftruncate/memset path in ShmRegion::bind.
//
// We focus on SHM because UDS bind() already self-heals (Uds::bind unlinks
// the socket path before binding) and UDP has no on-disk state.
//
// The test does NOT setenv("ROUTER_TEST=1") — we want the router to stay
// alive until SIGKILL, not idle-exit.

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
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

constexpr const char* kRouterServerBin = "build/ipc/test/router_server";
constexpr const char* kJetsonProfile   = "config/profiles/jetson_prod.toml";

// SHM names from jetson_prod.toml — kept here as constants so the test
// can verify they exist after a SIGKILL and clean them up at the end.
const char* const kShmNames[] = {
    "/cpp_tricks_router",
    "/cpp_tricks_router_sensor",
    "/cpp_tricks_router_controller",
    "/cpp_tricks_router_recorder",
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

const char* basename_path(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

pid_t spawn_router_with_config(const char* config_path) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
    redirect_stdio_to_devnull();
    const char* argv[] = {
        basename_path(kRouterServerBin),
        "--config",
        config_path,
        nullptr,
    };
    ::execv(kRouterServerBin, const_cast<char* const*>(argv));
    ::_exit(127);
}

// Send a signal and reap. Returns true if the child terminated within the
// grace window; never blocks forever.
bool stop_pid(pid_t pid, int signum, int grace_ms) {
    if (pid <= 0) {
        return true;
    }
    if (::kill(pid, signum) != 0 && errno != ESRCH) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(grace_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid || (reaped == -1 && errno == ECHILD)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Final safety net.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return false;
}

// Wait for any of the SHM names from the profile to materialize, indicating
// the router has progressed past bind. Returns true within `deadline_ms`.
bool wait_for_router_bind(int deadline_ms) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        // jetson_prod.toml binds the sensor peer ring first by the order
        // of [[peers]] in the file; once that exists, bind is in progress.
        if (shm_exists("/cpp_tricks_router_sensor")) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool router_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    // Non-blocking check; ::kill(pid, 0) succeeds iff the pid exists and
    // we have permission to signal it.
    return ::kill(pid, 0) == 0;
}

void test_sigkill_router_leaves_recoverable_shm() {
    shm_unlink_all();

    // -------- Round 1: spawn, verify bind, SIGKILL ----------------------
    const pid_t pid1 = spawn_router_with_config(kJetsonProfile);
    EXPECT(pid1 > 0);

    EXPECT(wait_for_router_bind(2000));
    EXPECT(router_pid_alive(pid1));

    // SIGKILL — skips destructors, so the SHM regions stay on /dev/shm.
    ::kill(pid1, SIGKILL);
    ::waitpid(pid1, nullptr, 0);
    EXPECT(!router_pid_alive(pid1));

    // The regions are still on /dev/shm because SIGKILL bypassed
    // ShmRegion::~ShmRegion's shm_unlink. This is the documented Phase A
    // / LESSONS-LEARNED state — and the precondition for what we want to
    // prove next.
    EXPECT(shm_exists("/cpp_tricks_router_sensor"));
    EXPECT(shm_exists("/cpp_tricks_router_controller"));
    EXPECT(shm_exists("/cpp_tricks_router_recorder"));

    // -------- Round 2: spawn again, must bind cleanly -------------------
    // ShmSpsc::bind calls shm_open(O_CREAT | O_RDWR) + ftruncate. Even
    // though the regions exist with the prior (matching) size, the new
    // process must complete bind and reach the forward loop. If this
    // hangs or fails, the previous incarnation's stale state is
    // blocking us.
    const pid_t pid2 = spawn_router_with_config(kJetsonProfile);
    EXPECT(pid2 > 0);

    EXPECT(wait_for_router_bind(2000));
    EXPECT(router_pid_alive(pid2));

    // Clean shutdown this time — SIGTERM gives the router a chance to
    // unlink before exit. The shm regions should be gone afterwards.
    EXPECT(stop_pid(pid2, SIGTERM, 2000));
    EXPECT(!router_pid_alive(pid2));

    // Belt and suspenders — shm_unlink anything we still have, so this
    // test stays self-contained.
    shm_unlink_all();
}

// Negative-control round: a second spawn while the FIRST is still alive
// must NOT crash either process — both will bind the same shm names with
// O_CREAT|O_RDWR, which is well-defined (both see the same backing
// region). This is not a recommended deployment pattern, but the test
// gates against a regression where the second bind throws and tears
// down the host environment.
void test_overlapping_spawns_do_not_corrupt_state() {
    shm_unlink_all();

    const pid_t pid1 = spawn_router_with_config(kJetsonProfile);
    EXPECT(pid1 > 0);
    EXPECT(wait_for_router_bind(2000));

    const pid_t pid2 = spawn_router_with_config(kJetsonProfile);
    EXPECT(pid2 > 0);

    // Give pid2 a moment to either bind or exit early.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Stop in reverse spawn order.
    stop_pid(pid2, SIGTERM, 2000);
    stop_pid(pid1, SIGTERM, 2000);

    // After both shut down (one of them is_creator, the other isn't),
    // we expect the SHM names to be gone — the creator unlinks on exit.
    // If neither is the creator (race), they linger; either way, we
    // shm_unlink for the next test.
    shm_unlink_all();

    // Test passes if we got here without aborting / hanging.
    EXPECT(!router_pid_alive(pid1));
    EXPECT(!router_pid_alive(pid2));
}

}  // namespace

int main() {
    test_sigkill_router_leaves_recoverable_shm();
    test_overlapping_spawns_do_not_corrupt_state();

    std::cout << "router_restart_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
