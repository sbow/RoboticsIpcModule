"""UDS bridge for a Python peer talking to the C++ router.

A ``RouterPeer`` owns one ``AF_UNIX`` ``SOCK_DGRAM`` socket bound at the
peer's UDS path declared in the topology profile. Every frame is one
64 B datagram (no length prefix, no stream framing — the router uses
``SOCK_DGRAM`` for natural message boundaries).

How the router identifies us
----------------------------
The C++ router resolves the source peer from the *sender's bound socket
path*, not from the ``source`` byte in the frame. See
``peer_id_from_recv`` in ``ipc/src/router/datagram_peer_resolver.hpp``:
the router calls ``recvfrom`` and matches the returned ``sun_path``
against ``PeerEntry::local.u.uds_path``. So we **must** ``bind`` to the
peer's declared path before sending, or the router will count our
datagrams as ``recv_unknown_source`` (the Phase D4 fault counter) and
drop them.

References
----------
- ipc/src/router/link.hpp                       C++ peer-side client mirror
- ipc/src/router/datagram_peer_resolver.hpp     source-id resolution
- ipc/src/ipc/datagram.hpp                      UDS socket layer
- docs/adr/0008-router-frame-v2.md              wire format
- examples/bridges/README.md                    bridge boundary policy
"""

from __future__ import annotations

import os
import socket
from pathlib import Path
from typing import Optional

from rim_router_frame import ROUTER_FRAME_SIZE, RouterFrame


class RouterPeer:
    """Bound UDS endpoint for a Python peer.

    Parameters
    ----------
    router_path:
        UDS path the router server is listening on (``[router].listen`` in
        the profile TOML, e.g. ``/tmp/rim_router.sock``).
    peer_path:
        UDS path declared for *this* peer in ``[[peers]]``. The router will
        ``sendto`` this path when fanning out frames; we must bind here.
    peer_id:
        Optional. Stored for self-stamping the ``source`` byte on
        publishes — purely informational because the router rewrites
        ``source`` on forward.
    recv_timeout:
        Optional. Seconds for blocking ``recv()``; ``None`` = blocking,
        ``0.0`` = non-blocking, anything > 0 maps to ``SO_RCVTIMEO``.

    Lifetime
    --------
    The bound socket path is unlinked on close. Use as a context manager
    or call ``close()`` explicitly to avoid leaving stale sockets that
    would block re-bind on restart (same hazard the C++ router solves
    with the ``unlink`` call at the top of ``Uds::bind``).
    """

    def __init__(
        self,
        router_path: str,
        peer_path: str,
        peer_id: int = 0,
        recv_timeout: Optional[float] = None,
    ) -> None:
        self.router_path = router_path
        self.peer_path = peer_path
        self.peer_id = peer_id
        self._sock: Optional[socket.socket] = None
        self._closed = False

        self._open(recv_timeout)

    def _open(self, recv_timeout: Optional[float]) -> None:
        Path(self.peer_path).parent.mkdir(parents=True, exist_ok=True)
        try:
            os.unlink(self.peer_path)
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise RuntimeError(
                f"could not unlink stale peer socket {self.peer_path!r}: {exc}"
            ) from exc

        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self._sock.bind(self.peer_path)
        except OSError as exc:
            self._sock.close()
            self._sock = None
            raise RuntimeError(
                f"bind {self.peer_path!r} failed: {exc}"
            ) from exc

        if recv_timeout is not None:
            self._sock.settimeout(recv_timeout)

    def set_recv_timeout(self, seconds: Optional[float]) -> None:
        """Set blocking recv timeout. ``None`` = blocking, ``0`` = non-blocking."""
        assert self._sock is not None, "socket closed"
        self._sock.settimeout(seconds)

    def fileno(self) -> int:
        assert self._sock is not None, "socket closed"
        return self._sock.fileno()

    def send_frame(self, frame: RouterFrame) -> None:
        """Send a fully-constructed frame to the router as one datagram."""
        assert self._sock is not None, "socket closed"
        data = frame.to_bytes()
        if len(data) != ROUTER_FRAME_SIZE:
            raise RuntimeError(
                f"frame is not exactly {ROUTER_FRAME_SIZE} bytes (got {len(data)})"
            )
        self._sock.sendto(data, self.router_path)

    def publish(
        self,
        payload: bytes | str = b"",
        *,
        topic_id: int = 0,
        flags: int = 0,
        seq: int = 0,
        timestamp_ns: int = 0,
        priority: int = 0,
    ) -> RouterFrame:
        """Convenience: build a frame and send it. Returns the frame sent."""
        if priority < 0 or priority > 7:
            raise ValueError("priority must fit in 3 bits (0..7)")
        full_flags = flags & 0x8F
        full_flags |= (priority & 0x07) << 4

        frame = RouterFrame.make(
            source=self.peer_id,
            flags=full_flags,
            topic_id=topic_id,
            seq=seq,
            timestamp_ns=timestamp_ns,
            payload=payload,
        )
        self.send_frame(frame)
        return frame

    def recv_frame(self) -> Optional[RouterFrame]:
        """Block (or wait until timeout) for one datagram. Returns ``None`` on timeout.

        Truncated datagrams (< 64 B) are dropped silently to match the
        router's Phase D4 ``recv_truncated`` policy on the server side.
        Downstream callers see ``None`` as "no valid frame this tick".
        """
        assert self._sock is not None, "socket closed"
        try:
            data, _ = self._sock.recvfrom(ROUTER_FRAME_SIZE)
        except (socket.timeout, BlockingIOError):
            return None
        if len(data) < ROUTER_FRAME_SIZE:
            return None
        return RouterFrame.from_bytes(data[:ROUTER_FRAME_SIZE])

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
        try:
            os.unlink(self.peer_path)
        except FileNotFoundError:
            pass

    def __enter__(self) -> "RouterPeer":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def router_now_ns() -> int:
    """Match the router's ``CLOCK_MONOTONIC_RAW`` source (ADR 0010).

    Python 3.7+ provides ``time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)``
    on Linux; falls back to ``time.monotonic_ns()`` on platforms that
    don't expose ``CLOCK_MONOTONIC_RAW`` (BSDs, macOS). Cross-host
    correlation is explicitly delegated per ADR 0010 — this helper is
    only useful for single-host comparable timestamps.
    """
    import time

    if hasattr(time, "CLOCK_MONOTONIC_RAW"):
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    return time.monotonic_ns()
