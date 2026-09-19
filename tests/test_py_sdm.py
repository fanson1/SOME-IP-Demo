"""v2 Service Discovery state machine tests (+ legacy byte interop)."""
import os
import random
import struct
import sys
import threading
import time
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _ROOT)
sys.path.insert(0, os.path.join(_ROOT, "platform", "python"))

from someip import sdm
from someip.sdm import (OfferedService, ServiceMonitor, ServicePublisher,
                        build_sd_message, encode_entry, encode_option_ipv4,
                        ipv4_endpoint, parse_sd_message, parse_entry)
from someip.types import SD_DEFAULT_TTL, SdEntryType


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def advance(self, d):
        self.t += d


def make_offer_raw(offered, ttl=SD_DEFAULT_TTL):
    entry = encode_entry(SdEntryType.OFFER_SERVICE, offered.service_id,
                         offered.instance_id, offered.major, ttl,
                         minor=offered.minor, idx1=0, opts1=1)
    option = encode_option_ipv4(offered.address, offered.port)
    return build_sd_message([entry], [option], header_client=0,
                            header_session=1).to_bytes()


class TestCodec(unittest.TestCase):
    def test_entry_roundtrip(self):
        raw = encode_entry(SdEntryType.OFFER_SERVICE, 0x1234, 0x5678, 0x01,
                           60, minor=0x00000001)
        self.assertEqual(len(raw), sdm.ENTRY_SIZE)
        e = parse_entry(raw)
        self.assertEqual(e["type"], SdEntryType.OFFER_SERVICE)
        self.assertEqual(e["service_id"], 0x1234)
        self.assertEqual(e["instance_id"], 0x5678)
        self.assertEqual(e["major_version"], 0x01)
        self.assertEqual(e["ttl"], 60)          # spec-correct 24-bit TTL
        self.assertEqual(e["minor_version"], 1)

    def test_ttl_wire_offset(self):
        raw = encode_entry(SdEntryType.OFFER_SERVICE, 1, 2, 3, 0xABCDEF,
                           minor=0)
        # bytes 9..11 carry the 24-bit TTL (major stays at byte 8)
        self.assertEqual(raw[8], 3)
        self.assertEqual(raw[9:12], b"\xAB\xCD\xEF")

    def test_subscribe_entry(self):
        raw = encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP, 0x1234, 0x5678,
                           0x01, 60, eventgroup_id=0x0001)
        e = parse_entry(raw)
        self.assertEqual(e["eventgroup_id"], 0x0001)
        self.assertIsNone(e["minor_version"])

    def test_option_endpoint(self):
        opt = encode_option_ipv4("192.168.1.10", 30500)
        parsed = sdm.parse_options(opt, 0, len(opt))
        self.assertEqual(parsed[0]["type"], 0x04)
        self.assertEqual(ipv4_endpoint(parsed[0]), ("192.168.1.10", 30500))

    def test_byte_compat_with_legacy(self):
        from someip.legacy import sd as legacy_sd

        v2_entry = encode_entry(SdEntryType.OFFER_SERVICE, 0x1234, 0x5678,
                                0x01, 60, minor=0x00000001)
        v1_entry = legacy_sd.build_entry(SdEntryType.OFFER_SERVICE, 0x1234,
                                         0x5678, 0x01, 60,
                                         minor_version=0x00000001)
        self.assertEqual(v2_entry, v1_entry)

        v2_opt = encode_option_ipv4("10.0.0.5", 30500)
        v1_opt = legacy_sd.build_option_ipv4_endpoint("10.0.0.5", 30500)
        self.assertEqual(v2_opt, v1_opt.serialize())

        v2_msg = build_sd_message([v2_entry], [v2_opt], header_client=0x1111,
                                  header_session=7)
        # legacy parser must understand v2 bytes (except its known TTL read bug)
        msg, entries, options = legacy_sd.parse_sd_message(v2_msg.to_bytes())
        self.assertEqual(entries[0]["service_id"], 0x1234)
        self.assertEqual(entries[0]["minor_version"], 1)
        self.assertEqual(options[0].endpoint(), ("10.0.0.5", 30500, 0x11))


