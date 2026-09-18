"""Wire layer: SOME/IP header encode/decode with strict validation.

Byte layout is fully compatible with the v1/legacy stack
(``someip/legacy/header.py``):
    ">HHIHHBBBB"  (16 bytes header, payload after it).
"""

import struct

from .types import (MalformedMessage, MessageType, OversizedMessage,
                    PROTOCOL_VERSION, SomeIpError)

HEADER_FORMAT = ">HHIHHBBBB"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

_MAX_HEADER_LENGTH = 0x0FFFFF00  # sane upper bound: length field incl. header


class Header:
    __slots__ = ("service_id", "method_id", "client_id", "session_id",
                 "protocol_version", "interface_version", "message_type",
                 "return_code", "_length_field")

    def __init__(self, service_id, method_id, client_id=0, session_id=0,
                 protocol_version=PROTOCOL_VERSION, interface_version=0x01,
                 message_type=MessageType.REQUEST, return_code=0x00):
        self.service_id = service_id
        self.method_id = method_id
        self.client_id = client_id
        self.session_id = session_id
        self.protocol_version = protocol_version
        self.interface_version = interface_version
        self.message_type = message_type
        self.return_code = return_code
        self._length_field = 0

    @property
    def message_id(self):
        return (self.service_id << 16) | self.method_id

    @property
    def request_id(self):
        return (self.client_id << 16) | self.session_id

    def to_bytes(self):
        return struct.pack(
            HEADER_FORMAT,
            self.service_id, self.method_id,
            8 + 0,  # length placeholder, overwritten by caller
            self.client_id, self.session_id,
            self.protocol_version, self.interface_version,
            self.message_type, self.return_code)

    @classmethod
    def from_bytes(cls, data):
        if len(data) < HEADER_SIZE:
            raise MalformedMessage("packet shorter than 16-byte header")
        (service_id, method_id, length, client_id, session_id,
         protocol_version, interface_version, message_type,
         return_code) = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
        if length < 8 or length > _MAX_HEADER_LENGTH:
            raise MalformedMessage(
                "invalid length field %d" % length)
        h = cls(service_id, method_id, client_id, session_id,
                protocol_version, interface_version, message_type,
                return_code)
        h._length_field = length
        return h


class Message:
    """A validated SOME/IP message (header + payload, may span datagrams)."""

    __slots__ = ("header", "payload", "_length_field")

    def __init__(self, header, payload):
        self.header = header
        self.payload = payload

    @property
    def message_id(self):
        return self.header.message_id

    @property
    def request_id(self):
        return self.header.request_id

    @property
    def is_notification(self):
        return self.header.message_type == MessageType.NOTIFICATION

    @property
    def is_tp(self):
        return bool(self.header.message_type & 0x20)

    def to_bytes(self):
        length = 8 + len(self.payload)
        data = bytearray(struct.pack(
            HEADER_FORMAT,
            self.header.service_id, self.header.method_id, length,
            self.header.client_id, self.header.session_id,
            self.header.protocol_version, self.header.interface_version,
            self.header.message_type, self.header.return_code))
        data.extend(self.payload)
        return bytes(data)

    @classmethod
    def from_bytes(cls, data, strict=True):
        """Parse a single datagram/stream frame.

        With strict=True an exact length match is required. With strict=False
        trailing bytes are ignored (streaming reads).
        """
        h = Header.from_bytes(data)
        expected = h._length_field - 8  # payload bytes declared by the header
        available = len(data) - HEADER_SIZE
        if strict and available != expected:
            raise MalformedMessage(
                "length mismatch: header says %d payload, got %d"
                % (expected, available))
        if available < expected:
            raise PartialMessage(h, expected - available)
        payload = bytes(data[HEADER_SIZE:HEADER_SIZE + expected])
        return cls(h, payload)


class PartialMessage(SomeIpError):
    """Raised when more bytes are needed to complete a message."""

    def __init__(self, header, missing):
        super().__init__("need %d more bytes" % missing)
        self.header = header
        self.missing = missing