// Minimal stdlib RFC 6455 WebSocket server, broadcast-only.
//
// Scope (deliberately small):
//   - HTTP/1.1 upgrade handshake (Sec-WebSocket-Key / Sec-WebSocket-Accept)
//   - Outbound text-frame encoder (server -> client, no mask per RFC 6455 §5.3)
//   - Inbound control-frame parser (CLOSE / PING / PONG only — broadcast use
//     case has no inbound data frames; we drop unmasked or fragmented data
//     frames with a 1002 close)
//   - Per-connection cleanup on close / error / TCP EOF
//
// Out of scope (would require a real ws library):
//   - Permessage-deflate compression extension
//   - Inbound text/binary data parsing
//   - Fragmented message reassembly
//   - Subprotocol negotiation
//   - TLS termination (run behind nginx/caddy if you need wss://)
//
// Why stdlib instead of `ws` from npm: matches the F2 python_peer stdlib
// discipline. ~150 LOC keeps the bridge fully self-contained — the smoke
// runs without an `npm install` step, mirroring how the Python smoke
// runs without `pip install`. References to the spec:
//   - RFC 6455 §1.3 (handshake), §5.2 (frame format), §5.5.1 (CLOSE),
//     §5.5.2 (PING), §5.5.3 (PONG)
//   - Magic GUID: 258EAFA5-E914-47DA-95CA-C5AB0DC85B11

import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { EventEmitter } from 'node:events';

const WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const OPCODE_CONT = 0x0;
const OPCODE_TEXT = 0x1;
const OPCODE_BINARY = 0x2;
const OPCODE_CLOSE = 0x8;
const OPCODE_PING = 0x9;
const OPCODE_PONG = 0xA;

function computeAcceptKey(secWebSocketKey) {
    return createHash('sha1')
        .update(secWebSocketKey + WS_MAGIC, 'utf8')
        .digest('base64');
}

// Build a single unfragmented outbound text frame (FIN=1, no mask).
function encodeTextFrame(text) {
    const payload = Buffer.from(text, 'utf8');
    const len = payload.length;

    let header;
    if (len < 126) {
        header = Buffer.alloc(2);
        header[0] = 0x80 | OPCODE_TEXT;  // FIN=1, opcode=text
        header[1] = len;
    } else if (len < 65536) {
        header = Buffer.alloc(4);
        header[0] = 0x80 | OPCODE_TEXT;
        header[1] = 126;
        header.writeUInt16BE(len, 2);
    } else {
        header = Buffer.alloc(10);
        header[0] = 0x80 | OPCODE_TEXT;
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(len), 2);
    }

    return Buffer.concat([header, payload]);
}

function encodeCloseFrame(code = 1000, reason = '') {
    const reasonBuf = Buffer.from(reason, 'utf8');
    const payload = Buffer.alloc(2 + reasonBuf.length);
    payload.writeUInt16BE(code, 0);
    reasonBuf.copy(payload, 2);

    const header = Buffer.alloc(2);
    header[0] = 0x80 | OPCODE_CLOSE;
    header[1] = payload.length;  // payload is always < 126 in practice
    return Buffer.concat([header, payload]);
}

function encodePongFrame(payload) {
    const header = Buffer.alloc(2);
    header[0] = 0x80 | OPCODE_PONG;
    header[1] = payload.length;
    return Buffer.concat([header, payload]);
}

// Parse one inbound frame starting at offset 0 of `buf`. Returns
// { frame, consumed } if a complete frame is in the buffer, or null if
// more bytes are needed. Per RFC 6455 §5.3 client-to-server frames MUST
// be masked; we close 1002 if mask bit is unset.
function tryParseFrame(buf) {
    if (buf.length < 2) return null;
    const b0 = buf[0];
    const b1 = buf[1];
    const fin = (b0 & 0x80) !== 0;
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
        if (big > BigInt(Number.MAX_SAFE_INTEGER)) {
            return { frame: { protocolError: true, reason: 'payload too large' }, consumed: buf.length };
        }
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
    const masked_payload = buf.subarray(cursor, cursor + payloadLen);
    let payload;
    if (masked) {
        payload = Buffer.alloc(payloadLen);
        for (let i = 0; i < payloadLen; i += 1) {
            payload[i] = masked_payload[i] ^ maskKey[i & 3];
        }
    } else {
        payload = Buffer.from(masked_payload);
    }

    return {
        frame: { fin, opcode, masked, payload, protocolError: false },
        consumed: cursor + payloadLen,
    };
}

// One connected client. Wraps the underlying TCP socket; the websocket
// server keeps a Set of these for broadcasting.
class WsClient extends EventEmitter {
    constructor(socket, server) {
        super();
        this.socket = socket;
        this.server = server;
        this.buf = Buffer.alloc(0);
        this.closed = false;

        socket.on('data', (chunk) => this._onData(chunk));
        socket.on('error', (err) => this._onError(err));
        socket.on('close', () => this._onSocketClose());
    }