class TestPublisher(unittest.TestCase):
    def _publisher(self, clock, ttl=SD_DEFAULT_TTL):
        svc = OfferedService(0x1234, 0x5678, 0x01, 0x00000001,
                             "10.0.0.5", 30500, eventgroups={0x0001})
        pub = ServicePublisher([svc], now=clock, rand=random.Random(0))
        return pub, svc

    def test_offer_schedule_backoff(self):
        clock = FakeClock()
        pub, _ = self._publisher(clock)
        clock.advance(1.0)  # past initial delay
        msgs = pub.process()
        self.assertEqual(len(msgs), 1)
        entries = parse_sd_message(msgs[0])
        self.assertEqual(entries[0]["type"], SdEntryType.OFFER_SERVICE)
        # first retry comes after base_delay (0.2s), not immediately
        clock.advance(0.1)
        self.assertEqual(pub.process(), [])
        clock.advance(0.2)
        self.assertEqual(len(pub.process()), 1)  # 2nd announcement

    def test_find_answered(self):
        clock = FakeClock()
        pub, svc = self._publisher(clock)
        clock.advance(1.0)
        pub.process()
        find_raw = build_sd_message(
            [encode_entry(SdEntryType.FIND_SERVICE, 0x1234, 0x5678, 0x01,
                          SD_DEFAULT_TTL, minor=0)], [],
            header_client=0x0001, header_session=1).to_bytes()
        pub.handle_datagram(find_raw, ("10.0.0.2", 30490))
        msgs = pub.process()
        self.assertEqual(len(msgs), 1)
        entries = parse_sd_message(msgs[0])
        self.assertEqual(entries[0]["type"], SdEntryType.OFFER_SERVICE)

    def test_subscribe_ack_registers_subscriber(self):
        clock = FakeClock()
        pub, svc = self._publisher(clock)
        clock.advance(1.0)
        pub.process()
        sub_raw = build_sd_message(
            [encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP, 0x1234, 0x5678,
                          0x01, SD_DEFAULT_TTL, eventgroup_id=0x0001, idx1=0,
                          opts1=1)],
            [encode_option_ipv4("10.0.0.2", 30501)],
            header_client=0x0001, header_session=2).to_bytes()
        pub.handle_datagram(sub_raw, ("10.0.0.2", 30490))
        msgs = pub.process()
        entries = parse_sd_message(msgs[0])
        self.assertEqual(entries[0]["type"],
                         SdEntryType.SUBSCRIBE_EVENTGROUP_ACK)
        self.assertEqual(entries[0]["eventgroup_id"], 0x0001)
        self.assertIn(("10.0.0.2", 30501), pub.subscribers(0x1234, 0x5678))

    def test_subscribe_nack_unknown_group(self):
        clock = FakeClock()
        pub, _ = self._publisher(clock)
        sub_raw = build_sd_message(
            [encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP, 0x1234, 0x5678,
                          0x01, SD_DEFAULT_TTL, eventgroup_id=0x0099, idx1=0,
                          opts1=1)],
            [encode_option_ipv4("10.0.0.2", 30501)],
            header_client=0x0001, header_session=2).to_bytes()
        pub.handle_datagram(sub_raw, ("10.0.0.2", 30490))
        entries = parse_sd_message(pub.process()[0])
        self.assertEqual(entries[0]["type"],
                         SdEntryType.SUBSCRIBE_EVENTGROUP_NACK)

    def test_stop_offer_ttl_zero(self):
        clock = FakeClock()
        pub, _ = self._publisher(clock)
        msgs = pub.stop()
        entries = parse_sd_message(msgs[0])
        self.assertEqual(entries[0]["type"], SdEntryType.OFFER_SERVICE)
        self.assertEqual(entries[0]["ttl"], 0)

    def test_sawtooth_guards_stale_session(self):
        clock = FakeClock()
        pub, _ = self._publisher(clock)
        find_base = (lambda sid: build_sd_message(
            [encode_entry(SdEntryType.FIND_SERVICE, 0x1234, 0x5678, 0x01,
                          SD_DEFAULT_TTL, minor=0)], [],
            header_client=0x0001, header_session=sid).to_bytes())
        pub.handle_datagram(find_base(5), ("10.0.0.2", 30490))
        self.assertEqual(len(pub.process()), 1)  # answered
        pub.handle_datagram(find_base(3), ("10.0.0.2", 30490))
        self.assertEqual(len(pub.process()), 0)  # stale -> ignored


