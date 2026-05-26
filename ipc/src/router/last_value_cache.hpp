#pragma once

// LastValueCache<N> — subscriber-side cache holding the most recent frame
// received per source peer id.
//
// Use case (B4 / subscriber bridges): dashboards, recorders, and ML peers
// that want "what is the latest reading from each sensor?" without storing
// a per-source ring. Updated on the read path; queried out-of-band.
//
// Non-goals:
//   * Not thread-safe. One owner per cache instance. If you fan out frames
//     to multiple consumers from one RouterClient, give each their own cache.
//   * Not a router-side primitive — the router stays stateless on the hot
//     path (DESIGN-PRINCIPLES.md: no per-message std::string / per-message
//     allocation). Caching is a subscriber concern.
//   * No expiry / TTL. Add at the call site if needed; this header stays
//     std-only and dependency-free so it fits the embedded layout.
//
// Storage: std::array<Slot, N> in the cache object itself. With N=256
// (default) the footprint is 256 * (32 B frame + 1 B flag + padding) ≈ 8.5 KB,
// which is fine for an on-robot subscriber. Drop N if you know your peer id
// space is small.

#include "router/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

template<std::size_t N = 256>
class LastValueCache {
    static_assert(N > 0, "LastValueCache requires non-zero N");
    static_assert(N <= 256,
                  "RouterFrame source id is uint8_t; N>256 has unreachable slots");

public:
    LastValueCache() = default;

    // Record the most recent frame from `source`. Idempotent for repeated
    // calls with the same source; later updates overwrite earlier ones.
    void update(uint8_t source, const RouterFrame& frame) noexcept {
        if (source >= N) {
            return;
        }
        slots_[source].frame = frame;
        slots_[source].valid = true;
    }

    // True iff at least one update(source, ...) has been recorded since
    // construction or the last clear().
    bool has(uint8_t source) const noexcept {
        if (source >= N) {
            return false;
        }
        return slots_[source].valid;
    }

    // Copy the latest frame for `source` into `out`. Returns false (and
    // leaves `out` untouched) if no frame has been recorded for `source`.
    bool latest(uint8_t source, RouterFrame& out) const noexcept {
        if (source >= N || !slots_[source].valid) {
            return false;
        }
        out = slots_[source].frame;
        return true;
    }

    // Number of distinct sources with at least one recorded frame.
    std::size_t size() const noexcept {
        std::size_t count = 0;
        for (const auto& s : slots_) {
            if (s.valid) {
                ++count;
            }
        }
        return count;
    }

    // Drop every cached frame. After clear(), has(s) is false for all s.
    void clear() noexcept {
        for (auto& s : slots_) {
            s.valid = false;
        }
    }

private:
    struct Slot {
        RouterFrame frame{};
        bool        valid = false;
    };

    std::array<Slot, N> slots_{};
};
