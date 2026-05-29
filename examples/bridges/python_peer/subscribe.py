#!/usr/bin/env python3
"""Subscribe to router traffic as the ``python_tooling`` peer.

Default args target the x86_dev UDS profile:

    examples/bridges/python_peer/subscribe.py

Equivalent to:

    examples/bridges/python_peer/subscribe.py \\
        --router-path /tmp/rim_router.sock \\
        --peer-path   /tmp/rim_router_python_tooling.sock \\
        --peer-id     7

The router fans out frames per ``[[routes]]`` in the loaded profile. On
``x86_dev.toml`` the route ``source = 2 dest = [3, 7]`` ensures every
controller frame reaches peer 7. ``source = 7 dest = [3]`` lets this
peer publish to the recorder via ``publish.py``.

Exit
----
Ctrl-C exits cleanly (unbinds the socket so a re-run can re-bind without
``EADDRINUSE``). The C++ router does the same dance.
"""

from __future__ import annotations

import argparse
import signal
import sys

from rim_router_frame import RouterFrameView
from rim_router_peer import RouterPeer

_PEER_NAMES = {
    1: "sensor",
    2: "controller",
    3: "recorder",
    4: "vision_capture",
    5: "ml_inference",
    6: "mavlink_gateway",
    7: "python_tooling",
    8: "dashboard_feed",
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--router-path", default="/tmp/rim_router.sock",
                   help="router listen path (default: %(default)s)")
    p.add_argument("--peer-path", default="/tmp/rim_router_python_tooling.sock",
                   help="this peer's bound path (default: %(default)s)")
    p.add_argument("--peer-id", type=int, default=7,
                   help="this peer's id (default: %(default)s)")
    p.add_argument("--count", type=int, default=0,
                   help="exit after N frames (0 = forever)")
    p.add_argument("--quiet", action="store_true",
                   help="suppress per-frame log lines; print summary on exit")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    stop = {"flag": False}

    def _stop(*_: object) -> None:
        stop["flag"] = True

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    received = 0
    by_source: dict[int, int] = {}

    with RouterPeer(
        router_path=args.router_path,
        peer_path=args.peer_path,
        peer_id=args.peer_id,
        recv_timeout=0.2,
    ) as peer:
        print(
            f"[subscribe] bound {args.peer_path} -> router {args.router_path}; "
            f"peer_id={args.peer_id}",
            file=sys.stderr,
        )

        while not stop["flag"]:
            frame = peer.recv_frame()
            if frame is None:
                continue

            received += 1
            by_source[frame.source] = by_source.get(frame.source, 0) + 1

            if not args.quiet:
                view = RouterFrameView.from_frame(frame)
                source_name = _PEER_NAMES.get(view.source, f"id={view.source}")
                print(
                    f"[recv] source={source_name}(id={view.source}) "
                    f"topic={view.topic_id} seq={view.seq} "
                    f"ts_ns={view.timestamp_ns} payload={view.payload!r}"
                )

            if args.count and received >= args.count:
                break

    summary_parts = [
        f"{_PEER_NAMES.get(src, f'id={src}')}({src})={count}"
        for src, count in sorted(by_source.items())
    ]
    print(
        f"[subscribe] received={received} from {{{', '.join(summary_parts)}}}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
