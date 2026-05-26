#pragma once

// Demo CLI helper: pick a log path for a known role from argv, with a
// caller-supplied default for the no-argv case.
//
// This header is **demo-shaped** (lives under ipc/test/, not ipc/src/) but
// is used by both the router_client demo and the cli_args_test regression.
// The arity rules below regression-guard the lesson learned during Phase A:
//
//   role = "controller" → argv[5] preferred, argv[3] fallback
//   role = "recorder"   → argv[4] preferred, argv[3] fallback
//
// Asymmetry comes from the demo CLI shapes (see router_client.cpp usage()
// for the full grammar); controller has one extra positional argument.
//
// `fallback_fn` is invoked only when none of the argv positions for the
// role are present. Returning nullptr from `fallback_fn` is fine — the
// helper will produce an empty std::string in that case. Unknown roles
// always return an empty string and never invoke the fallback.

#include <cstring>
#include <string>

inline std::string log_path_for_role(
    const char* role,
    int argc,
    char* argv[],
    const char* (*fallback_fn)(const char* role)) {
    auto fallback = [&]() -> std::string {
        if (!fallback_fn) {
            return {};
        }
        const char* s = fallback_fn(role);
        return s ? std::string(s) : std::string();
    };

    if (std::strcmp(role, "controller") == 0) {
        if (argc >= 6) {
            return argv[5];
        }
        if (argc >= 4) {
            return argv[3];
        }
        return fallback();
    }
    if (std::strcmp(role, "recorder") == 0) {
        if (argc >= 5) {
            return argv[4];
        }
        if (argc >= 4) {
            return argv[3];
        }
        return fallback();
    }
    return {};
}
