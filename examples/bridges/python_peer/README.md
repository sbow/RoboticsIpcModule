# `python_peer` — Phase F F2

> **Status:** Sketch only — Phase F F2 deliverable. No code in this directory yet.

A minimal Python subscriber / publisher matching the [`RouterFrame` v2](../../../docs/adr/0008-router-frame-v2.md) layout. Connects to the router over UDS (or UDP for HIL / sim_cloud). Useful for offline tooling and training scripts that need to ingest live router traffic without embedding into the C++ control loop.

**Peer ID:** 7 (`python_tooling`)

## Contract

The wire-level contract a Python implementation must mirror is documented in the Phase E E1 reference layout — read this **before** opening a Phase F session:

- [Peer catalog row](../../../docs/robotics-reference-layout.md#peer-catalog) — `python_tooling` peer 7, "matches v2 frame layout via ctypes"
- [Integration pattern — Python bridge](../../../docs/robotics-reference-layout.md#python-bridge-python_tooling-peer-7) — process shape, transport recommendation, anti-patterns

## Phase F plan

[`robotics-ipc-module/plans/F-interoperability-bridges.md` → F2](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f2--python-bridge-example) specifies:

- `ctypes` `struct` (or small C extension) for the 64 B `RouterFrame` v2 layout — fields `source` / `flags` / `topic_id` / `seq` / `timestamp_ns` / `sideband_idx` / `sideband_len` (uint48) / `sideband_seq` / 32 B payload, all host little-endian
- Smoke test: run against `router_server uds` on laptop
- Optional: JSON metadata path for ML tooling (not in hot path)

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `Python.h` in `ipc/src/`.** The C++ core stays Python-free. Python embedding (if ever needed) lives elsewhere.
- **No `pybind11` or CPython linkage in `libipc`.** The bridge is a *separate process*, not an in-process embedding.
- **Re-implement the frame layout, don't reach into the C++ headers.** Stable wire format = stable Python port; subtle ABI drift won't break the bridge silently.
