"""SOME/IP-TP: segmentation and reassembly (UDP MTU-safe chunking).

Wire layout of a TP segment (16-byte SOME/IP header + 4-byte TP header +
segment payload), see PRS 696 / SOME/IP-TP:

    SOME/IP header length field = 8 + 4 + segment_payload_size
    message_type is the TP variant (TP_REQUEST / TP_RESPONSE / ...).
    TP header bits (big-endian u32):
        [31..16] reserved (must be 0)
        [15]     MoreSegmentsFlag (1 = more segments follow)
        [14..0]  offset of this segment, in 16-byte units

Reassembly is automatically triggered when a full message exceeds one
segment; the transport length cap is 1392 bytes of payload per segment
(multiple of 16, keeps offsets integral and the datagram under 1500 B).

Sessions are keyed by (service, method, client, session) and evicted on a
stale timeout (`tick()`), bounding memory under replay/attack conditions.
"""

import struct
import time

from .types import (MAX_PAYLOAD_NO_TP, SomeIpError)
from .wire import Header, Message

TP_HEADER_SIZE = 4
MAX_SEGMENT = MAX_PAYLOAD_NO_TP  # 1392 -> all segment offsets stay /16
DEFAULT_TIMEOUT = 5.0
MAX_SESSIONS = 4096

_TP_TO_MSG = {
    0x20: 0x00,  # TP_REQUEST      -> REQUEST
    0x21: 0x80,  # TP_RESPONSE     -> RESPONSE
    0x22: 0x81,  # TP_ERROR        -> ERROR
    0x23: 0x02,  # TP_NOTIFICATION -> NOTIFICATION
    0x24: 0x01,  # TP_REQUEST_NO_RETURN -> REQUEST_NO_RETURN
}
_MSG_TO_TP = {v: k for k, v in _TP_TO_MSG.items()}


class TpError(SomeIpError):
    """TP-layer failure."""


class TpOutOfOrder(TpError):
    """Segments arrived with a gap (possible loss/replay)."""


class TpHeader:
    __slots__ = ("more", "offset")

    def __init__(self, more=False, offset=0):
        self.more = bool(more)
        self.offset = offset

    def to_bytes(self):
        if self.offset < 0 or self.offset > 0x7FFF:
            raise TpError("TP offset out of range: %d" % self.offset)
        field = (0x8000 if self.more else 0) | self.offset
        return struct.pack(">I", field)

    @classmethod
    def from_bytes(cls, data):
        if len(data) < TP_HEADER_SIZE:
            raise TpError("short TP header")
        field = struct.unpack(">I", data[:TP_HEADER_SIZE])[0]
        if field & 0xFFFF0000:
            raise TpError("reserved bits set in TP header: 0x%08X" % field)
        return cls(more=bool(field & 0x8000), offset=field & 0x7FFF)


def tp_variant(msg_type):
    try:
        return _MSG_TO_TP[msg_type]
    except KeyError:
        raise TpError("cannot segment message type 0x%02X" % msg_type) from None


def restore_message_type(tp_type):
    try:
        return _TP_TO_MSG[tp_type]
    except KeyError:
        raise TpError("unknown TP message type 0x%02X" % tp_type) from None


def segment(message, max_segment=MAX_SEGMENT):
    """Split one complete message into a list of frames.

    Small messages are returned unchanged (single element). Large messages
    become one TP frame per segment, with contiguous 16-byte-aligned offsets.
    """
    if message.is_tp:
        raise TpError("cannot segment an already-TP message")
    payload = message.payload
    if len(payload) <= max_segment:
        return [message]

    chunks = [payload[i:i + max_segment] for i in range(0, len(payload), max_segment)]
    tp_type = tp_variant(message.header.message_type)
    out = []
    for i, chunk in enumerate(chunks):
        h = Header(message.header.service_id, message.header.method_id,
                   message.header.client_id, message.header.session_id,
                   message.header.protocol_version, message.header.interface_version,
                   tp_type, message.header.return_code)
        tp = TpHeader(more=(i < len(chunks) - 1), offset=i * max_segment)
        out.append(Message(h, tp.to_bytes() + chunk))
    return out


class Reassembler:
    """Keyed by (service, method, client, session) with stale eviction."""

    def __init__(self, timeout=DEFAULT_TIMEOUT, max_sessions=MAX_SESSIONS):
        self._timeout = timeout
        self._max = max_sessions
        self._sessions = {}

    @staticmethod
    def _key(header):
        return (header.service_id, header.method_id, header.client_id,
                header.session_id)

    def size(self):
        return len(self._sessions)

    def tick(self, now=None):
        """Drop sessions idle longer than the configured timeout."""
        now = time.monotonic() if now is None else now
        stale = [k for k, (_, _, ts) in self._sessions.items()
                 if now - ts > self._timeout]
        for k in stale:
            del self._sessions[k]

    def add(self, frame, now=None):
        """Feed one wire frame.

        - non-TP frame  -> returned unchanged (pass-through).
        - TP frame      -> accumulated; returns the full reconstructed
          :class:`Message` on the final segment, else ``None``.
          A gap raises :class:`TpOutOfOrder` and drops the session.
        """
        header = frame.header
        if not frame.is_tp:
            return frame
        if len(frame.payload) < TP_HEADER_SIZE:
            raise TpError("TP frame with no TP header")
        tp = TpHeader.from_bytes(frame.payload)
        data = frame.payload[TP_HEADER_SIZE:]
        if tp.offset == 0:
            # (re)start the session: idempotent against sequence restart
            self._sessions[self._key(header)] = ([], 0, time.monotonic())
        now = time.monotonic() if now is None else now
        buffered, expected, _ = self._sessions.get(self._key(header), ([], 0, now))
        if len(self._sessions) > self._max:
            self._evict_oldest()
        if tp.offset != expected:
            self._sessions.pop(self._key(header), None)
            raise TpOutOfOrder(
                "expected offset %d, got %d" % (expected, tp.offset))
        buffered.append(data)
        expected += len(data)
        self._sessions[self._key(header)] = (buffered, expected, now)
        if not tp.more:
            payload = b"".join(buffered)
            h = Header(header.service_id, header.method_id,
                       header.client_id, header.session_id,
                       header.protocol_version, header.interface_version,
                       restore_message_type(header.message_type),
                       header.return_code)
            del self._sessions[self._key(header)]
            return Message(h, payload)
        return None

    def _evict_oldest(self):
        oldest = min(self._sessions.items(), key=lambda kv: kv[1][2])
        del self._sessions[oldest[0]]


__all__ = [
    "TP_HEADER_SIZE", "MAX_SEGMENT", "DEFAULT_TIMEOUT", "MAX_SESSIONS",
    "TpError", "TpOutOfOrder", "TpHeader", "segment", "Reassembler",
]