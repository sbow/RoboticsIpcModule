// RouterFrame v2 (ADR 0008) — Node port.
//
// Mirrors the 64 B C++ struct in ipc/src/router/frame.hpp byte-for-byte.
// Used by Node bridge processes that want to read or write router traffic
// without embedding Node into libipc (per ADR 0004).
//
// The static_assert on the C++ side pins host little-endian; this port
// mirrors that assumption with explicit Buffer.readUIntXXLE / writeUIntXXLE
// calls. Porting to a big-endian host requires a new ADR + an explicit swap
// layer on both sides.
//
// References:
//   - docs/adr/0008-router-frame-v2.md          wire format
//   - docs/adr/0004-robotics-module-boundaries.md   why this lives outside libipc
//   - ipc/src/router/frame.hpp                  authoritative C++ struct
//   - examples/bridges/python_peer/rim_router_frame.py   peer port (Python)

export const ROUTER_FRAME_VERSION = 2;
export const ROUTER_FRAME_SIZE = 64;
export const ROUTER_PAYLOAD_SIZE = 32;

export const SIDEBAND_IDX_NONE = 0xFFFF;
// JavaScript Number can safely represent integers up to 2^53 - 1, so the
// 48-bit u48 sideband_len fits comfortably as a plain Number.
export const SIDEBAND_LEN_MAX = 0xFFFF_FFFF_FFFF;

export const FLAG_HAS_SIDEBAND = 1 << 0;
export const FLAG_KEYFRAME = 1 << 1;
export const FLAG_IS_ACK = 1 << 2;
export const FLAG_EOS = 1 << 3;
export const FLAG_PRIORITY_MASK = 0b0111_0000;
export const FLAG_PRIORITY_SHIFT = 4;
export const FLAG_RESERVED_MASK = 0b1000_0000;

export const ENDPOINT_INVALID = 0;
export const ENDPOINT_SERVER = 255;

const FIELD_OFFSETS = Object.freeze({
    source: 0,
    flags: 1,
    topic_id: 2,
    seq: 4,
    timestamp_ns: 8,
    sideband_idx: 16,
    sideband_len: 18,
    sideband_seq: 24,
    payload: 32,
});

const PEER_NAMES = Object.freeze({
    1: 'sensor',
    2: 'controller',
    3: 'recorder',
    4: 'vision_capture',
    5: 'ml_inference',
    6: 'mavlink_gateway',
    7: 'python_tooling',
    8: 'dashboard_feed',
});

export function peerName(sourceId) {
    return PEER_NAMES[sourceId] ?? `id=${sourceId}`;
}

// RouterFrame mirrors the C++ struct, but JavaScript has no fixed-layout
// struct primitive — we expose getters/setters over an underlying 64 B
// Buffer instead. The Buffer is the canonical wire representation; the
// JS-side fields are derived views.
export class RouterFrame {
    constructor(buf) {
        if (buf === undefined) {
            this.buf = Buffer.alloc(ROUTER_FRAME_SIZE);
        } else if (Buffer.isBuffer(buf) && buf.length === ROUTER_FRAME_SIZE) {
            // Take ownership of a 64 B buffer (no copy). Caller must not
            // mutate the original after handing it over.
            this.buf = buf;
        } else if (Buffer.isBuffer(buf)) {
            throw new RangeError(
                `RouterFrame: expected ${ROUTER_FRAME_SIZE}-byte buffer, got ${buf.length}`
            );
        } else {
            throw new TypeError('RouterFrame: argument must be a Buffer or undefined');
        }
    }

    static fromBytes(data) {
        if (!Buffer.isBuffer(data)) {
            throw new TypeError('RouterFrame.fromBytes: argument must be a Buffer');
        }
        if (data.length < ROUTER_FRAME_SIZE) {
            throw new RangeError(
                `RouterFrame.fromBytes: need ${ROUTER_FRAME_SIZE} bytes, got ${data.length}`
            );
        }
        return new RouterFrame(Buffer.from(data.subarray(0, ROUTER_FRAME_SIZE)));
    }

    toBytes() {
        return this.buf;
    }

    get source() { return this.buf.readUInt8(FIELD_OFFSETS.source); }
    set source(v) { this.buf.writeUInt8(v & 0xFF, FIELD_OFFSETS.source); }

    get flags() { return this.buf.readUInt8(FIELD_OFFSETS.flags); }
    set flags(v) { this.buf.writeUInt8(v & 0xFF, FIELD_OFFSETS.flags); }

    get topic_id() { return this.buf.readUInt16LE(FIELD_OFFSETS.topic_id); }
    set topic_id(v) { this.buf.writeUInt16LE(v & 0xFFFF, FIELD_OFFSETS.topic_id); }

    get seq() { return this.buf.readUInt32LE(FIELD_OFFSETS.seq); }
    set seq(v) { this.buf.writeUInt32LE(v >>> 0, FIELD_OFFSETS.seq); }

    // u64 returns BigInt to avoid the 53-bit safe-integer ceiling that
    // CLOCK_MONOTONIC_RAW timestamps will cross after ~104 days of uptime.
    get timestamp_ns() { return this.buf.readBigUInt64LE(FIELD_OFFSETS.timestamp_ns); }
    set timestamp_ns(v) {
        const big = typeof v === 'bigint' ? v : BigInt(v);
        this.buf.writeBigUInt64LE(big, FIELD_OFFSETS.timestamp_ns);
    }

    get sideband_idx() { return this.buf.readUInt16LE(FIELD_OFFSETS.sideband_idx); }
    set sideband_idx(v) { this.buf.writeUInt16LE(v & 0xFFFF, FIELD_OFFSETS.sideband_idx); }

