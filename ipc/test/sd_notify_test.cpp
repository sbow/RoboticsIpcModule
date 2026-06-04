// C6 — systemd readiness notification unit test (ADR 0015).
//
// router_app.h implements the sd_notify(3) wire protocol inline (no libsystemd):
// a datagram written to the AF_UNIX socket named by $NOTIFY_SOCKET. This test
// stands up a receiving datagram socket, points $NOTIFY_SOCKET at it, and
// asserts router_notify_ready() / router_notify_stopping() deliver the exact
// status lines systemd expects. It also locks the no-op contract (unset/empty
// $NOTIFY_SOCKET) and the abstract-namespace ('@'-prefixed) address form.

#include "router_app.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

#define EXPECT_STR_EQ(a, b)                                                 \
    do {                                                                    \
        ++g_total;                                                          \
        const std::string _a = (a);                                         \
        const std::string _b = (b);                                         \
        if (_a != _b) {                                                     \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT_STR_EQ(" #a ", " #b ") -> '"              \
                      << _a << "' != '" << _b << "'\n";                     \
        }                                                                   \
    } while (0)

// Bind a SOCK_DGRAM receiver. addr_path is either a filesystem path or, when
// it starts with '@', an abstract-namespace name (leading '@' → NUL byte).
// Returns the fd (>=0) or -1; *out_len receives the sockaddr length used.
int make_receiver(const char* addr_path, socklen_t& out_len) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::size_t len = std::strlen(addr_path);
    std::memcpy(addr.sun_path, addr_path, len + 1);
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
        out_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
    } else {
        ::unlink(addr_path);  // clear a stale socket from a prior run
        out_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len + 1);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), out_len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Non-blocking single-datagram read; returns the payload as a string ("" if
// nothing was waiting).
std::string drain_one(int fd) {
    char buf[256];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

void test_ready_delivers_READY_1_filesystem_socket() {
    std::string path = "/tmp/rim_sdnotify_test_" + std::to_string(::getpid()) + ".sock";
    socklen_t len = 0;
    const int rx = make_receiver(path.c_str(), len);
    EXPECT(rx >= 0);
    if (rx < 0) return;

    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    EXPECT(router_notify_ready() == true);
    EXPECT_STR_EQ(drain_one(rx), "READY=1");

    EXPECT(router_notify_stopping() == true);
    EXPECT_STR_EQ(drain_one(rx), "STOPPING=1");

    ::unsetenv("NOTIFY_SOCKET");
    ::close(rx);
    ::unlink(path.c_str());
}

void test_abstract_namespace_socket() {
    // systemd uses '@'-prefixed (abstract) NOTIFY_SOCKET on most distros.
    std::string name = "@rim_sdnotify_abstract_" + std::to_string(::getpid());
    socklen_t len = 0;
    const int rx = make_receiver(name.c_str(), len);
    EXPECT(rx >= 0);
    if (rx < 0) return;

    ::setenv("NOTIFY_SOCKET", name.c_str(), 1);
    EXPECT(router_notify_ready() == true);
    EXPECT_STR_EQ(drain_one(rx), "READY=1");

    ::unsetenv("NOTIFY_SOCKET");
    ::close(rx);
}

void test_no_socket_is_noop() {
    ::unsetenv("NOTIFY_SOCKET");
    EXPECT(router_notify_ready() == false);
    EXPECT(router_notify_stopping() == false);
    EXPECT(router_sd_notify("WATCHDOG=1") == false);
}

void test_empty_socket_is_noop() {
    ::setenv("NOTIFY_SOCKET", "", 1);
    EXPECT(router_notify_ready() == false);
    ::unsetenv("NOTIFY_SOCKET");
}

void test_oversized_path_is_rejected_not_crash() {
    // A path longer than sockaddr_un::sun_path must fail cleanly (false), not
    // overflow. Build one well past the ~108-byte limit.
    std::string huge(512, 'x');
    huge[0] = '/';
    ::setenv("NOTIFY_SOCKET", huge.c_str(), 1);
    EXPECT(router_notify_ready() == false);
    ::unsetenv("NOTIFY_SOCKET");
}

void test_null_state_is_noop() {
    std::string path = "/tmp/rim_sdnotify_null_" + std::to_string(::getpid()) + ".sock";
    socklen_t len = 0;
    const int rx = make_receiver(path.c_str(), len);
    EXPECT(rx >= 0);
    if (rx < 0) return;
    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    EXPECT(router_sd_notify(nullptr) == false);
    ::unsetenv("NOTIFY_SOCKET");
    ::close(rx);
    ::unlink(path.c_str());
}

}  // namespace

int main() {
    test_ready_delivers_READY_1_filesystem_socket();
    test_abstract_namespace_socket();
    test_no_socket_is_noop();
    test_empty_socket_is_noop();
    test_oversized_path_is_rejected_not_crash();
    test_null_state_is_noop();

    std::cout << "sd_notify_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
