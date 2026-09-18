"""AUTOSAR SOME/IP wire-format (de)serialization.

Covers the primitive set and structured containers used in production
interfaces:
  bool|u8|u16|u32|u64|i8|i16|i32|i64|f32|f64
  string    (u16 length, UTF-8, NUL-terminated)
  bytes     (u16 length)
  array     (u32 count, then items)
  struct    (fixed layout, concatenated fields)
  union     (u8 selector, then chosen variant)

All multi-byte integers are big-endian. Out-of-bounds reads raise
MalformedMessage (never IndexError), so a hostile peer cannot crash a parser.
"""

import struct

from .types import MalformedMessage

_U8 = struct.Struct(">B")
_U16 = struct.Struct(">H")
_U32 = struct.Struct(">I")
_U64 = struct.Struct(">Q")
_I8 = struct.Struct(">b")
_I16 = struct.Struct(">h")
_I32 = struct.Struct(">i")
_I64 = struct.Struct(">q")
_F32 = struct.Struct(">f")
_F64 = struct.Struct(">d")


class Writer:
    def __init__(self):
        self._buf = bytearray()

    def data(self):
        return bytes(self._buf)

    def u8(self, v): self._buf += _U8.pack(v); return self
    def u16(self, v): self._buf += _U16.pack(v); return self
    def u32(self, v): self._buf += _U32.pack(v); return self
    def u64(self, v): self._buf += _U64.pack(v); return self
    def i8(self, v): self._buf += _I8.pack(v); return self
    def i16(self, v): self._buf += _I16.pack(v); return self
    def i32(self, v): self._buf += _I32.pack(v); return self
    def i64(self, v): self._buf += _I64.pack(v); return self
    def f32(self, v): self._buf += _F32.pack(v); return self
    def f64(self, v): self._buf += _F64.pack(v); return self
    def bool(self, v): return self.u8(1 if v else 0)

    def string(self, s):
        raw = s.encode("utf-8")
        self.u16(len(raw) + 1)
        self._buf += raw
        self._buf += b"\x00"
        return self

    def bytes(self, raw):
        self.u16(len(raw))
        self._buf += raw
        return self

    def array(self, items, item_writer):
        self.u32(len(items))
        for item in items:
            item_writer(self, item)
        return self

    def struct(self, writer_fn):
        writer_fn(self)
        return self

    def union(self, selector, data, variant_writer):
        self.u8(selector)
        variant_writer(self, data)
        return self


class Reader:
    def __init__(self, data):
        self._data = bytes(data)
        self._pos = 0

    @property
    def remaining(self):
        return len(self._data) - self._pos

    def _take(self, n):
        if self._pos + n > len(self._data):
            raise MalformedMessage(
                "short read at %d: need %d, have %d"
                % (self._pos, n, len(self._data) - self._pos))
        out = self._data[self._pos:self._pos + n]
        self._pos += n
        return out

    def u8(self): return _U8.unpack(self._take(1))[0]
    def u16(self): return _U16.unpack(self._take(2))[0]
    def u32(self): return _U32.unpack(self._take(4))[0]
    def u64(self): return _U64.unpack(self._take(8))[0]
    def i8(self): return _I8.unpack(self._take(1))[0]
    def i16(self): return _I16.unpack(self._take(2))[0]
    def i32(self): return _I32.unpack(self._take(4))[0]
    def i64(self): return _I64.unpack(self._take(8))[0]
    def f32(self): return _F32.unpack(self._take(4))[0]
    def f64(self): return _F64.unpack(self._take(8))[0]
    def bool(self): return bool(self.u8())

    def string(self):
        n = self.u16()
        if n < 1:
            raise MalformedMessage("string length must be >= 1")
        raw = self._take(n - 1)
        terminator = self._take(1)
        if terminator != b"\x00":
            raise MalformedMessage("string missing NUL terminator")
        return raw.decode("utf-8")

    def bytes(self):
        n = self.u16()
        return self._take(n)

    def array(self, item_reader):
        count = self.u32()
        if count > self.remaining:
            raise MalformedMessage("array count too large: %d" % count)
        return [item_reader(self) for _ in range(count)]

    def struct(self, reader_fn):
        reader_fn(self)

    def union(self, variant_reader, variants):
        selector = self.u8()
        reader = variants.get(selector)
        if reader is None:
            raise MalformedMessage("unknown union selector %d" % selector)
        return selector, reader(self)