# `node_gateway` — Phase F F3

> **Status:** Sketch only — Phase F F3 deliverable. No code in this directory yet.

A Node.js process that subscribes to the router over UDS and broadcasts to browsers over WebSocket. Intended as the back-end for a developer dashboard or operations console. The C++ router has no Node API surface — the gateway parses the [`RouterFrame` v2](../../../docs/adr/0008-router-frame-v2.md) layout itself.

**Peer ID:** 8 (`dashboard_feed`)

## Contract

- [Peer catalog row](../../../docs/robotics-reference-layout.md#peer-catalog) — `dashboard_feed` peer 8, "reads everything; forwards to browser"
- [Integration pattern — Node dashboard gateway](../../../docs/robotics-reference-layout.md#node-dashboard-gateway-dashboard_feed-peer-8) — process shape, sideband decoding policy

## Phase F plan

[`robotics-ipc-module/plans/F-interoperability-bridges.md` → F3](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f3--node-dashboard-gateway-example) specifies:

- UDS client → WebSocket broadcast (TypeScript or JS)
- README-only build instructions; **no npm dependency** wired into the main `Makefile` unless behind an isolated target
- Frame layout port: TypeScript `interface` or `Buffer` decoder for the 64 B `RouterFrame` v2

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `node.h` / N-API headers in `ipc/src/`.** The C++ core stays JavaScript-free.
- **The gateway is a separate process.** No in-process Node embedding in `libipc`.
- **`npm install` does not gate the C++ build.** A fresh Jetson or CI runner with no Node toolchain still builds and tests the router; Node integration is opt-in.
