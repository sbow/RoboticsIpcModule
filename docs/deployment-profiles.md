# Deployment profiles

> **Phase F F1 deliverable** (peer 7 added in F2). Operator-facing companion to the four TOML profiles under [`config/profiles/`](../config/profiles/). Pick the right profile for the host, understand the trade-offs each one bakes in, and know where the documented gaps are.

## Profile selector

| Host shape | Profile | Transport | Use when |
|------------|---------|-----------|----------|
| **NVIDIA Jetson on a robot** (L4T) | [`jetson_prod.toml`](../config/profiles/jetson_prod.toml) | SHM (all peers) | Embedded compute; lowest-latency local IPC; CSI / V4L cameras + CUDA inference + control loop co-located |
| **x86 dev laptop / workstation** | [`x86_dev.toml`](../config/profiles/x86_dev.toml) | UDS (all peers) | Local iteration; debugging; running a Python or Node bridge against the router; tracing with `strace` / `tcpdump` (UDS-aware) |
| **Hardware-in-the-loop bench** | [`hil.toml`](../config/profiles/hil.toml) | UDP loopback (`127.0.0.1`) | Plant replaced by mocks; deterministic injection; tools that need a recordable UDP wire (e.g. `tcpdump -i lo`) |
| **Cloud / CI simulation** | [`sim_cloud.toml`](../config/profiles/sim_cloud.toml) | UDP across a container subnet | Multi-container test deployment; cross-machine SHM is not supported by the module ([ADR 0005](adr/0005-payload-policy-and-sideband.md)) |

**Swapping rule:** peer IDs and route rules are identical across all four profiles. Only the transport scheme + address strings change. A peer binary built against the demo `router_client` interface works against any profile by passing `--config <profile>.toml` to the router and a transport-matched address to the client.

## Peer catalog

