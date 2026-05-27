// Phase D2 — slow recorder integration scenario.
//
// Three SHM peers (sensor / controller / recorder) on a router that fans
// sensor publishes out to both subscribers. The controller drains
// continuously; the recorder drains at 1/10th the sensor rate. With the
// recorder slot ring sized to one production-like 256 × 64 B configuration
// (ADR 0009), the recorder's ring fills and the router starts dropping
// frames addressed to recorder.
//
// Pass criteria (per Phase D2 plan + ADR 0006 + D2a):
//   * dropped_full_per_peer[recorder] > 0  (recorder fell behind)
//   * dropped_full_per_peer[controller] == 0  (controller kept up)
//   * controller's receive count >= 99% of sensor's publish count
//   * no deadlock — sensor finishes within a deadline, threads join cleanly

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/metrics.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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

constexpr const char* kRouterListen   = "/rim_slow_recorder_router";
constexpr const char* kSensorShm      = "/rim_slow_recorder_sensor";
constexpr const char* kControllerShm  = "/rim_slow_recorder_controller";
constexpr const char* kRecorderShm    = "/rim_slow_recorder_recorder";

constexpr uint8_t kSensorId     = 1;
constexpr uint8_t kControllerId = 2;
constexpr uint8_t kRecorderId   = 3;

// Production-like ring sizing per ADR 0009: 256 slots × 64 B payload.
// A single RouterFrame v2 fits exactly in one slot stride; 256 slots gives
// recorder ~250 ms of head room at the 1 kHz publish rate, enough that the
// failure mode (recorder ring full → drops) is reproducible but not
// instant.
constexpr uint32_t kSlotCount  = 256;
constexpr uint32_t kMaxPayload = 64;

constexpr PeerEntry kPeers[] = {
    {kSensorId,     "sensor",     peer_shm(kSensorShm),     kSlotCount, kMaxPayload},
    {kControllerId, "controller", peer_shm(kControllerShm), kSlotCount, kMaxPayload},
    {kRecorderId,   "recorder",   peer_shm(kRecorderShm),   kSlotCount, kMaxPayload},
};

constexpr RouterTopology kTopo = {
    .peers         = kPeers,
    .peer_count    = sizeof(kPeers) / sizeof(kPeers[0]),
    .router_listen = peer_shm(kRouterListen),
};

constexpr RouteRule kRules[] = {
    {kSensorId, kControllerId, kRecorderId},   // sensor -> {controller, recorder}
};

void cleanup_shm() {
    ::shm_unlink(kRouterListen);
    ::shm_unlink(kSensorShm);
    ::shm_unlink(kControllerShm);
    ::shm_unlink(kRecorderShm);
}

// Test parameters — chosen to keep the test fast (~1 s wall) while
// reliably triggering recorder backpressure (recorder rate < sensor rate
// by 10×, sensor publishes 5× the ring depth).
constexpr int kSensorFrames        = 2000;
constexpr int kSensorPeriodUs      = 500;   // ~2 kHz publish rate
constexpr int kRecorderPeriodUs    = 5000;  // ~200 Hz drain rate (10× slower)
constexpr int kRouterDeadlineMs    = 4000;  // absolute upper bound on the test
constexpr int kPostSensorDrainMs   = 200;   // grace period for the router /
                                            // controller to drain in-flight
                                            // frames after the sensor stops

