#pragma once

#include "router/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Routing primitive — the source-id-indexed fan-out table the router uses
// to decide which peers receive a copy of every frame.
//
// Phase F C5 Scope A — kMaxRouteDests lifts the previous 2-destination
// cap. 8 mirrors the 8-peer catalog (SYSTEM-VISION.md): a single rule can
// now fan out to every peer in the topology. Bumping further is a one-line
// constant change, but if a deployment regularly needs >8 destinations per
// rule that's usually a signal the topology should move to per-topic
// routing (parked C5 Scope C).

constexpr size_t kMaxRouteDests = 8;

struct RouteTargets {
    std::array<uint8_t, kMaxRouteDests> ids{};
    size_t count = 0;

    const uint8_t* begin() const { return ids.data(); }
    const uint8_t* end() const { return ids.data() + count; }
};

struct RouteRule {
    uint8_t source = kEndpointInvalid;
    uint8_t dest_count = 0;
    std::array<uint8_t, kMaxRouteDests> dest{};
};

// Variadic factory — preferred over raw aggregate-init at call sites because
// it deduces dest_count from the argument list, refuses zero-dest rules at
// compile time, and refuses rules with more than kMaxRouteDests destinations
// at compile time. Constexpr-friendly so `constexpr RouteRule kRules[] = ...`
// still works.
//
//   constexpr RouteRule r1 = make_route(1, 2);          // 1 -> {2}
//   constexpr RouteRule r2 = make_route(1, 2, 3);       // 1 -> {2, 3}
//   constexpr RouteRule r3 = make_route(1, 2, 3, 4, 8); // 1 -> {2, 3, 4, 8}
template<typename... Dests>
constexpr RouteRule make_route(uint8_t source, Dests... dests) noexcept {
    static_assert(sizeof...(dests) >= 1,
                  "make_route: rule must have at least one destination");
    static_assert(sizeof...(dests) <= kMaxRouteDests,
                  "make_route: fan-out exceeds kMaxRouteDests");
    return RouteRule{
        source,
        static_cast<uint8_t>(sizeof...(dests)),
        {static_cast<uint8_t>(dests)...},
    };
}

// First-match-wins lookup. The hot path on every forwarded frame. Both
// SHM and datagram links call this with their (rules, rule_count, source)
// just before fan-out (link.hpp / shm_router_link.hpp). When the first
// rule whose source matches has dest_count == 0 (impossible if built via
// make_route or the topology loader, but defensive) the loop returns an
// empty RouteTargets and the link will skip forwarding.
inline RouteTargets route_targets_for(
    const RouteRule* rules,
    size_t rule_count,
    uint8_t source) {
    for (size_t i = 0; i < rule_count; ++i) {
        if (rules[i].source != source) {
            continue;
        }
        RouteTargets t;
        const size_t n = rules[i].dest_count;
        t.count = n;
        for (size_t k = 0; k < n; ++k) {
            t.ids[k] = rules[i].dest[k];
        }
        return t;
    }
    return {};
}

struct ForwardResult {
    uint8_t source = kEndpointInvalid;
    RouteTargets targets{};

    explicit operator bool() const { return source != kEndpointInvalid; }
};
