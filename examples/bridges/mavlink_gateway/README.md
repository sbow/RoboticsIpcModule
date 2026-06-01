# `mavlink_gateway` — Phase F F4

> **Status:** Interface + ADR stub — per the F plan, F4 may ship as a README + ADR before any binary. **No working code in this directory yet.** The transport recommendation, peer-id reservation, profile shape, and command-flow surface are all locked; what's missing is the parser binary itself.

A separate process that opens the MCU serial link (`/dev/ttyTHS<N>` on Jetson Orin, `/dev/ttyUSB<N>` via FTDI on x86 dev), parses MAVLink v2 messages, and publishes compact status frames to the router. Subscribes to a controller-side route for outbound commands (mode change, arm, parameter set) and writes the corresponding MAVLink bytes back to the MCU.

**Peer ID:** 6 (`mavlink_gateway`)

**Transport (bridge → router):** UDS `SOCK_DGRAM` — see [ADR 0011 §UART / MAVLink](../../../docs/adr/0011-device-bridge-transports.md#uart--mavlink--uds-sock_dgram-peer-6-the-f4-worked-example) for the full rationale and the alternatives considered (notably `SOCK_STREAM` passthrough, considered + rejected).

```
examples/bridges/mavlink_gateway/
└── README.md             ← you are here (interface + ADR stub)
```

When the working binary lands it will follow the [bridges layout convention](../README.md#layout-convention-phase-f-target):

```
examples/bridges/mavlink_gateway/
├── README.md             ← updated to "implemented" status
├── frame_layout.h        ← MAVLink-msgid → topic_id mapping table
├── mavlink_gateway.cpp   ← serial open + parse + RouterFrame translator
├── Makefile              ← standalone (NOT wired into top-level make all)
└── smoke.sh              ← optional end-to-end with a virtual serial pair
```

## Why this is "interface + ADR" today

The Phase F plan's F4 deliverable line reads:

> **State: not implemented = document interface + ADR stub**

A working MAVLink bridge needs three pieces that this repo deliberately stays out of:

1. **A MAVLink C library.** The reference choice is [`mavlink/c_library_v2`](https://github.com/mavlink/c_library_v2) (header-only, autogen). Pinning a specific version + dialect (`common`, `ardupilotmega`, `standard`) is a downstream choice — different flight stacks ship different dialects, and the bridge author needs to match.
2. **An MCU you can talk to.** The bridge is meaningless without a flight controller (or a SITL instance like ArduPilot SITL / PX4 SITL) on the other end of the UART. Operator-specific hardware.
3. **A baudrate / parity / framing config that matches the MCU.** 57600 baud N81 is the MAVLink-over-serial default, but Pixhawk / Cube / Holybro autopilots vary. systemd unit `EnvironmentFile=` is the cleanest way to thread this through.

None of those belong in the module's CI loop. What this README + [ADR 0011](../../../docs/adr/0011-device-bridge-transports.md) lock down is the **interface that the eventual binary will present to the router**, so a downstream user can build the bridge against a stable contract.

## Contract on `RouterFrame` fields

| Field            | Bridge-side responsibility                                                                                          |
|------------------|---------------------------------------------------------------------------------------------------------------------|
| `source`         | Rewritten by the router on forward to `6` (resolved from the bridge's bound UDS path). Bridge does not need to set it correctly. |
| `flags`          | `priority` bits per [ADR 0008](../../../docs/adr/0008-router-frame-v2.md). `has_sideband` set only when bulk MAVLink data spills into a sideband region (see below). |
| `topic_id`       | u16 holding the MAVLink `msgid` (which is up to 24-bit in MAVLink v2 — bridge truncates to the 16 LSBs and rejects anything that doesn't fit; documents this in `frame_layout.h`). |
| `seq`            | Monotonic per-message counter assigned by the bridge. **Not** the MAVLink sequence — that's per (sysid, compid) and lives in the payload schema instead. |
| `timestamp_ns`   | Rewritten by the router on forward to `router_now_ns()` per [ADR 0010](../../../docs/adr/0010-router-timestamp-clock.md). The bridge may also stamp at receive-from-MCU time to expose MCU-to-host latency in the inline payload. |
| `sideband_*`     | Set only for bulk MAVLink data (parameter dumps, log replay) — see [Sideband](#sideband-optional) below. Default to `kSidebandIdxNone` for the heartbeat / status flow. |
| `payload` (32 B) | Compacted MAVLink message body. See [Inline payload schema](#inline-payload-schema-mapping-mavlink--routerframe). |

## Inline payload schema (mapping MAVLink → `RouterFrame`)

The bridge's job is to **compact MAVLink messages into the 32 B inline payload**, not to relay the raw MAVLink byte stream. The compaction is per-message, schema-stable across deployments, and lives in `frame_layout.h` so consumers can decode without a MAVLink dependency.

Reference compactions for the common high-rate MAVLink messages:

| MAVLink msgid | Name             | Compacted inline payload (≤ 32 B)                                                  | Notes                                                                 |
|---------------|------------------|-------------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `0`           | `HEARTBEAT`      | `u8 type`, `u8 autopilot`, `u8 base_mode`, `u32 custom_mode`, `u8 system_status`   | Hot — typically 1 Hz                                                  |
| `30`          | `ATTITUDE`       | `u32 time_boot_ms`, `f32 roll, pitch, yaw`, `f32 rollspeed, pitchspeed, yawspeed`  | 28 B, exactly fits with 4 B free                                      |
| `1`           | `SYS_STATUS`     | `u32 onboard_control_sensors_present`, `u32 *_enabled`, `u16 voltage_battery, current_battery`, `i8 battery_remaining` | Truncated — drop the less-interesting sensor masks if needed          |
| `253`         | `STATUSTEXT`     | `u8 severity` + ASCII text (truncated to 31 B, NUL-terminated)                      | Use the C++ `payload_bytes()` rstrip convention                       |
| `77`          | `COMMAND_ACK`    | `u16 command`, `u8 result`, `u8 progress`, `i32 result_param2`, `u8 target_system, target_component` | Bridge → controller plumbing                                          |

Messages with payloads > 32 B (parameter dumps, mission downloads, log fragments) take one of:

- **Drop and warn** if the operator hasn't enabled the bulk path. The router log shows a warning; the metric counter for the bridge increments.
- **Sideband** if the operator has set up an `mavlink_bulk` region (see below).

## Command flow (controller → bridge → MCU)

The reverse direction lets an in-host peer (typically `controller`, peer 2) send a command that the bridge translates back into MAVLink and writes to the UART.

```
┌──────────────┐  RouterFrame   ┌──────────────────┐  RouterFrame   ┌────────────────────┐  MAVLink TX  ┌──────────┐
│  controller  │───────────────▶│      router      │───────────────▶│  mavlink_gateway   │─────────────▶│   MCU    │
│  (peer 2)    │  topic_id =    │  route source=2  │  source=2,     │  decodes topic_id, │  termios     │ (flight  │
│              │  MAVLink msgid │  dest=[6, 3, 8]  │  topic_id=…    │  builds MAVLink,   │  write()     │  ctrl)   │
└──────────────┘                └──────────────────┘                │  write to UART     │              └──────────┘
                                                                    └────────────────────┘
```

The same route shape gives **the recorder + dashboard a free copy** of every controller-issued MCU command — useful for replay and live debugging. With per-topic routing (Phase G, formerly C5 Scope C), a subscriber can opt into only the MAVLink command flow without subscribing to the rest of the controller's traffic.

The bridge **must** throttle or coalesce upstream writes. Most autopilots cannot absorb back-to-back parameter-set commands at the router's hot-path rates, and the UART itself has a hard bandwidth limit (57600 baud ≈ 5.7 kB/s). The bridge owns this rate-limiting; the router is rate-agnostic.

## Profile shape (when the binary lands)

Peer 6 is **reserved but not declared** in all four F1 profiles (port 19106 in `hil.toml`; `10.0.0.7` in `sim_cloud.toml`; SHM region name `rim_router_mavlink_gateway` in `jetson_prod.toml`; UDS socket path slot in `x86_dev.toml`). Once the binary lands, the profile delta is:

```toml
# x86_dev.toml — append:
[[peers]]
id    = 6
name  = "mavlink_gateway"
local = "uds:/tmp/rim_router_mavlink_gateway.sock"

[[routes]]
source = 6                            # MCU status → recorder + dashboard
dest   = [3, 8]

# Replace the existing `source = 2` rule to include peer 6 as a command destination:
# old: source = 2 ; dest = [3, 7, 8]
# new: source = 2 ; dest = [3, 6, 7, 8]
```

The same change template applies to `hil.toml`, `sim_cloud.toml`, and `jetson_prod.toml` — only the transport family changes (UDP / UDP / SHM respectively). The route count goes from 5 to 6; the loader and existing integration tests already handle larger profiles (closed C5 Scope A's `kMaxRouteDests = 8` covers the wider `source = 2` fanout).

## Process supervision (systemd)

The eventual binary will follow the F2 / F3 pattern: standalone process, `examples/bridges/mavlink_gateway/Makefile` builds it independently, and the `robotics-ipc-module/deploy/systemd/rim-peer@.service` template starts it via `EnvironmentFile=-/etc/rim/peer-mavlink_gateway.env` with at minimum:

```ini
# /etc/rim/peer-mavlink_gateway.env
RIM_TRANSPORT=uds                                     # SHM on jetson_prod
RIM_EXTRA_ARGS=--device /dev/ttyTHS1 --baud 921600 --sysid 1 --compid 191
```

**Serial device permissions.** systemd unit needs `SupplementaryGroups=dialout` (Debian / Ubuntu) so the bridge can open `/dev/ttyTHS*` without running as root. On L4T this group exists by default. Alternative: a udev rule that grants the `rim` user direct access to the specific tty device.

## Sideband (optional, not in the F4 baseline)

For MAVLink parameter dumps, log replay, or `MISSION_ITEM_INT` bursts that exceed the 32 B inline payload, use the `mavlink_bulk` sideband class declared in [ADR 0005](../../../docs/adr/0005-payload-policy-and-sideband.md). The F4 baseline does **not** include this path; add only if a measured use case requires it (typically `PARAM_VALUE` floods during startup or `LOG_DATA` replay sessions).

The sideband region name follows the `rim_<peer>_<class>` convention: `/rim_mavlink_bulk` mapped at the bridge's preferred size (operator decision; ADR 0009 per-peer ring sizing applies if the bridge promotes to SHM transport too).

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `mavlink.h` in `ipc/src/`.** MAVLink stays inside this gateway process.
- **No replacing MAVLink with `RouterFrame` on the wire to the MCU.** That is explicitly out of scope per [SYSTEM-VISION.md](../../../robotics-ipc-module/SYSTEM-VISION.md#out-of-scope-for-the-whole-system-this-repos-module). MAVLink is the MCU contract; `RouterFrame` is the in-host contract.
- **No raw byte-stream relay through the router.** The bridge parses MAVLink, then emits one `RouterFrame` per message. See [ADR 0011 §UART / MAVLink](../../../docs/adr/0011-device-bridge-transports.md#uart--mavlink--uds-sock_dgram-peer-6-the-f4-worked-example) for the `SOCK_STREAM` alternative and why it was rejected.
- **No router-side MAVLink parser.** The router does not learn message ids, sequence handling, or dialect dispatch.
- **Serial-port ownership is the bridge's.** Exactly one `mavlink_gateway` instance opens `/dev/ttyTHS*` at a time (the UART driver enforces this in the kernel). For multi-MCU rigs, run multiple bridge processes — each binds to a distinct peer id, opens its own device, and connects to the router separately.

## Related documents

- [ADR 0011 — Device-bridge transports](../../../docs/adr/0011-device-bridge-transports.md) — **the** decision record for this and the SPI / I²C / CAN bridges' transport choice
- [ADR 0004 — Robotics module boundaries](../../../docs/adr/0004-robotics-module-boundaries.md) — module surface
- [ADR 0005 — Payload policy and sideband](../../../docs/adr/0005-payload-policy-and-sideband.md) — `mavlink_bulk` class
- [ADR 0008 — RouterFrame v2](../../../docs/adr/0008-router-frame-v2.md) — wire layout
- [ADR 0010 — Router timestamp clock](../../../docs/adr/0010-router-timestamp-clock.md) — `timestamp_ns` semantics
- [Phase F plan F4](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f4--mavlink-gateway-sketch)
- [`docs/robotics-reference-layout.md`](../../../docs/robotics-reference-layout.md#mavlink-gateway-mavlink_gateway-peer-6) — peer-6 integration pattern
- [`docs/deployment-profiles.md`](../../../docs/deployment-profiles.md) — profile selector + peer-6 reservation table
- [`examples/bridges/python_peer/README.md`](../python_peer/README.md) — F2 reference for stdlib-only UDS dgram peer (the F4 working binary will follow the same shape, in C++ rather than Python)
- [`examples/bridges/node_gateway/README.md`](../node_gateway/README.md) — F3 reference for stdlib-only UDP peer + WebSocket
- [Phase G — Declarative routing](../../../robotics-ipc-module/plans/G-declarative-routing.md) — per-topic dispatch will let MAVLink consumers subscribe to specific `msgid`s without re-parsing
- [SocketCAN / SPI / I²C device-bridge guidance](../../../docs/adr/0011-device-bridge-transports.md#per-interface-rationale) — same ADR, sibling sections
