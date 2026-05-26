#pragma once

#include <csignal>

// Shared graceful-shutdown flag for long-running IPC test binaries.
// Install handlers once at startup; poll app_stop_requested() in recv loops.

inline volatile std::sig_atomic_t* app_stop_flag() {
    static volatile std::sig_atomic_t flag = 0;
    return &flag;
}

inline void app_on_stop(int) {
    *app_stop_flag() = 1;
}

inline void install_app_stop_handlers() {
    struct sigaction sa{};
    sa.sa_handler = app_on_stop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

inline bool app_stop_requested() {
    return *app_stop_flag() != 0;
}

inline void app_reset_stop_flag() {
    *app_stop_flag() = 0;
}
