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
#include <string>
#include <string_view>
#include <unistd.h>

inline void install_router_stop_handlers() {
    install_app_stop_handlers();
}

inline bool router_stop_requested() {
    return app_stop_requested();
}

inline bool router_test_mode() {
    return std::getenv("ROUTER_TEST") != nullptr;
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
