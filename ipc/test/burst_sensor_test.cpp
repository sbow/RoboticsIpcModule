// Phase D2 — burst sensor integration scenario.
//
// Closes the deferred Phase C4 deliverable: prove that subscriber-side seq
// attribution (SourceSeqTracker from Phase D1) correctly tallies what the
// SHM router (with right-sized rings from D0) actually delivers under load.
//
// Topology: 1 sensor (id 1), 1 subscriber (id 2), single unicast route.
// Sensor publishes a sequence of seq-numbered RouterFrames as fast as the
// ring will accept; subscriber drains continuously and feeds every frame
// into a SourceSeqTracker.
//
// Invariants (with right-sized rings, the only "drops" we should see are
// transient ring saturations caused by the subscriber falling behind the
// sensor by more than the ring depth):
//
//   tracker.sample_count(sensor) == subscriber_recv
//   tracker.sample_count(sensor) + tracker.gap_count(sensor) == published_seqs
//   tracker.last_seq(sensor) == max_published_seq
//   tracker.out_of_order_count(sensor) == 0  (SHM SPSC preserves order)
//   tracker.duplicate_count(sensor) == 0     (no duplicates from publisher)
//   subscriber_recv >= 99% of published (acceptance bar; depends on timing)

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"
#include "router/source_seq_tracker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <thread>

