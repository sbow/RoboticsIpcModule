#!/usr/bin/env node
// Subscribe to router traffic as a UDP peer; print decoded frames to
// stdout. Useful for debugging the Node frame port without WebSocket in
// the loop (the gateway is the "Node frame port + WebSocket broadcast"
// composition of subscribe.js's UDP receive path and websocket_server.js).
//
// Default args target the hil.toml UDP profile's `dashboard_feed` (peer 8).
//
// Exit: Ctrl-C, or --count N for a counted exit. Matches python_peer's
// subscribe.py CLI surface 1:1 except where Node syntax differs.

import { parseArgs } from 'node:util';

import { RouterPeer } from './rim_router_peer.js';
import { peerName } from './rim_router_frame.js';

function parseCli() {
    const { values } = parseArgs({
        options: {
            'router-host': { type: 'string', default: '127.0.0.1' },
            'router-port': { type: 'string', default: '19100' },
            'peer-host':   { type: 'string', default: '127.0.0.1' },
            'peer-port':   { type: 'string', default: '19108' },
            'peer-id':     { type: 'string', default: '8' },
            'count':       { type: 'string', default: '0' },
            'quiet':       { type: 'boolean', default: false },
            'help':        { type: 'boolean', default: false },
        },
    });
    if (values.help) {
        process.stderr.write(
            'usage: subscribe.js [--router-host H] [--router-port P]\n' +
            '                    [--peer-host H]   [--peer-port P]   [--peer-id N]\n' +
            '                    [--count N]       [--quiet]\n'
        );
        process.exit(0);
    }
    return values;
}

async function main() {
    const args = parseCli();
    const exitAfter = Number(args.count);

    const peer = new RouterPeer({
        routerHost: args['router-host'],
        routerPort: args['router-port'],
        peerHost:   args['peer-host'],
        peerPort:   args['peer-port'],
        peerId:     args['peer-id'],
    });

    let received = 0;
    const bySource = new Map();
    let stopping = false;
    const stop = async () => {
        if (stopping) return;
        stopping = true;
        await peer.close();
        const parts = [...bySource.entries()]
            .sort(([a], [b]) => a - b)
            .map(([src, n]) => `${peerName(src)}(${src})=${n}`);
        process.stderr.write(
            `[subscribe] received=${received} from {${parts.join(', ')}}\n`
        );
    };

    process.on('SIGINT', () => { stop().then(() => process.exit(0)); });
    process.on('SIGTERM', () => { stop().then(() => process.exit(0)); });

    peer.on('frame', (frame) => {
        received += 1;
        bySource.set(frame.source, (bySource.get(frame.source) ?? 0) + 1);

        if (!args.quiet) {
            const view = frame.toView();
            process.stdout.write(
                `[recv] source=${view.source_name}(id=${view.source}) ` +
                `topic=${view.topic_id} seq=${view.seq} ` +
                `ts_ns=${view.timestamp_ns} ` +
                `payload=${JSON.stringify(view.payload_text)}\n`
            );
        }

        if (exitAfter && received >= exitAfter) {
            stop().then(() => process.exit(0));
        }
    });

    peer.on('error', (err) => {
        process.stderr.write(`[subscribe] peer error: ${err.message}\n`);
    });

    await peer.open();
    process.stderr.write(
        `[subscribe] bound udp ${args['peer-host']}:${args['peer-port']} ` +
        `-> router ${args['router-host']}:${args['router-port']}; ` +
        `peer_id=${args['peer-id']}\n`
    );
}

main().catch((err) => {
    process.stderr.write(`[subscribe] fatal: ${err.message}\n`);
    process.exit(1);
});
