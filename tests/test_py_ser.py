"""AUTOSAR wire-format serializer unit tests."""
import functools
import os
import struct
import sys
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "python"))

from someip2.ser import Writer, Reader
from someip2.types import MalformedMessage


class TestWriterReader(unittest.TestCase):
    def test_roundtrip_primitives(self):
        w = Writer()
        w.u8(0xAB).u16(0x1234).u32(0xDEADBEEF).u64(0x0102030405060708)
        w.i8(-1).i16(-2).i32(-3).i64(-4)
        w.f32(1.5).f64(-2.25).bool(True).bool(False)
        r = Reader(w.data())
        self.assertEqual(r.u8(), 0xAB)
        self.assertEqual(r.u16(), 0x1234)
        self.assertEqual(r.u32(), 0xDEADBEEF)
        self.assertEqual(r.u64(), 0x0102030405060708)
        self.assertEqual(r.i8(), -1)
        self.assertEqual(r.i16(), -2)
        self.assertEqual(r.i32(), -3)
        self.assertEqual(r.i64(), -4)
        self.assertAlmostEqual(r.f32(), 1.5, places=5)
        self.assertAlmostEqual(r.f64(), -2.25, places=9)
        self.assertIs(r.bool(), True)
        self.assertIs(r.bool(), False)
        self.assertEqual(r.remaining, 0)

    def test_string_and_bytes(self):
        w = Writer().string("hello").bytes(b"\x01\x02\x03")
        r = Reader(w.data())
        self.assertEqual(r.string(), "hello")
        self.assertEqual(r.bytes(), b"\x01\x02\x03")

    def test_array_and_struct(self):
        def write_item(w, v):
            w.u16(v)

        w = Writer().array([1, 2, 300], write_item)

        def wstruct(w):
            w.u8(7).string("tag")
        w.struct(wstruct)

        r = Reader(w.data())
        values = r.array(lambda s: s.u16())
        self.assertEqual(values, [1, 2, 300])
        r.struct(lambda s: (self.assertEqual(s.u8(), 7),
                            self.assertEqual(s.string(), "tag")))
        self.assertEqual(r.remaining, 0)

    def test_union(self):
        variants = {
            0: lambda r: r.u8(),
            1: lambda r: r.string(),
        }
        w = Writer().union(1, "payload", lambda w_, d: w_.string(d))
        sel, data = Reader(w.data()).union(lambda r: None, variants)
        self.assertEqual((sel, data), (1, "payload"))

    def test_short_read_raises(self):
        w = Writer().u16(0x1234)
        r = Reader(w.data())
        self.assertEqual(r.u16(), 0x1234)
        with self.assertRaises(MalformedMessage):
            r.u32()

    def test_array_too_large(self):
        w = Writer().u32(1000)  # count but no items
        r = Reader(w.data())
        with self.assertRaises(MalformedMessage):
            r.array(lambda s: s.u8())

    def test_string_missing_terminator(self):
        w = Writer().u16(3).struct(lambda w_: w_.u8(0x41).u8(0x42))  # len 3, but no NUL
        r = Reader(w.data())
        with self.assertRaises(MalformedMessage):
            r.string()

    def test_unknown_union_selector(self):
        w = Writer().u8(99).u8(0)
        with self.assertRaises(MalformedMessage):
            Reader(w.data()).union(lambda r: None, {0: lambda r: r.u8()})


if __name__ == "__main__":
    unittest.main()