namespace {

int assertions_run    = 0;
int assertions_failed = 0;

#define EXPECT(cond) do {                                                  \
    ++assertions_run;                                                      \
    if (!(cond)) {                                                         \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT failed @ " << __FILE__ << ':' << __LINE__     \
                  << " : " #cond "\n";                                     \
    }                                                                      \
} while (0)

#define EXPECT_EQ(a, b) do {                                               \
    ++assertions_run;                                                      \
    const auto _a = (a);                                                   \
    const auto _b = (b);                                                   \
    if (_a != _b) {                                                        \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT_EQ failed @ " << __FILE__ << ':' << __LINE__  \
                  << " : " #a " == " #b " (" << _a << " vs " << _b         \
                  << ")\n";                                                \
    }                                                                      \
} while (0)

constexpr const char* kRouterListen = "/cpp_tricks_burst_sensor_router";
constexpr const char* kSensorShm    = "/cpp_tricks_burst_sensor_sensor";
constexpr const char* kSubShm       = "/cpp_tricks_burst_sensor_sub";

constexpr uint8_t kSensorId = 1;
constexpr uint8_t kSubId    = 2;

constexpr uint32_t kSlotCount  = 256;   // ADR 0009 production sizing
constexpr uint32_t kMaxPayload = 64;

constexpr PeerEntry kPeers[] = {
    {kSensorId, "sensor", peer_shm(kSensorShm), kSlotCount, kMaxPayload},
    {kSubId,    "sub",    peer_shm(kSubShm),    kSlotCount, kMaxPayload},
};

constexpr RouterTopology kTopo = {
    .peers         = kPeers,
    .peer_count    = sizeof(kPeers) / sizeof(kPeers[0]),
    .router_listen = peer_shm(kRouterListen),
};

constexpr RouteRule kRules[] = {
    {kSensorId, kSubId, 0},
};

void cleanup_shm() {
    ::shm_unlink(kRouterListen);
    ::shm_unlink(kSensorShm);
    ::shm_unlink(kSubShm);
}

// Test sized to keep wall time ~1 s while exercising many ring depths
// worth of frames. 5000 frames = ~20× the ring depth.
constexpr int kBurstFrames        = 5000;
constexpr int kRouterDeadlineMs   = 4000;
constexpr int kPostBurstDrainMs   = 300;

void test_burst_sensor_seq_attribution() {
    cleanup_shm();

    auto router_link = ShmRouterLink::server(kTopo);
    router_link.bind_router({});

    IpcEndpoint<ShmSpsc> sensor_client;
    bind_shm_endpoint(sensor_client, kPeers[0], false);
    IpcEndpoint<ShmSpsc> sub_client;
    bind_shm_endpoint(sub_client, kPeers[1], false);

    std::atomic<bool>     stop_router{false};
    std::atomic<bool>     stop_sub{false};
    std::atomic<uint64_t> sub_recv{0};
    std::atomic<uint32_t> first_observed_seq{UINT32_MAX};
    std::atomic<uint32_t> last_observed_seq{0};

    SourceSeqTracker<> tracker;

    std::thread router_thread([&]() {
        uint64_t ts = 1;
        RouterFrame scratch;
        while (!stop_router.load(std::memory_order_relaxed)) {
            router_link.forward(scratch, ts++, kRules, std::size(kRules));
        }
    });

    // Subscriber thread — drains as fast as possible; each frame goes
    // into the SourceSeqTracker. We don't share `tracker` across threads:
    // the subscriber thread is the sole writer, and the main thread reads
    // it only after subscriber join().
    std::thread sub_thread([&]() {
        char buf_storage[256];
        while (!stop_sub.load(std::memory_order_relaxed)) {
            Buffer buf = Buffer::writable(buf_storage, sizeof(buf_storage));
            ShmSpsc::RecvResult rr{};
            if (!ShmSpsc::try_recv(sub_client.handle(), buf, rr)) {
                continue;
            }
            if (buf.size < kRouterFrameSize) {
                continue;
            }
            RouterFrame frame;
            std::memcpy(frame.bytes, buf.data, kRouterFrameSize);
            const uint32_t s = frame.seq();
            tracker.observe(frame.source(), s);

            uint32_t expected_first = first_observed_seq.load(std::memory_order_relaxed);
            if (expected_first == UINT32_MAX) {
                first_observed_seq.compare_exchange_strong(
                    expected_first, s, std::memory_order_relaxed);
            }
            last_observed_seq.store(s, std::memory_order_relaxed);
            sub_recv.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRouterDeadlineMs);

    uint64_t pub_ok       = 0;
    uint64_t pub_attempts = 0;
    for (int i = 0; i < kBurstFrames; ++i) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "burst_sensor_test: deadline exceeded during publish\n";
            ++assertions_failed;
            break;
        }
        RouterFrame f;
        f.init(kSensorId);
        f.set_seq(static_cast<uint32_t>(i));
        ++pub_attempts;
        ShmSpsc::SendParams params{.payload = f.read_only()};
        // Retry on full so every seq actually leaves the publisher — the
        // ring fills only if the router thread hasn't drained yet, which
        // is a brief condition (the router runs in a tight forward()
        // loop). This guarantees seq 0..N-1 are published in order.
        while (sensor_client.try_send(params, f.read_only()) != ShmSendResult::Ok) {
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
            std::this_thread::yield();
        }
        ++pub_ok;
    }

    // Let the router fully drain the sensor req ring and the subscriber
    // consume any in-flight frames before reading metrics.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPostBurstDrainMs));

    stop_router.store(true, std::memory_order_relaxed);
    stop_sub.store(true, std::memory_order_relaxed);
    router_thread.join();
    sub_thread.join();

    const ShmRouterMetrics& m = router_link.metrics();
    const uint64_t fwd      = m.forwarded.load();
    const uint64_t drop     = m.dropped_full_per_peer[kSubId].load();
    const uint64_t recv     = sub_recv.load();
    const uint32_t first_s  = first_observed_seq.load();
    const uint32_t last_s   = last_observed_seq.load();
    const uint64_t samples  = tracker.sample_count(kSensorId);
    const uint64_t gaps     = tracker.gap_count(kSensorId);
    const uint64_t oo       = tracker.out_of_order_count(kSensorId);
    const uint64_t dups     = tracker.duplicate_count(kSensorId);

    std::cout << "  burst sensor: pub=" << pub_ok
              << " fwd=" << fwd
              << " drop[sub]=" << drop
              << " recv=" << recv
              << " seq[first..last]=" << first_s << ".." << last_s
              << " gaps=" << gaps
              << " out_of_order=" << oo
              << " dups=" << dups << '\n';

    EXPECT_EQ(pub_ok, static_cast<uint64_t>(kBurstFrames));

    // Sample count from the tracker mirrors the subscriber recv count
    // exactly (one observe() per recv).
    EXPECT_EQ(samples, recv);

    // Per-peer accounting: forwarded + drop_to_sub == pub_ok.
    EXPECT_EQ(fwd + drop, pub_ok);
    // Subscriber received everything the router forwarded (modulo timing
    // at the very tail — the post-burst drain window covers that).
    EXPECT_EQ(recv, fwd);

    // **C4 closing invariant:** observed + missed == published-range.
    // The publisher emitted seq 0..N-1 in order; the tracker should see
    // last_seq == N-1 and the "samples + gaps" accounting should equal
    // the seq range it covered.
    EXPECT_EQ(last_s, static_cast<uint32_t>(kBurstFrames - 1));
    // First observation came from somewhere in [0, gaps]. Samples + gaps
    // counts how many seq positions we covered (the gap counter accounts
    // for every skipped position between consecutive observations).
    EXPECT_EQ(samples + gaps, static_cast<uint64_t>(last_s - first_s + 1));

    // SHM SPSC preserves order; no duplicates, no out-of-order frames.
    EXPECT_EQ(oo, 0u);
    EXPECT_EQ(dups, 0u);

    // Acceptance bar: at least 99% of frames make it through with the
    // right-sized rings (ADR 0009). Tighter bars require more controlled
    // pacing and belong in the D3 soak script.
    EXPECT(recv * 100 >= pub_ok * 99);

    cleanup_shm();
}

}  // namespace

int main() {
    test_burst_sensor_seq_attribution();

    std::cout << "burst_sensor_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
