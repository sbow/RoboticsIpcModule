#pragma once

// Library-side lifecycle primitives shared between RouterServer / RouterClient
// and apps. App-only conveniences (signal handlers, logging, ROUTER_TEST env)
// live in router_app.h and may freely include this header.
//
// Layering rule (DESIGN-PRINCIPLES.md): library headers under src/router/
// MUST NOT include router_app.h. Apps / demos may include either.

#include "ipc/app_shutdown.hpp"

#include <chrono>
#include <csignal>

inline volatile std::sig_atomic_t* router_stop_flag() {
    return app_stop_flag();
}

inline bool router_idle_expired(
    std::chrono::steady_clock::time_point last,
    std::chrono::milliseconds limit) {
    return std::chrono::steady_clock::now() - last > limit;
}
