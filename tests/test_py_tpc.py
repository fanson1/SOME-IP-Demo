"""v2 SOME/IP-TP layer unit tests (segment/reassemble, ordering, eviction)."""
import os
import struct
import sys
import time
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "platform", "python"))

from someip.tpc import Reassembler, TpHeader, TpOutOfOrder, segment
from someip.wire import Header, Message


def _frame(payload, msg_type=0x00, client_id=0x1111, session_id=0x2222):
    return Message(Header(0x1234, 0x0002, client_id=client_id,
                          session_id=session_id, message_type=msg_type),
                   payload)


class TestTpHeader(unittest.TestCase):
    def test_golden_bytes(self):
        self.assertEqual(TpHeader(more=False, offset=0).to_bytes(),
                         struct.pack(">I", 0x00000000))
        self.assertEqual(TpHeader(more=True, offset=0).to_bytes(),
                         struct.pack(">I", 0x00008000))
        self.assertEqual(TpHeader(more=True, offset=1392).to_bytes(),
                         struct.pack(">I", 0x00008570))  # 0x8000 | 0x570

    def test_roundtrip(self):
        for more, off in [(True, 0), (True, 1392), (False, 2784)]:
            raw = TpHeader(more, off).to_bytes()
            h = TpHeader.from_bytes(raw)
            self.assertEqual((h.more, h.offset), (more, off))

    def test_rejects_reserved_bits(self):
        with self.assertRaises(Exception):
            TpHeader.from_bytes(struct.pack(">I", 0xFFFFFFFF))

    def test_rejects_negative_offset(self):
        with self.assertRaises(Exception):
            TpHeader(offset=0x8000).to_bytes()


class TestSegment(unittest.TestCase):
    def test_small_message_untouched(self):
        frames = segment(_frame(b"\x00" * 100))
        self.assertEqual(len(frames), 1)
        self.assertFalse(frames[0].is_tp)

    def test_large_message_two_segments(self):
        payload = bytes(i & 0xFF for i in range(2000))
        frames = segment(_frame(payload))
        self.assertEqual(len(frames), 2)

        f0 = frames[0]
        self.assertTrue(f0.is_tp)
        self.assertEqual(f0.header.message_type, 0x20)  # TP_REQUEST
        tp0 = TpHeader.from_bytes(f0.payload)
        self.assertTrue(tp0.more)
        self.assertEqual(tp0.offset, 0)
        self.assertEqual(f0.payload[4:], payload[:1392])

        f1 = frames[1]
        tp1 = TpHeader.from_bytes(f1.payload)
        self.assertFalse(tp1.more)
        self.assertEqual(tp1.offset, 1392)
        self.assertEqual(f1.payload[4:], payload[1392:])

        # datagram stays under the 1500-byte IPv4 MTU
        self.assertLessEqual(len(f0.to_bytes()) + 8 + 20, 1500)

    def test_length_field_of_tp_frame(self):
        payload = b"\xaa" * 2000
        f0 = segment(_frame(payload))[0]
        parsed = Message.from_bytes(f0.to_bytes())
        self.assertEqual(parsed.payload[4:], payload[:1392])


class TestReassemble(unittest.TestCase):
    def test_roundtrip_various_sizes(self):
        for size in (1392, 1393, 2000, 2784, 3000, 4096, 7000):
            payload = bytes((i * 7) & 0xFF for i in range(size))
            orig = _frame(payload)
            reass = Reassembler()
            got = None
            for f in segment(orig):
                got = reass.add(f)
            self.assertEqual(got.payload, payload, "size=%d" % size)
            self.assertEqual(got.header.message_type, 0x00)
            self.assertEqual(got.header.request_id, orig.header.request_id)

    def test_response_type_restored(self):
        payload = b"\xbb" * 3000
        orig = _frame(payload, msg_type=0x80)  # RESPONSE
        reass = Reassembler()
        got = None
        for f in segment(orig):
            got = reass.add(f)
        self.assertEqual(got.header.message_type, 0x80)

    def test_passthrough_non_tp(self):
        reass = Reassembler()
        small = _frame(b"hi")
        self.assertIs(reass.add(small), small)

    def test_interleaved_sessions(self):
        payload_a = bytes([0xAA]) * 3000
        payload_b = bytes([0xBB]) * 3000
        seg_a = segment(_frame(payload_a, session_id=0x0001))
        seg_b = segment(_frame(payload_b, msg_type=0x80, session_id=0x0002))
        reass = Reassembler()
        gots = {}
        for a, b in zip(seg_a, seg_b):
            if reass.add(a) is not None:
                gots["a"] = True
            r = reass.add(b)
            if r is not None:
                gots["b"] = r
        self.assertTrue(gots["a"])
        self.assertEqual(gots["b"].payload, payload_b)

    def test_out_of_order_raises(self):
        payload = bytes(3000)
        frames = segment(_frame(payload))
        reass = Reassembler()
        self.assertIsNone(reass.add(frames[0]))
        with self.assertRaises(TpOutOfOrder):
            reass.add(frames[-1])  # skips middle segment

    def test_timeout_eviction(self):
        payload = bytes(3000)
        frames = segment(_frame(payload))
        reass = Reassembler(timeout=0.1)
        self.assertIsNone(reass.add(frames[0]))
        self.assertEqual(reass.size(), 1)
        reass.tick(now=time.monotonic() + 1.0)
        self.assertEqual(reass.size(), 0)
        with self.assertRaises(TpOutOfOrder):
            reass.add(frames[1])

    def test_session_cap_enforced(self):
        reass = Reassembler(timeout=60.0, max_sessions=2)
        for i in range(3):
            f = segment(_frame(bytes(3000), session_id=0x2000 + i))[0]
            self.assertIsNone(reass.add(f))
        self.assertLessEqual(reass.size(), 2)


if __name__ == "__main__":
    unittest.main()