# Lessons learned — From cpp_tricks IPC/router development

Concrete failures and fixes from this codebase. **Agents:** grep for these anti-patterns before marking a phase complete.

---

## Architecture & refactor

| Lesson | Detail | Action |
|--------|--------|--------|
| Monolithic headers do not scale | `router_protocol.h` mixed framing, routing, UDS branches | Keep layers in ADR 0002 layout; umbrella includes only |
| Registry with embedded ports/paths | Old `EndpointRegistry` blocked SHM/DDS | Use `RouterTopology` + `PeerAddress` union |
| Duplicate `RouterPeers<Uds/Udp>` | Copy-paste forwarding | Single `DatagramRouterLink<Transport>` + adapters |
| Templates at app boundary | Role code repeated `template<typename Transport>` | `dispatch_transport_kind` + factories only |

---

## SHM router

| Lesson | Detail | Action |
|--------|--------|--------|
| One ring per peer | Shared single ring broke identity | Router creates N rings; client joins one; source = channel id |
| `router_listen` SHM name unused | Listen address in topology is validation only | Document in `shm_peer_address_io.hpp`; do not `shm_open` listen name unless ADR changes model |
| `if constexpr` needs `else` | `router_client` instantiated datagram path for `ShmSpsc` | Always `else` branch so wrong template is not compiled |
| SHM idle-exit vs datagram | `RouterServer::run` only idle-exited on **exceptions**; SHM `forward()` returns `{}` | Idle check on empty `forward()` + `yield` (see `node.hpp`) |
| SHM recv tight loop | `recv_message_until` spun at 100% CPU | `yield` each iteration; recorder loop too |
| Full ring = infinite spin | `shm_push_slot` blocked forever in router send path | **Fixed Phase C1:** `shm_try_push_slot` + `ShmSpsc::try_send`; `ShmRouterLink::send_to_peer` drops + `dropped_full` metric (ADR 0006). Client→router publish is still blocking — separate ADR. |
| `yield()` is not "sleep" | Baseline router idle CPU was **100% of one core** sustained — `std::this_thread::yield()` only reorders ready threads, doesn't deschedule on an idle box | **Fixed Phase C2:** `RouterRunOptions::idle_sleep_us` (default 1 ms) — idle CPU drops to ~1.6% one core. `eventfd` deferred to Phase F (ADR 0007). Measure CPU *before* claiming an idle path works. |

---

## Testing & demo apps

| Lesson | Detail | Action |
|--------|--------|--------|
| Wrong `argv` for log paths | Controller used `argv[4]` (port) instead of `argv[5]` (log) | **Fixed Phase D1:** `log_path_for_role` extracted to `ipc/test/router_cli_args.hpp` with callback-based fallback; `cli_args_test` (13 assertions) locks the arity matrix per the table at the bottom of this file. |
| Recorder log on socket path | `argc >= 3` took `argv[2]` for UDS recorder | UDS/UDP recorder log at `argv[4]` when `argc >= 5`; SHM at `argv[3]` when `argc >= 4` |
| Log file not cleared between scenarios | SHM test counted 10 lines (2×) | `unlink` recorder/controller logs at **every** `run_scenario` start |
| Integration test thresholds only | `router_test` counts CSV lines, no latency | Phase D: add unit tests + optional timing |
| `ROUTER_TEST=1` behavior | Idle-exit changes router/recorder lifetime | Never enable in production systemd units |

---

## Robustness & ops

