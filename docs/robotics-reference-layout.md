# Robotics reference layout

> **Phase E E1 deliverable.** Reference deployment shape for the IPC router and its peer processes across Jetson on-robot, x86 dev, HIL bench, and sim / CI cloud targets. Companion to [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md) (the north-star) and the per-profile TOML files under [config/profiles/](../config/profiles/).

## Purpose

This document tells someone deploying or extending the module:

- **Which peers** can run on the same router instance (catalog + IDs + data class).
- **Which transport** each peer typically uses on each deployment target.
- **What goes in the inline 32 B payload vs the sideband regions** (control plane vs bulk).
- **Where the seams are** between the router (C++ core) and peers (separate processes, any language).
- **What is documented today vs deferred** — explicit pointers to Phase F deliverables and the [post-phases robotics-integration review](../robotics-ipc-module/plans/post-phases-robotics-review.md).

It does **not**:

- Provide TensorRT, CUDA, V4L2, GStreamer, or MAVLink code (those live in user processes — see Phase F sketches).
- Claim safety-critical certification ([ADR 0004](adr/0004-robotics-module-boundaries.md) §Out of scope).
- Replace per-profile TOML — the profiles under [config/profiles/](../config/profiles/) remain the source of truth for addresses and ring sizing.

## Scope vs deferred

This is a **deployment-shape document**. It documents the contract surface (peer catalog, frame fields, sideband regions, resource paths) but does **not** ship integration code. The following are deliberately out of scope here:

| Topic | Where it lives |
|-------|----------------|
| `sd_notify` / systemd readiness | Beyond Phase E2 ([deploy/systemd/](../robotics-ipc-module/deploy/systemd/) ships `Type=simple` units; readiness signaling deferred — see parked review C6) |
| Cross-host time correlation | Beyond Phase E4 ([ADR 0010](adr/0010-router-timestamp-clock.md) lands `CLOCK_MONOTONIC_RAW` for single-host scope; cross-host delegated to user code or a future dedicated recorder — see parked review C8) |
| Python / Node / MAVLink bridge code | Phase F F2–F4 |
| Vision peer + camera capture code | Phase F F5 |
| Sideband `memory_class` (CUDA / NvBufSurface) parsing | Phase F (forward-declared in [ADR 0008](adr/0008-router-frame-v2.md)) |
| TensorRT contract beyond peer-catalog level, replay/sim peer, declarative-transport extensions (per-topic routes — promoted to Phase G; priority-aware QoS — merged into C7), mixed-transport networks (SHM + UDS + UDP from one router), RT pinning / `mlockall`, aarch64 CI dimension, `make install` / CMake export | Open backlog — see [plans/post-phases-robotics-review.md](../robotics-ipc-module/plans/post-phases-robotics-review.md) (considerations C1–C11; C5 Scopes A + B closed 2026-05-28; C5 Scope C promoted to [Phase G](../robotics-ipc-module/plans/G-declarative-routing.md) and Scope D merged into C7 on 2026-05-30) |

## Peer catalog

