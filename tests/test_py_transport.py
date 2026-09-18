"""v2 transport layer unit tests (loopback + multicast, no external deps)."""
import os
import select
import socket
import sys
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "platform", "python"))

from someip import transport
from someip.transport import (ReceiveTimeout, TcpConnection, TcpListener,
                             TransportError, UdpEndpoint)
from someip.wire import Header, Message


def _multicast_available():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        mreq = socket.inet_aton("224.244.224.245") + b"\x00\x00\x00\x00"
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        s.close()
        return True
    except OSError:
        s.close()
        return False


class TestUdpEndpoint(unittest.TestCase):
    def test_loopback_roundtrip(self):
        c = UdpEndpoint.unicast()
        s = UdpEndpoint.unicast()
        try:
            c.send(b"ping", ("127.0.0.1", s.local_port))
            data, addr = s.recv(timeout=1.0)
            self.assertEqual(data, b"ping")
            self.assertIn(addr[0], ("127.0.0.1",))
        finally:
            c.close()
            s.close()

    def test_recv_timeout(self):
        e = UdpEndpoint.unicast()
        try:
            with self.assertRaises(ReceiveTimeout):
                e.recv(timeout=0.05)
        finally:
            e.close()

    def test_send_after_close_raises(self):
        e = UdpEndpoint.unicast()
        e.close()
        with self.assertRaises(TransportError):
            e.send(b"x", ("127.0.0.1", 1))

    def test_context_manager(self):
        with UdpEndpoint.unicast(50060) as e:
            self.assertEqual(e.local_port, 50060)

    def test_multicast_roundtrip(self):
        if not _multicast_available():
            self.skipTest("multicast unsupported on this host")
        rx = None
        tx = None
        try:
            rx = UdpEndpoint.multicast(50061)
            tx = UdpEndpoint.for_group(50061)
            tx.send(b"mcast", ("224.244.224.245", 50061))
            data, _ = rx.recv(timeout=1.0)
            self.assertEqual(data, b"mcast")
        finally:
            if rx is not None:
                rx.close()
            if tx is not None:
                tx.close()


class TestTcp(unittest.TestCase):
    def test_stream_roundtrip_request_response(self):
        listener = TcpListener.bind(50062)
        conns = []

        def server_thread():
            c = listener.accept(timeout=2.0)
            conns.append(c)
            try:
                while True:
                    frame = c.recv_frame(timeout=2.0)
                    if frame is None:
                        break
                    resp = Message(
                        Header(frame.header.service_id, frame.header.method_id,
                               client_id=frame.header.client_id,
                               session_id=frame.header.session_id,
                               message_type=0x80),
                        b"\x00\x00\x00\x07")
                    c.send_frame(resp)
            except (EOFError, ReceiveTimeout):
                pass

        import threading
        thread = threading.Thread(target=server_thread, daemon=True)
        thread.start()

        try:
            with TcpConnection.connect("127.0.0.1", 50062) as cli:
                req = Message(Header(0x1234, 0x0002, client_id=0x0001,
                                     session_id=0x0001),
                              b"\x00\x00\x00\x03\x00\x00\x00\x04")
                cli.send_frame(req)
                resp = cli.recv_frame(timeout=2.0)
                self.assertIsNotNone(resp)
                self.assertEqual(resp.header.return_code, 0x00)
                self.assertEqual(resp.payload, b"\x00\x00\x00\x07")
        finally:
            listener.close()
            for c in conns:
                c.close()
            thread.join(timeout=1.0)

    def test_split_frame_reassembly(self):
        """A frame delivered in pieces must still reassemble on the client."""
        listener = TcpListener.bind(50063)
        import threading
        server_done = threading.Event()

        def server_thread():
            c = listener.accept(timeout=2.0)
            payload = b"\xaa" * 300
            raw = Message(Header(0x1234, 0x0001, session_id=0x0001), payload).to_bytes()
            for i in range(0, len(raw), 7):  # deliberately split
                c._sock.send(raw[i:i + 7])
            c.close()
            server_done.set()

        threading.Thread(target=server_thread, daemon=True).start()
        try:
            with TcpConnection.connect("127.0.0.1", 50063) as cli:
                frame = cli.recv_frame(timeout=2.0)
                self.assertIsNotNone(frame)
                self.assertEqual(frame.payload, b"\xaa" * 300)
        finally:
            listener.close()
            server_done.wait(timeout=1.0)

    def test_max_frame_cap_enforced(self):
        cli = TcpConnection(socket.socket())  # unconnected shell, feed only
        cli._buf._max_frame = 64
        from someip.transport import FrameTooLarge
        with self.assertRaises(FrameTooLarge):
            cli._buf.feed(b"x" * 65)


if __name__ == "__main__":
    unittest.main()