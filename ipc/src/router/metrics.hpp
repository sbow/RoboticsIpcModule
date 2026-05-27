#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Phase C3 / D2a / D4 — router link metrics.
//
// Lock-free atomic counters maintained on the router hot path. No heap
// allocation per frame; each metrics block is heap-allocated once per
// link (so the link object stays movable — std::atomic is non-movable).
//
// Counters monotonically increase for the life of the link and are intended
// to be sampled by an observer thread (status server, log line, future
// prometheus bridge). All loads use std::memory_order_relaxed because the
// publish order does not matter for monitoring.
//
// Two metrics structs ship today:
//   * ShmRouterMetrics      — SHM router link (Phase C3, extended in D2a)
//   * DatagramRouterMetrics — UDP / UDS router link (Phase D4)
// They share the field semantics defined below where both apply; differences
// are called out per struct.

constexpr std::size_t kPerPeerMetricsSlots = 256;  // peer id is uint8_t (0..255)

// Field semantics (SHM router server):
//   forwarded             — count of (source, dest) frame copies successfully
//                           placed in a peer ring (one increment per
//                           destination per frame).
//   dropped_full          — destination ring was full at try_send time; frame
//                           copy was dropped for that destination (ADR 0006).
//                           Aggregate across every destination peer.
//   dropped_full_per_peer — Phase D2a: parallel per-destination breakdown of
//                           `dropped_full`, indexed by peer id (0..255). Lets
//                           operators answer "which subscriber is the slow
//                           one?" without re-deriving from logs. Aggregate
//                           counter is preserved for backwards compatibility
//                           with Phase C consumers.
//                           Indexing convention:
//                             [0]   — kEndpointInvalid; never incremented
//                             [255] — kEndpointServer; never incremented
//                             [N]   — destination peer id N (1..254)
//   recv_empty            — forward() iteration where no peer channel had a
//                           frame available; increments on each empty poll.
//   recv_truncated        — peer published a buffer smaller than
//                           kRouterFrameSize; frame discarded.
struct ShmRouterMetrics {
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> dropped_full{0};
    std::atomic<uint64_t> recv_empty{0};
    std::atomic<uint64_t> recv_truncated{0};

    // Phase D2a — per-destination drop attribution. ~2 KiB per metrics block
    // (256 × 8 B); allocated once per ShmRouterLink. Atomics are non-movable
    // and non-copyable; std::array<std::atomic<T>, N> is value-initialized to
    // zero counters and stays in place for the life of the link.
    std::array<std::atomic<uint64_t>, kPerPeerMetricsSlots> dropped_full_per_peer{};
};

// Phase D4 — datagram (UDP / UDS) router link metrics.
//
// Datagrams have no SHM ring, so the dropped_full family does not apply
// (kernel-side drops are observable through SO_RXQ_OVFL / SO_RCVBUF stats,
// which a future phase may surface). The fields below cover the per-frame
// fault paths a router can detect from within recvfrom():
//
//   forwarded            — count of (source, dest) frame copies sent to a
//                          destination peer via sendto(). One increment per
//                          destination per frame.
//   recv_truncated       — peer sent a datagram smaller than kRouterFrameSize.
//                          Frame discarded; nothing forwarded. Catches buggy
//                          clients and partial sends on unreliable links.
//   recv_unknown_source  — datagram arrived from a (host, port) or socket
//                          path that does not match any peer in the topology.
//                          Frame discarded. Catches misconfigured peers and
//                          spoofed traffic at the router boundary.
//   recv_empty           — reserved for parity with ShmRouterMetrics. The
//                          datagram link's recv() currently throws on
//                          SO_RCVTIMEO so this counter is not yet driven
//                          (the RouterServer outer loop catches the
//                          exception); kept here so monitoring code can read
//                          a uniform field set across link types.
struct DatagramRouterMetrics {
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> recv_truncated{0};
    std::atomic<uint64_t> recv_unknown_source{0};
    std::atomic<uint64_t> recv_empty{0};
};