    // u48 stored as 6 little-endian bytes. Number is fine: 2^48 < 2^53.
    get sideband_len() { return this.buf.readUIntLE(FIELD_OFFSETS.sideband_len, 6); }
    set sideband_len(v) {
        if (v < 0 || v > SIDEBAND_LEN_MAX) {
            throw new RangeError(
                `sideband_len out of range (0..${SIDEBAND_LEN_MAX}): ${v}`
            );
        }
        this.buf.writeUIntLE(v, FIELD_OFFSETS.sideband_len, 6);
    }

    get sideband_seq() { return this.buf.readBigUInt64LE(FIELD_OFFSETS.sideband_seq); }
    set sideband_seq(v) {
        const big = typeof v === 'bigint' ? v : BigInt(v);
        this.buf.writeBigUInt64LE(big, FIELD_OFFSETS.sideband_seq);
    }

    // Payload getter returns a Buffer view (no copy). Callers that need
    // detached storage must Buffer.from() the result.
    get payload() {
        return this.buf.subarray(
            FIELD_OFFSETS.payload,
            FIELD_OFFSETS.payload + ROUTER_PAYLOAD_SIZE
        );
    }

    setPayload(data) {
        let src;
        if (typeof data === 'string') {
            src = Buffer.from(data, 'utf8');
        } else if (Buffer.isBuffer(data)) {
            src = data;
        } else if (data instanceof Uint8Array) {
            src = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
        } else {
            throw new TypeError('setPayload: data must be a string, Buffer, or Uint8Array');
        }
        if (src.length > ROUTER_PAYLOAD_SIZE) {
            throw new RangeError(
                `setPayload: data too large (${src.length} > ${ROUTER_PAYLOAD_SIZE})`
            );
        }
        this.buf.fill(0, FIELD_OFFSETS.payload, FIELD_OFFSETS.payload + ROUTER_PAYLOAD_SIZE);
        if (src.length > 0) {
            src.copy(this.buf, FIELD_OFFSETS.payload);
        }
    }

    // Strip trailing NULs to match the C++ payload accessor in
    // ipc/src/router/frame.hpp and python_peer.payload_bytes().
    payloadBytes() {
        const start = FIELD_OFFSETS.payload;
        const end = FIELD_OFFSETS.payload + ROUTER_PAYLOAD_SIZE;
        let last = end;
        while (last > start && this.buf[last - 1] === 0) {
            last -= 1;
        }
        return Buffer.from(this.buf.subarray(start, last));
    }

    payloadStr(encoding = 'utf8') {
        return this.payloadBytes().toString(encoding);
    }

    hasSideband() { return (this.flags & FLAG_HAS_SIDEBAND) !== 0; }
    isKeyframe() { return (this.flags & FLAG_KEYFRAME) !== 0; }
    isAck() { return (this.flags & FLAG_IS_ACK) !== 0; }
    isEos() { return (this.flags & FLAG_EOS) !== 0; }
    priority() { return (this.flags & FLAG_PRIORITY_MASK) >> FLAG_PRIORITY_SHIFT; }

    // Plain-JS view, useful for JSON dumps in WebSocket broadcasts and
    // for tests that need to assert on values without keeping a reference
    // to the underlying ctypes-style Buffer.
    toView() {
        const payload = this.payloadBytes();
        return {
            source: this.source,
            source_name: peerName(this.source),
            flags: this.flags,
            topic_id: this.topic_id,
            seq: this.seq,
            // BigInt → string in JSON (JSON.stringify cannot handle BigInt).
            timestamp_ns: this.timestamp_ns.toString(),
            sideband_idx: this.sideband_idx,
            sideband_len: this.sideband_len,
            sideband_seq: this.sideband_seq.toString(),
            payload_hex: payload.toString('hex'),
            payload_text: payload.toString('utf8'),
        };
    }

    describe(sourceName) {
        const name = sourceName ?? peerName(this.source);
        return (
            `source=${name} topic=${this.topic_id} seq=${this.seq} ` +
            `ts=${this.timestamp_ns} payload=${JSON.stringify(this.payloadStr())}`
        );
    }
}

// One-call constructor, mirrors python_peer.RouterFrame.make(...). The
// router rewrites `source` on forward (resolved from the sender's bound
// UDP port / UDS path), so setting it is purely informational for
// self-loopback debugging.
export function makeFrame({
    source = 0,
    flags = 0,
    topic_id = 0,
    seq = 0,
    timestamp_ns = 0n,
    sideband_idx = SIDEBAND_IDX_NONE,
    sideband_len = 0,
    sideband_seq = 0n,
    payload = '',
} = {}) {
    const f = new RouterFrame();
    f.source = source;
    f.flags = flags;
    f.topic_id = topic_id;
    f.seq = seq;
    f.timestamp_ns = timestamp_ns;
    f.sideband_idx = sideband_idx;
    f.sideband_len = sideband_len;
    f.sideband_seq = sideband_seq;
    f.setPayload(payload);
    return f;
}

// Self-check: every field offset matches the ADR 0008 table. Catches
// import-time drift if the C++ struct is ever re-laid-out — better to
// throw at module load than to silently misinterpret 64 B over the wire.
(function assertFieldOffsets() {
    const expected = {
        source: 0, flags: 1, topic_id: 2, seq: 4,
        timestamp_ns: 8, sideband_idx: 16,
        sideband_len: 18, sideband_seq: 24, payload: 32,
    };
    for (const [field, off] of Object.entries(expected)) {
        if (FIELD_OFFSETS[field] !== off) {
            throw new Error(
                `RouterFrame field offset drift: ${field} expected ${off}, ` +
                `got ${FIELD_OFFSETS[field]} (ADR 0008 mismatch)`
            );
        }
    }
})();