| Lesson | Detail | Action |
|--------|--------|--------|
| Blocking `recv` blocks SIGTERM | Long-running servers need poll | 200 ms `SO_RCVTIMEO` or `try_recv` + stop flag |
| `SIGKILL` skips destructors | Stale UDS/SHM until cleanup | Test `cleanup_*`; document in MODULE.md |
| Stop order matters less for SHM | Router creator unlinks on exit; stop router after clients in tests | Keep test `stop_all(recorder, controller, router)` |
| Silent `send_to_peer` miss | SHM dropped forwards to unknown dest | Throw on unknown dest (still); on **known dest with full ring**, drop + `dropped_full++` (Phase C, ADR 0006) — never silent |
| `std::atomic` member breaks `= default` move | Adding a `std::atomic<uint64_t>` to a class deletes its move ctor (atomics are non-movable). Pattern affected `ShmRouterMetrics` integration into `ShmRouterLink`. | Use `std::unique_ptr<MetricsBlock>` member. Metrics block stays heap-allocated; link is movable; `metrics()` returns `const&` that remains valid for link lifetime (ADR 0006). |
| 9-byte timestamp field in v1 frame | v1 used 9 B for `timestamp_ns` despite `uint64_t` covering 584 years in 8 B; the extra byte was wasted and forced a manual `for` loop in `set_timestamp_ns`/`timestamp_ns` (no native 9 B type). | **Fixed in v2:** 8 B `uint64_t` timestamp, host byte order, accessed via `std::memcpy`. Pick natural integer widths (1/2/4/8) — anything else costs you accessor complexity and screams "bug" to readers. ADR 0008. |
| Cache-line alignment is structural | v1 was 32 B (half a 64-B line); going to v2 64 B is "twice as big" by header but identical in cache lines touched per forward. Meanwhile the SHM slot stride was 1024 B regardless of frame size — the dominant cache effect was the slot, not the frame. | When sizing a header, think in **cache lines on the platforms of intent**, not bytes. 64 B is the universal x86_64 / aarch64 line. Pre-measure ring-slot stride before optimizing frame size. ADR 0008. |
| Big-endian "for forward compatibility" pays now, helps never | v1's big-endian timestamp was paid every forward by a manual shift-and-OR loop, for a hypothetical big-endian deployment that never came (and never will under ADR 0004's scope). | v2 uses host (little-endian) byte order and a `static_assert(std::endian::native == std::endian::little)`. Pay the byte-swap cost only when a real big-endian port motivates it. |
| Wire-format break? Just rip the band-aid | Maintaining v1 and v2 in parallel would have required version tag bytes, branch logic in every accessor, and a forever-shim. The project has zero external production consumers today. | **Pin the version per deployment, rebuild all peers atomically, preserve historical layout in the migration ADR.** Wait-for-real-pressure on multi-version coexistence; don't pre-pay it. ADR 0008. |
| Right-size the ring, not just the frame | The SHM slot stride dominated cache effects, not the 64 B frame inside it. Going from `max_payload = 1024` to `max_payload = 64` shrank per-peer ring memory by ~15× and made router-frame rings cache-resident on Jetson. The frame size discussion (ADR 0008) was the visible knob; the ring sizing was the *actual* cache lever. | When measuring "how big is this header?", also measure "how big is its container?". For SHM SPSC: `shm_region_size(slot_count, max_payload) = 64 + 2 × slot_count × (4 + max_payload)`. Make the slot stride match the frame and put the bulk in sideband. ADR 0009. |
| `PeerEntry` extension without source breakage | Adding `shm_slot_count` / `shm_max_payload` to `PeerEntry` could have broken every aggregate initializer (`{id, "name", peer_shm(...)}`) in compile-time topologies. | C++20 aggregate initialization fills trailing members from in-class default initializers — `uint32_t shm_slot_count = 0;` lets old call sites compile unchanged and gives bind helpers a sentinel for "use platform defaults." Always trailing fields + in-class defaults when extending aggregate structs. ADR 0009. |
| Loader errors should be frame-aware | A vanilla "value out of range" message wouldn't have helped an operator who set `shm_max_payload = 32` after Phase C. The right error message names the constraint: `"shm_max_payload 32 < kRouterFrameSize (64): cannot hold a RouterFrame"`. | Validation messages should reference the *purpose* of the constraint, not just the numeric bound. Cheap to write at load time; saves a debugging session. |
| Modular arithmetic over uint32 *is* the wrap detector | A subscriber-side gap detector for `RouterFrame::seq()` looks like it needs a "wrap flag" plus comparison logic. It doesn't: `uint32_t delta = seq - last_seq;` does the right thing for free, because unsigned subtraction wraps mod 2³². A `delta` in the lower half (`< 2³¹`) is forward; in the upper half it's backward (stale). | `SourceSeqTracker` (D1) leans on this directly. When a sequence number is monotonic mod-2^N, prefer **unsigned subtraction + window classification** over "did we just wrap?" booleans. ADR 0008 + D1. |
| Self-routing has no legitimate use | A topology rule `{source: 1, dest: [1]}` (or `{source: 1, dest: [2, 1]}`) would have the router send a peer's own publications back to itself — almost certainly a profile typo, but the loader happily accepted it until D1. | **Loader rejects self-routing at parse time** with a frame-aware message. Validate constraints that have *no benign interpretation* up front; you'll always be paying the debugging cost later otherwise. |
| Demo helpers that ship in the test directory still deserve tests | `log_path_for_role` was a 22-line free function in the `router_client.cpp` anonymous namespace — unreachable from a unit test, only exercised by the integration scenarios. The original argv bug shipped through that gap. | Phase D1 moved it to `ipc/test/router_cli_args.hpp` (callback-based fallback so the helper has no demo dependency) and added `cli_args_test`. Any function with a non-trivial branch matrix should live in a header that a unit test can include — even if it's demo-shaped. |
| `forward()` polls peer channels in topology order and returns on the first hit | When two peers both publish into the router simultaneously, `ShmRouterLink::forward()` consumes from the *first* peer that has data and returns. The second peer is only serviced if the first is idle. A test that pushed from both sides on every iteration starved one direction and asserted the wrong invariant. | This is intentional (single-thread, single-pass forwarding keeps the hot path branch-predictable) but it shapes how integration tests must be written: don't assume round-robin across sources. The Phase D2a per-peer attribution test was rewritten to use **one source / two destinations** instead of two sources — the destination side is where attribution matters anyway. |
| `std::atomic<T>` default value initialization is C++20 | A `std::array<std::atomic<uint64_t>, N>` value-initialized via `{}` only zeros every element on C++20+ — under C++17 each atomic was left uninitialized (UB on first load). Worth checking the compile flag any time you add an atomic-of-arrays member. | Phase D2a relies on `-std=c++20` (which the Makefile already enforces) for `dropped_full_per_peer{}` to start at zero without manual fill. Aggregate-init of atomic arrays is now the supported pattern. |
| Network sandboxes break UDP smoke tests but leave SHM/UDS alone | The Cursor agent sandbox blocks `socket(AF_INET, ...)` even on loopback, so `profile_switch_test`'s UDP round and any other AF_INET-touching test must run via the `command allowlist` (outside the sandbox). SHM and UDS tests are unaffected — UDS goes through `AF_UNIX`. | Document the sandbox boundary clearly so future tests can be designed around it. CI on a real Linux runner has no such restriction. |
| The "drop" gate isn't "is dropped_full > 0?" — it's "which peer?" | The Phase C global `dropped_full` counter tells you the fabric is shedding load but not where the slowness lives. Without per-peer attribution the slow-recorder test would have had to grep router logs to make its assertion. | **Phase D2a**: `dropped_full_per_peer[256]` makes the assertion a single `m.dropped_full_per_peer[recorder_id].load()` call. Per-peer counters sum to the aggregate (verified). Carry this discipline into Phase E datagram metrics — global is necessary but never sufficient. |
| `IFS=$'\n\t'` + space-separated env var = silent expansion | `idle_cpu_check.sh` and `latency_histogram.sh` set `IFS=$'\n\t'` for safety, then iterated over `for t in $TRANSPORTS` where `TRANSPORTS="shm uds udp"`. The loop ran exactly once over the whole string and recorded zero counts per transport. The script reported "(no data)" but exit code 0 — silent miscount. | When mixing strict IFS with space-separated input, split into an array up front: `read -r -a TRANSPORT_LIST <<<"$TRANSPORTS"` and iterate `"${TRANSPORT_LIST[@]}"`. Pattern applies to any future D3 / E script that takes a space-separated knob. |
| Wait for SHM bind, not for the PID, before sampling idle CPU | `idle_cpu_check.sh` originally started `router_server` and immediately called `pidstat`. The first sample window caught the bind + topology-load CPU spike and pulled the average above the 5 % floor on slow hosts. | Use the SHM region as the readiness signal: poll `[[ -e /dev/shm/cpp_tricks_router_sensor ]]` with a 3 s deadline, *then* open the measurement window. The router only `shm_open`s its peer rings after the topology has loaded and all `bind_*` calls have succeeded, so the file's existence is a clean monotonic edge from "starting" to "idle." |
| `pidstat` column layout drifts between sysstat versions | A naïve `awk '/Average/ {print $8}'` parser breaks the moment sysstat reorders columns or adds a guest-CPU field. The 12.6.x release on Ubuntu 24 and the 11.x release on older Jetson images differ on whether `%guest` ships, which shifts the `%CPU` column index. | Discover the column dynamically by header: `awk '/%CPU/ && /^[# ]*Time/ { for (i=1; i<=NF; ++i) if ($i=="%CPU") cpu_col=i } /^Average:/ { print $cpu_col }'`. Same pattern works for any pidstat / mpstat / sar table where the field set is version-dependent. |
| Leak detection needs an honest baseline | `shm_leak_check.sh` first version snapshotted the pre-run resource count *as-is* and then asserted `after == before`. If a prior interrupted run had left two `/dev/shm/cpp_tricks_*` files behind, "delta == 0" was trivially satisfied even when the current run leaked a new one — the leftover masked the new leak. | Warn the operator about pre-existing resources, **clean them**, *then* snapshot the (now-zero) baseline. Only the just-finished run is what the gate is actually measuring. Apply to any "diff before / after" gate (`/dev/shm`, `/tmp`, FDs, threads). |
| `pgrep -f` matches anywhere in the command line, including sandbox wrappers | `pgrep -af router_server` returned both real routers and `cursorsandbox` host wrappers whose argv contained the substring `router_server --config ...`. The output looked alarming ("we have orphaned routers!") but the processes were the parent's sandbox launcher, owned by a different UID and unkillable from inside the sandbox. | Use `pgrep -x` (exact-name match against the executable basename) when you want strictly *the* router process, not anything whose argv happens to mention it. `pgrep -f` is the right tool only when you specifically need the wrapper / pipeline view. |
| Silent drop paths are invisible drop paths | `DatagramRouterLink::forward()` had two `return {}` branches (truncated frame, unknown source) that dropped the datagram without any observable side effect. Operators could only diagnose them by tcpdump'ing the router host. | Phase D4 added `DatagramRouterMetrics` (mirrors the `ShmRouterMetrics` pattern: heap-owned via `unique_ptr` so the link stays movable, `metrics()` accessor returns `const&` valid for the link's lifetime). Every drop-by-design path now bumps a counter. Rule: if `forward()` returns `{}` without forwarding, something incrementable should happen first. |
| The router can't distinguish "misconfigured peer" from "spoofed traffic" — and that's fine | When a UDP datagram arrives from a `(host, port)` not in the topology, the router *could* try to authenticate, log a warning, or correlate against a recent peer registry. None of these belong in the hot path. | `recv_unknown_source` increments and the frame is dropped. **The boundary between fabric and policy is the metric.** A monitoring layer (Phase F bridges) can correlate the counter against access logs to decide whether the increment is a missing peer entry or active spoofing; the router itself has no opinion. Same logic applies to `recv_truncated` — a buggy client and a hostile sender are indistinguishable from one syscall. |
| `waitpid(pid, nullptr, 0)` is unbounded even after SIGKILL | An indefinite blocking wait is fine when you trust the kernel will reap your child after SIGKILL — but if signal delivery is somehow blocked (sandbox quirks, namespace boundaries, your own bug in the signaling path), the wait blocks forever and CI hangs. `shm_leak_check.sh` exposed this by running `fault_injection_test` inside the cursor sandbox: parent → child signal delivery was flaky and the test process sat in `do_wait` indefinitely. | Wrap subprocess cleanup in a bounded helper that escalates SIGTERM → SIGKILL → polled `WNOHANG` on a single deadline (`reap_bounded()` in `fault_injection_test`). After the deadline, *give up gracefully* — log it, but never block. Hung test = hung CI = hung leak check = lost trust in the gate. |
| Sandboxes can permit `socket(AF_INET, ...)` but deny `bind()` | The cursor agent sandbox sometimes lets `socket()` succeed and then refuses `bind()` at the next syscall — so a "skip if socket fails" path isn't enough on its own. The next syscall will throw and, if uncaught, terminate the test process via `std::terminate`. | Wrap both `socket()` *and* `bind_router()` (and any other early-network calls) in their own try/catch + `[skip]` paths. Treat the sandbox as a layered restriction system: every layer might fail independently. Same advice applies to `shm_open` / `ftruncate` / `mmap` if a future test hits the SHM path in a tighter sandbox. |
| Lingering "router_server" cmdline strings outlast their processes | After interrupting tests, `pgrep -a router_server` showed PIDs that no longer existed as real router processes — they were cursorsandbox host wrappers (and their /proc entries) whose argv still contained `--config config/profiles/jetson_prod.toml`. `kill -9` on them failed with EPERM because they were owned by a different UID / sandbox session. | Don't rely on `pgrep` matching to clean up after a test; rely on test-owned cleanup (`shm_unlink_all()` in `EXIT` traps, `reap_bounded()` for spawned children). When `pgrep -a` shows surprises, check `/proc/<pid>/comm` (basename, not argv) to distinguish real processes from wrappers. |
| Topology load-time validation is cheaper than bind-time failure | A profile with `shm_max_payload = 32` would have made `ShmSpsc::bind` fail at the `ftruncate` step — a clear runtime error, but only visible *after* the router has already started, opened FDs, taken locks. ADR 0009 moved this check to load time so a misconfigured profile fails *before* any side effect is taken on disk. | `fault_injection_test` Scenario 5 cross-references topology_loader_test to make this visible at the D4 level: "a profile that would later crash bind_router is rejected at the boundary." The principle generalises — every validation you can do at parse time is a fault you don't pay for at runtime. |
| Same-UID kill isn't enough across sandbox / session boundaries — `sudo pkill -KILL -f` is the documented escape hatch | When an orphan `router_server` survives a hung test (D4 mid-traffic SIGKILL hang inside the sandbox), `kill -9 <pid>` from *any* shell — including one with the agent harness's "all" permission — failed with **EPERM**, even though the process was owned by the same UID (1000 / shaun) shown in `/proc/<pid>/status`. The block is at the session / cgroup / pid-namespace layer the sandbox installs, not at the UID layer Linux normally consults. The processes are reachable from outside the sandbox's session but not from inside it. | **Recovery recipe (record this — you will need it again):** `sudo pkill -KILL -f "build/ipc/test/router_server"` followed by `rm -f /dev/shm/cpp_tricks_* /tmp/cpp_tricks_*.sock`. Anchor `-f` on the *binary path* (not just `router_server`) so you don't sweep up unrelated sandbox-wrapper processes whose argv happens to mention the name. Mirror lives in `robotics-ipc-module/scripts/README.md` under "Manual cleanup / recovery." If the agent harness keeps hitting this, lifting the test's reaping to a per-shell `trap EXIT` or a wrapper script would prevent the orphans from being created in the first place — a Phase E follow-up if it bites a third time. |

