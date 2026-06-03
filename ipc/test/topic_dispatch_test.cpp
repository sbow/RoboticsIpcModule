// Phase G — per-topic dispatch integration scenario (ADR 0013).
//
// Proves the router's hot path reads frame.topic_id() and dispatches on
// (source, topic_id), end-to-end over SHM — not just at the route_targets_for
// unit level (that lives in routing_test).
//
// Topology: 1 sensor (id 1) + 3 subscribers (ids 2, 3, 4). The sensor
// publishes a round-robin of three topic_ids (100, 200, 300). Each
// subscriber declares exactly one per-topic route:
//
//   (source 1, topic 100) -> {2}     sub_a wants only topic 100
//   (source 1, topic 200) -> {3}     sub_b wants only topic 200
//   (source 1, topic 300) -> {4}     sub_c wants only topic 300
//
// Invariants:
//   * Each subscriber receives ONLY its declared topic (wrong_topic == 0).
//     This is the core Phase G guarantee and holds regardless of drops.
//   * Each topic dispatches to exactly one destination, so the router's
//     forwarded count equals the total received across all subscribers.
//   * With right-sized rings, >= 99% of each topic's frames arrive (the
//     same acceptance bar burst_sensor_test uses).

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
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

constexpr const char* kRouterShm = "/rim_topic_dispatch_router";
constexpr const char* kSensorShm = "/rim_topic_dispatch_sensor";
constexpr const char* kSubAShm   = "/rim_topic_dispatch_sub_a";
constexpr const char* kSubBShm   = "/rim_topic_dispatch_sub_b";
constexpr const char* kSubCShm   = "/rim_topic_dispatch_sub_c";

constexpr uint8_t kSensorId = 1;
constexpr uint8_t kSubAId   = 2;   // topic 100
constexpr uint8_t kSubBId   = 3;   // topic 200
constexpr uint8_t kSubCId   = 4;   // topic 300

constexpr uint16_t kTopicA = 100;
constexpr uint16_t kTopicB = 200;
constexpr uint16_t kTopicC = 300;

constexpr uint32_t kSlotCount  = 256;   // ADR 0009 production sizing
constexpr uint32_t kMaxPayload = 64;

constexpr PeerEntry kPeers[] = {
    {kSensorId, "sensor", peer_shm(kSensorShm), kSlotCount, kMaxPayload},
    {kSubAId,   "sub_a",  peer_shm(kSubAShm),   kSlotCount, kMaxPayload},
    {kSubBId,   "sub_b",  peer_shm(kSubBShm),   kSlotCount, kMaxPayload},
    {kSubCId,   "sub_c",  peer_shm(kSubCShm),   kSlotCount, kMaxPayload},
};

constexpr RouterTopology kTopo = {
    .peers         = kPeers,
    .peer_count    = sizeof(kPeers) / sizeof(kPeers[0]),
    .router_listen = peer_shm(kRouterShm),
};

// Per-topic routes — the heart of the test. First-match-wins on
// (source, topic_id); each topic has exactly one destination.
constexpr RouteRule kRules[] = {
    make_topic_route(kSensorId, kTopicA, kSubAId),
    make_topic_route(kSensorId, kTopicB, kSubBId),
    make_topic_route(kSensorId, kTopicC, kSubCId),
};

void cleanup_shm() {
    ::shm_unlink(kRouterShm);
    ::shm_unlink(kSensorShm);
    ::shm_unlink(kSubAShm);
    ::shm_unlink(kSubBShm);
    ::shm_unlink(kSubCShm);
}

constexpr int kFramesPerTopic = 1000;
constexpr int kTotalFrames    = kFramesPerTopic * 3;
constexpr int kRouterDeadlineMs = 4000;
constexpr int kPostBurstDrainMs = 300;

struct SubCounters {
    std::atomic<uint64_t> recv{0};
    std::atomic<uint64_t> wrong_topic{0};
};

void drain_subscriber(IpcEndpoint<ShmSpsc>& client,
                      uint16_t expected_topic,
                      SubCounters& counters,
                      std::atomic<bool>& stop) {
    char buf_storage[256];
    while (!stop.load(std::memory_order_relaxed)) {
        Buffer buf = Buffer::writable(buf_storage, sizeof(buf_storage));
        ShmSpsc::RecvResult rr{};
        if (!ShmSpsc::try_recv(client.handle(), buf, rr)) {
            continue;
        }
        if (buf.size < kRouterFrameSize) {
            continue;
        }
        RouterFrame frame;
        std::memcpy(frame.bytes, buf.data, kRouterFrameSize);
        if (frame.topic_id() != expected_topic) {
            counters.wrong_topic.fetch_add(1, std::memory_order_relaxed);
        }
        counters.recv.fetch_add(1, std::memory_order_relaxed);
    }
}

