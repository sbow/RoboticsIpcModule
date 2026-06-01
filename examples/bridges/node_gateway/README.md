# `node_gateway` — Phase F F3 (implemented)

A Node.js process that subscribes to the C++ router as `dashboard_feed` (peer 8) over **UDP** and broadcasts every received frame as one **JSON message over WebSocket**. Intended as the back-end for a developer dashboard, operations console, or any browser-side observability tooling — the browser never speaks the binary v2 wire format.

**Peer ID:** 8 (`dashboard_feed`)

**Status:** F3 deliverable — usable today; pure stdlib (no `npm install` required).

```
examples/bridges/node_gateway/
├── README.md             ← you are here
├── package.json          ← Node >=18, type=module, no deps
├── rim_router_frame.js   ← RouterFrame v2 Buffer port (64 B byte-for-byte)
├── rim_router_peer.js    ← UDP subscriber + publisher (Node dgram)
├── websocket_server.js   ← RFC 6455 server subset (stdlib only; broadcast-only)
├── gateway.js            ← entry point: UDP subscriber + WebSocket broadcast
├── subscribe.js          ← debug CLI: receive + decode + print (no WebSocket)
├── publish.js            ← debug CLI: build + send N frames
├── ws_test_client.js     ← stdlib WebSocket client subset (used by smoke)
└── smoke.sh              ← end-to-end smoke through router + gateway + WS client
```

## What this bridge is for

- **Live dashboards** — a browser opens `ws://gateway:25080`, receives one JSON message per router frame, renders charts / tables / 3-D pose without ever decoding 64 B binary.
- **Lightweight observability** — drop the gateway next to the router on any UDP profile (`hil.toml`, `sim_cloud.toml`, or a custom UDP variant), point a browser at it, watch traffic.
- **A worked reference for other-language gateways** — the byte-offset self-check in `rim_router_frame.js` documents the wire contract precisely (matches the assertions in [`python_peer/rim_router_frame.py`](../python_peer/rim_router_frame.py) 1:1).

## Transport reachability

The router supports three transports. **Node's stdlib `dgram` module only supports UDP** — `AF_UNIX SOCK_DGRAM` (the UDS variant) has no Node stdlib equivalent.

