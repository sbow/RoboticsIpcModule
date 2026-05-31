// Phase D1 / Phase F C5 Scope A — route_targets_for edge cases
// (ipc/src/router/routing.hpp).
//
// route_targets_for is the dispatch primitive used by every router link
// (ShmRouterLink::forward, DatagramRouterLink::forward). Exercising its
// edge cases in a pure unit test catches regressions in the route table
// semantics without standing up a full router.
//
// Coverage:
//   - Phase D1 — empty rules, unknown source, unicast / 2-broadcast,
//     first-match-wins, dest sentinel, kEndpointInvalid source,
//     ForwardResult truthiness.
//   - Phase F C5 Scope A — fan-out beyond 2 (3, kMaxRouteDests), mixed-
//     width rules, dest_count truncation semantics, make_route factory
//     deduction, constexpr usability.

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
        make_route(1, 2, 3),
        make_route(2, 3),
    };
    RouteTargets t = route_targets_for(rules, 2, 99);
    EXPECT_EQ(t.count, 0u);
}

void test_unicast_rule_returns_one_target() {
    const RouteRule rules[] = {
        make_route(1, 2),
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 1u);
    EXPECT_EQ(t.ids[0], 2);
}

void test_broadcast_rule_returns_two_targets_in_order() {
    const RouteRule rules[] = {
        make_route(1, 2, 3),
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
        make_route(1, 2, 3),
        make_route(1, 4, 5),   // ignored — source 1 already matched above
    };
    RouteTargets t = route_targets_for(rules, 2, 1);
    EXPECT_EQ(t.count, 2u);
    EXPECT_EQ(t.ids[0], 2);
    EXPECT_EQ(t.ids[1], 3);
}

