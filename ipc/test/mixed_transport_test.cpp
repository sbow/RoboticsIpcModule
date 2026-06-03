// Phase H — mixed-transport router integration test (ADR 0014).
//
// Proves that a single MixedRouterServer process can route between peers on
// different transports in one in-process hop (no bridge), in BOTH egress
// directions: SHM ingress -> UDS egress, and UDS ingress -> SHM egress.
//
// Topology (one router, three peers, three transports of routing exercised):
//
//   peer 1  shm_sensor  (SHM ring)   -- publisher only
//   peer 2  uds_node    (UDS socket) -- subscriber AND publisher
//   peer 3  shm_sub     (SHM ring)   -- subscriber only
//
//   routes:  1 -> [2]   (SHM  -> UDS  : SHM read, UDS send)
//            2 -> [3]   (UDS  -> SHM  : UDS read, SHM send)
//
// End-to-end chain: shm_sensor publishes seq-numbered frames; the router
// forwards each to the UDS node; the node echoes them back to the router
// (now resolved as source 2); the router forwards those to the SHM
// subscriber. A frame therefore traverses SHM -> router -> UDS -> router ->
// SHM, touching both cross-transport send paths.
//
// Why a SHM peer is either source or sink (not both): each SHM peer owns one
// SPSC ring, which is single-direction. UDS is connectionless datagram, so the
// node uses one socket for both directions. This mirrors the real mixed shape
// (jetson_mixed.toml): SHM for the hot compute peers, UDS for stateful
// subscribers that also talk back.
//
// Invariants:
//   * shm_sub receives ~100% of the published frames (>= 99% acceptance bar),
//     every one stamped source == 2 (proving the UDS->SHM hop re-resolved the
//     source), with seq preserved end-to-end and in order (SHM SPSC).
//   * Router metrics roll up: forwarded counts both hops.

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/mixed_router_server.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/timestamp.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
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

constexpr const char* kSensorShm      = "/rim_mixed_sensor";
constexpr const char* kSubShm         = "/rim_mixed_sub";
constexpr const char* kRouterShm      = "/rim_mixed_router";
constexpr const char* kRouterUdsPath  = "/tmp/rim_mixed_router.sock";
constexpr const char* kNodeUdsPath    = "/tmp/rim_mixed_node.sock";

constexpr uint8_t kSensorId = 1;  // SHM publisher
constexpr uint8_t kNodeId   = 2;  // UDS subscriber + publisher
constexpr uint8_t kSubId    = 3;  // SHM subscriber

constexpr uint32_t kSlotCount  = 256;
constexpr uint32_t kMaxPayload = 64;

const PeerEntry kPeers[] = {
    {kSensorId, "shm_sensor", peer_shm(kSensorShm), kSlotCount, kMaxPayload},
    {kNodeId,   "uds_node",   peer_uds(kNodeUdsPath), 0, 0},
    {kSubId,    "shm_sub",    peer_shm(kSubShm), kSlotCount, kMaxPayload},
};

const RouterTopology kTopo = {
    .peers          = kPeers,
    .peer_count     = sizeof(kPeers) / sizeof(kPeers[0]),
    .router_listen  = peer_shm(kRouterShm),
    .has_listen_uds = true,
    .listen_uds     = peer_uds(kRouterUdsPath),
};

constexpr RouteRule kRules[] = {
    make_route(kSensorId, kNodeId),  // SHM -> UDS
    make_route(kNodeId,   kSubId),   // UDS -> SHM
};

void cleanup() {
    ::shm_unlink(kSensorShm);
    ::shm_unlink(kSubShm);
    ::shm_unlink(kRouterShm);
    ::unlink(kRouterUdsPath);
    ::unlink(kNodeUdsPath);
}

constexpr int kFrames           = 1000;
constexpr int kRouterDeadlineMs = 4000;
constexpr int kDrainMs          = 400;

