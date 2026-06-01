# ADR 0011: Device-bridge transports (UART / SPI / I2C / CAN)

- **Status:** Accepted
- **Date:** 2026-05-31
- **Builds on:** [ADR 0003](0003-transport-agnostic-router.md) (router supports three transports: SHM SPSC rings, UDS `SOCK_DGRAM`, UDP), [ADR 0004](0004-robotics-module-boundaries.md) ("the router transports; neighbours interpret"), [ADR 0005](0005-payload-policy-and-sideband.md) (control plane vs sideband; inline payload is for metadata, bulk goes via shared regions), [ADR 0008](0008-router-frame-v2.md) (the 64 B `RouterFrame` v2 wire surface bridges must mirror)
- **Closes (documentation portion):** [Phase F F4](../../robotics-ipc-module/plans/F-interoperability-bridges.md#f4--mavlink-gateway-sketch) — F4 ships as "interface + ADR stub" per the F plan. Working binary is deferred.
- **Scope:** Which IPC transport each common embedded-device bridge should use to connect its parsed output to the router on Jetson Orin (and similar Linux SBCs). Covers UART / MAVLink, SPI, I²C, and CAN / CAN-FD. Defines bridge layer responsibilities, what the router does not do, and the alternatives that were considered + rejected.

## Context

The robotics module already has bridges for two foreign-language consumers:

- **[F2 `python_peer`](../../examples/bridges/python_peer/)** — Python stdlib UDS `SOCK_DGRAM` peer that mirrors the v2 frame layout via `ctypes`.
- **[F3 `node_gateway`](../../examples/bridges/node_gateway/)** — Node stdlib UDP peer + WebSocket broadcast.

Both of those bridges talk to *language runtimes*, not to hardware. Phase F4 introduces the first **hardware-device bridge** — MAVLink over UART to a flight controller MCU — and that opens a broader design question. A typical Orin-based robot will eventually grow more device bridges:

| Device class | Linux kernel surface           | Typical use on a robot                          | Typical rate |
|--------------|--------------------------------|-------------------------------------------------|--------------|
| UART (TTL/RS-232/RS-485) | `/dev/ttyTHS<N>` / `/dev/ttyUSB<N>` via `termios` | MAVLink to flight controller; servo controller; UART-attached LIDAR | 50 Hz – 1 kHz parsed messages |
| SPI          | `/dev/spidev<bus>.<cs>` via `ioctl(SPI_IOC_MESSAGE)` | High-rate IMU, pressure / barometer, ADC, fast sensor arrays | 200 Hz – 4 kHz |
| I²C          | `/dev/i2c-<N>` via `ioctl(I2C_RDWR)` | Lower-rate environmental sensors (BME280, TMP117), small displays, IO expanders, smart batteries | 10 Hz – 200 Hz |
| CAN / CAN-FD | SocketCAN `AF_CAN` `SOCK_RAW` on `can<N>` / `vcan<N>` | Drive motors / VESC / EV-style stacks, MCU-to-MCU bus, automotive payload, J1939 | up to ~1 kHz frames (classic) / ~10 kfps (FD) |

Each of these is **below OSI layer 4** — they are physical / data-link interfaces with very different framing semantics (stream of bytes, fixed-size transfers, request/response transactions, discrete frames). The router's three transports — SHM SPSC rings, UDS `SOCK_DGRAM`, UDP — are all *transport-layer* shapes. **The bridge's job is to translate between the two layers.**

The router cannot grow a built-in MAVLink parser, a SocketCAN consumer, a `spidev` poll loop, or an `i2c_rdwr` driver. That would violate [ADR 0004](0004-robotics-module-boundaries.md): the router transports; neighbours interpret. Hardware bridges are the right neighbour for these protocols, in the same way `python_peer` and `node_gateway` are the right neighbours for foreign runtimes.

What this ADR fixes is the **router-facing transport** each hardware bridge should pick, and the per-interface rationale that drives the choice. The decisions are deliberately conservative and uniform — once an operator has a hardware bridge running, the router treats it indistinguishably from any other peer.

## Decision

| Device class       | Recommended bridge → router transport | Why                                                      |
|--------------------|---------------------------------------|----------------------------------------------------------|
| UART / **MAVLink** | UDS `SOCK_DGRAM` (peer 6 today)       | Bridge parses MAVLink stream → one `RouterFrame` per message; discrete-message semantics on the router edge match the v2 frame model |
| **SPI**            | SHM SPSC ring                         | Rate (≥ 1 kHz IMU class) + sample-shape (fixed-size, zero-copy desirable) + control-loop adjacency justify the SHM hot path |
| **I²C**            | UDS `SOCK_DGRAM`                      | Request/response transaction model fits dgram; rate too low to need SHM |
| **CAN / CAN-FD**   | UDS `SOCK_DGRAM`                      | SocketCAN already exposes discrete frame semantics; one CAN frame → one `RouterFrame` is a natural mapping |

All four are **bridges in their own process** under `examples/bridges/`. None of them require new transport support inside the router library — every recommendation uses an existing supported transport ([ADR 0003](0003-transport-agnostic-router.md), [ADR 0008](0008-router-frame-v2.md) wire format).

### The cross-cutting rule

> **A device-side stream / transaction / frame is the bridge's problem.**
> **A router-side `RouterFrame` is the bridge's output.**
> The router never speaks SPI, UART, I²C, or CAN — it only speaks SHM / UDS / UDP, and only in 64 B `RouterFrame` v2 units.

This is the same rule [ADR 0010](0010-router-timestamp-clock.md) applied to time: keep the hot path stateless and protocol-free, push protocol knowledge into the neighbour that has the right state and latency budget. A MAVLink parser, a SocketCAN reader, and an SPI poll loop are all stateful, blocking, and device-specific — they belong in a separate process.

## Per-interface rationale

### UART / MAVLink → UDS `SOCK_DGRAM` (peer 6, the F4 worked example)

**Physical surface.** On Jetson Orin the flight-controller MCU is typically wired to the dev-kit 40-pin header's Tegra High-Speed UART (`/dev/ttyTHS<N>`, `N` per L4T pin-mux config) or to an FTDI cable enumerating as `/dev/ttyUSB<N>`. UART exposes a stream of bytes — there is no message boundary at the kernel layer.

**MAVLink framing.** MAVLink v2 frames a self-delimited message inside that stream: STX byte + length + incompat-flags + compat-flags + sequence + sysid + compid + 3-byte msgid + payload (0…253 B) + 16-bit CRC + optional 13 B signature. Parsing requires a stream-aware state machine that resyncs on STX after any corruption. The reference C library (`mavlink/c_library_v2`) provides this; a custom parser is also tractable in a few hundred lines.

**Architecture.**

```
┌──────────────────┐  RX byte stream   ┌────────────────────┐  SOCK_DGRAM   ┌────────────────┐
│ /dev/ttyTHS<N>   │──────────────────▶│  mavlink_gateway   │──────────────▶│  controller    │
│ (MCU flight ctl) │   termios @ baud  │  (parse → frame)   │               │  recorder      │
│                  │◀──────────────────│                    │◀──────────────│  dashboard etc │
└──────────────────┘  TX byte stream   └────────────────────┘  SOCK_DGRAM
                       (MAVLink bytes)        ▲      ▼            (commands)
                                              │      │
                                       parse MAVLink  pack RouterFrame
                                       (one msg per   (one msg per
                                        recv-side fn)  send-side fn)
```

The bridge owns both directions:

- **Inbound (MCU → host).** Read UART bytes, run them through the MAVLink parser, and for each complete decoded message emit **one** `RouterFrame` to the router. Common compact MAVLink messages (`HEARTBEAT`, `ATTITUDE`, `SYS_STATUS`, `STATUSTEXT`, `COMMAND_ACK`) fit comfortably in the 32 B inline payload — pack the relevant fields into a stable schema and put the original MAVLink `msgid` in `topic_id` so subscribers can dispatch without re-parsing.
- **Outbound (host → MCU).** Subscribe to a controller-side route (e.g. `source = 2 dest = [6]` once the bridge ships). On each received `RouterFrame`, build the corresponding MAVLink message and `write()` it to the UART. Throttle / coalesce as needed (most MCU autopilots cannot absorb back-to-back parameter-set commands at hot-path rates).

**Why UDS `SOCK_DGRAM` for the bridge → router edge, not `SOCK_STREAM`.** This is the question the F4 brief asked explicitly, and the answer is deliberate.

The UART itself is a stream — that is true. But after the bridge has parsed MAVLink, **every message is a discrete unit with a known boundary**. Relaying parsed messages over `SOCK_STREAM` would mean:

1. **Re-streamifying after parsing.** The bridge already did the byte-by-byte state machine work; emitting back into a stream forces the router (or every subscriber) to redo the framing on its side. That's a regression on locality of knowledge.
2. **New transport in the router library.** The router today supports `SOCK_DGRAM` on UDS (see [`ipc/src/ipc/datagram.hpp`](../../ipc/src/ipc/datagram.hpp)). Adding `SOCK_STREAM` means a new `IpcEndpoint<UdsStream>`, a new `peer_id_from_recv<UdsStream>` (which can't resolve sources from a `sun_path` because connected streams have one — so the source-resolution model breaks), and a new framing layer to recover message boundaries from the byte stream (we just removed that complexity by parsing in the bridge). That's a lot of work for negative architectural value.
3. **Connection state in the router.** `SOCK_DGRAM` is connectionless — the router accepts datagrams from any bound peer and resolves their identity via [`peer_id_from_recv`](../../ipc/src/router/datagram_peer_resolver.hpp). `SOCK_STREAM` introduces accept loops, connection lifetime, half-close semantics, and a per-peer file descriptor that must be tracked. The router's "stateless hot path" property starts to leak.
4. **Composition with existing route rules.** [`RouteRule`](../../ipc/src/router/routing.hpp) operates on `(source, topic_id)` keys (closed C5 Scope A: up to 8 destinations; closed C5 Scope B: declarative topic registry; per-topic dispatch in [Phase G](../../robotics-ipc-module/plans/G-declarative-routing.md)). A stream-shaped peer either fits this model by being implicitly framed (which is what the bridge already does) or it doesn't fit at all. There is no third option that keeps the router's routing primitive coherent.

The user-facing summary: the bridge's *input* surface (the UART) is stream-shaped because the kernel API is. The bridge's *output* surface to the router stays datagram-shaped because that is what RouterFrame v2 already is.

**Profile shape (when the F4 binary lands).** Peer 6 is already reserved across all four F1 profiles (`x86_dev.toml`, `jetson_prod.toml`, `hil.toml`, `sim_cloud.toml` — see [`docs/deployment-profiles.md`](../deployment-profiles.md) §Peer catalog). When the implementation lands it will add:

```toml
[[peers]]
id    = 6
name  = "mavlink_gateway"
local = "uds:/tmp/rim_router_mavlink_gateway.sock"   # x86_dev variant

[[routes]]
source = 6                            # MCU status → recorder + dashboard
dest   = [3, 8]

[[routes]]
source = 2                            # controller commands → MCU
dest   = [6, 3, 8]                    # plus the existing recorder + dashboard taps
```

On `jetson_prod.toml` (all-SHM today) the bridge would use the same SHM region pattern as the other peers (`/rim_router_mavlink_gateway`), per [ADR 0009](0009-per-peer-ring-sizing.md) per-peer ring sizing.

### SPI → SHM SPSC ring

**Physical surface.** Jetson Orin's pin-muxed SPI bus is exposed as `/dev/spidev<bus>.<cs>` once the device tree enables the `spi-tegra114` driver for that pin set. The kernel API is full-duplex `ioctl(SPI_IOC_MESSAGE)` with `spi_ioc_transfer` descriptors — the bridge supplies tx + rx buffers and a clock speed.

**Why SHM.** Three factors push SPI bridges to the SHM SPSC ring rather than UDS/UDP:

1. **Rate.** A modern IMU (BMI088, ICM-42688, IIM-42652) clocks SPI at 1–10 MHz and is happy to deliver gyro + accel at 1–4 kHz. UART/MAVLink rates are an order of magnitude lower; CAN classic is bursty around 1 kHz max. At 4 kHz with 32-byte samples, the per-frame copy and syscall overhead of `SOCK_DGRAM` starts to compete with the actual data; the SHM SPSC path's sub-microsecond producer/consumer hop wins by orders of magnitude.
2. **Sample shape.** SPI sensor frames are fixed-size and small (typically 12–48 bytes raw, plus per-sample metadata). They map cleanly into the SPSC ring's slot format, fit comfortably in the 32 B `RouterFrame` inline payload, and don't need a sideband region.
3. **Control-loop adjacency.** A common pattern is to put the high-rate IMU on the same compute that runs the control loop, with a tight latency budget (sub-1ms publisher-to-controller). SHM is the only one of the three transports that gives that latency without measurement noise.

**Architecture.**

```
┌──────────────┐  SPI_IOC_MESSAGE   ┌────────────────┐   SHM SPSC ring   ┌─────────────────┐
│ /dev/spidev  │───────────────────▶│  spi_bridge    │──────────────────▶│  controller     │
│ (IMU, ADC,   │   fixed-size       │  (poll → pack  │   /rim_router_<n> │  (or any        │
│  baro, etc.) │   transfers        │   → push slot) │   per-peer ring   │   consumer)     │
└──────────────┘                    └────────────────┘                   └─────────────────┘
                                          ▲   │
                                     spidev ioctl   ring push (try_send)
                                     loop (timer    matches existing C++
                                     or DRDY irq    peer code paths exactly
                                     poll())        — same primitive as
                                                    every other SHM peer.
```

The bridge poll loop typically does:

```c
// pseudo-code
while (running) {
    spi_ioc_transfer xfers[2] = { … };
    ioctl(fd, SPI_IOC_MESSAGE(2), xfers);
    pack_sample_into_inline_payload(&frame, &rx_buf);
    frame.seq         = seq++;
    frame.topic_id    = TOPIC_IMU_PROPRIO;
    frame.timestamp_ns = router_now_ns();   // ADR 0010
    shm_try_push_slot(ring, &frame);        // drops on full per ADR 0006
}
```

`shm_try_push_slot` is the existing primitive in [`ipc/src/router/shm_router_link.hpp`](../../ipc/src/router/shm_router_link.hpp); drop-on-full + per-peer metrics are inherited for free per [ADR 0006](0006-shm-backpressure-and-metrics.md). If a single sample exceeds 32 B (rare — full 6-DOF IMU with timestamps fits comfortably), the bridge can move the bulk into a per-peer sideband region per [ADR 0005](0005-payload-policy-and-sideband.md) and keep summary stats in the inline payload.

**Why not UDS dgram for SPI?** It works at sub-kHz rates and is fine for a one-off prototype, but the resulting profile lies — operators reading `x86_dev.toml` see "IMU on UDS" and underbudget their latency. The recommendation says SHM so the deployment-time intent is honest.

### I²C → UDS `SOCK_DGRAM`

**Physical surface.** `/dev/i2c-<N>` accessed via `ioctl(I2C_RDWR)` with an `i2c_rdwr_ioctl_data` argument that batches a list of `i2c_msg` (each one a read or write to a 7-bit slave address). Per-transaction overhead is dominated by the bus clock and slave's ACK behaviour.

**Why UDS dgram.** I²C is rate-limited by the bus itself (typically 100 kHz standard mode, 400 kHz fast mode, 1 MHz fast-mode-plus on Orin). A reasonable sensor mix saturates well below 200 Hz aggregate per bus. At those rates, the SHM path is overkill — the syscall cost of `recvfrom` is invisible. UDS dgram is also a more obvious code path for the bridge's request/response semantics ("read 6 bytes from BMP390 at 0x77, decode, emit") and aligns the I²C bridge with the other low-rate peers (`recorder`, `dashboard_feed`, `python_tooling`).

If an I²C bus is misused at near-clock-limit rates for some sustained burst, the bridge can promote to SHM on a per-bus basis without redesigning anything — `RouterTopology` already lets each peer pick its transport independently within the constraints of [`docs/deployment-profiles.md`](../deployment-profiles.md) §single-transport-per-router (parked C11). For the F4 baseline the recommendation is UDS dgram.

### CAN / CAN-FD → UDS `SOCK_DGRAM`

**Physical surface.** [SocketCAN](https://www.kernel.org/doc/html/latest/networking/can.html) exposes the bus as an `AF_CAN` `SOCK_RAW` socket bound to a `can<N>` or `vcan<N>` interface. `read()` returns one `struct can_frame` (classic, 8 B payload) or `struct canfd_frame` (FD, 64 B payload) per call. Critically — and this is the architectural fit — **kernel SocketCAN already speaks discrete-message semantics**, with full per-frame metadata (CAN ID, RTR / FD flags, DLC, timestamp).

**Why UDS dgram.** One CAN frame in → one `RouterFrame` out. The CAN ID (11-bit standard or 29-bit extended) can map directly onto `RouterFrame.topic_id` (with a documented escape for the 29-bit case, e.g. shift the extended-ID flag into `flags`). The CAN payload fits in the 32 B inline payload (classic CAN's 8 B trivially; CAN-FD's up to 64 B uses the sideband region named for `mavlink_bulk`-style bulk classes per [ADR 0005](0005-payload-policy-and-sideband.md), or a dedicated `can_bulk` class if it surfaces).

**Architecture.**

```
┌──────────┐   read()    ┌─────────────────┐  SOCK_DGRAM   ┌──────────────┐
│ /dev/can0│────────────▶│  can_bridge     │──────────────▶│  consumer A  │
└──────────┘             │  (SocketCAN →   │               └──────────────┘
                         │   RouterFrame)  │  SOCK_DGRAM   ┌──────────────┐
                         │                 │──────────────▶│  consumer B  │
                         └─────────────────┘               └──────────────┘
                                                                  │
                                                            (if off-host)
                                                                  ▼
                                                            UDP gateway peer
```

This is, deliberately, the same shape as the MAVLink and I²C bridges — once the operator has built one device bridge, the next one is recognisably the same. The router does not care which kernel interface the bridge is talking to upstream; from the router's point of view it is just another UDS-bound peer pushing `RouterFrame` v2 datagrams.

**Why not SHM for CAN?** Rate per bus is bounded (~1 kHz classic, ~10 kHz FD bursty) and frames carry a small payload — the SHM win is modest. Keeping CAN on UDS dgram matches every other discrete-message bridge and avoids an SHM region per CAN bus on a host that may have multiple controllers wired up.

## Out of scope (delegated)

- **Vision capture (CSI / V4L2 / USB cameras).** Handled by the [`vision_capture`](../../examples/bridges/vision_peer/) bridge (peer 4) with bulk pixel data in the `vision_nv12` / `vision_jpeg` sideband regions per [ADR 0005](0005-payload-policy-and-sideband.md). Not in this ADR; F5 sketch addresses it separately.
- **ML inference / CUDA tensors.** Handled by the [`ml_inference`](../../robotics-ipc-module/SYSTEM-VISION.md) peer (peer 5) with tensor data in `ml_tensor_in` / `ml_tensor_out` sideband regions per [ADR 0005](0005-payload-policy-and-sideband.md). The bridge ADR for ML is also future work; parked under [C1 TensorRT integration contract](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c1--tensorrt-integration-contract) and [C2 CUDA / sideband `memory_class` parsing](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c2--cuda--sideband-memory_class-parsing).
- **Audio (ALSA / I²S).** Not part of the current peer catalog. Pattern would mirror CSI camera — bulk audio frames in a sideband region, metadata inline.
- **GPIO interrupts.** Edge events fit the dgram model trivially (one event = one `RouterFrame`); no separate ADR needed.
- **Mixed-transport routers.** Parked under [C11](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks); needed when one router instance must serve SHM + UDS + UDP peers simultaneously (e.g. fast-path SPI IMU on SHM alongside slow-path I²C sensor on UDS in the same `jetson_prod.toml`).

## Alternatives considered

### A — Route raw device streams through the router

A version of "let the router own protocol decoding" — register `/dev/ttyTHS0`, `/dev/spidev0.0`, `/dev/i2c-7`, `can0` directly with the router and let subscribers decode.

- **Reject reason:** Catastrophic for [ADR 0004](0004-robotics-module-boundaries.md). The router would grow MAVLink, SocketCAN, SPI, I²C drivers — each with its own dependencies, init sequences, blocking semantics, and failure modes. The hot path stops being stateless. This is the option this ADR exists to refuse.

### B — `SOCK_STREAM` UDS bridge edge (the F4 brief's "consider alternatives")

Use `SOCK_STREAM` on the bridge → router edge so the bridge is a stream pump and consumers parse messages themselves.

- **Reject reason:** Spelled out in detail under "Why UDS `SOCK_DGRAM` for the bridge → router edge" above. Short version: re-streamifying after parsing is a regression on locality of knowledge; adding `SOCK_STREAM` to the router library breaks the `peer_id_from_recv` source-resolution model and adds connection-lifetime state to the hot path; the existing routing primitive operates on discrete frames, not byte ranges.

### C — Separate transport per device class beyond what's listed

Add device-shaped transports (e.g. a "CAN ID demultiplexing" transport, a "MAVLink heartbeat" transport).

- **Reject reason:** That's the *router* doing protocol-aware routing. Phase G's per-topic routing is the right primitive to compose — once the topic registry is dispatch-coupled, a consumer can declare "I want MAVLink msgid=ATTITUDE, source=mavlink_gateway" without the router needing MAVLink knowledge. Until then, single-topic-id-per-message-class on the bridge side is enough.

### D — Mixed SHM + dgram for the same device family

E.g. MAVLink heartbeats on UDS but parameter dumps on SHM.

- **Reject reason:** Doable later via the sideband mechanism (heartbeat in inline payload, bulk dumps in `mavlink_bulk` sideband). Doesn't justify a separate "MAVLink-via-SHM-too" transport.

### E — Defer all device-bridge decisions until they're each individually built

- **Reject reason:** This is the F4 deliverable. The F plan explicitly allows F4 to ship as "interface + ADR stub" — the operator-facing value is *the decision matrix*, written down once, so a downstream user building any of these four bridges has an obvious starting point. Waiting until each bridge ships would mean four ADRs scattered across phases; this consolidation is cheaper to maintain.

## Consequences

### Positive

- **Uniform discoverability.** An operator who needs to add CAN support reads this ADR, finds the recommended transport, picks a peer ID, and follows the same shape as MAVLink / I²C. The bridge layer feels like one thing, not four ad-hoc things.
- **No router library changes required.** Every recommendation uses an existing transport. The bridge tree under `examples/bridges/` grows; `ipc/src/router/` does not.
- **Stable cross-references.** ADR 0011 anchors the decision; the F4 README points at it for MAVLink, and future SPI / I²C / CAN bridge READMEs point at it for their interface.
- **Routing primitive unaffected.** `RouteRule`, `RouterTopology`, and `peer_id_from_recv` keep working as-is. Bridges plug in at the same level any other peer does.
- **Sideband pattern reused.** For interfaces that occasionally need bulk transfer (CAN-FD frames near the 64 B max, MAVLink parameter dumps, large I²C blocks), the existing [ADR 0005](0005-payload-policy-and-sideband.md) sideband mechanism is the answer — no new wire surface.

### Negative

- **Bridges remain user-built.** This ADR documents the shape; it does not ship working bridges for SPI / I²C / CAN. F4's worked example is MAVLink, and even that ships as README + ADR per the F plan. A real bridge ecosystem is downstream work.
- **No automated profile validation against the ADR.** A profile that puts the IMU on UDS dgram will still parse and run; the topology loader cannot tell that the operator violated the SPI-prefers-SHM recommendation. The check is operator-facing only.
- **Per-bus tuning is left to the operator.** SHM ring sizing for a 4 kHz IMU bridge is per-bridge work; this ADR points at [ADR 0009](0009-per-peer-ring-sizing.md) but does not prescribe specific `shm_slot_count` / `shm_max_payload` values per device class.

### Neutral

- **The router does not learn any new transport.** That is the point.
- **Sideband classes can grow.** [ADR 0005](0005-payload-policy-and-sideband.md) is the right home for any new bulk class needed by a device bridge (e.g. `can_bulk`, `spi_log`) — this ADR doesn't preclude additions, it just doesn't introduce them.
- **Phase G (per-topic routing) does not change this ADR's recommendation.** Once per-topic routing lands, a CAN consumer can subscribe to specific CAN IDs without re-parsing — the *bridge* still uses UDS dgram, the *consumer* gets finer-grained dispatch from the router. Composes cleanly.

## Verification

- This ADR is documentation-only. No code change ships with it.
- The F4 README (`examples/bridges/mavlink_gateway/README.md`) cross-references this ADR and uses the recommended UDS dgram pattern when it lands as a working binary.
- Future SPI / I²C / CAN bridge READMEs follow the same template (peer id + transport + bridge process shape + ADR 0011 link).
- The peer-6 reservation in all four F1 profiles (`x86_dev.toml`, `jetson_prod.toml`, `hil.toml`, `sim_cloud.toml`) remains intact — the ADR documents the eventual profile shape but does not declare peer 6 yet (consistent with F4's "interface + ADR stub" deliverable).

## References

- [ADR 0003 — Transport-agnostic router](0003-transport-agnostic-router.md): router supports SHM / UDS dgram / UDP transports through a uniform peer-address abstraction
- [ADR 0004 — Robotics module boundaries](0004-robotics-module-boundaries.md): "the router transports; neighbours interpret" — the policy bridges enforce
- [ADR 0005 — Payload policy and sideband](0005-payload-policy-and-sideband.md): inline 32 B for metadata, sideband region for bulk; class names (`mavlink_bulk`, `vision_nv12`, `ml_tensor_*`) and the SHM region convention
- [ADR 0006 — SHM backpressure and metrics](0006-shm-backpressure-and-metrics.md): drop-on-full + per-peer drop attribution that high-rate SPI bridges inherit for free
- [ADR 0008 — RouterFrame v2](0008-router-frame-v2.md): the 64 B wire surface every bridge mirrors
- [ADR 0009 — Per-peer SHM ring sizing](0009-per-peer-ring-sizing.md): `shm_slot_count` / `shm_max_payload` parameters every SHM-using bridge inherits
- [ADR 0010 — Router timestamp clock](0010-router-timestamp-clock.md): bridges that stamp their own frames use `router_now_ns()` for single-host comparability
- [Phase F plan F4](../../robotics-ipc-module/plans/F-interoperability-bridges.md#f4--mavlink-gateway-sketch)
- [Phase G — Declarative routing](../../robotics-ipc-module/plans/G-declarative-routing.md) — per-topic dispatch will let consumers subscribe to specific MAVLink msgid / CAN ID streams without the router learning the protocols
- [Post-phases robotics-integration review C11 — mixed-transport networks](../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks): needed when one router instance must serve SHM + UDS + UDP peers simultaneously (e.g. SPI IMU on SHM + I²C / CAN on UDS in the same profile)
- [`examples/bridges/mavlink_gateway/README.md`](../../examples/bridges/mavlink_gateway/README.md): F4 interface stub, the worked example that this ADR refers to
- SocketCAN: <https://www.kernel.org/doc/html/latest/networking/can.html>
- L4T pin-mux + spidev / i2c-dev / serial-tegra docs: <https://docs.nvidia.com/jetson/archives/r36.4.3/DeveloperGuide/HR/JetsonModuleAdaptationAndBringUp/JetsonAgxOrinSeries.html>
