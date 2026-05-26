#pragma once

#include <atomic>
#include <cstdint>

// Phase C3 — router link metrics.
//
// Lock-free atomic counters maintained on the router hot path. No heap
// allocation per frame; the metrics block itself is heap-allocated once per
// link (so the link object stays movable — std::atomic is non-movable).
//
// Counters monotonically increase for the life of the link and are intended
// to be sampled by an observer thread (status server, log line, future
// prometheus bridge). All loads use std::memory_order_relaxed because the
// publish order does not matter for monitoring.
//
// Field semantics (SHM router server):
//   forwarded       — count of (source, dest) frame copies successfully placed
//                     in a peer ring (one increment per destination per frame).
//   dropped_full    — destination ring was full at try_send time; frame copy
//                     was dropped for that destination (ADR 0006).
//   recv_empty      — forward() iteration where no peer channel had a frame
//                     available; increments on each empty poll.
//   recv_truncated  — peer published a buffer smaller than kRouterFrameSize;
//                     frame discarded.
//
// Datagram links may populate a subset (forwarded / recv_empty); dropped_full
// is SHM-specific until eventfd / per-peer rings exist on UDP/UDS.
struct ShmRouterMetrics {
    std::atomic<uint64_t> forwarded{0};
    std::atomic<uint64_t> dropped_full{0};
    std::atomic<uint64_t> recv_empty{0};
    std::atomic<uint64_t> recv_truncated{0};
};
