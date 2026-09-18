"""Transport layer: UDP (unicast/multicast) and TCP endpoints.

Design points (see docs/v2-architecture.md, layer ``transport``):

- ``UdpEndpoint`` — bound datagram socket, thread-safe ``send``, pollable
  ``recv`` with timeout (``select`` based, no busy loop).
- multicast support (join/leave group, configurable interface + TTL).
- ``TcpConnection`` — buffered byte stream with SOME/IP frame extraction
  (supports coalesced / split- across-segment frames), thread-safe send.
- ``TcpListener`` — accept loop with optional handoff callback.
- A maximum frame size caps memory usage; oversized frames are rejected.
"""

import errno
import select
import socket
import struct
import threading

from . import wire
from .types import SD_MULTICAST_ADDRESS, SD_PORT, SomeIpError

MAX_UDP_DATAGRAM = 65535
MAX_TCP_FRAME = 16 * 1024 * 1024  # 16 MiB sane cap for a single SOME/IP frame
_DEFAULT_TIMEOUT = 0.5


class TransportError(SomeIpError):
    """Endpoint-level failure."""


class ReceiveTimeout(TimeoutError, TransportError):
    """No data arrived within the requested timeout."""


class FrameTooLarge(TransportError):
    """A SOME/IP frame on a TCP stream exceeds the configured cap."""


class _sockutil:
    @staticmethod
    def nonblocking(family=socket.AF_INET, socktype=socket.SOCK_DGRAM):
        s = socket.socket(family, socktype)
        s.setblocking(False)
        return s

    @staticmethod
    def reuse(s, reuse=True):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1 if reuse else 0)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1 if reuse else 0)
        except OSError:  # not all platforms expose SO_REUSEPORT
            pass


class UdpEndpoint:
    """A bound UDP socket with thread-safe send and pollable receive."""

    def __init__(self, sock):
        self._sock = sock
        self._send_lock = threading.Lock()
        self._closed = False

    # -- constructors -----------------------------------------------------
    @classmethod
    def unicast(cls, port=0, address="0.0.0.0", reuse=False):
        s = _sockutil.nonblocking()
        _sockutil.reuse(s, reuse)
        s.bind((address, port))
        return cls(s)

    @classmethod
    def multicast(cls, port=SD_PORT, group=SD_MULTICAST_ADDRESS,
                  interface_ip=None, reuse=True):
        s = _sockutil.nonblocking()
        _sockutil.reuse(s, reuse)
        # bind the wildcard address so multiple interfaces receive datagrams
        s.bind(("0.0.0.0", port))
        return cls(s).join_group(group, interface_ip)

    @classmethod
    def for_group(cls, port=SD_PORT, group=SD_MULTICAST_ADDRESS,
                  interface_ip=None, ttl=4):
        """Sender socket for emitting traffic to a multicast group."""
        s = _sockutil.nonblocking()
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, ttl)
        if interface_ip is not None:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                         socket.inet_aton(interface_ip))
        return cls(s).join_group(group, interface_ip)

    # -- multicast membership ---------------------------------------------
    def join_group(self, group, interface_ip=None):
        mreq = struct.pack("4s4s", socket.inet_aton(group),
                           socket.inet_aton(interface_ip or "0.0.0.0"))
        try:
            self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        except OSError as exc:
            raise TransportError(
                "cannot join multicast group %s: %s" % (group, exc)) from exc
        return self

    def leave_group(self, group, interface_ip=None):
        mreq = struct.pack("4s4s", socket.inet_aton(group),
                           socket.inet_aton(interface_ip or "0.0.0.0"))
        try:
            self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_DROP_MEMBERSHIP, mreq)
        except OSError:
            pass
        return self

    # -- io ---------------------------------------------------------------
    def send(self, data, addr):
        """Send ``data`` to ``(host, port)``. Safe to call from any thread."""
        if self._closed:
            raise TransportError("endpoint is closed")
        with self._send_lock:
            try:
                return self._sock.sendto(data, addr)
            except (BlockingIOError, InterruptedError):
                return self._send_blocking(data, addr)

    def _send_blocking(self, data, addr):
        _, w, _ = select.select([], [self._sock], [], _DEFAULT_TIMEOUT)
        if not w or self._closed:
            raise TransportError("send buffer full / endpoint closed")
        with self._send_lock:
            try:
                return self._sock.sendto(data, addr)
            except OSError as exc:
                if exc.errno == errno.EMSGSIZE:
                    raise TransportError("datagram too large: %d bytes" % len(data))
                raise TransportError("send failed: %s" % exc) from exc

    def recv(self, max_size=MAX_UDP_DATAGRAM, timeout=_DEFAULT_TIMEOUT):
        """Return ``(data, addr)`` or raise :class:`ReceiveTimeout`."""
        if self._closed:
            raise TransportError("endpoint is closed")
        r, _, _ = select.select([self._sock], [], [], timeout)
        if not r:
            raise ReceiveTimeout("no datagram within %.3fs" % timeout)
        try:
            return self._sock.recvfrom(max_size)
        except (BlockingIOError, InterruptedError) as exc:
            raise ReceiveTimeout("transient receive failure") from exc

    @property
    def fileno(self):
        return self._sock.fileno()

    @property
    def local_port(self):
        return self._sock.getsockname()[1]

    def close(self):
        if not self._closed:
            self._closed = True
            self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class _StreamBuffer:
    """Growable receive buffer that extracts complete SOME/IP frames."""

    def __init__(self, max_frame=MAX_TCP_FRAME):
        self._buf = bytearray()
        self._max_frame = max_frame

    def feed(self, chunk):
        if len(chunk) > self._max_frame:
            raise FrameTooLarge("chunk exceeds %d byte cap" % self._max_frame)
        self._buf.extend(chunk)
        if len(self._buf) > self._max_frame:
            raise FrameTooLarge("buffered stream exceeds %d byte cap" % self._max_frame)
        return self._buf

    def try_frame(self):
        """Pop one complete SOME/IP message, or ``None`` if incomplete."""
        if len(self._buf) < wire.HEADER_SIZE:
            return None
        try:
            frame = wire.Message.from_bytes(bytes(self._buf), strict=False)
        except wire.PartialMessage:
            return None
        except wire.MalformedMessage as exc:
            raise TransportError("malformed SOME/IP frame on stream: %s" % exc) from exc
        del self._buf[:wire.HEADER_SIZE + len(frame.payload)]
        return frame

    def pending(self):
        return len(self._buf)


