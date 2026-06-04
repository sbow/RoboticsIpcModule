#pragma once

// router_app.h — app-only conveniences for router demos / bridges.
//
// Library headers (under src/router/) MUST NOT include this file. App and
// example code may include it freely; it re-exports router_stop_flag() and
// router_idle_expired() from router/lifecycle.hpp so existing test bins
// (router_server.cpp, router_client.cpp, router_test.cpp) keep their
// #include "router_app.h" line and compile unchanged.

#include "router/lifecycle.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

inline void install_router_stop_handlers() {
    install_app_stop_handlers();
}

inline bool router_stop_requested() {
    return app_stop_requested();
}

inline bool router_test_mode() {
    return std::getenv("ROUTER_TEST") != nullptr;
}

// C6: systemd readiness notification (sd_notify protocol), implemented
// WITHOUT linking libsystemd to keep the module dependency-light / header-only
// (see docs/adr/0015-systemd-readiness-notification.md and ADR 0004).
//
// The sd_notify(3) wire protocol is just a newline-separated datagram sent to
// the AF_UNIX socket named by $NOTIFY_SOCKET. We send it ourselves. When the
// router runs under a `Type=notify` unit, systemd holds dependent units gated
// `After=rim-router.service` until it receives our `READY=1` — so peers stop
// racing the router's first `shm_open` / `bind`.
//
// `state` is one sd_notify status line, e.g. "READY=1" or "STOPPING=1".
// Returns true iff a datagram was sent. A no-op returning false when
// $NOTIFY_SOCKET is unset (not under Type=notify, or run from a shell / test
// harness) or empty, so callers can invoke it unconditionally. Never throws.
inline bool router_sd_notify(const char* state) {
    const char* socket_path = std::getenv("NOTIFY_SOCKET");
    if (socket_path == nullptr || socket_path[0] == '\0' || state == nullptr) {
        return false;
    }

    const std::size_t path_len = std::strlen(socket_path);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // Need room for the path plus, for filesystem sockets, a trailing NUL.
    if (path_len == 0 || path_len >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, socket_path, path_len + 1);

    socklen_t addr_len;
    if (addr.sun_path[0] == '@') {
        // Abstract namespace: leading '@' is the systemd spelling of a NUL
        // first byte; the address length excludes any trailing NUL.
        addr.sun_path[0] = '\0';
        addr_len = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path_len);
    } else {
        addr_len = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path_len + 1);
    }

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    const std::size_t state_len = std::strlen(state);
    const ssize_t sent = ::sendto(
        fd, state, state_len, MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&addr), addr_len);
    ::close(fd);
    return sent == static_cast<ssize_t>(state_len);
}

// Signal that all endpoints are bound and dependent units may start.
inline bool router_notify_ready() {
    return router_sd_notify("READY=1");
}

// Signal that the router is shutting down (lets systemd distinguish a clean
// stop from a crash during the shutdown window).
inline bool router_notify_stopping() {
    return router_sd_notify("STOPPING=1");
}

// Phase B (B3): pluggable logger.
//
// The library itself never logs from the hot path (RouterServer / RouterClient
// receive an on_forward lambda; routing is std::string-free). Apps and bridges
// that *do* log can register a callback once at startup; downstream tooling
// (journald, ROS log, syslog, custom file rotator) plugs in here without the
// library having to know.
//
// Default behaviour (no callback registered): write "<msg>\n" to STDERR_FILENO.
// This preserves the pre-B3 behaviour for the demos and tests.
enum RouterLogLevel {
    ROUTER_LOG_INFO = 0,
    ROUTER_LOG_WARN = 1,
    ROUTER_LOG_ERR  = 2,
};

using RouterLogFn = void (*)(int level, const char* msg, std::size_t len);

inline RouterLogFn& router_log_fn_ref_() {
    static RouterLogFn fn = nullptr;
    return fn;
}

inline void router_set_log_fn(RouterLogFn fn) {
    router_log_fn_ref_() = fn;
}

inline RouterLogFn router_get_log_fn() {
    return router_log_fn_ref_();
}

// Leveled write. Routes through the registered RouterLogFn if any; otherwise
// writes "<msg>\n" to STDERR_FILENO. Always non-throwing.
inline void router_log(int level, std::string_view line) {
    if (RouterLogFn fn = router_log_fn_ref_()) {
        fn(level, line.data(), line.size());
        return;
    }
    (void)level;   // unused when no callback registered
    std::string out(line);
    out += '\n';
    (void)!::write(STDERR_FILENO, out.data(), out.size());
}

// Backward-compat overload: pre-B3 callers that pass no level get INFO.
inline void router_log(std::string_view line) {
    router_log(ROUTER_LOG_INFO, line);
}
