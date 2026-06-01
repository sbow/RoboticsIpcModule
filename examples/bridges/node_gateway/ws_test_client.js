#!/usr/bin/env node
// Minimal stdlib WebSocket client for the smoke test. Connects to
// ws://host:port, prints each received text frame to stdout, exits after
// --count messages or on Ctrl-C.
//
// Scope (deliberately small — symmetric with websocket_server.js):
//   - HTTP/1.1 upgrade handshake (random 16-byte client key, base64 encoded)
//   - Inbound text-frame decoder (server -> client frames are NOT masked
//     per RFC 6455 §5.3)
//   - Outbound masked close frame on exit (RFC 6455 §5.5.1 — client MUST
//     mask outbound frames)
//
// Out of scope: subprotocols, compression, binary frames, fragmentation
// reassembly. The gateway only ever sends unfragmented text frames, so
// the client only handles unfragmented text frames.

import { parseArgs } from 'node:util';
import { connect } from 'node:net';
import { createHash, randomBytes } from 'node:crypto';

const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const OPCODE_TEXT = 0x1;
const OPCODE_CLOSE = 0x8;
const OPCODE_PING = 0x9;
const OPCODE_PONG = 0xA;

function parseCli() {
    const { values } = parseArgs({
        options: {
            'host':  { type: 'string', default: '127.0.0.1' },
            'port':  { type: 'string', default: '25080' },
            'path':  { type: 'string', default: '/' },
            'count': { type: 'string', default: '0' },
            'connect-timeout-ms': { type: 'string', default: '2000' },
            'help':  { type: 'boolean', default: false },
        },
    });
    if (values.help) {
        process.stderr.write(
            'usage: ws_test_client.js [--host H] [--port P] [--path /] [--count N]\n' +
            '                         [--connect-timeout-ms MS]\n'
        );
        process.exit(0);
    }
    return values;
}

// Client-side outbound frames MUST be masked. Smoke only emits CLOSE.
function encodeMaskedCloseFrame(code = 1000, reason = '') {
    const reasonBuf = Buffer.from(reason, 'utf8');
    const payload = Buffer.alloc(2 + reasonBuf.length);
    payload.writeUInt16BE(code, 0);
    reasonBuf.copy(payload, 2);

    const mask = randomBytes(4);
    const masked = Buffer.alloc(payload.length);
    for (let i = 0; i < payload.length; i += 1) {
        masked[i] = payload[i] ^ mask[i & 3];
    }

    const header = Buffer.alloc(2);
    header[0] = 0x80 | OPCODE_CLOSE;
    header[1] = 0x80 | payload.length;  // mask=1, payload < 126
    return Buffer.concat([header, mask, masked]);
}

function encodeMaskedPongFrame(payload) {
    const mask = randomBytes(4);
    const masked = Buffer.alloc(payload.length);
    for (let i = 0; i < payload.length; i += 1) {
        masked[i] = payload[i] ^ mask[i & 3];
    }
    const header = Buffer.alloc(2);
    header[0] = 0x80 | OPCODE_PONG;
    header[1] = 0x80 | payload.length;
    return Buffer.concat([header, mask, masked]);
}

// Parse one inbound frame from `buf`. Returns { frame, consumed } or null
// if more bytes needed. Server frames per RFC 6455 §5.3 are unmasked.
function tryParseFrame(buf) {
    if (buf.length < 2) return null;
    const b0 = buf[0];
    const b1 = buf[1];
    const opcode = b0 & 0x0F;
    const masked = (b1 & 0x80) !== 0;
    let payloadLen = b1 & 0x7F;
    let cursor = 2;

    if (payloadLen === 126) {
        if (buf.length < cursor + 2) return null;
        payloadLen = buf.readUInt16BE(cursor);
        cursor += 2;
    } else if (payloadLen === 127) {
        if (buf.length < cursor + 8) return null;
        const big = buf.readBigUInt64BE(cursor);
        payloadLen = Number(big);
        cursor += 8;
    }

    let maskKey = null;
    if (masked) {
        if (buf.length < cursor + 4) return null;
        maskKey = buf.subarray(cursor, cursor + 4);
        cursor += 4;
    }

    if (buf.length < cursor + payloadLen) return null;
    let payload;
    if (masked) {
        payload = Buffer.alloc(payloadLen);
        for (let i = 0; i < payloadLen; i += 1) {
            payload[i] = buf[cursor + i] ^ maskKey[i & 3];
        }
    } else {
        payload = Buffer.from(buf.subarray(cursor, cursor + payloadLen));
    }

    return { frame: { opcode, payload }, consumed: cursor + payloadLen };
}