class TcpConnection:
    """A buffered SOME/IP stream connection (client side or accepted side)."""

    def __init__(self, sock):
        self._sock = sock
        self._send_lock = threading.Lock()
        self._buf = _StreamBuffer()
        self._closed = False

    @classmethod
    def connect(cls, host, port, timeout=_DEFAULT_TIMEOUT):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect((host, port))
        except OSError as exc:
            s.close()
            raise TransportError("connect %s:%d failed: %s" % (host, port, exc)) from exc
        s.setblocking(False)
        return cls(s)

    def send_frame(self, message):
        data = message.to_bytes()
        if self._closed:
            raise TransportError("connection is closed")
        with self._send_lock:
            try:
                self._sock.sendall(data)
            except (BlockingIOError, InterruptedError):
                self._send_frame_blocking(data)
        return len(data)

    def _send_frame_blocking(self, data):
        view = memoryview(data)
        while view:
            _, w, _ = select.select([], [self._sock], [], _DEFAULT_TIMEOUT)
            if not w or self._closed:
                raise TransportError("send buffer full / connection closed")
            n = self._sock.send(view)
            view = view[n:]

    def recv_frame(self, timeout=_DEFAULT_TIMEOUT):
        """Return the next complete SOME/IP message, or ``None`` on clean EOF."""
        if self._closed:
            raise TransportError("connection is closed")
        r, _, _ = select.select([self._sock], [], [], timeout)
        if not r:
            raise ReceiveTimeout("no frame within %.3fs" % timeout)
        chunk = self._sock.recv(65536)
        if not chunk:
            raise EOFError("peer closed the connection")
        self._buf.feed(chunk)
        return self._buf.try_frame()

    def fileno(self):
        return self._sock.fileno()

    def close(self):
        if not self._closed:
            self._closed = True
            try:
                self._sock.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


class TcpListener:
    """A listening TCP endpoint with timeout accept."""

    def __init__(self, sock, on_accept=None):
        self._sock = sock
        self._on_accept = on_accept
        self._closed = False

    @property
    def local_port(self):
        return self._sock.getsockname()[1]

    @classmethod
    def bind(cls, port=0, address="0.0.0.0", backlog=16):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        _sockutil.reuse(s, True)
        s.bind((address, port))
        s.listen(backlog)
        s.setblocking(False)
        return cls(s)

    def accept(self, timeout=_DEFAULT_TIMEOUT):
        """Return a :class:`TcpConnection` or raise :class:`ReceiveTimeout`."""
        if self._closed:
            raise TransportError("listener is closed")
        r, _, _ = select.select([self._sock], [], [], timeout)
        if not r:
            raise ReceiveTimeout("no connection within %.3fs" % timeout)
        conn, addr = self._sock.accept()
        conn.setblocking(False)
        return TcpConnection(conn)

    def close(self):
        if not self._closed:
            self._closed = True
            try:
                self._sock.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


__all__ = [
    "MAX_UDP_DATAGRAM", "MAX_TCP_FRAME", "TransportError", "ReceiveTimeout",
    "FrameTooLarge", "UdpEndpoint", "TcpConnection", "TcpListener",
]