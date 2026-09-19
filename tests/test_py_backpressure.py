import unittest

from someip import app
from someip.app import ClientV2
from someip.types import SomeIpError


class _Offer:
    available = True
    address = "10.0.0.1"
    port = 30500


class _Monitor:
    def offer(self, service_id, instance_id):
        return _Offer()


class BackpressureTest(unittest.TestCase):
    def test_in_flight_cap_raises(self):
        client = ClientV2()
        client._monitor = _Monitor()
        old = app.MAX_IN_FLIGHT_REQUESTS
        app.MAX_IN_FLIGHT_REQUESTS = 2
        try:
            client._session = 0
            with client._lock:
                client._pending = {}
                client._pending[1] = app._Pending(0x1234, 99999.0, None)
                client._pending[2] = app._Pending(0x1234, 99999.0, None)
            with self.assertRaises(SomeIpError):
                client.request(0x1234, 0x5678, 0x0001, b"")
        finally:
            app.MAX_IN_FLIGHT_REQUESTS = old

    def test_under_cap_is_fine_at_construction(self):
        # building a client never trips the cap
        self.assertIsNotNone(ClientV2(client_id=0x0701))

    def test_cap_default_sane(self):
        self.assertGreaterEqual(app.MAX_IN_FLIGHT_REQUESTS, 512)


if __name__ == "__main__":
    unittest.main()