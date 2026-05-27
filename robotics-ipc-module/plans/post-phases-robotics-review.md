# Post-phases robotics-integration review

> **Status:** deferred — revisit when Phase F closes (or earlier if explicitly reopened)
> **Source:** chat analysis dated 2026-05-26 ([Phase E fit-for-purpose review](f325cb57-00db-4d4e-a19c-2c45473839d1))
> **Trigger question:** _"Analyze Phase E — is it fit for purpose? Consider TensorRT, CUDA, ARM, x86 playback / simulated inputs, declarative transport."_

## Scope

This document captures the 10-point analysis surfaced when the user asked whether [Phase E](E-robotics-integration.md) as currently written is fit for purpose against real robotics-deployment requirements. The verdict was:

> Phase E as written is **necessary but not sufficient**. It is a deployment-shape phase (systemd units, reference layout, timestamp ADR) — it does not unlock the robotics-integration capabilities the user named.

Some considerations are already in [Phase F](F-interoperability-bridges.md) scope, some are underspecified inside Phase E itself, and some are not in any current plan. This document is the parking lot for all of them so nothing is lost while the existing phases close out.

## Revisit trigger

Reopen this document when **all** of:

- Phase E (E1–E4) marked complete in [STATUS.md](../STATUS.md)
- Phase F (F1–F5) marked complete in [STATUS.md](../STATUS.md)

At that point, walk each consideration, decide close / defer / scope into a new phase.

## How to reopen

1. Re-read this document end to end.
2. For each consideration in **Group 1** and **Group 2**, verify whether the closing E/F deliverables addressed the concern. Mark the outcome inline (e.g. `**Closed 2026-XX-XX:** ...`).
3. For **Group 3**, decide per item: close with a documented rationale, defer further, or promote into a new `plans/G-*.md`.
4. Update [STATUS.md](../STATUS.md) — remove the breadcrumb pointing here once every consideration is resolved.

## Group 1 — Verify when Phase F closes (already in Phase F scope)

- **C1** TensorRT integration contract — addressed by F5 / `examples/bridges/`?
- **C2** CUDA / sideband `memory_class` parsing — addressed by F5 + ADR 0008 forward declaration?
- **C9** Camera / GStreamer integration shape — addressed by F5 `vision_peer` sketch?

## Group 2 — Verify when Phase E closes (in plan but underspecified)

- **C6** systemd readiness signaling — E2 names the units but does not yet specify `Type=notify` / `sd_notify`. Confirm before E2 closes.
- **C8** Cross-host time sync — E4 forward-declares PTP / `CLOCK_MONOTONIC` raw. Confirm which path E4 actually lands.

## Group 3 — Not in any current plan (needs explicit decision)

- **C3** ARM / aarch64 verification
- **C4** Playback / simulation testing on x86
- **C5** Declarative transport gaps (topic registry, per-topic routes, QoS)
- **C7** Real-time / production knobs (`mlockall`, CPU pinning, SCHED_FIFO)
- **C10** Module consumption model (`make install` / CMake export vs. vendor-as-submodule)

---

## Considerations in detail

### C1 — TensorRT integration contract

**Finding.** The module is, by design, CPU/transport-only (per [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md)). There is no TensorRT code today, and there shouldn't be — `ml_inference` is a separate peer process. **But** the contract for "how does a TensorRT peer plug in?" is not concretely documented anywhere. [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) names the peer; that's it.

**Evidence.**

- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) — `ml_inference` named in peer catalog.
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) — explicitly excludes CUDA / TensorRT from core.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) (F5) — vision peer sketch only; no TensorRT example.

**Coverage today.** Partial — peer is named, no contract written.

**Options.**

1. Phase E E1 reference layout adds a written contract: which `RouterFrame` v2 fields a TensorRT peer reads/writes, which sideband regions carry input vs output tensors, who allocates, who frees, `topic_id` semantics.
2. Push contract into a worked Phase F example under `examples/bridges/ml_inference/`.
3. Document only as "user responsibility" and close.

---

### C2 — CUDA / sideband `memory_class` parsing

