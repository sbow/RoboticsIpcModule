#!/usr/bin/env node
// Publish N frames as a UDP peer, useful for driving smoke tests and for
// manual exercise of the gateway. Mirrors python_peer/publish.py.
//
// Defaults target the hil.toml UDP profile's sensor peer (id=1). The
// router resolves the source byte from the sender's bound UDP port (see
// peer_id_from_recv<Udp>), so we must bind at the sensor port before
// sending — handled by RouterPeer.open().

import { parseArgs } from 'node:util';

import { RouterPeer, routerNowNs } from './rim_router_peer.js';

function parseCli() {
    const { values } = parseArgs({
        options: {
            'router-host': { type: 'string', default: '127.0.0.1' },
            'router-port': { type: 'string', default: '19100' },
            'peer-host':   { type: 'string', default: '127.0.0.1' },
            'peer-port':   { type: 'string', default: '19101' },
            'peer-id':     { type: 'string', default: '1' },
            'count':       { type: 'string', default: '5' },
            'interval-ms': { type: 'string', default: '30' },
            'topic-id':    { type: 'string', default: '0' },
            'payload':     { type: 'string', default: 'node-frame' },
            'help':        { type: 'boolean', default: false },
        },
    });
    if (values.help) {
        process.stderr.write(
            'usage: publish.js [--router-host H] [--router-port P]\n' +
            '                  [--peer-host H]   [--peer-port P]   [--peer-id N]\n' +
            '                  [--count N]       [--interval-ms MS]\n' +
            '                  [--topic-id N]    [--payload TEXT]\n' +
            '\n' +
            'Each published frame gets a payload of "<payload>-<seq>" (e.g. node-frame-0,\n' +
            'node-frame-1, ...). seq counts up from 0.\n'
        );
        process.exit(0);
    }
    return values;
}

function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function main() {
    const args = parseCli();
    const count = Number(args.count);
    const interval = Number(args['interval-ms']);
    const topicId = Number(args['topic-id']);
    const basePayload = args.payload;

    const peer = new RouterPeer({
        routerHost: args['router-host'],
        routerPort: args['router-port'],
        peerHost:   args['peer-host'],
        peerPort:   args['peer-port'],
        peerId:     args['peer-id'],
    });

    await peer.open();
    process.stderr.write(
        `[publish] bound udp ${args['peer-host']}:${args['peer-port']} ` +
        `as peer_id=${args['peer-id']} -> router ${args['router-host']}:${args['router-port']}; ` +
        `count=${count} interval_ms=${interval}\n`
    );

    for (let seq = 0; seq < count; seq += 1) {
        const payload = `${basePayload}-${seq}`;
        await peer.publish({
            payload,
            topic_id: topicId,
            seq,
            timestamp_ns: routerNowNs(),
        });
        process.stdout.write(
            `[publish] seq=${seq} payload=${JSON.stringify(payload)}\n`
        );
        if (seq < count - 1 && interval > 0) {
            await sleep(interval);
        }
    }

    await peer.close();
    process.stderr.write(`[publish] done; sent ${count} frame(s)\n`);
}

main().catch((err) => {
    process.stderr.write(`[publish] fatal: ${err.message}\n`);
    process.exit(1);
});