void test_distinct_sources_each_match_their_own_rule() {
    const RouteRule rules[] = {
        make_route(1, 2, 3),
        make_route(2, 3),
        make_route(3, 1),
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
        make_route(1, 2, 3),
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    int observed = 0;
    for (const uint8_t* it = t.begin(); it != t.end(); ++it) {
        ++observed;
    }
    EXPECT_EQ(observed, 2);
}

void test_unicast_does_not_introduce_invalid_peer() {
    // A 1-dest rule must NOT iterate into the array tail (which is
    // default-zero-filled, i.e. kEndpointInvalid).
    const RouteRule rules[] = {
        make_route(1, 2),
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 1u);
    EXPECT(t.end() - t.begin() == 1);
    // Defensive: the slot at index 1 must stay 0 (the array default), so
    // even if a caller naively indexes past .count they see kEndpointInvalid
    // and not a stale peer id from a previous rule.
    EXPECT_EQ(t.ids[1], 0);
}

void test_kEndpointInvalid_source_does_not_match_a_normal_rule() {
    const RouteRule rules[] = {
        make_route(1, 2, 3),
        make_route(2, 3),
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

// --- Phase F C5 Scope A — fan-out beyond two destinations --------------

void test_fanout_to_three_destinations() {
    // The original cap. A 3-dest rule (e.g. sensor -> {controller,
    // recorder, dashboard_feed}) is the smallest case that the prior
    // 2-dest cap rejected and that Scope A unblocks.
    const RouteRule rules[] = {
        make_route(1, 2, 3, 8),
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 3u);
    EXPECT_EQ(t.ids[0], 2);
    EXPECT_EQ(t.ids[1], 3);
    EXPECT_EQ(t.ids[2], 8);
    EXPECT(t.end() - t.begin() == 3);
}

void test_fanout_to_kMaxRouteDests() {
    // Saturated fan-out — every slot used. This is the upper bound of
    // what a single rule can express; >kMaxRouteDests dests should move
    // to per-topic routing (parked C5 Scope C).
    static_assert(kMaxRouteDests == 8,
                  "test assumes kMaxRouteDests == 8 — update both together");
    // source=1, then 8 distinct destination ids (2..9). The static_assert
    // inside make_route would trip if this list had more than 8 dests.
    const RouteRule rules[] = {
        make_route(1, 2, 3, 4, 5, 6, 7, 8, 9),
    };
    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 8u);
    for (size_t k = 0; k < 8; ++k) {
        EXPECT_EQ(t.ids[k], static_cast<uint8_t>(k + 2));
    }
}

void test_mixed_width_rules_in_same_table() {
    // First-match-wins still picks the rule whose source matches, even
    // when widths vary widely across the table.
    const RouteRule rules[] = {
        make_route(1, 2),                              // 1 dest
        make_route(2, 3, 7, 8),                        // 3 dests
        make_route(4, 5, 3, 8),                        // 3 dests
        make_route(5, 2, 3, 4, 6, 7, 8),               // 6 dests
    };

    RouteTargets t1 = route_targets_for(rules, 4, 1);
    EXPECT_EQ(t1.count, 1u);
    EXPECT_EQ(t1.ids[0], 2);

    RouteTargets t2 = route_targets_for(rules, 4, 2);
    EXPECT_EQ(t2.count, 3u);
    EXPECT_EQ(t2.ids[0], 3);
    EXPECT_EQ(t2.ids[1], 7);
    EXPECT_EQ(t2.ids[2], 8);

    RouteTargets t5 = route_targets_for(rules, 4, 5);
    EXPECT_EQ(t5.count, 6u);
    EXPECT_EQ(t5.ids[0], 2);
    EXPECT_EQ(t5.ids[5], 8);
    // Unused tail must remain default-zero so naive over-iteration is safe.
    EXPECT_EQ(t5.ids[6], 0);
    EXPECT_EQ(t5.ids[7], 0);
}

void test_dest_count_truncation_does_not_leak_tail() {
    // Manually-constructed RouteRule with dest_count smaller than the
    // populated array. route_targets_for must honour dest_count exactly
    // and not iterate past it (defensive: the topology loader and
    // make_route() never produce such a state, but `RouteRule r{}` +
    // hand-edits could).
    RouteRule rule{};
    rule.source = 1;
    rule.dest_count = 2;
    rule.dest = {2, 3, 9, 9, 9, 9, 9, 9};
    const RouteRule rules[] = { rule };

    RouteTargets t = route_targets_for(rules, 1, 1);
    EXPECT_EQ(t.count, 2u);
    EXPECT_EQ(t.ids[0], 2);
    EXPECT_EQ(t.ids[1], 3);
    // ids[2] is the array default (0), NOT the stale 9 from the rule.
    EXPECT_EQ(t.ids[2], 0);
}

void test_make_route_factory_is_constexpr_and_compact() {
    // make_route() must be usable in constexpr contexts (kDemoRouteRules
    // is a constexpr array) and must set source / dest_count / dest
    // correctly with no padding surprises.
    constexpr RouteRule r1 = make_route(1, 2);
    static_assert(r1.source == 1, "");
    static_assert(r1.dest_count == 1, "");
    static_assert(r1.dest[0] == 2, "");
    static_assert(r1.dest[1] == 0, "");

    constexpr RouteRule r2 = make_route(1, 2, 3, 4, 8);
    static_assert(r2.source == 1, "");
    static_assert(r2.dest_count == 4, "");
    static_assert(r2.dest[0] == 2, "");
    static_assert(r2.dest[3] == 8, "");
    static_assert(r2.dest[4] == 0, "");

    // Runtime cross-check so the static_asserts above are not dead code.
    EXPECT_EQ(r1.dest_count, 1);
    EXPECT_EQ(r2.dest_count, 4);
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
    test_unicast_does_not_introduce_invalid_peer();
    test_kEndpointInvalid_source_does_not_match_a_normal_rule();
    test_forward_result_is_truthy_only_when_source_set();

    // C5 Scope A
    test_fanout_to_three_destinations();
    test_fanout_to_kMaxRouteDests();
    test_mixed_width_rules_in_same_table();
    test_dest_count_truncation_does_not_leak_tail();
    test_make_route_factory_is_constexpr_and_compact();

    std::cout << "routing_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
