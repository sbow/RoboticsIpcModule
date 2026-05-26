// Phase D1 — route_targets_for edge cases (ipc/src/router/routing.hpp).
//
// route_targets_for is the dispatch primitive used by every router link
// (ShmRouterLink::forward, DatagramRouterLink::forward). Exercising its
// edge cases in a pure unit test catches regressions in the route table
// semantics without standing up a full router.

#include "router/routing.hpp"
#include "router/frame.hpp"

#include <cstdint>
#include <iostream>

namespace {

int g_total  = 0;
int g_failed = 0;

#define EXPECT(cond)                                                        \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) {                                                      \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT(" #cond ")\n";                            \
        }                                                                   \
    } while (0)

#define EXPECT_EQ(a, b)                                                     \
    do {                                                                    \
        ++g_total;                                                          \
        const auto _a = (a);                                                \
        const auto _b = (b);                                                \
        if (!(_a == _b)) {                                                  \
            ++g_failed;                                                     \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__             \
                      << " EXPECT_EQ(" #a ", " #b ") -> "                   \
                      << +(_a) << " != " << +(_b) << "\n";                  \
        }                                                                   \
    } while (0)

void test_empty_rules_returns_empty_targets_for_any_source() {
    RouteTargets t = route_targets_for(nullptr, 0, 1);
    EXPECT_EQ(t.count, 0u);
    EXPECT(t.begin() == t.end());

    // Same for source 0 (kEndpointInvalid is 0) and 255 (kEndpointServer).
    EXPECT_EQ(route_targets_for(nullptr, 0, kEndpointInvalid).count, 0u);
    EXPECT_EQ(route_targets_for(nullptr, 0, kEndpointServer).count, 0u);
}

void test_unknown_source_returns_empty_targets() {
    const RouteRule rules[] = {
        {1, 2, 3},
        {2, 3, 0},
    };
    RouteTargets t = route_targets_for(rules, 2, 99);
    EXPECT_EQ(t.count, 0u);
}

void test_unicast_rule_returns_one_target() {
    // dest1 == 0 is the sentinel for "no second destination" (ADR 0001).
    const RouteRule rules[] = {
        {1, 2, 0},
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 1u);
    EXPECT_EQ(t.ids[0], 2);
}

void test_broadcast_rule_returns_two_targets_in_order() {
    const RouteRule rules[] = {
        {1, 2, 3},
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 2u);
    EXPECT_EQ(t.ids[0], 2);
    EXPECT_EQ(t.ids[1], 3);
}

void test_first_matching_rule_wins() {
    // The table semantics — first rule whose source matches is used; later
    // rules with the same source are ignored. ShmRouterLink / Datagram
    // links rely on this for "one frame in, fixed fan-out" determinism.
    const RouteRule rules[] = {
        {1, 2, 3},
        {1, 4, 5},   // ignored — source 1 already matched above
    };
    RouteTargets t = route_targets_for(rules, 2, 1);
    EXPECT_EQ(t.count, 2u);
    EXPECT_EQ(t.ids[0], 2);
    EXPECT_EQ(t.ids[1], 3);
}

void test_distinct_sources_each_match_their_own_rule() {
    const RouteRule rules[] = {
        {1, 2, 3},
        {2, 3, 0},
        {3, 1, 0},
    };
    RouteTargets t1 = route_targets_for(rules, 3, 1);
    EXPECT_EQ(t1.count, 2u);
    EXPECT_EQ(t1.ids[0], 2);
    EXPECT_EQ(t1.ids[1], 3);

    RouteTargets t2 = route_targets_for(rules, 3, 2);
    EXPECT_EQ(t2.count, 1u);
    EXPECT_EQ(t2.ids[0], 3);

    RouteTargets t3 = route_targets_for(rules, 3, 3);
    EXPECT_EQ(t3.count, 1u);
    EXPECT_EQ(t3.ids[0], 1);
}

void test_iterator_range_matches_count() {
    const RouteRule rules[] = {
        {1, 2, 3},
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    int observed = 0;
    for (const uint8_t* it = t.begin(); it != t.end(); ++it) {
        ++observed;
    }
    EXPECT_EQ(observed, 2);
}

void test_dest1_zero_does_not_introduce_invalid_peer() {
    // A rule {src, dest0, 0} is a unicast rule. The router must NOT emit
    // a second forward to peer id 0 (kEndpointInvalid) just because the
    // sentinel is in the dest1 slot.
    const RouteRule rules[] = {
        {1, 2, 0},
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 1u);
    // The unused slot stays default-constructed (0); but count is 1,
    // so iteration won't see it.
    EXPECT(t.end() - t.begin() == 1);
}

void test_kEndpointInvalid_source_does_not_match_a_normal_rule() {
    // No rule should be authored with source = 0; the router stamps the
    // real source byte before calling. Defensive: ensure 0 doesn't match
    // {0, _, _} accidentally either (no such rules in well-formed
    // topologies, but the function should still behave deterministically).
    const RouteRule rules[] = {
        {1, 2, 3},
        {2, 3, 0},
    };
    EXPECT_EQ(route_targets_for(rules, 2, kEndpointInvalid).count, 0u);
}

void test_forward_result_is_truthy_only_when_source_set() {
    ForwardResult empty;
    EXPECT(!static_cast<bool>(empty));
    EXPECT_EQ(empty.source, kEndpointInvalid);

    ForwardResult populated;
    populated.source = 7;
    populated.targets = RouteTargets{{{2, 3}}, 2};
    EXPECT(static_cast<bool>(populated));
    EXPECT_EQ(populated.targets.count, 2u);
}

}  // namespace

int main() {
    test_empty_rules_returns_empty_targets_for_any_source();
    test_unknown_source_returns_empty_targets();
    test_unicast_rule_returns_one_target();
    test_broadcast_rule_returns_two_targets_in_order();
    test_first_matching_rule_wins();
    test_distinct_sources_each_match_their_own_rule();
    test_iterator_range_matches_count();
    test_dest1_zero_does_not_introduce_invalid_peer();
    test_kEndpointInvalid_source_does_not_match_a_normal_rule();
    test_forward_result_is_truthy_only_when_source_set();

    std::cout << "routing_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
