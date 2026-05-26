#pragma once

// SourceSeqTracker<N> — subscriber-side helper that tracks per-source seq
// progression on the v2 RouterFrame (ADR 0008 added `seq`).
//
// Use case (Phase D / F subscriber bridges): given a stream of frames from
// multiple sources, answer "how many frames did we miss from source X?" and
// "did we just see a stale frame (seq went backwards)?" — without keeping
// every frame in a per-source ring.
//
// Seq semantics (ADR 0008):
//   * Per-source monotonic uint32 set by the publisher.
//   * Wraps at 2^32; subscribers use modular arithmetic for gap detection.
//   * Each source's seq starts at whatever value the publisher sends first;
//     this tracker treats the first observation as the baseline.
//
// Pairing with LastValueCache:
//   On every subscriber read:
//     cache.update(source, frame);
//     tracker.observe(source, frame.seq());
//   The two collaborate but have independent concerns — keep them separate.
//
// Non-goals:
//   * Not thread-safe. One owner per tracker instance.
//   * No per-source TTL or reset; if a publisher restarts and its seq
//     resets to 0 while the tracker still has it at e.g. 1'000'000'000,
//     the next observe() will report ~3 billion "missed" frames. That's
//     a feature, not a bug — but consumers that want restart-tolerant
//     behavior should reset(source) on a known recovery boundary.
//   * Not a router-side primitive (no per-frame allocation/state on the
//     router hot path; DESIGN-PRINCIPLES.md).

#include "router/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

template<std::size_t N = 256>
class SourceSeqTracker {
    static_assert(N > 0, "SourceSeqTracker requires non-zero N");
    static_assert(N <= 256,
                  "RouterFrame source id is uint8_t; N>256 has unreachable slots");

public:
    enum class Observation {
        First,          // first sample for this source since construction / reset
        InOrder,        // delta from last_seq == 1 (modular)
        Gap,            // delta > 1 (frames missed; modular)
        Duplicate,      // delta == 0 (same seq as last)
        OutOfOrder,     // delta == -1 .. -kBackwardWindow (modular); stale
    };

    // Sensible default: we consider seq deltas within [1, 2^31) as "forward"
    // and (-2^31, 0) as "backwards." Anything past 2^31 wraps interpretation
    // — but if a sane subscriber is missing 2 billion frames it has bigger
    // problems than reorder semantics.
    static constexpr uint32_t kForwardWindow  = 1u << 31;
    static constexpr uint32_t kBackwardWindow = 1u << 31;

    SourceSeqTracker() = default;

    // Record a seq observation for `source`. Returns the classification
    // (so the caller can log/metric without re-deriving it) and updates
    // internal counters as a side effect.
    Observation observe(uint8_t source, uint32_t seq) noexcept {
        if (source >= N) {
            return Observation::First;
        }
        Slot& slot = slots_[source];
        ++slot.samples;

        if (!slot.valid) {
            slot.valid     = true;
            slot.last_seq  = seq;
            return Observation::First;
        }

        // Modular subtraction: implicit uint32 wrap gives the unsigned
        // forward distance, which is what we want for monotonic-mod-wrap.
        const uint32_t delta = seq - slot.last_seq;
        if (delta == 0) {
            ++slot.duplicates;
            return Observation::Duplicate;
        }
        if (delta < kForwardWindow) {
            if (delta > 1) {
                // delta-1 frames went missing between last_seq and seq.
                slot.gaps      += static_cast<uint64_t>(delta - 1);
                slot.last_seq   = seq;
                return Observation::Gap;
            }
            slot.last_seq = seq;
            return Observation::InOrder;
        }
        // delta >= 2^31 — interpret as backwards (stale / out-of-order).
        ++slot.out_of_order;
        // Do NOT update last_seq; we keep the highest seq we've seen so
        // a future in-order packet computes the correct gap.
        return Observation::OutOfOrder;
    }

    // Has at least one frame been observed from this source?
    bool has(uint8_t source) const noexcept {
        return source < N && slots_[source].valid;
    }

    // Highest seq observed from `source`. Undefined if has(source) == false.
    uint32_t last_seq(uint8_t source) const noexcept {
        return source < N ? slots_[source].last_seq : 0u;
    }

    // Total seq positions skipped over for this source (sum over all gaps).
    // Wrap-aware via the modular subtraction in observe().
    uint64_t gap_count(uint8_t source) const noexcept {
        return source < N ? slots_[source].gaps : 0u;
    }

    // Number of observed frames whose seq == last_seq.
    uint64_t duplicate_count(uint8_t source) const noexcept {
        return source < N ? slots_[source].duplicates : 0u;
    }

    // Number of observations classified as OutOfOrder (delta in the
    // backward window).
    uint64_t out_of_order_count(uint8_t source) const noexcept {
        return source < N ? slots_[source].out_of_order : 0u;
    }

    // Total observe() calls for `source`.
    uint64_t sample_count(uint8_t source) const noexcept {
        return source < N ? slots_[source].samples : 0u;
    }

    // Forget everything we know about `source`; next observe() returns First.
    void reset(uint8_t source) noexcept {
        if (source < N) {
            slots_[source] = Slot{};
        }
    }

    // Forget everything for every source.
    void clear() noexcept {
        for (auto& s : slots_) {
            s = Slot{};
        }
    }

private:
    struct Slot {
        uint32_t last_seq     = 0;
        bool     valid        = false;
        uint64_t samples      = 0;
        uint64_t gaps         = 0;
        uint64_t duplicates   = 0;
        uint64_t out_of_order = 0;
    };

    std::array<Slot, N> slots_{};
};
