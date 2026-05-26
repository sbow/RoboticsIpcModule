# Planned: `ShmSpsc` transport (spin → optional eventfd idle)

Design note for a third IPC transport alongside `Udp` and `Uds` in `ipc.h`.
Roll our own first; add kernel wake only for the idle path.

## Goal

Lower same-machine round-trip latency vs UDS/UDP by avoiding per-message syscalls.
Keep the existing layering:

```
Transport (ShmSpsc)  →  syscall-free hot path when both sides are busy
       ↑
Client / Server    →  reusable endpoints (seed for larger project)
       ↑
EchoClient / EchoServer
```

Benchmark alongside `make test-ipc` / `make test-ipc-mp` (UDP, UDS, then SHM).

## Phase 1 — SPSC ring, spin only

**Model:** single-producer / single-consumer ring in shared memory. Client and server
are separate processes (or threads for in-process test); each maps the same region.

### Layout (sketch)

```text
[ ShmRegion ]
  head          (atomic<uint64_t>, cache-line aligned, producer writes)
  tail          (atomic<uint64_t>, cache-line aligned, consumer writes)
  slots[N]      fixed-size records: { uint32_t len; char data[MAX]; }
```

- **Producer:** claim slot at `head`, write payload, `release` store on `head`.
- **Consumer:** if `tail < head`, read slot, `release` store on `tail`.
- **Echo server:** recv slot from client→server ring, copy/write reply to server→client ring.

Two rings for bidirectional echo (or one ring + reverse roles — document choice when implementing).

### `ShmSpsc` transport trait (match `Udp` / `Uds`)

Same static surface as existing transports so `Client<ShmSpsc>` / `Server<ShmSpsc>` work unchanged:

| Method | Phase 1 behavior |
|--------|------------------|
| `bind(fd?, BindParams)` | Create/open SHM (`shm_open` or `memfd_create`), `mmap`, init atomics |
| `connect_or_send` | Push to outbound ring; **spin** until slot available if full |
| `recv_from` | Pop from inbound ring; **spin** while empty |
| `echo` | Push received buffer to reply ring |
| `kCfg` | Placeholder or empty — no socket fd; may use `int` shm fd only for setup |

Open design: `Client`/`Server` today assume a `Socket` fd. Options:

1. **Parallel base** — `ShmClient`/`ShmServer` hold `ShmRegion*` instead of `Socket` (minimal change to echo tests via type alias).
2. **Generalize endpoint** — optional `Socket` or SHM handle (more abstraction).
3. **Dummy fd** — memfd only for lifecycle; hot path ignores it (hacky, avoid if possible).

Prefer (1) or a thin `ShmEndpoint` with same method names as `Client`/`Server`.

### BindParams / SendParams (draft)

```cpp
struct BindParams {
    const char* name;       // e.g. "/cpp_tricks_shm_echo"
    size_t slot_count;    // power of 2
    size_t max_payload;   // e.g. 256
};

struct SendParams {
    Buffer payload;
};

struct RecvResult {
    // from address meaningless for SHM; keep empty or slot index for debug
};
```

### Phase 1 pros / accepted cons

| Pros | Cons |
|------|------|
| Lowest latency when both sides hot | Burns CPU while waiting (spin) |
| No kernel on hot path | SPSC only; MPMC is out of scope |
| Fits echo benchmark | Crash mid-message leaves ring inconsistent — document limitation |
| Teaching value (atomics, memory order) | No standard library; Linux-specific SHM APIs |

### Memory ordering

- Producer: write slot data → `head.store(old+1, memory_order_release)`
- Consumer: `head.load(memory_order_acquire)` before read
- Separate cache lines for `head` and `tail` (false sharing)

### Tests

1. In-process: two threads, one region (fast sanity).
2. Two-process: extend `echo_server` / `echo_client` with `shm` transport arg.
3. Add `run_shm_benchmark()` to `echo_tests.cpp` or `make test-ipc-shm`.

## Phase 2 — optional eventfd idle mode

Add **idle path only**; do not syscall on every message when the consumer is already busy.

### Pattern (hybrid)

```text
Hot path:  drain ring while slots available (atomics only, same as Phase 1)
Idle path: if ring empty after re-check → block on eventfd/epoll
Signal:    producer increments eventfd (write 8) after publishing, if consumer may be sleeping
```

### API sketch

```cpp
struct ShmSpscConfig {
    bool spin_only = true;   // Phase 1 default
    int idle_timeout_ms = 0; // 0 = block forever on eventfd
};
```

Or `BindParams { ..., WaitMode { Spin, EventFd } }`.

### Signaling protocol (avoid lost wakeup)

1. Consumer finds ring empty.
2. Consumer re-checks ring (acquire load on `head`).
3. If still empty: arm wait (read eventfd / prepare futex wait).
4. Re-check ring again before sleeping.
5. Producer: write message → release `head` → **then** signal eventfd.

Reference: Linux futex `FUTEX_WAIT` / `FUTEX_WAKE` on a word in the same SHM region is an alternative to eventfd; eventfd is easier to combine with `epoll` later.

### Phase 2 pros / cons

| Pros | Cons |
|------|------|
| Low CPU when idle | Wake latency (µs+) after sleep |
| Still syscall-free when busy | More races to test |
| epoll-friendly (eventfd) | Not in Phase 1 scope |

## Comparison targets (expected)

| Transport | Expectation |
|-----------|-------------|
| UDP loopback | Baseline (slowest of socket pair) |
| UDS datagram | Slightly better than UDP |
| ShmSpsc spin | Much higher trips/sec in echo benchmark |
| ShmSpsc + eventfd | Between UDS and spin when bursty; near spin when saturated |

## References

- **Pattern:** DPDK `rte_ring` (spin, C), hand-rolled HFT rings + futex (C)
- **Productized C++:** Eclipse iceoryx (SHM pub/sub, zero-copy) — study, don’t depend on for v1
- **Building blocks:** `shm_open`, `memfd_create`, `mmap`, C++ atomics, `eventfd(2)`
- **Prior art in repo:** `Udp`, `Uds` in `ipc.h`; echo tests in `cpp_tricks/ipc/test/`

## Implementation checklist

- [ ] Phase 1: `ShmRegion` + SPSC ring (single direction)
- [ ] Phase 1: `ShmSpsc` trait + echo over two rings
- [ ] Phase 1: integrate with `EchoClient` / `EchoServer` (or parallel client/server)
- [ ] Phase 1: `make test-ipc` / `test-ipc-mp` style benchmark
- [ ] Phase 2: `eventfd` per direction (or shared) + `WaitMode`
- [ ] Phase 2: lost-wakeup tests (pause consumer, send, verify wake)
- [ ] README + features table update when Phase 1 lands