void test_per_topic_dispatch_routes_each_topic_to_its_subscriber() {
    cleanup_shm();

    auto router_link = ShmRouterLink::server(kTopo);
    router_link.bind_router({});

    IpcEndpoint<ShmSpsc> sensor_client;
    bind_shm_endpoint(sensor_client, kPeers[0], false);
    IpcEndpoint<ShmSpsc> sub_a_client;
    bind_shm_endpoint(sub_a_client, kPeers[1], false);
    IpcEndpoint<ShmSpsc> sub_b_client;
    bind_shm_endpoint(sub_b_client, kPeers[2], false);
    IpcEndpoint<ShmSpsc> sub_c_client;
    bind_shm_endpoint(sub_c_client, kPeers[3], false);

    std::atomic<bool> stop_router{false};
    std::atomic<bool> stop_subs{false};

    SubCounters a, b, c;

    std::thread router_thread([&]() {
        uint64_t ts = 1;
        RouterFrame scratch;
        while (!stop_router.load(std::memory_order_relaxed)) {
            router_link.forward(scratch, ts++, kRules, std::size(kRules));
        }
    });

    std::thread sub_a_thread([&]() { drain_subscriber(sub_a_client, kTopicA, a, stop_subs); });
    std::thread sub_b_thread([&]() { drain_subscriber(sub_b_client, kTopicB, b, stop_subs); });
    std::thread sub_c_thread([&]() { drain_subscriber(sub_c_client, kTopicC, c, stop_subs); });

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRouterDeadlineMs);

    const uint16_t topics[3] = {kTopicA, kTopicB, kTopicC};
    uint64_t pub_ok = 0;
    for (int i = 0; i < kTotalFrames; ++i) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "topic_dispatch_test: deadline exceeded during publish\n";
            ++assertions_failed;
            break;
        }
        RouterFrame f;
        f.init(kSensorId);
        f.set_topic_id(topics[i % 3]);
        f.set_seq(static_cast<uint32_t>(i));
        ShmSpsc::SendParams params{.payload = f.read_only()};
        while (sensor_client.try_send(params, f.read_only()) != ShmSendResult::Ok) {
            if (std::chrono::steady_clock::now() > deadline) {
                break;
            }
            std::this_thread::yield();
        }
        ++pub_ok;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kPostBurstDrainMs));

    stop_router.store(true, std::memory_order_relaxed);
    stop_subs.store(true, std::memory_order_relaxed);
    router_thread.join();
    sub_a_thread.join();
    sub_b_thread.join();
    sub_c_thread.join();

    const ShmRouterMetrics& m = router_link.metrics();
    const uint64_t fwd     = m.forwarded.load();
    const uint64_t recv_a  = a.recv.load();
    const uint64_t recv_b  = b.recv.load();
    const uint64_t recv_c  = c.recv.load();
    const uint64_t total_recv = recv_a + recv_b + recv_c;

    std::cout << "  topic dispatch: pub=" << pub_ok
              << " fwd=" << fwd
              << " recv[a=" << recv_a << " b=" << recv_b << " c=" << recv_c << "]"
              << " wrong[a=" << a.wrong_topic.load()
              << " b=" << b.wrong_topic.load()
              << " c=" << c.wrong_topic.load() << "]\n";

    EXPECT_EQ(pub_ok, static_cast<uint64_t>(kTotalFrames));

    // Core Phase G guarantee — each subscriber saw ONLY its declared topic.
    EXPECT_EQ(a.wrong_topic.load(), 0u);
    EXPECT_EQ(b.wrong_topic.load(), 0u);
    EXPECT_EQ(c.wrong_topic.load(), 0u);

    // Each topic dispatched to exactly one destination, so the forwarded
    // count equals the total received across all three subscribers.
    EXPECT_EQ(fwd, total_recv);

    // Acceptance bar: with right-sized rings, >= 99% of each topic's frames
    // arrive at its subscriber (mirrors burst_sensor_test).
    EXPECT(recv_a * 100 >= static_cast<uint64_t>(kFramesPerTopic) * 99);
    EXPECT(recv_b * 100 >= static_cast<uint64_t>(kFramesPerTopic) * 99);
    EXPECT(recv_c * 100 >= static_cast<uint64_t>(kFramesPerTopic) * 99);

    cleanup_shm();
}

}  // namespace

int main() {
    test_per_topic_dispatch_routes_each_topic_to_its_subscriber();

    std::cout << "topic_dispatch_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