| Profile          | Transport | Reachable from this gateway?                                                                                                                                       |
|------------------|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `jetson_prod`    | SHM       | **No** — Node has no SHM ring API; parked as [C11 mixed-transport networks](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks). Run the gateway on x86 instead and forward via UDP. |
| `x86_dev`        | UDS       | **No (stdlib)** — Node has no `AF_UNIX SOCK_DGRAM`. Two opt-in paths: install the [`unix-dgram`](https://www.npmjs.com/package/unix-dgram) npm package, or swap the dashboard peer to UDP in a local profile variant. |
| `hil`            | UDP       | **Yes** — out-of-the-box; dashboard_feed is bound at `127.0.0.1:19108`. Default gateway CLI args target this profile.                                              |
| `sim_cloud`      | UDP       | **Yes** — out-of-the-box; dashboard_feed bound at `10.0.0.8:19208` (substitute your container subnet address).                                                     |

> The reachability matrix is a deliberate trade-off. F3's stdlib discipline (no `npm install`, mirrors F2) cleanly covers UDP profiles. UDS / SHM bridges are tracked under [parked C11](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks) for the same reason: solving them properly needs a mixed-transport router bridge, not a one-off Node hack.

## Quick start (HIL UDP profile)

From the repo root:

```bash
make all   # builds router_server

# Terminal 1 — router with UDP profile:
./build/ipc/test/router_server --config config/profiles/hil.toml

# Terminal 2 — gateway (defaults to dashboard_feed peer 8, ws on 25080):
node examples/bridges/node_gateway/gateway.js

# Terminal 3 — browser, or any WebSocket client:
#    open ws://127.0.0.1:25080/ in a browser console:
#      const ws = new WebSocket('ws://127.0.0.1:25080/');
#      ws.onmessage = (e) => console.log(JSON.parse(e.data));

# Terminal 4 — push a frame as the sensor peer (uses the Node publisher):
node examples/bridges/node_gateway/publish.js \
    --peer-port 19101 --peer-id 1 \
    --router-port 19100 --count 5
```

With the `hil.toml` route `source = 1 dest = [2, 3, 8]` (closed C5 Scope A — every compute rule taps dashboard as a trailing destination), every sensor frame arrives at the gateway and fans out to every connected WebSocket client.

## Smoke script

```bash
make all
./examples/bridges/node_gateway/smoke.sh
```

`smoke.sh` is a fully self-terminating end-to-end test that runs **Node on both ends + the C++ router in the middle + a stdlib WebSocket client**:

```
publish.js (peer 1) --[UDP]--> router_server --[UDP]--> gateway.js (peer 8)
                                                            |
                                                            v
                                               WebSocket text frame (broadcast)
                                                            |
                                                            v
                                                   ws_test_client.js
```

What it does:

1. Writes a minimal 2-peer UDP profile (sensor + dashboard_feed; route `source = 1 dest = [8]`) under `build/node_gateway_smoke/`.
2. Starts `router_server` with `ROUTER_TEST=1` so the router self-exits after idle (no kill signals needed — important in sandboxed CI).
3. Starts `gateway.js` as peer 8 with `--count 5`, waits for the `[gateway] ws listening` readiness line.
4. Starts `ws_test_client.js` connecting to `ws://127.0.0.1:25080` with `--count 5`.
5. Runs `publish.js` to send 5 frames as peer 1.
6. Asserts the router log shows 5 `router: sensor -> dashboard_feed` lines, the WS client decoded 5 broadcasts, and **each `seq` carries the expected `node-frame-<N>` payload byte-for-byte** by `grep`-ing the WS client's JSON output.

Total runtime: ~2 s. Tunable via env vars (see the script header — `COUNT`, `INTERVAL_MS`, `WS_PORT`, `LOG_DIR`, port overrides). Graceful skip if `node` is missing on `PATH` or if AF_INET / UDP loopback is unavailable in the sandbox (some CI environments block `AF_INET`).

Expected last line:

```
[smoke] OK — Node <-> C++ router <-> WebSocket parity verified
```

## WebSocket broadcast schema

One frame in → one text WebSocket frame out, body is a single JSON object. Fields:

| Field          | Type                | Source                                          |
|----------------|---------------------|-------------------------------------------------|
| `source`       | number (u8)         | `frame.source` (rewritten by router on forward) |
| `source_name`  | string              | catalog lookup (`sensor`, `controller`, ...)    |
| `flags`        | number (u8)         | `frame.flags` (bit 0 = sideband, 4-6 = priority)|
| `topic_id`     | number (u16)        | `frame.topic_id`                                |
| `seq`          | number (u32)        | `frame.seq` (publisher-assigned, monotonic)     |
| `timestamp_ns` | string (BigInt LE)  | `frame.timestamp_ns` — router's `CLOCK_MONOTONIC_RAW` (ADR 0010). Serialized as a string because JSON numbers are IEEE 754. |
| `sideband_idx` | number (u16)        | `frame.sideband_idx` (`65535` == none)          |
| `sideband_len` | number (u48)        | `frame.sideband_len` (bytes; fits in JS Number) |
| `sideband_seq` | string (BigInt)     | `frame.sideband_seq`                            |
| `payload_hex`  | string (32 B hex)   | exact 32 B inline payload as lowercase hex      |
| `payload_text` | string (UTF-8)      | best-effort UTF-8 decode of `payload_hex`, trailing NULs stripped (matches C++ `payload_bytes()`) |

The router's clock is `CLOCK_MONOTONIC_RAW` (ADR 0010 — single-host monotonic, slew-free, restart-surviving within a boot epoch). Cross-host time correlation is explicitly delegated to application-level fields or to a future recorder peer; the gateway forwards what it sees.

## CLI flags

### `gateway.js`

| Flag              | Default          | Notes                                          |
|-------------------|------------------|------------------------------------------------|
| `--router-host`   | `127.0.0.1`      | Router listen host (HIL UDP loopback default)  |
| `--router-port`   | `19100`          | Router listen UDP port                         |
| `--peer-host`     | `127.0.0.1`      | This peer's bound UDP host                     |
| `--peer-port`     | `19108`          | This peer's bound UDP port (hil dashboard)     |
| `--peer-id`       | `8`              | Peer id for self-stamping (router rewrites)    |
| `--ws-host`       | `127.0.0.1`      | WebSocket listen host                          |
| `--ws-port`       | `25080`          | WebSocket listen port                          |
| `--count`         | `0` (forever)    | Exit after N broadcasts (used by smoke)        |
| `--quiet`         | off              | Suppress per-frame stderr lines                |

`gateway.js` writes `[gateway] ws listening ws://host:port` to stderr once the WebSocket server is accepting connections — this is the stable readiness contract `smoke.sh` greps for.

### `subscribe.js`

Same UDP / peer flags as `gateway.js`, no WebSocket flags. Useful for verifying the Node frame port works without WebSocket in the loop (e.g. when iterating on the byte-offset port). Prints `[recv] ...` lines to stdout; summary on exit to stderr.

### `publish.js`

| Flag              | Default          | Notes                                          |
|-------------------|------------------|------------------------------------------------|
| `--router-host`   | `127.0.0.1`      |                                                |
| `--router-port`   | `19100`          |                                                |
| `--peer-host`     | `127.0.0.1`      |                                                |
| `--peer-port`     | `19101`          | Sensor peer default (UDP)                      |
| `--peer-id`       | `1`              | Sensor                                         |
| `--count`         | `5`              | Number of frames to publish                    |
| `--interval-ms`   | `30`             | Inter-frame spacing                            |
| `--topic-id`      | `0`              | u16 topic identifier                           |
| `--payload`       | `node-frame`     | Payload gets `-<seq>` appended per frame       |

### `ws_test_client.js`

| Flag                  | Default          | Notes                                      |
|-----------------------|------------------|--------------------------------------------|
| `--host`              | `127.0.0.1`      | Gateway WebSocket host                     |
| `--port`              | `25080`          | Gateway WebSocket port                     |
| `--path`              | `/`              | URL path                                   |
| `--count`             | `0` (forever)    | Exit after N text frames                   |
| `--connect-timeout-ms`| `2000`           | Initial TCP connect deadline               |

## How the router identifies the gateway

Per [`peer_id_from_recv<Udp>`](../../../ipc/src/router/datagram_peer_resolver.hpp) (closed by Phase D1's `resolver_test`), the C++ router resolves a sender's identity from the UDP **source host + port** returned by `recvfrom`, not from the `source` byte in the frame. So the gateway MUST `bind` to the declared peer host:port before sending anything to the router, or the router increments the [Phase D4](../../../docs/adr/0006-shm-backpressure-and-metrics.md) `recv_unknown_source` counter and drops the datagram.

`RouterPeer.open()` performs that bind synchronously before resolving its promise; `gateway.js`, `subscribe.js`, and `publish.js` all await `open()` before any further work.

## Boundary policy

Per [ADR 0004](../../../docs/adr/0004-robotics-module-boundaries.md) and the [bridges index](../README.md):

- **No `node.h` / N-API headers in `ipc/src/`.** The C++ core stays JavaScript-free.
- **The gateway is a separate process.** No in-process Node embedding in `libipc`.
- **`npm install` does not gate the C++ build.** A fresh Jetson or CI runner with no Node toolchain still builds and tests the router. `make all` never executes a Node command; the gateway is opt-in.
- **No `package-lock.json`** — the gateway has zero npm dependencies, so there is nothing to lock. If a future revision adds dependencies (e.g. opting into the `unix-dgram` package), the lockfile lands then.

## Known limitations

- **UDP-only out of the box.** UDS / SHM profiles require parked-C11 mixed-transport work or a one-off `unix-dgram` opt-in.
- **No sideband decoding.** The gateway exposes `sideband_idx` / `sideband_len` / `sideband_seq` in the JSON broadcast, but never reads the actual sideband region. A future variant could `mmap` the `/dev/shm/rim_*` region named by `sideband_idx` and base64-encode the bytes; for now the dashboard is metadata-only and the inline 32 B payload.
- **No backpressure on slow WebSocket clients.** Outbound text frames are written synchronously; if a browser stalls, the gateway's TCP send buffer fills and Node's `socket.write()` returns `false`. We do not currently buffer or drop — the kernel does. A future revision can add an internal ring and drop-oldest semantics.
- **No `wss://` / TLS.** Run behind nginx or caddy if the dashboard is exposed beyond loopback.
- **No subprotocol / compression / fragmentation.** The stdlib WebSocket implementation covers only what F3 needs (handshake + outbound unfragmented text frames + inbound control frames). Browsers handle these constraints transparently for the consumer-facing API.

## Cross-references

- Bridges index: [`examples/bridges/README.md`](../README.md)
- F3 plan: [`plans/F-interoperability-bridges.md` § F3](../../../robotics-ipc-module/plans/F-interoperability-bridges.md#f3--node-dashboard-gateway-example)
- Wire format: [ADR 0008 — RouterFrame v2](../../../docs/adr/0008-router-frame-v2.md)
- Module boundary: [ADR 0004 — robotics-module boundaries](../../../docs/adr/0004-robotics-module-boundaries.md)
- Time semantics: [ADR 0010 — `CLOCK_MONOTONIC_RAW`](../../../docs/adr/0010-router-timestamp-clock.md)
- Routing context: [`docs/deployment-profiles.md`](../../../docs/deployment-profiles.md) — profile selector, route topology, closed C5 Scope A dashboard tap
- Forward dispatch: [parked C5 Scope C → Phase G — Declarative routing](../../../robotics-ipc-module/plans/G-declarative-routing.md) — per-topic subscription will eventually let the gateway declare "show me only topic X" instead of receiving every fan-out frame
- Reachability constraints: [parked C11 — mixed-transport networks](../../../robotics-ipc-module/plans/post-phases-robotics-review.md#c11--mixed-transport-networks)
- Companion bridge: [`examples/bridges/python_peer/README.md`](../python_peer/README.md) — F2 Python ctypes bridge; same wire-format port discipline