All four profiles declare the same seven peers, with stable IDs per the [SYSTEM-VISION.md catalog](../robotics-ipc-module/SYSTEM-VISION.md#peer-catalog-illustrative) and [E1 reference layout](robotics-reference-layout.md#peer-catalog):

| ID | Name | Phase status | Role |
|----|------|--------------|------|
| 1 | `sensor` | Demo binary today | IMU / proprioceptive aggregate; small periodic frames |
| 2 | `controller` | Demo binary today | Control loop; subscribes sensor + ml; publishes commands |
| 3 | `recorder` | Demo binary today | Black-box log (CSV); subscribe-all via routes |
| 4 | `vision_capture` | Sketch — Phase F F5 | CSI/V4L camera pipeline; metadata frames + NV12 sideband |
| 5 | `ml_inference` | Sketch — Phase F F5 | CUDA / TensorRT engine; metadata frames + tensor sidebands |
| 7 | `python_tooling` | **Implemented — Phase F F2** ([`examples/bridges/python_peer/`](../examples/bridges/python_peer/)) | Python subscriber / publisher; UDS today, ctypes RouterFrame port |
| 8 | `dashboard_feed` | Sketch — Phase F F3 | Node UDS → WebSocket gateway |

**Peer ID 6 (`mavlink_gateway`) is intentionally absent until F4.** The profile files leave room (port 19106 in `hil.toml`, address `10.0.0.7` in `sim_cloud.toml`) so adding it later does not require renumbering.

## Route topology

Routes are identical across all four profiles (only transport changes). Each rule fans one source out to up to **two** destinations — the current routing API caps `RouteRule.dest0/dest1` at two slots (see [Known limitations §2-destination cap](#known-limitations) below).

```mermaid
flowchart LR
  S[sensor 1]
  C[controller 2]
  REC[recorder 3]
  V[vision_capture 4]
  ML[ml_inference 5]
  PY[python_tooling 7]
  DASH[dashboard_feed 8]

  S -->|sensor data + tap| C
  S -->|sensor data + tap| REC
  C -->|commands| REC
  C -->|controller tap| PY
  V -->|metadata + sideband ref| ML
  V -->|metadata + sideband ref| REC
  ML -->|results + sideband ref| C
  ML -->|results + sideband ref| REC
  PY -->|tooling output| REC

  DASH:::sketch
  classDef sketch fill:#eee,stroke:#888,stroke-dasharray:4 4
```

In TOML:

```toml
[[routes]]
source = 1                            # sensor   → controller + recorder
dest   = [2, 3]

[[routes]]
source = 2                            # controller → recorder + python_tooling (F2 tap)
dest   = [3, 7]

[[routes]]
source = 4                            # vision_capture → ml_inference + recorder
dest   = [5, 3]

[[routes]]
source = 5                            # ml_inference   → controller + recorder
dest   = [2, 3]

[[routes]]
source = 7                            # python_tooling → recorder (F2)
dest   = [3]
```

Recorder (peer 3) is the central "log + tap" point — every active source has a destination of 3, so the recorder sees the full dataflow. `python_tooling` (peer 7) gets a controller tap via `source = 2 dest = [3, 7]` so the F2 Python bridge can observe the control plane without taking a destination slot away from the recorder.

## Per-profile shape

### `jetson_prod.toml` (SHM)

**Why all-SHM:** the production target is a single Jetson host running the entire stack co-resident. SHM SPSC rings under `/dev/shm/rim_router_*` are the lowest-latency local IPC available, with per-peer ring sizing ([ADR 0009](adr/0009-per-peer-ring-sizing.md)) keeping each ring inside one cache line per slot.

**Per-peer ring sizing.** Every peer's control-plane ring is `shm_slot_count = 256`, `shm_max_payload = 64` (one `RouterFrame` v2 per slot). Each peer ring footprint is `64 + 2 × 256 × 68 ≈ 35 KiB` of `/dev/shm`.

**Sideband regions.** Two peers declare sideband regions for bulk data (separate SHM segments, [ADR 0005](adr/0005-payload-policy-and-sideband.md)):

| Owner | Region | Size | Class | Purpose |
|-------|--------|------|-------|---------|
| `vision_capture` (4) | `/rim_vision_nv12` | 8 MiB | `vision_nv12` | One 1080p NV12 frame per slot |
| `ml_inference` (5) | `/rim_ml_tensor_in` | 16 MiB | `ml_tensor_in` | Input tensor handed off from vision |
| `ml_inference` (5) | `/rim_ml_tensor_out` | 4 MiB | `ml_tensor_out` | Inference output handed back to controller |

Sideband regions are **owned by the peer**, not the router. The bridge process (vision, ml) creates them with `shm_open` + `ftruncate`; the router transports the descriptors (`sideband_idx` / `sideband_seq` / `sideband_len`) in `RouterFrame` v2 fields and never reads the bulk bytes.

**Cleanup.** Router unlinks-then-creates its own peer rings on bind. Sideband regions persist across router restarts; the [`rim-router-cleanup.sh`](../robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh) helper called by `rim-router.service`'s `ExecStopPost=` proactively `rm -f`s the anticipated `/dev/shm/rim_router_*` and `/dev/shm/rim_vision_*` / `/dev/shm/rim_ml_*` names.

### `x86_dev.toml` (UDS)

**Why UDS:** dev laptops may not have `CAP_IPC_LOCK` or a tmpfs sized for SHM rings, and UDS is plenty fast for non-realtime iteration (`make test-ipc` UDS round-trip ≈ 0.8 M trips/5 s, well above any human-perceptible threshold). UDS is also more debuggable — `strace -e network -p $PID` on a peer is human-readable; SHM atomics in `perf` traces are not.

**Path convention.** `/tmp/rim_router_*.sock`. Production-style x86 deployments (systemd + dedicated `rim` user) typically prefer `/run/rim/`, which is created by `install -d -o rim -g rim /run/rim`. The shipped profile uses `/tmp/` because `/run/` requires root and would break `make test-router`. To migrate, replace the path prefix in the TOML and the `ReadWritePaths=` directive in [`rim-router.service`](../robotics-ipc-module/deploy/systemd/rim-router.service).

**Demo CLI default-path mismatch.** The compile-time demo CLI defaults (`router_client sensor uds` with no explicit path) connect to `/tmp/rim_router_a.sock` per [`router_client_config.h`](../ipc/test/router_client_config.h), **not** to the descriptive paths in `x86_dev.toml`. When using the profile end-to-end, pass explicit paths:

```sh
./build/ipc/test/router_server --config config/profiles/x86_dev.toml &
./build/ipc/test/router_client sensor   uds /tmp/rim_router.sock /tmp/rim_router_sensor.sock &
./build/ipc/test/router_client recorder uds /tmp/rim_router.sock /tmp/rim_router_recorder.sock /tmp/rec.log
```

### `hil.toml` (UDP loopback)

**Why UDP on loopback:** the HIL workbench replaces hardware peers with mock processes. UDP on `127.0.0.1` is the simplest "wire" that survives `tcpdump -i lo -X` and lets a fault-injection tool craft truncated / unknown-source / replay datagrams (covered by [`fault_injection_test.cpp`](../ipc/test/fault_injection_test.cpp) D4 scenarios).

**Port allocation:**

| Peer | Port | Notes |
|------|------|-------|
| router listen | 19100 | |
| sensor (1) | 19101 | |
| controller (2) | 19102 | |
| recorder (3) | 19103 | |
| vision_capture (4) | 19104 | |
| ml_inference (5) | 19105 | |
| _mavlink_gateway (6)_ | _19106_ | _Reserved for F4_ |
| python_tooling (7) | 19107 | Added in F2 (UDP variant not yet wired in [`python_peer/`](../examples/bridges/python_peer/), which is UDS-only today) |
| dashboard_feed (8) | 19108 | |

### `sim_cloud.toml` (UDP, container subnet)

**Why UDP across a container subnet:** the orchestrator (Kubernetes, Docker Compose, etc.) assigns one container per peer. Cross-host SHM is not supported by the module ([ADR 0005](adr/0005-payload-policy-and-sideband.md) — SHM is single-host by definition).

**Address allocation:** `10.0.0.x:1920x` per peer (placeholder; orchestrator overrides). Address `10.0.0.7` is reserved for F4 (`mavlink_gateway`); `python_tooling` (7) lives at `10.0.0.8:19207`.

## Known limitations

These limitations are baked into F1 and will be revisited as Phase F progresses and the parked review surfaces during the post-phases pass. None are profile-file issues — they are router / routing API constraints that the profiles **document honestly** rather than hide.

### Single transport per router instance

The current router architecture is templated on one transport per `RouterServer<T>` instance. `ShmRouterLink::bind_router` silently skips non-SHM peers in the topology; `send_to_peer` throws if a route targets a peer it doesn't have a channel for. **Mixing SHM and UDS peers in one profile crashes the router on the first cross-transport forward.**

Consequence for F1: all six peers in `jetson_prod.toml` ride SHM, even though IO-heavy peers like the recorder (writes log files) and dashboard_feed (bridges to Node.js) would prefer UDS in production. Two workarounds an operator can apply today:

- **Multi-router topology.** Run a second router process with a UDS-only profile, and have one bridge peer subscribe to the SHM router and republish to the UDS router. Not in F1 scope.
- **Native SHM client in Node.js.** Use a small N-API addon to read the SHM ring directly. Heavy but feasible; the wire layout is documented in [ADR 0008](adr/0008-router-frame-v2.md) §RouterFrame v2.

The proper fix — a `MixedTransportRouterLink` that fans out across SHM/UDS/UDP from a single instance, or alternatively a factory-generated bridge daemon pattern between single-transport routers, or peer-side dual-protocol bridges — is captured in [parked review C11 §Mixed-transport networks](../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks) (three options + variants, decision rubric, co-design hint with [C5 §Declarative transport gaps](../robotics-ipc-module/plans/post-phases-robotics-review.md#c5--declarative-transport-layer-gaps)).

### 2-destination cap

`RouteRule` carries `dest0` + `dest1` only. A peer that needs to fan out to three or more destinations cannot be expressed in a single rule. With the **first-match-wins** lookup in [`routing.hpp::route_targets_for`](../ipc/src/router/routing.hpp), a second rule with the same source is **never evaluated** — the loop returns on the first match.

Consequence for F1: `dashboard_feed` (peer 8) has no inbound route. To deliver sensor / controller / vision / ml frames to a browser, an operator either:

- Runs the dashboard as a tap on the recorder process (recorder reads everything, re-emits to a WebSocket — needs custom code, not in F1).
- Picks a single source to mirror by adding one targeted rule (e.g. `[[routes]] source=2 dest=[3,8]` would send controller frames to recorder AND dashboard at the cost of dropping recorder-only as a single rule; trade-off documented).
- Waits for the parked-C5 fix.

### Dashboard limitation

For the reasons above, the F1 profiles **declare** peer 8 (`dashboard_feed`) for catalog completeness and resource-name reservation, but its in-router data path is empty. The F3 Node gateway sketch under [`examples/bridges/node_gateway/`](../examples/bridges/node_gateway/) covers what the bridge looks like; the routing answer lands when C5 closes.

### Peer 6 absent

`mavlink_gateway` (6) is a reserved ID in the [8-peer catalog](robotics-reference-layout.md#peer-catalog) but not declared yet. F4 will append it — the four profile files leave port / address ranges open. `python_tooling` (7) was added in F2 — see [`examples/bridges/python_peer/`](../examples/bridges/python_peer/).

## Resource-name conventions

All runtime resources use the `rim_` prefix per the [Phase D→E rename](../docs/adr/0004-robotics-module-boundaries.md) (Resource-name note):

| Kind | Convention | Examples |
|------|-----------|----------|
| Router listen address (SHM) | `/dev/shm/rim_router` | jetson_prod |
| Peer control-plane ring (SHM) | `/dev/shm/rim_router_<role>` | `rim_router_sensor`, `rim_router_controller`, `rim_router_vision_capture`, `rim_router_ml_inference`, `rim_router_recorder`, `rim_router_python_tooling`, `rim_router_dashboard` |
| Sideband region (SHM) | `/dev/shm/rim_<class>_<channel>` | `rim_vision_nv12`, `rim_ml_tensor_in`, `rim_ml_tensor_out` |
| Router listen address (UDS) | `/tmp/rim_router.sock` | x86_dev |
| Peer control-plane (UDS) | `/tmp/rim_router_<role>.sock` | `rim_router_sensor.sock`, `rim_router_python_tooling.sock`, etc. |
| Demo log file (UDS / SHM) | `/tmp/rim_router_<role>.log` | `rim_router_recorder.log` (recorder CSV) |

[`shm_leak_check.sh`](../robotics-ipc-module/scripts/shm_leak_check.sh) and [`rim-router-cleanup.sh`](../robotics-ipc-module/deploy/systemd/rim-router-cleanup.sh) both rely on the `rim_*` prefix to recognize module-owned resources.

## Operator hand-off checklist

When deploying to a new host:

1. **Pick a profile** from the [selector table](#profile-selector) above.
2. **Edit addresses** if the defaults don't match the host (e.g. swap `/tmp/` → `/run/rim/` for x86 production; substitute the `10.0.0.x` placeholders in `sim_cloud.toml` for the orchestrator-assigned subnet).
3. **Validate the profile** before installing:
   ```sh
   ./build/ipc/test/router_server --config <your-profile>.toml &
   sleep 1 && kill %1   # router exits cleanly if the profile parsed and bound
   ```
   A clean exit with no `shm:` / `bind` errors on stderr is the smoke test.
4. **Install the systemd units** per [`robotics-ipc-module/deploy/systemd/README.md`](../robotics-ipc-module/deploy/systemd/README.md). Point `ExecStart=` at the chosen profile.
5. **Configure peer environment files** at `/etc/rim/peer-<role>.env` (`RIM_TRANSPORT=shm|uds|udp`, `RIM_EXTRA_ARGS=...`) per [`rim-peer@.service`](../robotics-ipc-module/deploy/systemd/rim-peer@.service).
6. **Run the regression gate**:
   ```sh
   make ci   # full CI mirror; ~30 s on a dev box
   ```

## Forward references

| Topic | Where it lands |
|-------|----------------|
| Python bridge (peer 7) | **Phase F F2 implemented** — [`examples/bridges/python_peer/`](../examples/bridges/python_peer/) (UDS today; UDP variant deferred — see the bridge README) |
| Node dashboard (peer 8) implementation | Phase F F3; stub [`examples/bridges/node_gateway/`](../examples/bridges/node_gateway/) |
| MAVLink gateway (peer 6) | Phase F F4; stub [`examples/bridges/mavlink_gateway/`](../examples/bridges/mavlink_gateway/) |
| Vision + ML sideband `memory_class` parsing | Phase F F5; stub [`examples/bridges/vision_peer/`](../examples/bridges/vision_peer/) |
| Mixed-transport router fanout (SHM + UDS + UDP from one router) | Parked review C11 — three options (factory bridge daemons / mixed-transport router / peer-side bridging) + decision rubric |
| 2-destination cap, topic registry, QoS | Parked review C5 (declarative-transport gaps) |
| Replay-grade recorder | Parked review C4 (replay needs a richer log format than today's CSV) |
| TensorRT contract depth, CUDA memory_class, ARM CI, RT pinning, cross-host time, camera shape, consumption model | Parked review C1–C10 — see [plans/post-phases-robotics-review.md](../robotics-ipc-module/plans/post-phases-robotics-review.md) |

## References

- [config/profiles/](../config/profiles/) — the four TOML files
- [SYSTEM-VISION.md](../robotics-ipc-module/SYSTEM-VISION.md) — north-star deployment targets + peer catalog
- [docs/robotics-reference-layout.md](robotics-reference-layout.md) — Phase E E1 deployment shape; sister doc to this one
- [ipc/src/router/topology_loader.hpp](../ipc/src/router/topology_loader.hpp) — TOML schema reference
- [ADR 0004](adr/0004-robotics-module-boundaries.md) — module boundaries, `rim_*` resource-name convention
- [ADR 0005](adr/0005-payload-policy-and-sideband.md) — payload policy + sideband regions
- [ADR 0009](adr/0009-per-peer-ring-sizing.md) — per-peer SHM ring sizing
- [ADR 0010](adr/0010-router-timestamp-clock.md) — `CLOCK_MONOTONIC_RAW` timestamp policy
- [robotics-ipc-module/deploy/systemd/README.md](../robotics-ipc-module/deploy/systemd/README.md) — Phase E E2 systemd integration
- [plans/F-interoperability-bridges.md](../robotics-ipc-module/plans/F-interoperability-bridges.md) — Phase F plan
- [plans/post-phases-robotics-review.md](../robotics-ipc-module/plans/post-phases-robotics-review.md) — open backlog (C1–C11); C11 specifically addresses the mixed-transport limitation called out in [Known limitations](#known-limitations) above
