// Phase D1 — subscriber-side seq attribution / gap detection.
//
// Tests SourceSeqTracker (ipc/src/router/source_seq_tracker.hpp) against
// the RouterFrame v2 seq surface (ADR 0008). Closes Phase C's deferred
// C4 by giving subscribers a deterministic way to attribute frame loss
// without per-frame allocation.
//
// In-process only — no datagram I/O, no fork/exec. We construct frames
// with known seq sequences, feed them through SourceSeqTracker (often
// alongside LastValueCache for realism), and assert the bookkeeping.

#include "router/source_seq_tracker.hpp"
#include "router/last_value_cache.hpp"
#include "router/frame.hpp"

#include <cstdint>
#include <iostream>
#include <string>

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
                      << _a << " != " << _b << "\n";                        \
        }                                                                   \
    } while (0)

RouterFrame make_frame(uint8_t source, uint32_t seq) {
    RouterFrame f;
    f.init(source);
    f.set_seq(seq);
    return f;
}

void test_first_observation_classifies_as_first() {
    SourceSeqTracker<> t;
    EXPECT(!t.has(7));
    const auto cls = t.observe(7, 42);
    EXPECT(cls == SourceSeqTracker<>::Observation::First);
    EXPECT(t.has(7));
    EXPECT_EQ(t.last_seq(7), 42u);
    EXPECT_EQ(t.sample_count(7), 1ull);
    EXPECT_EQ(t.gap_count(7), 0ull);
}

void test_perfectly_in_order_stream_has_zero_gaps() {
    SourceSeqTracker<> t;
    for (uint32_t s = 100; s < 200; ++s) {
        const auto cls = t.observe(3, s);
        EXPECT(cls == (s == 100 ? SourceSeqTracker<>::Observation::First
                                : SourceSeqTracker<>::Observation::InOrder));
    }
    EXPECT_EQ(t.sample_count(3), 100ull);
    EXPECT_EQ(t.gap_count(3), 0ull);
    EXPECT_EQ(t.duplicate_count(3), 0ull);
    EXPECT_EQ(t.out_of_order_count(3), 0ull);
    EXPECT_EQ(t.last_seq(3), 199u);
}

void test_single_gap_accounts_missing_frames() {
    SourceSeqTracker<> t;
    t.observe(1, 0);
    t.observe(1, 1);
    t.observe(1, 2);
    const auto cls = t.observe(1, 10);   // seq jumped 3 -> 10; missed 3..9 (7 frames)
    EXPECT(cls == SourceSeqTracker<>::Observation::Gap);
    EXPECT_EQ(t.gap_count(1), 7ull);
    EXPECT_EQ(t.last_seq(1), 10u);
    EXPECT_EQ(t.sample_count(1), 4ull);

    // Continue in order — gap count must stay at 7.
    t.observe(1, 11);
    t.observe(1, 12);
    EXPECT_EQ(t.gap_count(1), 7ull);
    EXPECT_EQ(t.last_seq(1), 12u);
}

void test_duplicate_is_counted_separately_no_gap() {
    SourceSeqTracker<> t;
    t.observe(1, 100);
    const auto cls = t.observe(1, 100);
    EXPECT(cls == SourceSeqTracker<>::Observation::Duplicate);
    EXPECT_EQ(t.duplicate_count(1), 1ull);
    EXPECT_EQ(t.gap_count(1), 0ull);
    EXPECT_EQ(t.last_seq(1), 100u);
}

void test_out_of_order_does_not_advance_last_seq() {
    SourceSeqTracker<> t;
    t.observe(2, 1000);
    t.observe(2, 1005);   // forward gap of 4
    EXPECT_EQ(t.gap_count(2), 4ull);
    EXPECT_EQ(t.last_seq(2), 1005u);

    // A delayed frame with seq=1002 arrives — stale.
    const auto cls = t.observe(2, 1002);
    EXPECT(cls == SourceSeqTracker<>::Observation::OutOfOrder);
    EXPECT_EQ(t.out_of_order_count(2), 1ull);
    EXPECT_EQ(t.gap_count(2), 4ull);             // unchanged
    EXPECT_EQ(t.last_seq(2), 1005u);             // unchanged

    // Forward progress resumes from the high-water mark; one frame missed
    // since 1005 -> 1007 is seq 1006.
    t.observe(2, 1007);
    EXPECT_EQ(t.gap_count(2), 5ull);
    EXPECT_EQ(t.last_seq(2), 1007u);
}

void test_wrap_around_2_to_the_32_is_treated_as_in_order() {
    SourceSeqTracker<> t;
    // Approach the wrap.
    t.observe(5, 0xFFFFFFFEu);
    t.observe(5, 0xFFFFFFFFu);   // in-order +1
    EXPECT_EQ(t.gap_count(5), 0ull);
    EXPECT_EQ(t.last_seq(5), 0xFFFFFFFFu);

    // Wrap to 0.
    const auto cls = t.observe(5, 0u);
    EXPECT(cls == SourceSeqTracker<>::Observation::InOrder);
    EXPECT_EQ(t.gap_count(5), 0ull);
    EXPECT_EQ(t.last_seq(5), 0u);

    // Wrap with a gap: 0 -> 5 misses 1..4 (4 frames).
    t.observe(5, 5u);
    EXPECT_EQ(t.gap_count(5), 4ull);
    EXPECT_EQ(t.last_seq(5), 5u);
}