void test_slow_recorder_isolated_attribution() {
    cleanup_shm();

    auto router_link = ShmRouterLink::server(kTopo);
    router_link.bind_router({});

    // Client endpoints for the three peer roles — each thread owns exactly
    // one endpoint (SPSC contract).
    IpcEndpoint<ShmSpsc> sensor_client;
    bind_shm_endpoint(sensor_client, kPeers[0], false);
    IpcEndpoint<ShmSpsc> controller_client;
    bind_shm_endpoint(controller_client, kPeers[1], false);
    IpcEndpoint<ShmSpsc> recorder_client;
    bind_shm_endpoint(recorder_client, kPeers[2], false);

    std::atomic<bool>     stop_router{false};
    std::atomic<bool>     stop_controller{false};
    std::atomic<bool>     stop_recorder{false};
    std::atomic<uint64_t> controller_recv{0};
    std::atomic<uint64_t> recorder_recv{0};
    std::atomic<uint64_t> sensor_pub_attempts{0};
    std::atomic<uint64_t> sensor_pub_ok{0};

    // Router forwarder — drives ShmRouterLink::forward() at maximum rate.
    std::thread router_thread([&]() {
        uint64_t ts = 1;
        RouterFrame scratch;
        while (!stop_router.load(std::memory_order_relaxed)) {
            router_link.forward(scratch, ts++, kRules, std::size(kRules));
        }
    });

    // Controller drain — recv as fast as possible (no pacing).
    std::thread controller_thread([&]() {
        char buf_storage[256];
        while (!stop_controller.load(std::memory_order_relaxed)) {
            Buffer buf = Buffer::writable(buf_storage, sizeof(buf_storage));
            ShmSpsc::RecvResult rr{};
            if (ShmSpsc::try_recv(controller_client.handle(), buf, rr)) {
                controller_recv.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    // Recorder drain — paced at 1/10th of the sensor rate; this is where
    // the backpressure originates.
    std::thread recorder_thread([&]() {
        char buf_storage[256];
        while (!stop_recorder.load(std::memory_order_relaxed)) {
            Buffer buf = Buffer::writable(buf_storage, sizeof(buf_storage));
            ShmSpsc::RecvResult rr{};
            if (ShmSpsc::try_recv(recorder_client.handle(), buf, rr)) {
                recorder_recv.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(kRecorderPeriodUs));
        }
    });

    // Sensor publish loop on the main thread.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRouterDeadlineMs);

    for (int i = 0; i < kSensorFrames; ++i) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "slow_recorder_test: deadline exceeded during publish\n";
            ++assertions_failed;
            break;
        }
        RouterFrame f;
        f.init(kSensorId);
        f.set_seq(static_cast<uint32_t>(i));
        sensor_pub_attempts.fetch_add(1, std::memory_order_relaxed);
        ShmSpsc::SendParams params{.payload = f.read_only()};
        if (sensor_client.try_send(params, f.read_only()) == ShmSendResult::Ok) {
            sensor_pub_ok.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(kSensorPeriodUs));
    }

    // Let the router drain anything still in the sensor's req ring before
    // we read counters.
    std::this_thread::sleep_for(std::chrono::milliseconds(kPostSensorDrainMs));

    stop_router.store(true, std::memory_order_relaxed);
    stop_controller.store(true, std::memory_order_relaxed);
    stop_recorder.store(true, std::memory_order_relaxed);
    router_thread.join();
    controller_thread.join();
    recorder_thread.join();

    const ShmRouterMetrics& m = router_link.metrics();
    const uint64_t pub_ok        = sensor_pub_ok.load();
    const uint64_t pub_attempts  = sensor_pub_attempts.load();
    const uint64_t drop_to_ctrl  = m.dropped_full_per_peer[kControllerId].load();
    const uint64_t drop_to_rec   = m.dropped_full_per_peer[kRecorderId].load();
    const uint64_t fwd_total     = m.forwarded.load();
    const uint64_t drop_total    = m.dropped_full.load();
    const uint64_t ctrl_recv     = controller_recv.load();
    const uint64_t rec_recv      = recorder_recv.load();

    std::cout << "  slow recorder: pub_attempts=" << pub_attempts
              << " pub_ok=" << pub_ok
              << " fwd=" << fwd_total
              << " drop_total=" << drop_total
              << " drop[ctrl]=" << drop_to_ctrl
              << " drop[rec]=" << drop_to_rec
              << " ctrl_recv=" << ctrl_recv
              << " rec_recv=" << rec_recv << '\n';

    // Sensor published at least most of what it tried (router thread keeps
    // the sensor req ring drained).
    EXPECT(pub_ok >= pub_attempts * 9 / 10);
    EXPECT_EQ(pub_attempts, static_cast<uint64_t>(kSensorFrames));

    // **D2a attribution gate.** Recorder must have taken drops; controller
    // must have taken none.
    EXPECT(drop_to_rec > 0);
    EXPECT_EQ(drop_to_ctrl, 0u);

    // Per-peer counters sum to the aggregate.
    EXPECT_EQ(drop_to_ctrl + drop_to_rec, drop_total);

    // Controller stayed current. With the fast drain it should see >= 99%
    // of the sensor frames the router managed to forward to it
    // (fwd_total = forwards to ctrl + forwards to rec; per-route the split
    // is 1:1 on every forward, so ctrl forwards = fwd_total - drop_to_rec
    // ... but more directly, ctrl forwards = pub_ok - drop_to_ctrl).
    const uint64_t ctrl_fwd = pub_ok - drop_to_ctrl;
    EXPECT(ctrl_recv >= ctrl_fwd * 99 / 100);

    // Recorder receive count is bounded above by the ring depth plus what
    // it drained during the test — a sanity check that we didn't somehow
    // bypass backpressure.
    EXPECT(rec_recv <= pub_ok);
    EXPECT(rec_recv > 0);

    // Untouched peer slots stay quiet.
    EXPECT_EQ(m.dropped_full_per_peer[0].load(), 0u);
    EXPECT_EQ(m.dropped_full_per_peer[kSensorId].load(), 0u);
    EXPECT_EQ(m.dropped_full_per_peer[255].load(), 0u);

    cleanup_shm();
}

}  // namespace

int main() {
    test_slow_recorder_isolated_attribution();

    std::cout << "slow_recorder_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
