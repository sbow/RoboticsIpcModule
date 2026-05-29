# `python_peer` — Phase F F2 (implemented)

A minimal Python bridge that publishes and subscribes to the C++ router over UDS, matching the [`RouterFrame` v2](../../../docs/adr/0008-router-frame-v2.md) wire layout via `ctypes`.

**Peer ID:** 7 (`python_tooling`)

**Status:** F2 deliverable — usable today; pure stdlib (no third-party Python deps).

```
examples/bridges/python_peer/
├── README.md            ← you are here
├── rim_router_frame.py  ← RouterFrame v2 ctypes Structure (64 B byte-for-byte port)
├── rim_router_peer.py   ← UDS subscriber + publisher
├── subscribe.py         ← CLI: receive + decode + print
├── publish.py           ← CLI: build + send N frames
└── smoke.sh             ← end-to-end smoke against x86_dev.toml
```

## What this bridge is for

- **Offline tooling / training scripts** that need to ingest live router traffic without embedding into the C++ control loop.
- **Lightweight publishers** (mock sensors, scripted test stimuli, replay-from-CSV experiments) that feed the router during development.
- **A worked reference** for any other-language bridge that needs to mirror the v2 frame layout — the byte-offset assertions in `rim_router_frame.py` and `rim_router_peer.py` document the contract precisely.

## Quick start (x86 dev laptop, UDS)

From the repo root, the fastest path is the [smoke script](#smoke-script) below — it spins everything up and verifies wire compatibility in ~2 seconds. To poke at things by hand:

```bash
make all                                            # builds router_server
./build/ipc/test/router_server \
    --config config/profiles/x86_dev.toml &         # router listens on /tmp/rim_router.sock

# In one terminal — publish 5 frames as peer 7 (python_tooling):
python3 examples/bridges/python_peer/publish.py --count 5
```

The default `x86_dev.toml` route `source = 7 dest = [3]` sends every publish to the recorder. The router log shows:

```
[info] router: python_tooling -> recorder payload=python-tooling-0
[info] router: python_tooling -> recorder payload=python-tooling-1
...
```

To also **subscribe** as the python peer, the route `source = 2 dest = [3, 7]` taps controller traffic into peer 7. In one terminal:

```bash
python3 examples/bridges/python_peer/subscribe.py
```

In another, start a controller (this also wants a sensor publishing into it):

```bash
./build/ipc/test/router_client controller uds /tmp/rim_router.sock &
./build/ipc/test/router_client sensor    uds /tmp/rim_router.sock
```

Now the subscriber prints lines like:

```
[recv] source=controller(id=2) topic=0 seq=0 ts_ns=12345678901 payload=b'control-0'
```

**Note:** the router rewrites `timestamp_ns` on forward (per [ADR 0010](../../../docs/adr/0010-router-timestamp-clock.md)), so the value the subscriber sees is the router's `CLOCK_MONOTONIC_RAW`, not the publisher's. That's intentional — subscribers should treat `timestamp_ns` as a router-anchored arrival stamp.

## Smoke script

```bash
make all
./examples/bridges/python_peer/smoke.sh
```

`smoke.sh` is a fully self-terminating end-to-end test that runs **Python on both ends**:

```
publish.py (peer 7) --[UDS]--> router_server --[UDS]--> subscribe.py (peer 3)
```

What it does:

1. Writes a minimal 2-peer profile under `build/python_peer_smoke/` (peers 3 + 7 only, with route `source = 7 dest = [3]`).
2. Starts `router_server --config <that profile>` under `ROUTER_TEST=1` so the router self-exits on idle (no kill signals needed — useful in sandboxes that block cross-process kill).
3. Starts `subscribe.py` bound at the peer-3 path with `--count 5` so it exits after receiving 5 frames.
4. Runs `publish.py` for 5 frames as peer 7.
5. Verifies the router log shows 5 forward lines, the subscriber decoded 5 frames, and each `seq` carries the expected `payload=b'python-tooling-N'` byte-for-byte.

This exercises the **complete wire format both ways**: Python `RouterFrame.make()` → C++ `RouterFrame::set_source` (router) → Python `RouterFrame.from_bytes()`. Any drift between the Python ctypes layout and the C++ struct would fail the payload comparison.

Tunables via env:

| Variable | Default | Notes |
|----------|---------|-------|
| `ROUTER_BIN` | `build/ipc/test/router_server` | router binary path |
| `COUNT` | `5` | frames to publish |
| `INTERVAL_MS` | `30` | inter-publish sleep |
| `LOG_DIR` | `build/python_peer_smoke/` | log + profile location |

Logs land under `LOG_DIR`: `profile.toml`, `router.log`, `subscribe.log`, `publish.log`.

## Wire-format contract

`rim_router_frame.RouterFrame` is a `ctypes.LittleEndianStructure` with `_pack_ = 1` and field offsets matching `ipc/src/router/frame.hpp` exactly:

| Offset | Size | Field | Python type | Notes |
|--------|------|-------|-------------|-------|
| 0 | 1 | `source` | `c_uint8` | router rewrites on forward |
| 1 | 1 | `flags` | `c_uint8` | bit 0 = has_sideband; bits 4..6 = priority (0..7) |
| 2 | 2 | `topic_id` | `c_uint16` | publisher-set; subscribers dispatch on this |
| 4 | 4 | `seq` | `c_uint32` | per-source monotonic; wraps |
| 8 | 8 | `timestamp_ns` | `c_uint64` | `CLOCK_MONOTONIC_RAW` per [ADR 0010](../../../docs/adr/0010-router-timestamp-clock.md) |
| 16 | 2 | `sideband_idx` | `c_uint16` | `0xFFFF` = no sideband (`SIDEBAND_IDX_NONE`) |
| 18 | 6 | `sideband_len` | u48 (stored as `c_uint8 * 6`) | exposed as `int` property |
| 24 | 8 | `sideband_seq` | `c_uint64` | slot index in sideband region |
| 32 | 32 | `payload` | `c_uint8 * 32` | inline scratchpad, zero-padded |
| **64** | | | | total size |

The `_field_offset` assertions in `rim_router_frame.py` fail-fast at import time if any of those offsets drift. If you change the C++ layout, this file must change too — and the version bump goes in [ADR 0008](../../../docs/adr/0008-router-frame-v2.md) per the migration policy.

`RouterFrame.make(...)` is the convenience constructor; `RouterFrameView` is a frozen dataclass copy for downstream consumers that should not hold raw `ctypes` buffers.

## How the router identifies a Python peer

The router does **not** trust the `source` byte in the frame. Per `peer_id_from_recv` in [`ipc/src/router/datagram_peer_resolver.hpp`](../../../ipc/src/router/datagram_peer_resolver.hpp), the source peer is resolved from the *sender's bound socket path* (`recvfrom`'s `sun_path` for UDS, `sin_port` for UDP). Our `RouterPeer.__init__` therefore **binds** before sending:

