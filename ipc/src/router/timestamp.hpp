#pragma once

// Library-side timestamp helper. Returns CLOCK_MONOTONIC_RAW in nanoseconds
// since boot, suitable for stamping RouterFrame::timestamp_ns on the publish
// path.
//
// Policy (see docs/adr/0010-router-timestamp-clock.md):
//   - Single-host monotonic, slew-free (immune to NTP / adjtime).
//   - Survives router restart within a single boot epoch.
//   - Cross-host correlation is delegated to user code or a future dedicated
//     recorder module — the router intentionally does not bring PTP / NTP /
//     CLOCK_TAI into the hot path.
//
// User peers that want to stamp their own frames with the same clock the
// router uses on forward include this header and call router_now_ns().
//
// Layering: library-side. May be included by ipc/src/ headers and by app
// code; must not depend on router_app.h.

#include <cstdint>
#include <ctime>

inline uint64_t router_now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}
