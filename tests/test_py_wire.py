"""v2 wire layer unit tests + v1 byte-level interop."""
import os
import struct
import sys
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "python"))

from someip2.wire import Header, Message, MalformedMessage, PartialMessage

GOLDEN = bytes.fromhex(
    "12340002000000101111222201010000"
    "0000000300000004")


class TestWireV2(unittest.TestCase):
    def test_golden_roundtrip(self):
        h = Header(0x1234, 0x0002, client_id=0x1111, session_id=0x2222,
                   message_type=0x00)
        msg = Message(h, b"\x00\x00\x00\x03\x00\x00\x00\x04")
        self.assertEqual(msg.to_bytes(), GOLDEN)

        parsed = Message.from_bytes(GOLDEN)
        self.assertEqual(parsed.header.service_id, 0x1234)
        self.assertEqual(parsed.header.method_id, 0x0002)
        self.assertEqual(parsed.header.client_id, 0x1111)
        self.assertEqual(parsed.header.session_id, 0x2222)
        self.assertEqual(parsed.payload, b"\x00\x00\x00\x03\x00\x00\x00\x04")

    def test_rejects_short_packet(self):
        with self.assertRaises(MalformedMessage):
            Message.from_bytes(GOLDEN[:8])

    def test_rejects_length_mismatch_strict(self):
        bad = GOLDEN + b"\x00"  # trailing byte, header length still 0x10
        with self.assertRaises(MalformedMessage):
            Message.from_bytes(bad, strict=True)

    def test_tolerates_trailing_when_not_strict(self):
        bad = GOLDEN + b"\x00"
        parsed = Message.from_bytes(bad, strict=False)
        self.assertEqual(parsed.header.message_id, 0x12340002)

    def test_rejects_invalid_length_field(self):
        bad = bytearray(GOLDEN)
        struct.pack_into(">I", bad, 4, 4)  # length < 8
        with self.assertRaises(MalformedMessage):
            Message.from_bytes(bytes(bad))

    def test_raises_partial_for_stream(self):
        truncated = GOLDEN[:20]  # header demands 16 payload bytes, got 4
        with self.assertRaises(PartialMessage):
            Message.from_bytes(truncated, strict=False)


class TestInteropWithV1(unittest.TestCase):
    """Bytes produced by v1 must parse as v2 and vice versa."""

    def test_v1_bytes_parse_in_v2(self):
        from someip.header import SomeIpMessage as V1

        v1 = V1(service_id=0x1234, method_id=0x0002, payload=b"\x00\x00\x00\x03"
                b"\x00\x00\x00\x04", client_id=0x1111, session_id=0x2222,
                message_type=0x00, return_code=0x00)
        raw = v1.serialize()
        self.assertEqual(raw, GOLDEN)

        v2 = Message.from_bytes(raw, strict=True)
        self.assertEqual(v2.header.message_id, v1.message_id)
        self.assertEqual(v2.header.request_id, v1.request_id)

    def test_v2_bytes_parse_in_v1(self):
        from someip.header import SomeIpMessage as V1

        h = Header(0x1234, 0x8001, client_id=0x0000, session_id=0x0001,
                   message_type=0x02)  # NOTIFICATION event
        payload = struct.pack(">II", 1700000000, 88)
        v2 = Message(h, payload)
        v1 = V1.deserialize(v2.to_bytes())
        self.assertEqual(v1.service_id, 0x1234)
        self.assertEqual(v1.method_id, 0x8001)
        self.assertEqual(v1.message_type, 0x02)
        self.assertEqual(v1.payload, payload)


if __name__ == "__main__":
    unittest.main()