void test_mixed_transport_chain() {
    cleanup();

    // Router: one process, three transports' worth of routing.
    MixedRouterServer router(kTopo);
    router.bind_router();
    EXPECT(router.has_shm());
    EXPECT(router.has_uds());
    EXPECT(!router.has_udp());

    // SHM publisher + subscriber clients join their rings (router created them).
    IpcEndpoint<ShmSpsc> sensor_client;
    bind_shm_endpoint(sensor_client, kPeers[0], false);
    IpcEndpoint<ShmSpsc> sub_client;
    bind_shm_endpoint(sub_client, kPeers[2], false);

    // UDS node: one socket bound to its own path. The router resolves its
    // source from this bound address, and delivers 1->2 frames here.
    IpcEndpoint<Uds> node;
    node.bind(Uds::BindParams{.path = kNodeUdsPath});

    volatile std::sig_atomic_t stop_router = 0;
    std::atomic<bool> stop_workers{false};
    std::atomic<uint64_t> node_relayed{0};
    std::atomic<uint64_t> sub_recv{0};
    std::atomic<uint64_t> sub_wrong_source{0};
    std::atomic<uint32_t> sub_last_seq{0};
    std::atomic<bool>     sub_order_ok{true};

    RouterRunOptions opts;
    opts.idle_sleep_us = 0;  // yield-only: lowest pickup latency for the test

    std::thread router_thread([&]() {
        router.run(kRules, std::size(kRules), router_now_ns,
                   [](uint8_t, uint8_t, const RouterFrame&) {},
                   opts, &stop_router);
    });

    // UDS node thread: drain frames the router delivers (1->2) and immediately
    // relay each one back to the router's UDS listen (publishing 2->3).
    std::thread node_thread([&]() {
        char storage[256];
        while (!stop_workers.load(std::memory_order_relaxed)) {
            Buffer buf = Buffer::writable(storage, sizeof(storage));
            Uds::RecvResult rr{};
            if (!node.try_recv(buf, rr)) {
                std::this_thread::yield();
                continue;
            }
            if (buf.size < kRouterFrameSize) {
                continue;
            }
            // Relay verbatim to the router's UDS listen. The router stamps the
            // source from our bound address (peer 2), so frame.source() is
            // overwritten on the next hop regardless of its current value.
            Buffer out = Buffer::read_only(storage, kRouterFrameSize);
            Uds::send_to(node.fd(), kRouterUdsPath, out);
            node_relayed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // SHM subscriber thread: drain peer 3's ring; verify source + order.
    std::thread sub_thread([&]() {
        char storage[256];
        bool first = true;
        uint32_t prev = 0;
        while (!stop_workers.load(std::memory_order_relaxed)) {
            Buffer buf = Buffer::writable(storage, sizeof(storage));
            ShmSpsc::RecvResult rr{};
            if (!ShmSpsc::try_recv(sub_client.handle(), buf, rr)) {
                continue;
            }
            if (buf.size < kRouterFrameSize) {
                continue;
            }
            RouterFrame frame;
            std::memcpy(frame.bytes, buf.data, kRouterFrameSize);
            if (frame.source() != kNodeId) {
                sub_wrong_source.fetch_add(1, std::memory_order_relaxed);
            }
            const uint32_t s = frame.seq();
            if (!first && s <= prev) {
                sub_order_ok.store(false, std::memory_order_relaxed);
            }
            first = false;
            prev = s;
            sub_last_seq.store(s, std::memory_order_relaxed);
            sub_recv.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Publish the burst from the SHM sensor (source 1). Retry on a full ring so
    // every seq leaves the publisher in order.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRouterDeadlineMs);
    uint64_t pub_ok = 0;
    for (int i = 0; i < kFrames; ++i) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "mixed_transport_test: publish deadline exceeded\n";
            ++assertions_failed;
            break;
        }
        RouterFrame f;
        f.init(kSensorId);
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

    std::this_thread::sleep_for(std::chrono::milliseconds(kDrainMs));
    stop_workers.store(true, std::memory_order_relaxed);
    stop_router = 1;
    router_thread.join();
    node_thread.join();
    sub_thread.join();

    const uint64_t relayed = node_relayed.load();
    const uint64_t recv    = sub_recv.load();
    const uint64_t wrong   = sub_wrong_source.load();
    const uint32_t last_s  = sub_last_seq.load();

    std::cout << "  mixed transport: pub=" << pub_ok
              << " uds_relayed=" << relayed
              << " shm_sub_recv=" << recv
              << " wrong_source=" << wrong
              << " last_seq=" << last_s
              << " forwarded=" << router.forwarded() << '\n';

    EXPECT_EQ(pub_ok, static_cast<uint64_t>(kFrames));

    // The UDS node saw essentially all SHM->UDS frames (acceptance bar 99%).
    EXPECT(relayed * 100 >= pub_ok * 99);

    // The SHM subscriber received essentially all UDS->SHM frames.
    EXPECT(recv * 100 >= pub_ok * 99);

    // Every frame the subscriber saw was re-stamped to source 2 by the
    // UDS->SHM hop (proves the router resolved the UDS sender, not the
    // original SHM source baked into the frame).
    EXPECT_EQ(wrong, 0u);

    // SHM SPSC preserves order along the final hop.
    EXPECT(sub_order_ok.load());

    // Router forwarded both hops: ~pub_ok (SHM->UDS) + ~relayed (UDS->SHM).
    EXPECT(router.forwarded() >= relayed + recv);

    cleanup();
}

}  // namespace

int main() {
    test_mixed_transport_chain();

    std::cout << "mixed_transport_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