**Finding.** [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) forward-declares `[[peers.sideband]] memory_class` (`shm` / `cuda_managed` / `cuda_host` / `nvbufsurface`), but the topology loader does **not** parse it. The `SidebandRegion` struct has no memory-class field. This is currently a Phase F deliverable (F5).

**Evidence.**

- [ipc/src/router/topology_loader.hpp](../../ipc/src/router/topology_loader.hpp) — `[[peers.sideband]]` parser (~lines 321–358 as of 2026-05-26): reads `name`, `max_payload_bytes`, optional `version`. No `class` / `memory_class` / `cuda_device` extraction.
- [ipc/src/router/sideband.hpp](../../ipc/src/router/sideband.hpp) — `SidebandRegion` struct (~lines 69–83): only `name`, `max_payload_bytes`, `version`.
- [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) lines ~129–161 — forward declares the schema for Phase F.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) line ~57 — F5 commits to landing the field.
- Repo grep for CUDA / NvBufSurface in `ipc/src/`: zero hits (only ADRs, plan docs, and a `#ifdef __CUDACC__` guard inside `third_party/tomlplusplus/toml.hpp`).

**Coverage today.** No — only SHM-class sidebands are usable.

**Options.**

1. **Promote `memory_class` parsing out of Phase F into Phase E.** ~30-line parser change + `SidebandRegion` field. The CUDA-aware *consumer* code stays in Phase F.
2. Keep deferred to Phase F as designed — and during Phase F closure, verify F5 actually lands the parser + struct field, not just the README sketch.
3. Document explicitly that only `class = "shm"` sidebands are supported in v1.

---

### C3 — ARM / aarch64 verification

**Finding.** Jetson (aarch64, L4T) is the named target ([ipc/MODULE.md](../../ipc/MODULE.md) line ~16, [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) line ~117, [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md) lines ~57, ~94) but the module has never actually been built or tested on aarch64 hardware in CI.

**Evidence.**

- Zero `#ifdef __aarch64__` / `NEON` / `tegra` / `Jetson` symbols in `ipc/src/` library or test code (only documentation mentions + toml++ ARM64 compile guards in `third_party/`).
- [.github/workflows/ci.yml](../../.github/workflows/ci.yml) — `ubuntu-latest` x86_64 runner only; no aarch64 matrix.
- Jetson validation today is **indirect**: running `jetson_prod.toml` profile on an x86 host + idle-CPU script.

**Coverage today.** No — code *should* be aarch64-clean (header-only C++20 atomics, no x86 intrinsics), but "should be" is not "is".

**Options.**

1. **Document only** — add an aarch64 build recipe to `ipc/MODULE.md`. Run it on actual hardware out-of-band; record result in [STATUS.md](../STATUS.md). No CI change.
2. **Cross-compile CI** — add `aarch64-linux-gnu-g++` cross-build matrix to [.github/workflows/ci.yml](../../.github/workflows/ci.yml). Builds only; no execution.
3. **Full aarch64 test CI** — use `qemu-user-static` or GitHub's ARM runners to actually execute the suite. Highest confidence, highest cost.

---

### C4 — Playback / simulation testing on x86

**Finding.** This is the largest delta between the plan and the stated requirements. There is **no replay / playback engine** in the repo (grep for `replay`, `playback`, `tape`, `simulator` — zero hits in code). The recorder peer writes a 3-field CSV that lacks the fields needed for replay. The `hil.toml` profile is just an address swap, not a sim harness.

**Evidence.**

- Recorder output format ([ipc/test/router_client.cpp](../../ipc/test/router_client.cpp) lines ~37–44):

  ```cpp
  void append_record(const std::string& log_path, const RouterFrame& frame) {
      ...
      out << static_cast<int>(frame.source()) << ','
          << frame.timestamp_ns() << ','
          << frame.payload() << '\n';
  }
  ```

  Missing for replay: `topic_id`, `seq`, `flags`, sideband refs, sideband bulk bytes, original cadence, capture-time (router-relative monotonic only — see C8).

- [config/profiles/hil.toml](../../config/profiles/hil.toml) — line ~3 comment: "plant/sensors replaced by mock processes (Phase F examples)". Those mock binaries do not exist; `examples/` tree is absent entirely.
- Recorder peer ([ipc/test/router_client.cpp](../../ipc/test/router_client.cpp) lines ~119–137): passive `recv` loop, no sideband capture.

