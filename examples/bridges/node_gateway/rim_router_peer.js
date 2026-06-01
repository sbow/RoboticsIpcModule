// UDP bridge for a Node peer talking to the C++ router.
//
// A `RouterPeer` owns one `dgram.Socket` ('udp4') bound at the peer's UDP
// host:port declared in the topology profile (e.g. `hil.toml` peer 8 is at
// `127.0.0.1:19108`). Every frame is one 64 B datagram (no length prefix,
// no stream framing — the router uses `SOCK_DGRAM` for natural message
// boundaries on UDP just as it does on UDS).
//
// How the router identifies us
// ----------------------------
// The C++ router resolves the source peer from the sender's UDP source
// host:port, not from the `source` byte in the frame. See
// `peer_id_from_recv<Udp>` in `ipc/src/router/datagram_peer_resolver.hpp`:
// the router calls `recvfrom` and matches the returned `sockaddr_in`
// host + port against `PeerEntry::local.u.udp`. So we MUST `bind` to the
// peer's declared host:port before sending, or the router will count our
// datagrams as `recv_unknown_source` (the Phase D4 fault counter) and
// drop them.
//
// Why not UDS?
// ------------
// Node's stdlib `dgram` module supports only UDP. `AF_UNIX SOCK_DGRAM`
// (the transport the router uses for UDS profiles like `x86_dev.toml`)
// has no Node stdlib equivalent — Node's `net` covers `AF_UNIX` but only
// as `SOCK_STREAM`. UDS-DGRAM access in Node requires either the
// `unix-dgram` npm package or a small N-API binding. See the
// node_gateway/README.md "Transport reachability" section for the full
// matrix and the opt-in path.
//
// References:
//   - ipc/src/router/link.hpp                       C++ peer-side client mirror
//   - ipc/src/router/datagram_peer_resolver.hpp     source-id resolution
//   - ipc/src/ipc/datagram.hpp                      UDP socket layer
//   - docs/adr/0008-router-frame-v2.md              wire format
//   - examples/bridges/python_peer/rim_router_peer.py   peer port (Python)

import dgram from 'node:dgram';
import { EventEmitter } from 'node:events';

import {
    ROUTER_FRAME_SIZE,
    RouterFrame,
    makeFrame,
} from './rim_router_frame.js';

export class RouterPeer extends EventEmitter {
    // Construction does not bind the socket; callers must `await open()`
    // first. This mirrors the synchronous-bind discipline of the Python
    // peer but adapted to Node's async UDP socket lifecycle.
    constructor({ routerHost, routerPort, peerHost, peerPort, peerId = 0 } = {}) {
        super();
        if (!routerHost || !routerPort || !peerHost || !peerPort) {
            throw new Error(
                'RouterPeer requires routerHost, routerPort, peerHost, peerPort'
            );
        }
        this.routerHost = routerHost;
        this.routerPort = Number(routerPort);
        this.peerHost = peerHost;
        this.peerPort = Number(peerPort);
        this.peerId = Number(peerId) & 0xFF;
        this.sock = null;
        this.closed = false;
    }

    async open() {
        if (this.sock) {
            throw new Error('RouterPeer.open: already open');
        }
        const sock = dgram.createSocket({ type: 'udp4', reuseAddr: false });
        this.sock = sock;

        sock.on('message', (msg, rinfo) => {
            // Truncated datagrams (< 64 B) are dropped silently to match
            // the router's Phase D4 `recv_truncated` policy on the server
            // side. Downstream callers see no 'frame' event for those.
            if (msg.length < ROUTER_FRAME_SIZE) {
                this.emit('truncated', msg.length, rinfo);
                return;
            }
            try {
                const frame = RouterFrame.fromBytes(msg);
                this.emit('frame', frame, rinfo);
            } catch (err) {
                this.emit('error', err);
            }
        });

        sock.on('error', (err) => this.emit('error', err));

        await new Promise((resolve, reject) => {
            const onError = (err) => {
                sock.removeListener('listening', onListening);
                reject(new Error(
                    `bind ${this.peerHost}:${this.peerPort} failed: ${err.message}`
                ));
            };
            const onListening = () => {
                sock.removeListener('error', onError);
                resolve();
            };
            sock.once('error', onError);
            sock.once('listening', onListening);
            sock.bind({ address: this.peerHost, port: this.peerPort, exclusive: true });
        });
    }

    sendFrame(frame) {
        if (!this.sock) {
            throw new Error('RouterPeer.sendFrame: socket not open');
        }
        const data = frame.toBytes();
        if (data.length !== ROUTER_FRAME_SIZE) {
            throw new Error(
                `frame is not exactly ${ROUTER_FRAME_SIZE} bytes (got ${data.length})`
            );
        }
        return new Promise((resolve, reject) => {
            this.sock.send(data, 0, data.length, this.routerPort, this.routerHost, (err) => {
                if (err) reject(err); else resolve();
            });
        });
    }

    publish({
        payload = '',
        topic_id = 0,
        flags = 0,
        seq = 0,
        timestamp_ns = 0n,
        priority = 0,
    } = {}) {
        if (priority < 0 || priority > 7) {
            throw new RangeError('priority must fit in 3 bits (0..7)');
        }
        let fullFlags = flags & 0x8F;
        fullFlags |= (priority & 0x07) << 4;

        const frame = makeFrame({
            source: this.peerId,
            flags: fullFlags,
            topic_id,
            seq,
            timestamp_ns,
            payload,
        });
        return this.sendFrame(frame).then(() => frame);
    }

    async close() {
        if (this.closed) return;
        this.closed = true;
        if (this.sock) {
            const sock = this.sock;
            this.sock = null;
            await new Promise((resolve) => sock.close(resolve));
        }
    }
}

// Match the router's `CLOCK_MONOTONIC_RAW` timestamp source (ADR 0010) as
// closely as Node stdlib allows. Node's `process.hrtime.bigint()` returns
// nanoseconds since an arbitrary boot-anchored origin (CLOCK_MONOTONIC on
// Linux), which is NOT identical to CLOCK_MONOTONIC_RAW — MONOTONIC is
// subject to NTP slew, RAW is not. For dashboard / debug single-host use
// the difference is negligible (< 1 ppm typically). Cross-host correlation
// is explicitly delegated per ADR 0010; this helper is only useful for
// single-host comparable timestamps. The router rewrites `timestamp_ns`
// on forward anyway (see `run_forward_loop` in `router_server.cpp`), so
// publish-time stamps are mostly informational.
export function routerNowNs() {
    return process.hrtime.bigint();
}
