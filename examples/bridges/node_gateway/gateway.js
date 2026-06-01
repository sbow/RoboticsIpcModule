#!/usr/bin/env node
// Phase F F3 entry point: subscribe to the C++ router over UDP, broadcast
// every received frame as one JSON message over WebSocket. Browser /
// developer-dashboard clients connect to ws://host:port and consume the
// stream without ever speaking the binary v2 wire format themselves.
//
// Default args target the `hil.toml` UDP profile's `dashboard_feed` (peer
// 8) on the standard loopback ports. See README "Quick start" for the
// per-profile invocation table.
//
// Exit
// ----
// SIGINT / SIGTERM trigger a graceful shutdown: WebSocket clients receive
// a 1001 close frame, the UDP socket is closed, the HTTP listener is
// torn down. The bound UDP port is released to the kernel via socket
// close (UDP has no `unlink` equivalent on the file system).
//
// References:
//   - docs/adr/0008-router-frame-v2.md             wire format
//   - docs/adr/0004-robotics-module-boundaries.md  why this lives outside libipc
//   - docs/robotics-reference-layout.md            node_gateway integration pattern

import { parseArgs } from 'node:util';

import { RouterPeer } from './rim_router_peer.js';
import { WebSocketServer } from './websocket_server.js';

function parseCli() {
    const { values } = parseArgs({
        options: {
            'router-host': { type: 'string', default: '127.0.0.1' },
            'router-port': { type: 'string', default: '19100' },
            'peer-host':   { type: 'string', default: '127.0.0.1' },
            'peer-port':   { type: 'string', default: '19108' },
            'peer-id':     { type: 'string', default: '8' },
            'ws-host':     { type: 'string', default: '127.0.0.1' },
            'ws-port':     { type: 'string', default: '25080' },
            'count':       { type: 'string', default: '0' },
            'quiet':       { type: 'boolean', default: false },
            'help':        { type: 'boolean', default: false },
        },
    });

    if (values.help) {
        process.stderr.write(
            'usage: gateway.js [--router-host H] [--router-port P]\n' +
            '                  [--peer-host H]   [--peer-port P]   [--peer-id N]\n' +
            '                  [--ws-host H]     [--ws-port P]\n' +
            '                  [--count N]       [--quiet]\n' +
            '\n' +
            'Subscribes to the router as the dashboard_feed peer (default id=8) and\n' +
            'broadcasts each received frame as a JSON message over WebSocket.\n' +
            '\n' +
            'Defaults match the hil.toml UDP profile (router 127.0.0.1:19100,\n' +
            'dashboard_feed bound at 127.0.0.1:19108, WebSocket on 127.0.0.1:25080).\n'
        );
        process.exit(0);
    }

    return values;
}

async function main() {
    const args = parseCli();
    const exitAfter = Number(args.count);
    const quiet = args.quiet;

    const peer = new RouterPeer({
        routerHost: args['router-host'],
        routerPort: args['router-port'],
        peerHost:   args['peer-host'],
        peerPort:   args['peer-port'],
        peerId:     args['peer-id'],
    });

    const ws = new WebSocketServer({
        host: args['ws-host'],
        port: args['ws-port'],
    });

    let received = 0;
    let stopping = false;
    const stop = async () => {
        if (stopping) return;
        stopping = true;
        await peer.close();
        await ws.close();
        process.stderr.write(
            `[gateway] received=${received}; ws_clients=${ws.clients.size}; exiting\n`
        );
    };

    process.on('SIGINT', () => { stop().then(() => process.exit(0)); });
    process.on('SIGTERM', () => { stop().then(() => process.exit(0)); });

    peer.on('frame', (frame) => {
        received += 1;
        const view = frame.toView();
        const text = JSON.stringify(view);
        const fanout = ws.broadcast(text);
        if (!quiet) {
            process.stderr.write(
                `[gateway] frame source=${view.source_name}(id=${view.source}) ` +
                `topic=${view.topic_id} seq=${view.seq} ` +
                `payload=${JSON.stringify(view.payload_text)} ` +
                `ws_fanout=${fanout}\n`
            );
        }
        if (exitAfter && received >= exitAfter) {
            stop().then(() => process.exit(0));
        }
    });

    peer.on('truncated', (len) => {
        process.stderr.write(`[gateway] dropped truncated datagram (${len} bytes)\n`);
    });

    peer.on('error', (err) => {
        process.stderr.write(`[gateway] peer error: ${err.message}\n`);
    });

    ws.on('client-connect', (_client) => {
        process.stderr.write(`[gateway] ws client connected (total=${ws.clients.size})\n`);
    });

    ws.on('error', (err) => {
        process.stderr.write(`[gateway] ws server error: ${err.message}\n`);
    });

    await peer.open();
    process.stderr.write(
        `[gateway] bound udp ${args['peer-host']}:${args['peer-port']} ` +
        `as peer_id=${args['peer-id']} -> router ${args['router-host']}:${args['router-port']}\n`
    );

    await ws.listen();
    // The smoke greps for the "[gateway] ws listening" line to know the
    // WebSocket server is ready to accept clients. The pattern is part
    // of the gateway's stable stderr contract — do not change without
    // updating smoke.sh.
    process.stderr.write(`[gateway] ws listening ws://${ws.host}:${ws.port}\n`);
}

main().catch((err) => {
    process.stderr.write(`[gateway] fatal: ${err.message}\n`);
    process.exit(1);
});