    _onData(chunk) {
        if (this.closed) return;
        this.buf = this.buf.length === 0 ? chunk : Buffer.concat([this.buf, chunk]);

        while (!this.closed) {
            const parsed = tryParseFrame(this.buf);
            if (!parsed) break;
            this.buf = this.buf.subarray(parsed.consumed);
            const frame = parsed.frame;

            if (frame.protocolError) {
                this._sendCloseAndShutdown(1002, frame.reason ?? 'protocol error');
                return;
            }
            if (!frame.masked && (frame.opcode === OPCODE_TEXT
                || frame.opcode === OPCODE_BINARY
                || frame.opcode === OPCODE_CONT)) {
                // RFC 6455 §5.3: client-to-server data frames MUST be masked.
                this._sendCloseAndShutdown(1002, 'unmasked client data frame');
                return;
            }

            switch (frame.opcode) {
                case OPCODE_CLOSE:
                    this._sendCloseAndShutdown(1000, '');
                    return;
                case OPCODE_PING:
                    this._safeWrite(encodePongFrame(frame.payload));
                    break;
                case OPCODE_PONG:
                    break;
                default:
                    // We don't expect inbound data frames in the broadcast
                    // pattern. Drop them silently (the client can use the
                    // gateway as one-way regardless of what it sends).
                    break;
            }
        }
    }

    _onError(err) {
        this.emit('socket-error', err);
        this._shutdown();
    }

    _onSocketClose() {
        if (this.closed) return;
        this.closed = true;
        this.server._unregister(this);
        this.emit('close');
    }

    _safeWrite(buf) {
        if (this.closed || this.socket.destroyed) return false;
        try {
            return this.socket.write(buf);
        } catch (err) {
            this._onError(err);
            return false;
        }
    }

    _sendCloseAndShutdown(code, reason) {
        if (this.closed) return;
        this._safeWrite(encodeCloseFrame(code, reason));
        this._shutdown();
    }

    _shutdown() {
        if (this.closed) return;
        this.closed = true;
        this.server._unregister(this);
        this.socket.end();
        this.emit('close');
    }

    sendText(text) {
        return this._safeWrite(encodeTextFrame(text));
    }
}

export class WebSocketServer extends EventEmitter {
    constructor({ host = '127.0.0.1', port = 0 } = {}) {
        super();
        this.host = host;
        this.port = Number(port);
        this.clients = new Set();
        this.http = null;
        this.closed = false;
    }

    async listen() {
        if (this.http) {
            throw new Error('WebSocketServer.listen: already listening');
        }
        const http = createServer((req, res) => {
            res.writeHead(400, { 'Content-Type': 'text/plain' });
            res.end('this endpoint speaks WebSocket only\n');
        });
        this.http = http;

        http.on('upgrade', (req, socket, head) => this._handleUpgrade(req, socket, head));
        http.on('error', (err) => this.emit('error', err));

        await new Promise((resolve, reject) => {
            const onError = (err) => {
                http.removeListener('listening', onListening);
                reject(err);
            };
            const onListening = () => {
                http.removeListener('error', onError);
                resolve();
            };
            http.once('error', onError);
            http.once('listening', onListening);
            http.listen(this.port, this.host);
        });

        const addr = http.address();
        this.port = addr.port;
        this.host = addr.address;
    }

    _handleUpgrade(req, socket, head) {
        const upgrade = (req.headers.upgrade ?? '').toLowerCase();
        const key = req.headers['sec-websocket-key'];
        const version = req.headers['sec-websocket-version'];

        if (upgrade !== 'websocket' || !key || version !== '13') {
            socket.write('HTTP/1.1 400 Bad Request\r\n\r\n');
            socket.destroy();
            return;
        }

        const accept = computeAcceptKey(key);
        const response = [
            'HTTP/1.1 101 Switching Protocols',
            'Upgrade: websocket',
            'Connection: Upgrade',
            `Sec-WebSocket-Accept: ${accept}`,
            '', '',
        ].join('\r\n');

        socket.write(response);
        socket.setNoDelay(true);

        const client = new WsClient(socket, this);
        this.clients.add(client);
        this.emit('client-connect', client);

        if (head && head.length > 0) {
            client._onData(head);
        }
    }

    broadcast(text) {
        let sent = 0;
        for (const client of this.clients) {
            if (client.sendText(text)) sent += 1;
        }
        return sent;
    }

    _unregister(client) {
        this.clients.delete(client);
    }

    async close() {
        if (this.closed) return;
        this.closed = true;
        for (const client of this.clients) {
            client._sendCloseAndShutdown(1001, 'gateway shutting down');
        }
        this.clients.clear();
        if (this.http) {
            const http = this.http;
            this.http = null;
            await new Promise((resolve) => http.close(resolve));
        }
    }
}