**Coverage today.** No — no replay, no extended log format, no mock peers.

**Options.**

1. **Minimum (Phase E or new Phase G):** Define a canonical recorder tape format (ADR + a small `tape_format.hpp`): binary, length-prefixed records covering every `RouterFrame` field + sideband descriptor bytes. Don't implement the player yet.
2. **Full (new Phase G or expansion of Phase F):** Land the recorder upgrade *and* a `replay_peer` under `examples/sim/` that reads the tape and re-emits at original cadence (or scaled). Most useful for "test on x86 with playback inputs."
3. Treat playback as user-responsibility infrastructure; document the wire fields a user-written recorder would need to capture; close.

> Recommended by the analysis as the **single highest-leverage** missing capability for the stated robotics use case.

---

### C5 — Declarative transport layer (gaps)

**Finding.** A declarative transport layer exists at the per-peer level (TOML profiles cover address, ring sizing, sideband regions). What is **not** declarative: topic registry, per-topic routing, QoS, transport-kind as first-class.

**Evidence.**

- TOML schema today ([ipc/src/router/topology_loader.hpp](../../ipc/src/router/topology_loader.hpp) `build_from_`): `[router].listen`, `[[peers]]` (`id`, `name`, `local`, optional SHM ring sizing), `[[peers.sideband]]` (name, max_payload_bytes, optional version), `[[routes]]` (source, dest = array of 1–2 peer IDs).
- Routes are **per-source**, not per-topic ([ipc/src/router/routing.hpp](../../ipc/src/router/routing.hpp) `RouteRule { source, dest0, dest1 }`, ~lines 17–21). Lookup is `route_targets_for(rules, rule_count, source)` — first matching rule wins.
- Topic registry: **none.** `topic_id` is a `u16` wire field in `RouterFrame` ([ipc/src/router/frame.hpp](../../ipc/src/router/frame.hpp) lines ~87–94) with no central table mapping `topic_id → name + sideband_idx + payload schema`. Publishers magic-number it.
- QoS: 3-bit `priority` field in frame `flags` (ADR 0008) is set but the router does **not** act on it. Drop policy is fixed at drop-on-full per destination ([ShmRouterMetrics::dropped_full_per_peer](../../ipc/src/router/metrics.hpp)).
- Transport kind is embedded in `listen`/`local` URI string (`uds:/...`, `udp:host:port`, `shm:/name`) rather than first-class.

**Coverage today.** Partial — peer+address+ring+sideband are declarative; routing semantics and QoS are not.

**Options.**

1. **Document the current schema authoritatively** in [docs/robotics-reference-layout.md](../../docs/robotics-reference-layout.md) (E1) and treat the rest as publisher discipline.
2. **Add a `[[topics]]` registry** (topic_id → name + sideband_idx + default payload class). Router behavior unchanged; tooling and bridges can validate.
3. **Make routes per-topic** (changes `RouteRule` shape, routing dispatch, all profiles).
4. **Make the router act on `priority`** (priority queue draining order in `forward_loop` and `ShmRouterLink::forward`).

---

### C6 — systemd readiness signaling (`sd_notify` / `Type=notify`)

**Finding.** Zero `sd_notify` / `libsystemd` / `Type=notify` / `NotifySocket` references in the repo. The router binds and enters its forward loop without signaling readiness. Phase E E2 lists the units but does not specify the notify type.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — `run_shm_router` (~lines 82–88) calls `bind_shm_router_listen` then `run_forward_loop`. No callback after SHM regions are mapped.
- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) E2 — names `rim-router.service` / `rim-peer@.service` but does not specify `Type=notify`.
- [robotics-ipc-module/README.md](../README.md) line ~24 — future unit names only.

**Coverage today.** No. **Why it matters.** Without `Type=notify`, peer units gated `After=rim-router.service` will start the moment `router_server` is `exec()`'d, not when the SHM regions are actually bound. Peers will race their first `shm_open` and intermittently fail. Real Jetson-production footgun.

**Options.**

