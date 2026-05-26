// Phase C1/C3 — verify ShmRouterLink drops frames instead of spinning when a
// peer's destination ring is full, and that metrics counters track the
// outcome accurately. Documented in docs/adr/0006-shm-backpressure-and-metrics.md.

#include "ipc.hpp"
#include "router/frame.hpp"
#include "router/metrics.hpp"
#include "router/peer_table.hpp"
#include "router/routing.hpp"
#include "router/shm_router_link.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/mman.h>

namespace {

int assertions_run = 0;
int assertions_failed = 0;

#define EXPECT(cond) do {                                                  \
    ++assertions_run;                                                      \
    if (!(cond)) {                                                         \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT failed @ " << __FILE__ << ':' << __LINE__     \
                  << " : " #cond "\n";                                    \
    }                                                                      \
} while (0)

#define EXPECT_EQ(a, b) do {                                               \
    ++assertions_run;                                                      \
    if ((a) != (b)) {                                                      \
        ++assertions_failed;                                               \
        std::cerr << "EXPECT_EQ failed @ " << __FILE__ << ':' << __LINE__  \
                  << " : " #a " == " #b " (" << (a) << " vs " << (b)       \
                  << ")\n";                                                \
    }                                                                      \
} while (0)

constexpr const char* kRouterListenName = "/cpp_tricks_router_bp_test";
constexpr const char* kPeerAShm = "/cpp_tricks_router_bp_test_A";
constexpr const char* kPeerBShm = "/cpp_tricks_router_bp_test_B";

constexpr PeerEntry kPeers[] = {
    {1, "A", peer_shm(kPeerAShm)},
    {2, "B", peer_shm(kPeerBShm)},
};

constexpr RouterTopology kTopo = {
    .peers = kPeers,
    .peer_count = sizeof(kPeers) / sizeof(kPeers[0]),
    .router_listen = peer_shm(kRouterListenName),
};

constexpr RouteRule kRules[] = {
    {1, 2, 0},   // A -> B
};

void cleanup_shm() {
    ::shm_unlink(kRouterListenName);
    ::shm_unlink(kPeerAShm);
    ::shm_unlink(kPeerBShm);
}

uint64_t fake_clock = 1;
uint64_t fake_now_ns() { return fake_clock++; }

// Push one frame into peer A's request ring (simulating peer A publishing).
// Uses the same SHM region as the router server, but mapped as a client view.
void publish_frame_as_peer_a(IpcEndpoint<ShmSpsc>& peer_a_client,
                              uint8_t source_id,
                              const std::string& payload) {
    RouterFrame frame;
    frame.init(source_id);
    frame.set_payload(payload);
    ShmSpsc::SendParams params{.payload = frame.read_only()};
    const ShmSendResult r = peer_a_client.try_send(params, frame.read_only());
    EXPECT(r == ShmSendResult::Ok);
}

// Try to push one frame into peer A's request ring; return whether it landed.
bool try_publish_frame_as_peer_a(IpcEndpoint<ShmSpsc>& peer_a_client,
                                  uint8_t source_id,
                                  const std::string& payload) {
    RouterFrame frame;
    frame.init(source_id);
    frame.set_payload(payload);
    ShmSpsc::SendParams params{.payload = frame.read_only()};
    return peer_a_client.try_send(params, frame.read_only()) == ShmSendResult::Ok;
}

void test_normal_forward_increments_metric() {
    cleanup_shm();

    auto link = ShmRouterLink::server(kTopo);
    link.bind_router({});

    IpcEndpoint<ShmSpsc> peer_a_client;
    peer_a_client.bind(ShmSpsc::BindParams{.name = kPeerAShm, .create = false});

    IpcEndpoint<ShmSpsc> peer_b_client;
    peer_b_client.bind(ShmSpsc::BindParams{.name = kPeerBShm, .create = false});

    const ShmRouterMetrics& m = link.metrics();
    EXPECT_EQ(m.forwarded.load(), 0u);
    EXPECT_EQ(m.dropped_full.load(), 0u);
    EXPECT_EQ(m.recv_empty.load(), 0u);

    publish_frame_as_peer_a(peer_a_client, 1, "hello");

    RouterFrame scratch;
    const ForwardResult fwd = link.forward(scratch, fake_now_ns(),
                                            kRules, std::size(kRules));
    EXPECT(static_cast<bool>(fwd));
    EXPECT_EQ(fwd.source, static_cast<uint8_t>(1));
    EXPECT_EQ(fwd.targets.count, static_cast<size_t>(1));
    EXPECT_EQ(fwd.targets.ids[0], static_cast<uint8_t>(2));

    EXPECT_EQ(m.forwarded.load(), 1u);
    EXPECT_EQ(m.dropped_full.load(), 0u);

    char storage[1024];
    Buffer buf = Buffer::writable(storage, sizeof(storage));
    ShmSpsc::RecvResult rr{};
    EXPECT(ShmSpsc::try_recv(peer_b_client.handle(), buf, rr));
    EXPECT(buf.size >= kRouterFrameSize);

    cleanup_shm();
}

void test_empty_forward_increments_recv_empty() {
    cleanup_shm();
    auto link = ShmRouterLink::server(kTopo);
    link.bind_router({});

    RouterFrame scratch;
    for (int i = 0; i < 7; ++i) {
        const ForwardResult fwd = link.forward(scratch, fake_now_ns(),
                                                kRules, std::size(kRules));
        EXPECT(!static_cast<bool>(fwd));
    }
    EXPECT_EQ(link.metrics().recv_empty.load(), 7u);
    EXPECT_EQ(link.metrics().forwarded.load(), 0u);
    EXPECT_EQ(link.metrics().dropped_full.load(), 0u);

    cleanup_shm();
}

void test_full_ring_drops_and_no_spin() {
    cleanup_shm();
    auto link = ShmRouterLink::server(kTopo);
    link.bind_router({});

    IpcEndpoint<ShmSpsc> peer_a_client;
    peer_a_client.bind(ShmSpsc::BindParams{.name = kPeerAShm, .create = false});

    // Intentionally do NOT bind a peer-B client; B's reply ring (which is
    // where the router writes) has no consumer.
    const ShmRouterMetrics& m = link.metrics();

    // Pump frames through the router until many drops have accumulated. We
    // start each iteration by trying to publish one A frame, then forwarding.
    // The default slot_count is 256, so after ~256 forwards B will be full
    // and every subsequent forward will register a dropped_full.
    constexpr int kIterations = 1024;
    RouterFrame scratch;
    int published = 0;
    int forwarded_calls = 0;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);