void test_wrap_around_with_pre_wrap_gap() {
    // The dramatic case: source's last seq we saw was just before wrap,
    // then we observe a seq that's already past the wrap.
    SourceSeqTracker<> t;
    t.observe(6, 0xFFFFFFF0u);
    // Subscriber misses 0xFFFFFFF1..0xFFFFFFFF (15 frames) and 0..2
    // (3 frames) — 18 total gap.
    const auto cls = t.observe(6, 3u);
    EXPECT(cls == SourceSeqTracker<>::Observation::Gap);
    EXPECT_EQ(t.gap_count(6), 18ull);
    EXPECT_EQ(t.last_seq(6), 3u);
}

void test_independent_sources_do_not_interfere() {
    SourceSeqTracker<> t;
    t.observe(10, 100);
    t.observe(20, 5000);

    t.observe(10, 105);   // 4-frame gap on source 10
    t.observe(20, 5001);  // in-order on source 20

    EXPECT_EQ(t.gap_count(10), 4ull);
    EXPECT_EQ(t.gap_count(20), 0ull);
    EXPECT_EQ(t.last_seq(10), 105u);
    EXPECT_EQ(t.last_seq(20), 5001u);

    // Reset one source; the other survives.
    t.reset(10);
    EXPECT(!t.has(10));
    EXPECT(t.has(20));
    EXPECT_EQ(t.gap_count(20), 0ull);
}

void test_reset_clears_only_target_source() {
    SourceSeqTracker<> t;
    t.observe(1, 1);
    t.observe(1, 5);   // gap=3
    t.observe(2, 100);
    EXPECT_EQ(t.gap_count(1), 3ull);

    t.reset(1);
    EXPECT(!t.has(1));
    EXPECT_EQ(t.gap_count(1), 0ull);
    EXPECT_EQ(t.sample_count(1), 0ull);
    // Source 2 untouched.
    EXPECT(t.has(2));
    EXPECT_EQ(t.last_seq(2), 100u);

    // Re-observing 1 starts fresh.
    const auto cls = t.observe(1, 1000);
    EXPECT(cls == SourceSeqTracker<>::Observation::First);
    EXPECT_EQ(t.last_seq(1), 1000u);
}

void test_clear_drops_every_source() {
    SourceSeqTracker<> t;
    for (uint8_t s = 0; s < 32; ++s) {
        t.observe(s, s * 100);
    }
    for (uint8_t s = 0; s < 32; ++s) {
        EXPECT(t.has(s));
    }
    t.clear();
    for (uint8_t s = 0; s < 32; ++s) {
        EXPECT(!t.has(s));
        EXPECT_EQ(t.gap_count(s), 0ull);
        EXPECT_EQ(t.sample_count(s), 0ull);
    }
}

// Integration: subscriber pattern — feed frames through both
// LastValueCache and SourceSeqTracker on the same read path.
void test_integration_with_last_value_cache_under_loss() {
    LastValueCache<> cache;
    SourceSeqTracker<> tracker;

    // Publisher emits 1000 frames at source=1; "network" loses every 10th.
    uint32_t published = 0;
    uint32_t delivered = 0;
    for (uint32_t s = 0; s < 1000; ++s) {
        ++published;
        if (s % 10 == 5) {
            continue;  // simulate a drop
        }
        ++delivered;
        RouterFrame f = make_frame(1, s);
        f.set_payload(std::string("p-") + std::to_string(s));
        cache.update(1, f);
        tracker.observe(1, s);
    }

    // 100 single-frame gaps -> total missed = 100.
    EXPECT_EQ(published, 1000u);
    EXPECT_EQ(delivered, 900u);
    EXPECT_EQ(tracker.sample_count(1), 900ull);
    EXPECT_EQ(tracker.gap_count(1), 100ull);
    EXPECT_EQ(tracker.last_seq(1), 999u);

    // Cache shows the last frame we delivered (seq 999).
    RouterFrame latest;
    EXPECT(cache.latest(1, latest));
    EXPECT_EQ(latest.seq(), 999u);
    EXPECT(latest.payload() == std::string("p-999"));
}

void test_n_template_bounds_no_unreachable_writes() {
    SourceSeqTracker<8> t;       // intentionally small
    t.observe(0, 1);
    t.observe(7, 1);
    EXPECT(t.has(0));
    EXPECT(t.has(7));
    // Source ids >= N are silently ignored (return First but record nothing).
    const auto cls = t.observe(8, 42);
    EXPECT(cls == SourceSeqTracker<8>::Observation::First);
    EXPECT(!t.has(8));   // out of range
    EXPECT_EQ(t.last_seq(8), 0u);
    EXPECT_EQ(t.sample_count(8), 0ull);
}

}  // namespace

int main() {
    test_first_observation_classifies_as_first();
    test_perfectly_in_order_stream_has_zero_gaps();
    test_single_gap_accounts_missing_frames();
    test_duplicate_is_counted_separately_no_gap();
    test_out_of_order_does_not_advance_last_seq();
    test_wrap_around_2_to_the_32_is_treated_as_in_order();
    test_wrap_around_with_pre_wrap_gap();
    test_independent_sources_do_not_interfere();
    test_reset_clears_only_target_source();
    test_clear_drops_every_source();
    test_integration_with_last_value_cache_under_loss();
    test_n_template_bounds_no_unreachable_writes();

    std::cout << "datagram_seq_test: " << (g_total - g_failed) << '/'
              << g_total << " assertions passed\n";
    return g_failed == 0 ? 0 : 1;
}