1. Add `sd_notify("READY=1")` after bind in [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — either via `<systemd/sd-daemon.h>` behind a `HAVE_LIBSYSTEMD` define, or a small inline socket-write to `$NOTIFY_SOCKET` to keep `libsystemd` out of the link line.
2. Document the race + require peer-side retry-with-backoff on `shm_open` (already the right defensive posture either way).
3. Both — `sd_notify` for clean startup ordering, plus document the retry as a backstop.

---

### C7 — Real-time / production knobs (`mlockall`, CPU pinning, SCHED_FIFO)

**Finding.** Zero hits for `mlockall`, `sched_setaffinity`, `SCHED_FIFO`, `LimitRTPRIO`, `MemoryLock`, `CPUAffinity`, `Nice=`, `pthread_setaffinity_np`. The Phase E plan does not list these as targets.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) — default scheduler, paged memory, 200 ms poll timeout, 1 ms idle sleep ([ADR 0007](../../docs/adr/0007-router-idle-wake.md)). No init hook for hardening.
- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) — no mention of `mlockall` / `CPUAffinity` / RT scheduling.

**Coverage today.** No. **Why it matters.** Robotics control loops typically pin the IPC hot path to an isolated core with `mlockall` + `SCHED_FIFO` to avoid page-fault and preemption jitter. Without an opt-in path, downstream users must patch.

**Options.**

1. Add an opt-in init hook in `router_app.h` (`router_lock_memory()`, `router_pin_to_core(int)`) + a documented `[Service]` snippet (`MemoryLock=infinity`, `CPUAffinity=2`, `LimitRTPRIO=80`) in the Phase E2 unit files.
2. Treat as user responsibility — document the recommended systemd directives in E1 only; no code changes.
3. Promote into a new Phase G "production hardening" deliverable.

---

### C8 — Cross-host time sync (PTP / NTP)

**Finding.** The router clock is `steady_clock::now() - kStartTime` — monotonic-relative-to-router-start. Useless across hosts and reset on every router restart. E4 forward-declares PTP but Phase E does not yet land an implementation choice.

**Evidence.**

- [ipc/test/router_server.cpp](../../ipc/test/router_server.cpp) lines ~16–22:

  ```cpp
  using Clock = std::chrono::steady_clock;
  const Clock::time_point kStartTime = Clock::now();
  uint64_t ns_since_start() {
      return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              Clock::now() - kStartTime).count());
  }
  ```

  Stamped on forward ([ipc/src/router/link.hpp](../../ipc/src/router/link.hpp) line ~95; [ipc/src/router/shm_router_link.hpp](../../ipc/src/router/shm_router_link.hpp) line ~91).

- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md) E4 — "replace router-relative ns with `CLOCK_MONOTONIC` raw or PTP offset field."

**Coverage today.** Forward reference only (E4). **Why it matters.** HIL/sim, recorded-tape replay (C4), and any latency measurement across hosts all break without a cross-host clock or at minimum a process-stable monotonic clock.

**Options.**

1. **`CLOCK_MONOTONIC_RAW`** — single-host monotonic immune to NTP slew. Small change. Insufficient for cross-host correlation.
2. **`CLOCK_TAI` + PTP** — cross-host, requires `ptp4l` / `phc2sys` on the host. Bigger system story; document the host config.
3. **Both fields in the frame** — publisher monotonic_ns + router_recv_ns. Enables end-to-end latency. Currently the frame has only one `timestamp_ns`. **Note:** ADR 0008 fixed the frame at 64 B; this would require carving the existing `timestamp_ns` into two narrower fields or stealing bytes from the 32 B payload — non-trivial.

---

### C9 — Camera / GStreamer / V4L2 integration shape

**Finding.** No camera-side code in the repo (grep `gstreamer`, `v4l2`, `nvbuf`, `dmabuf`, `csi` — only docs/plans). [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) lines ~98–102 name `vision_capture` as a future peer; F5 places the sketch in `examples/bridges/vision_peer/README.md` — and `examples/` does not exist.

**Evidence.**

- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md) — names peer, mentions NV12/JPEG/NVMM sideband classes.
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md) F5 — sketch only, README-shaped deliverable.
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md) line ~152 — `examples/bridges/vision_metadata/` Phase F territory.

