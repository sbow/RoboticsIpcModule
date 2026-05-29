"""RouterFrame v2 (ADR 0008) — Python port.

Mirrors the 64 B C++ struct in ipc/src/router/frame.hpp byte-for-byte.
Used by Python bridge processes that want to read or write router traffic
without embedding CPython into libipc (per ADR 0004).

The static_assert on the C++ side pins host little-endian; this port
mirrors that assumption with ctypes.LittleEndianStructure. Porting to a
big-endian host requires a new ADR + an explicit swap layer on both
sides.

References
----------
- docs/adr/0008-router-frame-v2.md          wire format
- docs/adr/0004-robotics-module-boundaries.md   why this lives outside libipc
- ipc/src/router/frame.hpp                  authoritative C++ struct
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass

ROUTER_FRAME_VERSION = 2
ROUTER_FRAME_SIZE = 64
ROUTER_PAYLOAD_SIZE = 32

SIDEBAND_IDX_NONE = 0xFFFF
SIDEBAND_LEN_MAX = 0x0000_FFFF_FFFF_FFFF  # u48

FLAG_HAS_SIDEBAND = 1 << 0
FLAG_KEYFRAME = 1 << 1
FLAG_IS_ACK = 1 << 2
FLAG_EOS = 1 << 3
FLAG_PRIORITY_MASK = 0b0111_0000
FLAG_PRIORITY_SHIFT = 4
FLAG_RESERVED_MASK = 0b1000_0000

ENDPOINT_INVALID = 0
ENDPOINT_SERVER = 255


class RouterFrame(ctypes.LittleEndianStructure):
    """64-byte router frame, layout-compatible with the C++ ``RouterFrame``.

    Field offsets and widths match ADR 0008 exactly. ``sideband_len`` is a
    6-byte unsigned integer in the wire format; ctypes has no native 48-bit
    type, so it is stored as ``c_uint8 * 6`` and exposed as an ``int``
    property below.
    """

    _pack_ = 1
    _fields_ = [
        ("source", ctypes.c_uint8),                    # offset 0
        ("flags", ctypes.c_uint8),                     # offset 1
        ("topic_id", ctypes.c_uint16),                 # offset 2
        ("seq", ctypes.c_uint32),                      # offset 4
        ("timestamp_ns", ctypes.c_uint64),             # offset 8
        ("sideband_idx", ctypes.c_uint16),             # offset 16
        ("_sideband_len_bytes", ctypes.c_uint8 * 6),   # offset 18 (u48 LE)
        ("sideband_seq", ctypes.c_uint64),             # offset 24
        ("payload", ctypes.c_uint8 * ROUTER_PAYLOAD_SIZE),  # offset 32
    ]

    @property
    def sideband_len(self) -> int:
        return int.from_bytes(bytes(self._sideband_len_bytes), "little")

    @sideband_len.setter
    def sideband_len(self, value: int) -> None:
        if value < 0 or value > SIDEBAND_LEN_MAX:
            raise ValueError(
                f"sideband_len out of range (0..{SIDEBAND_LEN_MAX}): {value}"
            )
        raw = value.to_bytes(6, "little")
        ctypes.memmove(self._sideband_len_bytes, raw, 6)

    def has_sideband(self) -> bool:
        return (self.flags & FLAG_HAS_SIDEBAND) != 0

    def is_keyframe(self) -> bool:
        return (self.flags & FLAG_KEYFRAME) != 0

    def is_ack(self) -> bool:
        return (self.flags & FLAG_IS_ACK) != 0

    def is_eos(self) -> bool:
        return (self.flags & FLAG_EOS) != 0

    def priority(self) -> int:
        return (self.flags & FLAG_PRIORITY_MASK) >> FLAG_PRIORITY_SHIFT

    def set_payload(self, data: bytes | bytearray | memoryview) -> None:
        """Copy ``data`` into the 32 B inline payload, zero-padding the tail."""
        view = memoryview(data).cast("B")
        if len(view) > ROUTER_PAYLOAD_SIZE:
            raise ValueError(
                f"payload too large ({len(view)} > {ROUTER_PAYLOAD_SIZE})"
            )
        ctypes.memset(self.payload, 0, ROUTER_PAYLOAD_SIZE)
        if len(view) > 0:
            ctypes.memmove(self.payload, bytes(view), len(view))

    def payload_bytes(self) -> bytes:
        """Return the inline payload with trailing NULs stripped (matches C++)."""
        raw = bytes(self.payload)
        return raw.rstrip(b"\x00")

    def payload_str(self, encoding: str = "utf-8", errors: str = "replace") -> str:
        return self.payload_bytes().decode(encoding, errors=errors)

    def to_bytes(self) -> bytes:
        return bytes(self)

    @classmethod
    def from_bytes(cls, data: bytes | bytearray | memoryview) -> "RouterFrame":
        if len(data) != ROUTER_FRAME_SIZE:
            raise ValueError(
                f"RouterFrame.from_bytes: expected {ROUTER_FRAME_SIZE} bytes, got {len(data)}"
            )
        return cls.from_buffer_copy(bytes(data))

    @classmethod
    def make(
        cls,
        source: int = 0,
        topic_id: int = 0,
        seq: int = 0,
        timestamp_ns: int = 0,
        flags: int = 0,
        payload: bytes | bytearray | memoryview | str = b"",
        sideband_idx: int = SIDEBAND_IDX_NONE,
        sideband_len: int = 0,
        sideband_seq: int = 0,
    ) -> "RouterFrame":
        """Construct a fully-initialized frame in one call.

        ``source`` is publisher-set but the router overwrites it on forward
        (the router resolves source from the sender's bound socket path or
        ``sockaddr_in`` port — see ``peer_id_from_recv`` in C++). Leaving it
        zero is fine; setting it to your peer id is useful for self-loopback
        debugging where the router isn't in the loop.
        """
        frame = cls()
        frame.source = source & 0xFF
        frame.flags = flags & 0xFF
        frame.topic_id = topic_id & 0xFFFF
        frame.seq = seq & 0xFFFFFFFF
        frame.timestamp_ns = timestamp_ns & 0xFFFFFFFFFFFFFFFF
        frame.sideband_idx = sideband_idx & 0xFFFF
        frame.sideband_len = sideband_len
        frame.sideband_seq = sideband_seq & 0xFFFFFFFFFFFFFFFF

        data = payload.encode("utf-8") if isinstance(payload, str) else payload
        frame.set_payload(data)
        return frame

    def describe(self, source_name: str | None = None) -> str:
        name = source_name if source_name is not None else f"id={self.source}"
        return (
            f"source={name} topic={self.topic_id} seq={self.seq} "
            f"ts={self.timestamp_ns} payload={self.payload_str()!r}"
        )


assert ctypes.sizeof(RouterFrame) == ROUTER_FRAME_SIZE, (
    f"RouterFrame must be exactly {ROUTER_FRAME_SIZE} bytes (ADR 0008); "
    f"ctypes reports {ctypes.sizeof(RouterFrame)}"
)


def _field_offset(name: str) -> int:
    return getattr(RouterFrame, name).offset


assert _field_offset("source") == 0
assert _field_offset("flags") == 1
assert _field_offset("topic_id") == 2
assert _field_offset("seq") == 4
assert _field_offset("timestamp_ns") == 8
assert _field_offset("sideband_idx") == 16
assert _field_offset("_sideband_len_bytes") == 18
assert _field_offset("sideband_seq") == 24
assert _field_offset("payload") == 32


@dataclass(frozen=True)
class RouterFrameView:
    """Plain-Python view of a RouterFrame, useful for logging / JSON dumps.

    Decouples downstream consumers from the ctypes object (which holds a
    raw memory buffer). Round-trip with ``RouterFrame.make(**view.kwargs())``
    if needed.
    """

    source: int
    flags: int
    topic_id: int
    seq: int
    timestamp_ns: int
    sideband_idx: int
    sideband_len: int
    sideband_seq: int
    payload: bytes

    @classmethod
    def from_frame(cls, frame: RouterFrame) -> "RouterFrameView":
        return cls(
            source=frame.source,
            flags=frame.flags,
            topic_id=frame.topic_id,
            seq=frame.seq,
            timestamp_ns=frame.timestamp_ns,
            sideband_idx=frame.sideband_idx,
            sideband_len=frame.sideband_len,
            sideband_seq=frame.sideband_seq,
            payload=frame.payload_bytes(),
        )