async function main() {
    const args = parseCli();
    const host = args.host;
    const port = Number(args.port);
    const path = args.path;
    const exitAfter = Number(args.count);
    const connectTimeoutMs = Number(args['connect-timeout-ms']);

    const clientKey = randomBytes(16).toString('base64');
    const expectedAccept = createHash('sha1')
        .update(clientKey + WS_MAGIC, 'utf8')
        .digest('base64');

    const sock = connect({ host, port });
    sock.setNoDelay(true);

    let received = 0;
    let handshakeDone = false;
    let buf = Buffer.alloc(0);
    let exiting = false;

    const exit = (code) => {
        if (exiting) return;
        exiting = true;
        try {
            sock.write(encodeMaskedCloseFrame(1000, ''));
        } catch (_) {
            // best-effort close; the server may already be gone.
        }
        sock.end();
        process.stderr.write(`[ws] received=${received}; exit=${code}\n`);
        setTimeout(() => process.exit(code), 50);
    };

    process.on('SIGINT', () => exit(0));
    process.on('SIGTERM', () => exit(0));

    const connectTimer = setTimeout(() => {
        process.stderr.write(`[ws] connect timeout after ${connectTimeoutMs}ms\n`);
        sock.destroy();
        process.exit(2);
    }, connectTimeoutMs);

    sock.on('connect', () => {
        clearTimeout(connectTimer);
        const req = [
            `GET ${path} HTTP/1.1`,
            `Host: ${host}:${port}`,
            'Upgrade: websocket',
            'Connection: Upgrade',
            `Sec-WebSocket-Key: ${clientKey}`,
            'Sec-WebSocket-Version: 13',
            '', '',
        ].join('\r\n');
        sock.write(req);
        process.stderr.write(`[ws] sent upgrade request to ${host}:${port}\n`);
    });

    sock.on('data', (chunk) => {
        buf = buf.length === 0 ? chunk : Buffer.concat([buf, chunk]);

        if (!handshakeDone) {
            const idx = buf.indexOf('\r\n\r\n');
            if (idx === -1) return;
            const headerText = buf.subarray(0, idx).toString('utf8');
            buf = buf.subarray(idx + 4);
            const statusLine = headerText.split('\r\n')[0] ?? '';
            if (!/^HTTP\/1\.1 101\b/.test(statusLine)) {
                process.stderr.write(`[ws] bad upgrade response: ${statusLine}\n`);
                exit(3);
                return;
            }
            const acceptMatch = headerText.match(/^Sec-WebSocket-Accept:\s*(\S+)/im);
            const accept = acceptMatch?.[1];
            if (!accept || accept !== expectedAccept) {
                process.stderr.write(
                    `[ws] sec-websocket-accept mismatch: got ${accept}, expected ${expectedAccept}\n`
                );
                exit(4);
                return;
            }
            handshakeDone = true;
            process.stderr.write(`[ws] handshake ok, waiting for frames\n`);
        }

        while (handshakeDone && !exiting) {
            const parsed = tryParseFrame(buf);
            if (!parsed) break;
            buf = buf.subarray(parsed.consumed);
            const { opcode, payload } = parsed.frame;
            if (opcode === OPCODE_TEXT) {
                received += 1;
                process.stdout.write(`[broadcast] ${payload.toString('utf8')}\n`);
                if (exitAfter && received >= exitAfter) {
                    exit(0);
                    return;
                }
            } else if (opcode === OPCODE_CLOSE) {
                exit(0);
                return;
            } else if (opcode === OPCODE_PING) {
                try { sock.write(encodeMaskedPongFrame(payload)); } catch (_) { /* */ }
            }
            // Other opcodes (PONG, BINARY, CONT) are ignored.
        }
    });

    sock.on('error', (err) => {
        process.stderr.write(`[ws] socket error: ${err.message}\n`);
        clearTimeout(connectTimer);
        if (!exiting) {
            process.exit(5);
        }
    });

    sock.on('close', () => {
        if (!exiting) {
            process.stderr.write(`[ws] connection closed by peer; received=${received}\n`);
            process.exit(received >= exitAfter && exitAfter > 0 ? 0 : 6);
        }
    });
}

main();
