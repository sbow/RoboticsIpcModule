#!/usr/bin/env python3
"""Publish frames as the ``python_tooling`` peer.

Default args target the x86_dev UDS profile:

    examples/bridges/python_peer/publish.py --count 5

Each frame:
    source       = 7  (router rewrites this on forward; informational here)
    topic_id     = --topic-id (default 0)
    seq          = monotonic 0..count-1
    timestamp_ns = router_now_ns() (CLOCK_MONOTONIC_RAW per ADR 0010)
    payload      = --payload, suffixed with seq

Per the x86_dev.toml route ``source = 7 dest = [3]`` the recorder
receives every frame; downstream peers can be added by extending the
route table within the 2-destination cap (parked review C5).
"""

from __future__ import annotations

import argparse
import signal
import sys
import time

from rim_router_peer import RouterPeer, router_now_ns


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--router-path", default="/tmp/rim_router.sock",
                   help="router listen path (default: %(default)s)")
    p.add_argument("--peer-path", default="/tmp/rim_router_python_tooling.sock",
                   help="this peer's bound path (default: %(default)s)")
    p.add_argument("--peer-id", type=int, default=7,
                   help="this peer's id (default: %(default)s)")
    p.add_argument("--count", type=int, default=5,
                   help="number of frames to publish")
    p.add_argument("--interval-ms", type=int, default=80,
                   help="sleep between publishes (ms)")
    p.add_argument("--payload", default="python-tooling",
                   help="payload prefix; gets '-<seq>' appended")
    p.add_argument("--topic-id", type=int, default=0,
                   help="topic id stamped into every frame")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    stop = {"flag": False}

    def _stop(*_: object) -> None:
        stop["flag"] = True

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    with RouterPeer(
        router_path=args.router_path,
        peer_path=args.peer_path,
        peer_id=args.peer_id,
    ) as peer:
        print(
            f"[publish] bound {args.peer_path} -> router {args.router_path}; "
            f"peer_id={args.peer_id}; sending {args.count} frame(s)",
            file=sys.stderr,
        )

        for seq in range(args.count):
            if stop["flag"]:
                break
            payload = f"{args.payload}-{seq}".encode("utf-8")
            frame = peer.publish(
                payload=payload,
                topic_id=args.topic_id,
                seq=seq,
                timestamp_ns=router_now_ns(),
            )
            print(
                f"[send] seq={frame.seq} topic={frame.topic_id} "
                f"ts_ns={frame.timestamp_ns} payload={frame.payload_bytes()!r}"
            )
            if seq < args.count - 1 and args.interval_ms > 0:
                time.sleep(args.interval_ms / 1000.0)

    print(f"[publish] done; sent {args.count} frame(s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