**Coverage today.** No.

**Options.**

1. Phase E E1 documents the contract (which `topic_id` / `sideband_idx` / `seq` semantics; who owns the SHM region; how frames are released back); leave code for Phase F.
2. Defer entirely to Phase F as scoped.
3. Treat as out-of-scope and explicitly close.

---

### C10 — Module consumption model

**Finding.** No `make install`, no CMake config-mode export, no pkg-config. [robotics-ipc-module/install.sh](../install.sh) installs Cursor *skills*, not the library. Downstream apps are expected to vendor `ipc/src/` as a submodule and add the include + link flags by hand.

**Evidence.**

- [Makefile](../../Makefile) lines ~1–31:

  ```makefile
  #   ipc/src/     header-only library
  # Public entry points:
  #   #include "ipc.hpp"
  #   #include "router_protocol.hpp"
  #   #include "router_app.h"
  IPC_INC := -I$(IPC_ROOT)/src -I$(THIRD_PARTY)/tomlplusplus
  IPC_TEST_LDFLAGS := -lrt
  ```

- No `CMakeLists.txt`, no `.pc`, no `*.cmake` export file anywhere.
- [ipc/MODULE.md](../../ipc/MODULE.md) §Toolchain / §Public entry points — documents `-Iipc/src`, optional `-Ithird_party/tomlplusplus`, link `-lrt -pthread`.

**Coverage today.** Implicit (header-only vendoring is the de facto policy).

**Options.**

1. Make policy explicit — add a "Consuming this module" section to [ipc/MODULE.md](../../ipc/MODULE.md) (or [robotics-ipc-module/README.md](../README.md)) covering submodule pin recipe, include/link flags, ABI policy (none — header-only), version selection.
2. Add a minimal CMake config-mode export (`RoboticsIpcModuleConfig.cmake` + `target_include_directories` + `INTERFACE` target) as a Phase F (or G) deliverable.
3. Add `make install` that lays headers into `$(PREFIX)/include/ipc/`.

---

## Summary matrix

| ID | Consideration | Status today | Phase E/F coverage | Group |
|----|----|----|----|----|
| C1 | TensorRT integration contract | Peer named, no contract | Phase F (F5 sketch) | 1 |
| C2 | CUDA / sideband `memory_class` | Unparsed | Phase F (F5) | 1 |
| C3 | ARM / aarch64 verification | Docs only; no CI dim | Not planned | 3 |
| C4 | Playback / sim on x86 | None; CSV recorder unusable | Not planned | 3 |
| C5 | Declarative transport gaps | Per-peer only; no topic/QoS | Partial in E1 docs | 3 |
| C6 | systemd readiness | No `sd_notify` | E2 (underspecified) | 2 |
| C7 | RT pinning / `mlockall` | None | Not planned | 3 |
| C8 | Cross-host time sync | `steady_clock` relative ns | E4 (forward-decl only) | 2 |
| C9 | Camera / GStreamer shape | Docs only | Phase F (F5) | 1 |
| C10 | Module consumption model | Implicit submodule pattern | Not planned | 3 |

## References

- [robotics-ipc-module/plans/E-robotics-integration.md](E-robotics-integration.md)
- [robotics-ipc-module/plans/F-interoperability-bridges.md](F-interoperability-bridges.md)
- [robotics-ipc-module/SYSTEM-VISION.md](../SYSTEM-VISION.md)
- [robotics-ipc-module/STATUS.md](../STATUS.md)
- [docs/adr/0004-robotics-module-boundaries.md](../../docs/adr/0004-robotics-module-boundaries.md)
- [docs/adr/0005-payload-policy-and-sideband.md](../../docs/adr/0005-payload-policy-and-sideband.md)
- [docs/adr/0008-router-frame-v2.md](../../docs/adr/0008-router-frame-v2.md)
- [docs/adr/0009-per-peer-ring-sizing.md](../../docs/adr/0009-per-peer-ring-sizing.md)
- [ipc/MODULE.md](../../ipc/MODULE.md)
- Conversation: [Phase E fit-for-purpose review](f325cb57-00db-4d4e-a19c-2c45473839d1)