1. Unlink any stale socket at `peer_path` (matches the C++ `Uds::bind` dance).
2. `socket(AF_UNIX, SOCK_DGRAM)` + `bind(peer_path)`.
3. Sends go to `router_path` via `sendto`; receives come back on the bound socket.

If the bound path isn't in the loaded topology's `[[peers]]`, the router will increment its `recv_unknown_source` counter (Phase D4 fault gate) and drop the frame. So **always run a profile that lists peer 7 on UDS** — `x86_dev.toml` ships this; `jetson_prod.toml` lists it as SHM (not reachable from this Python bridge today — see [Limitations](#limitations) below).

## Choosing a transport for non-x86 deployments

| Profile | Peer 7 transport | Python reachable? | Notes |
|---------|------------------|-------------------|-------|
| `x86_dev.toml` | UDS | **yes** | recommended for dev / smoke |
| `hil.toml` | UDP loopback (port 19107) | **yes**, but needs a UDP variant of `RouterPeer` (not in F2) | trivial extension; binds `AF_INET` instead of `AF_UNIX` |
| `sim_cloud.toml` | UDP container subnet | **yes**, same caveat as `hil` | |
| `jetson_prod.toml` | SHM | **no** with the current Python bridge | the Jetson profile is all-SHM per [docs/deployment-profiles.md §Known limitations](../../../docs/deployment-profiles.md#known-limitations); a Python SHM client would need a C extension or a separate UDS bridge daemon |

A UDP variant of `RouterPeer` would only change the socket family + address tuple. Left out of F2 to keep the deliverable small; the UDS path is what the bridges README and Phase F plan call out as the primary demo.

## Boundary (do not violate)

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `Python.h` in `ipc/src/`.** The C++ core stays Python-free. CPython embedding (if ever needed) lives elsewhere.
- **No `pybind11` or CPython linkage in `libipc`.** This bridge is a *separate process*, not an in-process embedding.
- **Re-implement the frame layout, don't reach into the C++ headers.** Stable wire format = stable Python port; subtle ABI drift won't break the bridge silently. `rim_router_frame.py` is the source of truth for the Python side; the static asserts in `frame.hpp` are the source of truth for the C++ side. Both must move together if the wire format ever bumps.

## Limitations

- **UDS only in F2.** UDP would be a one-method change to `RouterPeer` (different socket family, different address tuple); deferred to keep F2 surface minimal.
- **No SHM client in this bridge.** Pure-Python SHM access to the router's SPSC rings would need either a small C extension or a UDS bridge daemon. Tracked as part of [parked review C11 — mixed-transport networks](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks); resolving C11 closes this gap.
- **Sideband decoding is not implemented.** The frame carries `sideband_idx` / `sideband_len` / `sideband_seq`; a real consumer maps the named region from the topology and reads `sideband_len` bytes at the slot identified by `sideband_seq`. The current Python bridge surfaces those fields but does not open the SHM region — that maps onto the Phase F F5 vision peer story. The Python side has everything it needs to grow there without breaking the wire contract.
- **Optional JSON-metadata path noted in the F2 plan is not implemented.** "Optional" in the plan; deferred — straightforward addition (decode the inline payload as JSON when the publisher uses that convention).

## Related documents

- [examples/bridges/README.md](../README.md) — bridge boundary policy
- [docs/robotics-reference-layout.md §Python bridge](../../../docs/robotics-reference-layout.md#python-bridge-python_tooling-peer-7) — integration pattern (Phase E E1)
- [docs/deployment-profiles.md](../../../docs/deployment-profiles.md) — profile selector (Phase F F1)
- [docs/adr/0004-robotics-module-boundaries.md](../../../docs/adr/0004-robotics-module-boundaries.md) — `libipc` boundary policy
- [docs/adr/0008-router-frame-v2.md](../../../docs/adr/0008-router-frame-v2.md) — wire layout this Python port mirrors
- [docs/adr/0010-router-timestamp-clock.md](../../../docs/adr/0010-router-timestamp-clock.md) — `CLOCK_MONOTONIC_RAW` clock source used by both sides
- [robotics-ipc-module/plans/F-interoperability-bridges.md §F2](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f2--python-bridge-example) — plan that this directory delivers