class TestMonitor(unittest.TestCase):
    def _monitor(self, clock):
        mon = ServiceMonitor(client_id=0x0001, now=clock, rand=random.Random(0))
        mon.client_address = "10.0.0.2"
        mon.client_port = 30501
        mon.find(0x1234, 0x5678, eventgroups={0x0001})
        return mon

    def test_find_at_initial_delay_then_backoff(self):
        clock = FakeClock()
        mon = self._monitor(clock)
        clock.advance(1.0)
        msgs = mon.process()
        self.assertEqual(len(msgs), 1)
        entries = parse_sd_message(msgs[0])
        self.assertEqual(entries[0]["type"], SdEntryType.FIND_SERVICE)
        clock.advance(0.1)
        self.assertEqual(mon.process(), [])
        clock.advance(0.3)
        self.assertEqual(len(mon.process()), 1)  # 2nd find (2x base delay)

    def test_offer_available_and_subscribe(self):
        clock = FakeClock()
        mon = self._monitor(clock)
        events = []
        mon.on_available = lambda k: events.append(("avail", k))
        mon.on_subscribe_ok = lambda k, eg: events.append(("sub", k, eg))
        svc = OfferedService(0x1234, 0x5678, 0x01, 0x00000001,
                             "10.0.0.5", 30500, eventgroups={0x0001})
        mon.handle_datagram(make_offer_raw(svc), ("10.0.0.5", 30490))
        self.assertIn(("avail", (0x1234, 0x5678)), events)
        self.assertEqual(mon._offers[(0x1234, 0x5678)].address, "10.0.0.5")
        sub_msgs = mon.process()
        self.assertEqual(len(sub_msgs), 1)
        entries = parse_sd_message(sub_msgs[0])
        self.assertEqual(entries[0]["type"], SdEntryType.SUBSCRIBE_EVENTGROUP)
        self.assertEqual(entries[0]["eventgroup_id"], 0x0001)

    def test_stop_offer_unavailable(self):
        clock = FakeClock()
        mon = self._monitor(clock)
        events = []
        mon.on_unavailable = lambda k: events.append(("gone", k))
        svc = OfferedService(0x1234, 0x5678, 0x01, 0x00000001,
                             "10.0.0.5", 30500)
        mon.handle_datagram(make_offer_raw(svc), ("10.0.0.5", 30490))
        self.assertEqual(events, [])
        mon.handle_datagram(make_offer_raw(svc, ttl=0), ("10.0.0.5", 30490))
        self.assertEqual(events, [("gone", (0x1234, 0x5678))])
        self.assertNotIn((0x1234, 0x5678), mon._offers)

    def test_ttl_expiry_unavailable(self):
        clock = FakeClock()
        mon = self._monitor(clock)
        events = []
        mon.on_unavailable = lambda k: events.append(("expired", k))
        svc = OfferedService(0x1234, 0x5678, 0x01, 0x1, "10.0.0.5", 30500)
        mon.handle_datagram(make_offer_raw(svc, ttl=2), ("10.0.0.5", 30490))
        self.assertEqual(events, [])
        clock.advance(2.5)
        mon.tick()
        self.assertEqual(events, [("expired", (0x1234, 0x5678))])

    def test_subscribe_nack_removes(self):
        clock = FakeClock()
        mon = self._monitor(clock)
        svc = OfferedService(0x1234, 0x5678, 0x01, 0x1, "10.0.0.5", 30500)
        mon.handle_datagram(make_offer_raw(svc), ("10.0.0.5", 30490))
        mon.process()  # drain subscribe
        self.assertIn((0x1234, 0x5678), mon._subscribed)
        nack_raw = build_sd_message(
            [encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP_NACK, 0x1234,
                          0x5678, 0x01, SD_DEFAULT_TTL, eventgroup_id=0x0001)],
            [], header_client=0x0001, header_session=2).to_bytes()
        mon.handle_datagram(nack_raw, ("10.0.0.5", 30490))
        self.assertNotIn((0x1234, 0x5678), mon._subscribed)

    def test_subscribe_ack_roundtrip_end_to_end(self):
        """Monitor <-> Publisher subscribe exchange over in-process queues."""
        clock = FakeClock()
        pub_svc = OfferedService(0x1234, 0x5678, 0x01, 0x00000001,
                                 "10.0.0.5", 30500, eventgroups={0x0001})
        pub = ServicePublisher([pub_svc], now=clock, rand=random.Random(0))
        mon = self._monitor(clock)
        acked = []
        mon.on_subscribe_ok = lambda k, eg: acked.append((k, eg))
        # find -> offer
        clock.advance(1.0)
        for f in mon.process():
            pub.handle_datagram(f.to_bytes(), ("10.0.0.2", 30490))
        for o in pub.process():
            mon.handle_datagram(o.to_bytes(), ("10.0.0.5", 30490))
        # subscribe -> ack
        for s in mon.process():
            pub.handle_datagram(s.to_bytes(), ("10.0.0.2", 30490))
        for a in pub.process():
            mon.handle_datagram(a.to_bytes(), ("10.0.0.5", 30490))
        self.assertEqual(acked, [((0x1234, 0x5678), 0x0001)])
        self.assertIn(("10.0.0.2", 30501), pub.subscribers(0x1234, 0x5678))


if __name__ == "__main__":
    unittest.main()