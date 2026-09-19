"""v2 app layer tests: threaded v2 service <-> v2 client over loopback."""
import os
import socket
import struct
import sys
import threading
import time
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "platform", "python"))

from someip import app as appmod
from someip.app import ClientV2, SomeipServiceV2

SERVICE_ID = 0x1357
INSTANCE_ID = 0x0246
METHOD_WHO = 0x0001
METHOD_ADD = 0x0002
METHOD_ECHO = 0x0003
METHOD_VOID = 0x0004
FIELD_LEVEL = 0x2000
EVENT_STATUS = 0x9002
EVENTGROUP_MAIN = 0x0001


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


@unittest.skipUnless(_multicast_available(), "multicast not available on host")
class TestServiceClient(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.service = SomeipServiceV2(
            SERVICE_ID, INSTANCE_ID, method_port=30510, event_port=30511)
        cls.service.add_method(METHOD_WHO, lambda p, a: (0x00, struct.pack(">H", 1)))
        cls.service.add_method(METHOD_ADD,
                               lambda p, a: (0x00, struct.pack(">I", sum(struct.unpack(">II", p)))))
        cls.service.add_method(METHOD_ECHO, lambda p, a: (0x00, p))
        cls.service.add_method(METHOD_VOID, lambda p, a: None)  # no response
        cls.service.add_uint32_field(FIELD_LEVEL, EVENTGROUP_MAIN, initial=42)
        cls.service.add_event(EVENT_STATUS, EVENTGROUP_MAIN)
        cls.service.start()
        cls.client = ClientV2().start()
        # give the service a moment to bind/converge
        time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.client.stop()
        cls.service.stop()

    def _wait_service(self):
        self.assertTrue(self.client.wait_for_service(SERVICE_ID, INSTANCE_ID,
                                                     timeout=5.0))

    def test_rpc(self):
        self._wait_service()
        rc, payload = self.client.request(SERVICE_ID, INSTANCE_ID, METHOD_WHO)
        self.assertEqual(rc, 0x00)
        self.assertEqual(struct.unpack(">H", payload)[0], 1)
        rc, payload = self.client.request(
            SERVICE_ID, INSTANCE_ID, METHOD_ADD, struct.pack(">II", 3, 4))
        self.assertEqual(rc, 0x00)
        self.assertEqual(struct.unpack(">I", payload)[0], 7)

    def test_unknown_method_returns_error(self):
        self._wait_service()
        rc, _ = self.client.request(SERVICE_ID, INSTANCE_ID, 0x00FF)
        self.assertEqual(rc, 0x03)  # E_UNKNOWN_METHOD

    def test_void_handler_times_out(self):
        self._wait_service()
        with self.assertRaises(TimeoutError):
            self.client.request(SERVICE_ID, INSTANCE_ID, METHOD_VOID,
                                timeout=0.3)

    def test_event_notification(self):
        self._wait_service()
        self.assertTrue(self.client.subscribe(SERVICE_ID, INSTANCE_ID,
                                              (EVENTGROUP_MAIN,)))
        got = []
        ev = threading.Event()
        self.client.on_event(EVENT_STATUS, lambda eid, p: (got.append(eid), ev.set()))
        self.service.publish_event(EVENT_STATUS, struct.pack(">I", 123))
        self.assertTrue(ev.wait(3.0))
        self.assertEqual(got, [EVENT_STATUS])

    def test_field_get_set_notify(self):
        self._wait_service()
        self.assertTrue(self.client.subscribe(SERVICE_ID, INSTANCE_ID,
                                              (EVENTGROUP_MAIN,)))
        rc, payload = self.client.request(SERVICE_ID, INSTANCE_ID, FIELD_LEVEL)
        self.assertEqual(rc, 0x00)
        self.assertEqual(struct.unpack(">I", payload)[0], 42)
        notify = []
        nev = threading.Event()
        self.client.on_event(FIELD_LEVEL + 2,
                             lambda eid, p: (notify.append(struct.unpack(">I", p)[0]), nev.set()))
        rc, _ = self.client.request(SERVICE_ID, INSTANCE_ID, FIELD_LEVEL + 1,
                                    struct.pack(">I", 88))
        self.assertEqual(rc, 0x00)
        self.assertTrue(nev.wait(3.0))
        self.assertEqual(notify, [88])
        rc, payload = self.client.request(SERVICE_ID, INSTANCE_ID, FIELD_LEVEL)
        self.assertEqual(struct.unpack(">I", payload)[0], 88)

    def test_tp_large_payload_roundtrip(self):
        self._wait_service()
        big = os.urandom(4000)  # exceeds 1392-byte UDP no-TP cap
        rc, payload = self.client.request(SERVICE_ID, INSTANCE_ID, METHOD_ECHO, big)
        self.assertEqual(rc, 0x00)
        self.assertEqual(payload, big)


if __name__ == "__main__":
    unittest.main()