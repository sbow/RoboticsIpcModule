#pragma once

// router_app.h — app-only conveniences for router demos / bridges.
//
// Library headers (under src/router/) MUST NOT include this file. App and
// example code may include it freely; it re-exports router_stop_flag() and
// router_idle_expired() from router/lifecycle.hpp so existing test bins
// (router_server.cpp, router_client.cpp, router_test.cpp) keep their
// #include "router_app.h" line and compile unchanged.

#include "router/lifecycle.hpp"

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

inline void router_log(std::string_view line) {
    std::string out(line);
    out += '\n';
    (void)!::write(STDERR_FILENO, out.data(), out.size());
}