    for (int i = 0; i < kIterations; ++i) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "shm_backpressure_test: deadline exceeded (spin?)\n";
            ++assertions_failed;
            break;
        }

        // Peer A's req ring (router consumes) eventually fills if forwarding
        // doesn't keep up — but it will, because we forward each iteration.
        if (try_publish_frame_as_peer_a(peer_a_client, 1, "drop-me")) {
            ++published;
        }

        // forward() must return promptly (no infinite spin) even when B is
        // full. The deadline above catches a regression.
        link.forward(scratch, fake_now_ns(), kRules, std::size(kRules));
        ++forwarded_calls;
    }

    EXPECT(published > 0);
    EXPECT(forwarded_calls == kIterations);

    const uint64_t fwd = m.forwarded.load();
    const uint64_t drop = m.dropped_full.load();

    // Sanity: combined accounting equals attempts on the forward-side
    // (each successful forward of A's frame results in exactly one
    // try_send to B; either Ok -> forwarded, or Full -> dropped_full).
    EXPECT_EQ(fwd + drop, static_cast<uint64_t>(published));

    // The default ring holds 256 slots, so we should have at least 256
    // successful forwards before B saturates.
    EXPECT(fwd >= 200);
    EXPECT(drop > 0);  // proves drop-on-full actually happened

    std::cout << "  full-ring scenario: published=" << published
              << " forwarded=" << fwd
              << " dropped_full=" << drop << '\n';

    cleanup_shm();
}

}  // namespace

int main() {
    test_normal_forward_increments_metric();
    test_empty_forward_increments_recv_empty();
    test_full_ring_drops_and_no_spin();

    std::cout << "shm_backpressure_test: " << (assertions_run - assertions_failed)
              << '/' << assertions_run << " assertions passed\n";

    return assertions_failed == 0 ? 0 : 1;
}