---

## Robotics / multi-environment (forward-looking)

| Lesson | Detail | Action |
|--------|--------|--------|
| 32 B inline payload is demo-only | v2 expanded inline from 22 B → 32 B (ADR 0008); still not where cameras/ML live | Phase B payload ADR + v2 frame `sideband_idx` / `sideband_seq` / `sideband_len`; cameras/ML go to sideband regions |
| Same peer IDs across environments | HIL should not recompile routes | Topology file per profile; stable `kEndpoint*` ids |
| Python/Node in core = bloat | Bridges must be separate processes | Phase F examples only |
| Jetson vs x86 differs by transport | Jetson: SHM; sim/cloud: UDP loopback | Profile YAML maps `local:` per peer |

---

## `if constexpr` / factory pattern

```cpp
// WRONG — both branches may be instantiated
if constexpr (std::is_same_v<T, ShmSpsc>) { return shm(); }
return datagram<T>();

// RIGHT
if constexpr (std::is_same_v<T, ShmSpsc>) { return shm(); }
else { return datagram<T>(); }
```

---

## CLI arity reference (demo client)

| Role | Transport | argc | Log path argv |
|------|-----------|------|----------------|
| controller | uds/udp | 6 | `argv[5]` |
| controller | shm | 4 | `argv[3]` |
| recorder | uds/udp | 5 | `argv[4]` |
| recorder | shm | 4 | `argv[3]` |

Add tests when touching argument parsing.
