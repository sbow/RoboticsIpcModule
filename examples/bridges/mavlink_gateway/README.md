# `mavlink_gateway` — Phase F F4

> **Status:** Sketch only — Phase F F4 deliverable. No code in this directory yet.

A separate process that opens the MCU serial link (`/dev/ttyUSB*` or `/dev/ttyTHS*` on Jetson), parses MAVLink messages, and publishes compact status frames to the router. Subscribes to a controller route for outbound commands (mode change, arm, parameter set) and writes those back to the MCU.

**Peer ID:** 6 (`mavlink_gateway`)

## Contract

- [Peer catalog row](../../../docs/robotics-reference-layout.md#peer-catalog) — `mavlink_gateway` peer 6, "MAVLink parsed → status frames"
- [Integration pattern — MAVLink gateway](../../../docs/robotics-reference-layout.md#mavlink-gateway-mavlink_gateway-peer-6) — serial ownership, command direction, optional secondary UDP listener

## Phase F plan

[`robotics-ipc-module/plans/F-interoperability-bridges.md` → F4](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f4--mavlink-gateway-sketch) specifies:

- Serial open, parse MAVLink, emit compact `RouterFrame` status to router (mode, attitude summary, ack flags — in the 32 B inline payload)
- Commands: controller route → gateway → MCU
- **State: not implemented = document interface + ADR stub** (per the F plan — F4 may ship as a README + ADR before any binary)

## Sideband (optional, not in the F4 baseline)

For high-rate parameter dumps or log replay over MAVLink that exceeds the 32 B inline payload, use the `mavlink_bulk` sideband class declared in [ADR 0005](../../../docs/adr/0005-payload-policy-and-sideband.md). The F4 baseline does **not** include this; add only if a measured use case requires it.

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `mavlink.h` in `ipc/src/`.** MAVLink stays inside this gateway process.
- **No replacing MAVLink with `RouterFrame` on the wire to the MCU.** That is explicitly out of scope per [SYSTEM-VISION.md](../../../robotics-ipc-module/SYSTEM-VISION.md#out-of-scope-for-the-whole-system-this-repos-module). MAVLink is the MCU contract; `RouterFrame` is the in-host contract.
- **Serial device permissions.** systemd unit for this peer needs `SupplementaryGroups=dialout` (Debian / Ubuntu) or equivalent to access `/dev/ttyUSB*`.
