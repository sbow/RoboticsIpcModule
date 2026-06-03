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

// Phase G — per-topic routing. A RouteRule may carry an optional topic
// selector. kRouteTopicAny is the sentinel for "this rule matches every
// topic_id" — i.e. the legacy source-only behavior. It deliberately reuses
// the 0xFFFF value (distinct namespace from kSidebandIdxNone): RouterFrame
// topic_id is u16, so the topology loader reserves 0xFFFF as the match-any
// value and rejects it as a literal topic in a profile, exactly as peer-id
// 0 / 255 are reserved.
constexpr uint16_t kRouteTopicAny = 0xFFFF;

struct RouteTargets {
    std::array<uint8_t, kMaxRouteDests> ids{};
    size_t count = 0;

    const uint8_t* begin() const { return ids.data(); }
    const uint8_t* end() const { return ids.data() + count; }
};

struct RouteRule {
    uint8_t  source     = kEndpointInvalid;
    uint8_t  dest_count = 0;
    // Phase G — topic selector. kRouteTopicAny (default) means "match any
    // topic", preserving source-only dispatch for rules built by make_route
    // or loaded from a profile without a `topic` field. A concrete value
    // restricts the rule to frames whose topic_id matches.
    uint16_t topic_id   = kRouteTopicAny;
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
        kRouteTopicAny,
        {static_cast<uint8_t>(dests)...},
    };
}

// Phase G — per-topic variant. Identical to make_route but pins the rule to
// a specific topic_id. A frame only matches this rule when its source AND
// topic_id both match. Constexpr-friendly, same compile-time fan-out guards.
//
//   constexpr RouteRule r = make_topic_route(1, 100, 2, 3); // (1, topic 100) -> {2, 3}
template<typename... Dests>
constexpr RouteRule make_topic_route(uint8_t source, uint16_t topic_id,
                                     Dests... dests) noexcept {
    static_assert(sizeof...(dests) >= 1,
                  "make_topic_route: rule must have at least one destination");
    static_assert(sizeof...(dests) <= kMaxRouteDests,
                  "make_topic_route: fan-out exceeds kMaxRouteDests");
    return RouteRule{
        source,
        static_cast<uint8_t>(sizeof...(dests)),
        topic_id,
        {static_cast<uint8_t>(dests)...},
    };
}

// First-match-wins lookup. The hot path on every forwarded frame. Both
// SHM and datagram links call this with their (rules, rule_count, source,
// topic_id) just before fan-out (link.hpp / shm_router_link.hpp). When the
// first rule whose (source, topic_id) matches has dest_count == 0
// (impossible if built via make_route or the topology loader, but
// defensive) the loop returns an empty RouteTargets and the link will skip
// forwarding.
//
// Phase G — the lookup is now two-dimensional. A rule matches when:
//   * its source equals the frame's source, AND
//   * its topic selector is kRouteTopicAny (matches any topic), OR equals
//     the frame's topic_id.
// The table is walked in declaration order, so a topic-specific rule placed
// before a source-only (kRouteTopicAny) rule for the same source takes
// precedence — first match wins, deterministically.
//
// `topic_id` defaults to kRouteTopicAny so legacy / topic-unaware callers
// keep source-only semantics: with that default, only kRouteTopicAny rules
// match (topic-specific rules are skipped), which is exactly the pre-Phase-G
// behavior. The hot path stays branch-light — a source-only rule
// short-circuits the topic compare.
inline RouteTargets route_targets_for(
    const RouteRule* rules,
    size_t rule_count,
    uint8_t source,
    uint16_t topic_id = kRouteTopicAny) {
    for (size_t i = 0; i < rule_count; ++i) {
        if (rules[i].source != source) {
            continue;
        }
        if (rules[i].topic_id != kRouteTopicAny
         && rules[i].topic_id != topic_id) {
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