Peer IDs are byte-valued (1–255) and stable across deployment profiles per the swapping rule in [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md#deployment-targets). Names are configured in topology TOML.

| ID | Name | Role | Inline frame use | Sideband use | Status today |
|----|------|------|------------------|--------------|--------------|
| 1 | `sensor` | Proprioceptive / IMU aggregate; periodic publish | `topic_id`, `seq`, ≤32 B payload | none | Demo binary (`router_client sensor`) |
| 2 | `controller` | Control loop; subscribes sensor, publishes commands | `topic_id`, `seq`, ≤32 B payload | optional (see `jetson_prod.toml`) | Demo binary (`router_client controller`) |
| 3 | `recorder` | Black-box log; subscribe-all | reads everything; writes log | none | Demo binary (`router_client recorder`); CSV format — **not playback-compatible** (see C4 in parked review) |
| 4 | `vision_capture` | CSI/V4L camera pipeline | metadata only (`topic_id`, `seq`, `timestamp_ns`, sideband descriptor) | NV12 / JPEG via SHM region | **Sketch only** — Phase F F5 |
| 5 | `ml_inference` | CUDA inference (e.g. TensorRT) | metadata + tensor descriptor | input + output tensors via SHM region | **Sketch only** — Phase F F5; CUDA-class sidebands deferred to Phase F |
| 6 | `mavlink_gateway` | MCU bridge over serial | compact status frames (mode, attitude, ack) | none | **Sketch only** — Phase F F4 |
| 7 | `python_tooling` | Training / scripts; offline batch | matches v2 frame layout via ctypes | optional | **Implemented** — Phase F F2 ([`examples/bridges/python_peer/`](../examples/bridges/python_peer/)) |
| 8 | `dashboard_feed` | Node UDP → WebSocket gateway (stdlib, no `npm install`) | reads everything; forwards to browser as JSON | none | **Implemented** — Phase F F3 ([`examples/bridges/node_gateway/`](../examples/bridges/node_gateway/)) |

**Frame layout** (64 B, host little-endian) per [ADR 0008](adr/0008-router-frame-v2.md):
`source` (u8) · `flags` (u8) · `topic_id` (u16) · `seq` (u32) · `timestamp_ns` (u64) · `sideband_idx` (u8) · `sideband_len` (u48) · `sideband_seq` (u8) · 32 B inline payload.

## Topology overview

The router is a single in-process forwarder per host. Same peer IDs and route rules in every profile; only addresses and ring sizing change.

```mermaid
flowchart LR
  subgraph host [Single Linux host: Jetson / x86 dev / HIL / sim node]
    R[Router C++ process]
    S[sensor 1]
    C[controller 2]
    REC[recorder 3]
    V[vision_capture 4]
    ML[ml_inference 5]
    MAV[mavlink_gateway 6]
  end

  subgraph bridges [Phase F bridge processes]
    PY[python_tooling 7]
    NODE[dashboard_feed 8]
  end

  S -->|publish| R
  R -->|fanout| C
  R -->|fanout| REC
  V -->|metadata frames + sideband| R
  ML -->|status frames + tensor sideband| R
  MAV -->|MCU status| R
  R -->|commands| C
  R -->|all frames| REC

  R <-->|UDS or UDP| PY
  R -->|UDS or UDP| NODE
  NODE -->|WebSocket| WEB[Browser]
```

**Per-deployment profile mapping** (Phase F F1 — see [docs/deployment-profiles.md](deployment-profiles.md) for the operator-facing breakdown):

| Profile | Transport | Listen address | Resource prefix |
|---------|-----------|----------------|-----------------|
| [config/profiles/jetson_prod.toml](../config/profiles/jetson_prod.toml) | SHM (all 6 peers) | `shm:/rim_router` | `/dev/shm/rim_router_*` + sideband `/dev/shm/rim_vision_*` / `rim_ml_*` |
| [config/profiles/x86_dev.toml](../config/profiles/x86_dev.toml) | UDS (all 6 peers) | `uds:/tmp/rim_router.sock` | `/tmp/rim_router_*.sock` |
| [config/profiles/hil.toml](../config/profiles/hil.toml) | UDP loopback | `udp:127.0.0.1:19100` | ports 19100–19108 (19106/19107 reserved for F4/F2) |
| [config/profiles/sim_cloud.toml](../config/profiles/sim_cloud.toml) | UDP | `udp:10.0.0.1:19200` (placeholder) | ports 19200–19208 across container subnet |

## Per-deployment shapes

### Jetson on-robot (NVIDIA Jetson, L4T)

**Transport.** Per-peer SHM SPSC rings under `/dev/shm/rim_router_*`. Each control-plane ring is sized to one cache line per slot (`shm_max_payload = 64`) × 256 slots ≈ 35 KiB per direction per peer ([ADR 0009](adr/0009-per-peer-ring-sizing.md)). Sideband regions are independent SHM segments sized for bulk payloads (8 MB NV12 frame, 16 MB ML tensor — see [`jetson_prod.toml`](../config/profiles/jetson_prod.toml)).

**Process layout** (deployment-time recommendation):

| Process | Started by | Notes |
|---------|-----------|-------|
| `router_server --config jetson_prod.toml` | systemd ([`rim-router.service`](../robotics-ipc-module/deploy/systemd/rim-router.service)) | Binds + unlinks SHM regions; idle CPU ~1.5–1.7 % single core ([ADR 0007](adr/0007-router-idle-wake.md)) |
| Sensor / controller / recorder C++ peers | systemd [`rim-peer@.service`](../robotics-ipc-module/deploy/systemd/rim-peer@.service) instances, `After=rim-router.service` | First connect can race router bind — see [parked review C6](../robotics-ipc-module/plans/post-phases-robotics-review.md#c6--systemd-readiness-signaling-sd_notify--typenotify); user-side retry-with-backoff recommended until `sd_notify` lands |
| `vision_capture` (Phase F) | systemd unit | Publishes metadata frame to router; writes NV12 to `/dev/shm/rim_vision_nv12` sideband |
| `ml_inference` (Phase F) | systemd unit | Reads `/dev/shm/rim_ml_tensor_in`, writes `/dev/shm/rim_ml_tensor_out`; CUDA / TensorRT engine is the **user's** code, not the module's |
| `mavlink_gateway` (Phase F) | systemd unit; needs `/dev/ttyUSB*` device access | Parses MAVLink, publishes compact status to router |

**Operational knobs.** None of the following are enabled by default; they belong in the user's systemd unit:

- `MemoryLock=infinity` — pin pages to avoid faults under load (recommended; see [parked review C7](../robotics-ipc-module/plans/post-phases-robotics-review.md#c7--real-time--production-knobs-mlockall-cpu-pinning-sched_fifo))
- `CPUAffinity=` — pin router to an isolated core
- `LimitRTPRIO=80` — enable `SCHED_FIFO` from inside the router (router does not call `sched_setscheduler` today; this is a knob for user code wrapping the router)

**Cleanup.** SHM regions persist across router crashes by design (router unlinks-then-creates on next start). Manual cleanup recipe in [scripts/README.md](../robotics-ipc-module/scripts/README.md). The [`shm_leak_check.sh`](../robotics-ipc-module/scripts/shm_leak_check.sh) gate verifies test suites leave no leftover `/dev/shm/rim_*` or `/tmp/rim_*.sock`.

### x86 dev (Linux laptop / workstation)

**Transport.** UDS sockets under `/tmp/rim_router_*.sock` ([`x86_dev.toml`](../config/profiles/x86_dev.toml)). Same peer IDs and route rules as Jetson; only addresses differ.

**Process layout.** Typically the developer runs `router_server --config config/profiles/x86_dev.toml` in one terminal and the demo peers (`router_client sensor` / `controller` / `recorder`) in others. Phase F bridges (Python / Node) connect over the same UDS paths.

**Notes.**

- No SHM means no `/dev/shm/rim_*` artifacts to clean up; just delete `/tmp/rim_router_*.sock` if a process crashes mid-bind. The router unlinks-then-binds, so a fresh start is enough in most cases.
- Sideband region examples in `jetson_prod.toml` do not apply here unless the developer explicitly adds `[[peers.sideband]]` entries — UDS-only profiles can still describe sideband regions; the regions are SHM segments independent of the control transport.

### HIL bench

**Transport.** UDP on `127.0.0.1` ([`hil.toml`](../config/profiles/hil.toml)). Plant / sensors / actuators are replaced by mock processes binding the same UDP ports.

**Status today.** The profile exists and validates; **mock processes do not exist** — Phase F F2/F4 examples are the intended source. See [parked review C4](../robotics-ipc-module/plans/post-phases-robotics-review.md#c4--playback--simulation-testing-on-x86) for the broader gap on playback / simulated inputs.

**Failure modes documented in [fault_injection_test.cpp](../ipc/test/fault_injection_test.cpp):**

- Truncated UDP datagrams → `DatagramRouterMetrics::recv_truncated`
- Unknown-source UDP frames → `DatagramRouterMetrics::recv_unknown_source`
- Wrong UDS path → clean throw
- Stale socket file rebind → recovered by `Uds::bind` unlink-then-bind

### Sim / CI cloud

**Transport.** UDP across a container subnet ([`sim_cloud.toml`](../config/profiles/sim_cloud.toml); the `10.0.0.x` addresses are placeholders the orchestrator overrides). Cross-host SHM is **not** supported ([ADR 0005](adr/0005-payload-policy-and-sideband.md), [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md#out-of-scope-for-the-whole-system-this-repos-module)).

**CI.** [.github/workflows/ci.yml](../.github/workflows/ci.yml) runs the unit + integration + router scenarios on x86_64 ubuntu-latest with `ccache`. Aggregate end-to-end < 1 min. aarch64 / Jetson is not currently a CI dimension — see [parked review C3](../robotics-ipc-module/plans/post-phases-robotics-review.md#c3--arm--aarch64-verification).

## Integration patterns

The router stays transport-only. Every domain integration (camera, ML, serial) is a separate peer process matching the [ADR 0008](adr/0008-router-frame-v2.md) wire layout. Stub directories for the Phase F bridges are scaffolded under [`examples/bridges/`](../examples/bridges/) — see the [bridges index](../examples/bridges/README.md) for the principles and the per-bridge stubs for contract pointers.

### Vision capture (`vision_capture`, peer 4)

**Status:** sketch only — stub at [`examples/bridges/vision_peer/`](../examples/bridges/vision_peer/); implementation lands in [F5](../robotics-ipc-module/plans/F-interoperability-bridges.md#f5--vision-metadata-peer-sketch). The reference layout below is the contract a user-written `vision_capture` peer should follow.

**Process shape.**

- Separate binary. Owns `/dev/video*` (USB UVC), `/dev/v4l-subdev*` (CSI), or NVMM buffers (Jetson `nvarguscamerasrc`).
- Encodes each captured frame into a sideband region (NV12 raw or JPEG compressed).
- Publishes a [RouterFrame v2](adr/0008-router-frame-v2.md) on the control plane with:

| Field | Use |
|-------|-----|
| `source` | 4 (`vision_capture`) |
| `flags` | `keyframe` bit for I-frames; `has_sideband` bit always set |
| `topic_id` | Camera index (e.g. 0x10 left, 0x11 right) |
| `seq` | Monotonic per-camera frame counter |
| `timestamp_ns` | Capture time per [ADR 0008](adr/0008-router-frame-v2.md) field semantics; clock is [`CLOCK_MONOTONIC_RAW`](adr/0010-router-timestamp-clock.md) — peers stamp via `router_now_ns()` from [`router/timestamp.hpp`](../ipc/src/router/timestamp.hpp) for single-host comparability with router-stamped frames |
| `sideband_idx` | Index into the peer's `[[peers.sideband]]` array |
| `sideband_seq` | Producer's slot index in the sideband ring (subscriber reads it back) |
| `sideband_len` | Bytes valid in the slot (`u48`, up to 256 TB) |
| Inline 32 B payload | Compact metadata: width, height, exposure, drop counter — **not** pixel data |

**Sideband region.** Configured in topology TOML under the peer's `[[peers.sideband]]` block. Example (from [`jetson_prod.toml`](../config/profiles/jetson_prod.toml) controller entry):

```toml
[[peers.sideband]]
class             = "vision_nv12"
name              = "/robot_vision_nv12"
max_payload_bytes = 8388608
```

Today only `name` / `max_payload_bytes` / optional `version` are parsed; `class` is informational. CUDA-class sidebands (`memory_class = "cuda_managed"` / `"nvbufsurface"`) are forward-declared in [ADR 0008](adr/0008-router-frame-v2.md) and land in Phase F (see [parked review C2](../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing)).

### ML inference (`ml_inference`, peer 5)

**Status:** sketch only — co-resident with the vision peer stub at [`examples/bridges/vision_peer/`](../examples/bridges/vision_peer/) for now (F5 covers both peers' contract surface). TensorRT / CUDA engine implementation is the **user's** code; the module provides transport for the input/output tensors.

**Process shape.**

- Separate binary. Subscribes to vision metadata frames from peer 4 (or directly from the route configured for ML).
- For each frame: maps the sideband region indexed by `frame.sideband_idx()`, reads `sideband_seq` to find the slot, copies (or zero-copies on Jetson once CUDA `memory_class` lands) the input tensor.
- Runs inference.
- Publishes a result `RouterFrame` with inline metadata (class id, confidence, latency_ns) and writes the full output tensor into a separate sideband region (`ml_tensor_out`).

**Contract on RouterFrame fields:** same as `vision_capture` but `source = 5` and `topic_id` distinguishes input vs output streams. Subscribers route by `source` today (per-source routing); the optional `[[topics]]` registry added by [closed C5 Scope B](../robotics-ipc-module/plans/post-phases-robotics-review.md#closure--scope-b-declarative-topic-registry-2026-05-28) lets bridges validate `topic_id` against a declared name + payload class, but does **not** drive dispatch yet — per-topic routing is the deliverable of [Phase G — Declarative routing](../robotics-ipc-module/plans/G-declarative-routing.md) (formerly C5 Scope C, promoted 2026-05-30).

### MAVLink gateway (`mavlink_gateway`, peer 6)

**Status:** sketch only — stub at [`examples/bridges/mavlink_gateway/`](../examples/bridges/mavlink_gateway/); implementation lands in [F4](../robotics-ipc-module/plans/F-interoperability-bridges.md#f4--mavlink-gateway-sketch).

**Process shape.**

- Separate binary, opens `/dev/ttyUSB*` (or CAN, or UDP to MAVLink simulator).
- Parses MAVLink, publishes compact status frames (mode, attitude summary, ack flags) into the inline 32 B payload.
- Subscribes to a command route from the controller (peer 2) and writes commands back to the MCU.
- Optional secondary UDP listener for ground-station tools (QGroundControl); that listener is **inside the gateway**, not in the router.

### Recorder (`recorder`, peer 3) — shipped

**Status:** demo binary today (`router_client recorder`).

**Process shape.**

- Subscribes to broadcast routes from sensor (1) and controller (2).
- Appends one CSV line per received frame to a log file (`/tmp/rim_router_c.log` by default).

**Limitation.** The current CSV format captures only `source,timestamp_ns,payload` — insufficient for cadence-faithful replay (missing `topic_id`, `seq`, `flags`, sideband refs, bulk bytes). Documented as [parked review C4](../robotics-ipc-module/plans/post-phases-robotics-review.md#c4--playback--simulation-testing-on-x86); a binary tape format + replay peer is a backlog candidate, not in Phase E scope.

### Python bridge (`python_tooling`, peer 7)

**Status:** implemented — [`examples/bridges/python_peer/`](../examples/bridges/python_peer/) ships a UDS subscriber + publisher using `ctypes.LittleEndianStructure` to mirror the 64 B [ADR 0008](adr/0008-router-frame-v2.md) frame layout byte-for-byte. End-to-end wire compatibility is verified by [`smoke.sh`](../examples/bridges/python_peer/smoke.sh) (Python publisher → C++ router → Python subscriber with byte-exact payload comparison).

**Process shape.** Standalone Python process (any interpreter, stdlib-only — no third-party deps). Connects to the router on UDS (recommended on x86 dev) or UDP (HIL / cloud — UDP variant not in F2 deliverable; trivial extension by switching the socket family in [`rim_router_peer.py`](../examples/bridges/python_peer/rim_router_peer.py)). The router resolves the source peer from the sender's bound socket path, so the Python peer **must** `bind()` at the peer-7 path declared in the profile before sending — see the [bridge README](../examples/bridges/python_peer/README.md#how-the-router-identifies-a-python-peer) for the details and the Phase D4 `recv_unknown_source` counter that catches misconfiguration. **Do not** embed CPython in `libipc` — header-only C++ keeps the boundary clean ([ADR 0004](adr/0004-robotics-module-boundaries.md)).

### Node dashboard gateway (`dashboard_feed`, peer 8)

**Status:** implemented in [F3](../robotics-ipc-module/plans/F-interoperability-bridges.md#f3--node-dashboard-gateway-example) at [`examples/bridges/node_gateway/`](../examples/bridges/node_gateway/) — fully self-terminating `smoke.sh` runs a `publish.js → C++ router → gateway.js → ws_test_client.js` round-trip in ~2 s with byte-exact payload verification.

**Process shape.** Node.js (≥ 18) process; pure stdlib (no `npm install`, no third-party deps). UDP client to router, WebSocket server to browser. Decodes the inline 32 B payload + sideband metadata into JSON for the browser; the C++ router has no Node API surface — the gateway parses the frame layout itself via a Buffer-backed `RouterFrame` port with byte-offset self-checks that fail-fast on load if the C++ struct ever drifts from the [ADR 0008](adr/0008-router-frame-v2.md) table.

**Contract on `RouterFrame` fields:** as a subscriber, the gateway reads every field and serializes them into one JSON message per UDP frame — `source` / `source_name` / `flags` / `topic_id` / `seq` / `timestamp_ns` (router's `CLOCK_MONOTONIC_RAW`, ADR 0010, emitted as a string because JSON numbers are IEEE 754) / `sideband_idx` / `sideband_len` / `sideband_seq` / `payload_hex` / `payload_text`. The router resolves the source peer from the sender's UDP source `host:port` via [`peer_id_from_recv<Udp>`](../ipc/src/router/datagram_peer_resolver.hpp), so the gateway **must** `bind` at the peer-8 host:port declared in the profile before sending — see [the bridge README](../examples/bridges/node_gateway/README.md#how-the-router-identifies-the-gateway) and the Phase D4 `recv_unknown_source` counter that catches misconfiguration.

**Transport reachability.** Node's stdlib `dgram` module supports only UDP. The F3 gateway works out of the box against `hil.toml` and `sim_cloud.toml` (UDP profiles). For `x86_dev.toml` (UDS) the gateway needs either the [`unix-dgram`](https://www.npmjs.com/package/unix-dgram) npm package or a small profile variant that swaps peer 8 to UDP. For `jetson_prod.toml` (SHM) the gateway is not reachable — parked under [C11 mixed-transport networks](../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks); the practical fix is running the gateway on x86 and forwarding via UDP. **Do not** embed Node in `libipc` — header-only C++ keeps the boundary clean ([ADR 0004](adr/0004-robotics-module-boundaries.md)).

## Operational notes

### Time semantics

The router stamps `timestamp_ns` with `CLOCK_MONOTONIC_RAW` per [ADR 0010](adr/0010-router-timestamp-clock.md). Properties:

- **Single-host, since-boot.** Comparable across processes on the same host within a boot epoch.
- **Slew-free.** Unaffected by NTP / `adjtime` — control-loop deadlines computed from frame timestamps don't see spooky jitter.
- **Survives router restart.** systemd `Restart=on-failure` does not reset the clock; subscribers can latch monotonically across router lifetime.
- **Resets on reboot. Not comparable across hosts.**

User peers that want to stamp their own frames with the same clock include [`router/timestamp.hpp`](../ipc/src/router/timestamp.hpp) and call `router_now_ns()`.

**Cross-host correlation is delegated** ([ADR 0010 §Out of scope](adr/0010-router-timestamp-clock.md)):

- **Application-level.** A peer that needs cross-host (HIL, sim_cloud) time correlation adds its own clock reading (PTP-anchored, sim-time, wall clock) inside its custom 32 B payload or in a sideband region. The router transports those bytes opaquely.
- **Future dedicated recorder module.** A stateful recorder — the kind needed for replay per [parked review C4](../robotics-ipc-module/plans/post-phases-robotics-review.md#c4--playback--simulation-testing-on-x86) — is the natural home for richer time semantics (PTP, capture-time, host clock skew tracking). Its relaxed latency / statelessness requirements make it the right place to absorb the complexity that the hot-path router refuses.

See also [parked review C8](../robotics-ipc-module/plans/post-phases-robotics-review.md#c8--cross-host-time-sync-ptp--ntp) — single-host portion is closed by ADR 0010; cross-host portion remains in the backlog as a *delegation*, not a deferred deliverable.

### Process supervision

systemd unit examples ship in [`robotics-ipc-module/deploy/systemd/`](../robotics-ipc-module/deploy/systemd/): [`rim-router.service`](../robotics-ipc-module/deploy/systemd/rim-router.service) (started first, owns SHM region lifecycle), [`rim-peer@.service`](../robotics-ipc-module/deploy/systemd/rim-peer@.service) (template; `%i` = `sensor` / `controller` / `recorder`; `After=`+`Requires=` the router), and [`rim-router-cleanup.sh`](../robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh) (`ExecStopPost=` helper). See the [deploy/systemd/README.md](../robotics-ipc-module/deploy/systemd/README.md) for install steps, customization knobs, and known limitations.

### Idle CPU

`router_server` sleeps `RouterRunOptions::idle_sleep_us` (default 1 ms) between empty polls ([ADR 0007](adr/0007-router-idle-wake.md)). Measured idle ≈ 1.5–1.7 % single core on Jetson profile. The [`idle_cpu_check.sh`](../robotics-ipc-module/scripts/idle_cpu_check.sh) gate enforces ≤ 5 %.

### Restart and recovery

Router unlinks-then-creates SHM regions on bind. UDS bind unlinks the socket path before listening. Datagram links rebind cleanly after `SO_REUSEADDR`. Verified by [router_restart_test.cpp](../ipc/test/router_restart_test.cpp) and [fault_injection_test.cpp](../ipc/test/fault_injection_test.cpp).

### Resource cleanup

Stale resources occasionally outlive a hard kill. Recovery recipes:

```sh
sudo pkill -KILL -f "build/ipc/test/router_server"   # kill orphans (last resort; documented in LESSONS-LEARNED.md)
rm -f /dev/shm/rim_router_* /tmp/rim_router_*.sock /tmp/rim_router_*.log
```

The [`shm_leak_check.sh`](../robotics-ipc-module/scripts/shm_leak_check.sh) script is the regression gate (delta == 0 across unit + integration + router suites).

## Forward references

| Topic | Where it lands |
|-------|----------------|
| systemd unit files | [robotics-ipc-module/deploy/systemd/](../robotics-ipc-module/deploy/systemd/) (Phase E E2) |
| Bridge pointers / scaffolding | [examples/bridges/](../examples/bridges/) (Phase E E3 scaffolding) |
| Timestamp clock | [ADR 0010](adr/0010-router-timestamp-clock.md) — `CLOCK_MONOTONIC_RAW` single-host; cross-host delegated (Phase E E4) |
| Profile templates + `deployment-profiles.md` | [docs/deployment-profiles.md](deployment-profiles.md) — Phase F F1 + F2 + closed C5 Scopes A + B (4 profiles × 7 peers; routes fan out to up to `kMaxRouteDests = 8`; optional `[[topics]]` registry; only the single-transport-per-router limitation remains open, cross-referenced to parked C11) |
| Python / Node / MAVLink / vision peer code | [Phase F F2–F5](../robotics-ipc-module/plans/F-interoperability-bridges.md) (F2 shipped in [`examples/bridges/python_peer/`](../examples/bridges/python_peer/); F3 shipped in [`examples/bridges/node_gateway/`](../examples/bridges/node_gateway/); F4/F5 are README-only stubs awaiting implementation) |
| Open considerations (TensorRT contract depth, CUDA `memory_class`, ARM CI, playback peer, declarative-transport extensions, RT pinning, cross-host time, camera shape, consumption model) | [plans/post-phases-robotics-review.md](../robotics-ipc-module/plans/post-phases-robotics-review.md) |
