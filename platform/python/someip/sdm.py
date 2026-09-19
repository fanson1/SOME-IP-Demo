"""Service Discovery state machine (byte-compatible with legacy/vSomeIP).

Wire layout follows AUTOSAR SOME/IP-SD (PRS 696) and matches the legacy v1
stack byte-for-byte:

    SD envelope : SOME/IP header (service 0xFFFF, method 0x8100, NOTIFICATION)
    payload     : u32 entries_length + entries(pad4) + u32 options_length + options(pad4)

    Entry (16 B): type(1) idx1(1) idx2(1) counts(1) svc(2) inst(2) major(1)
                  ttl(3, 24-bit) tail(4: minor_version | {eventgroup,reserved})
    Option IPv4  : length(2) type(1) reserved(1) addr(4) protocol(1) port(2)

TTL lives in wire bytes 9..11 (the legacy *parser* misread it — v2 reads the
spec-correct offset while writing identical bytes).

State machines
--------------
ServiceMonitor (client) : FIND with initial delay + exponential retries then
    periodic cycle; offers tracked with TTL expiry -> availability callbacks;
    SUBSCRIBE request + renewal, AcK/Nack handling; StopOffer (TTL=0).

ServicePublisher (service): OFFER with same backoff schedule; immediate answer
    to matching FIND; SUBSCRIBE -> Ack (or Nack); StopOffer emission on stop().

Both are driven by the owning thddread: call ``process(now)`` to emit due
messages into an internal send queue, and ``handle_datagram(raw, src)`` to
consume inbound SD datagrams. They never block and never touch sockets, so
they are fully unit-testable and socket I/O stays in the app layer.
"""

import random
import socket
import struct
import time

from .types import (SD_DEFAULT_CYCLE, SD_DEFAULT_TTL, SD_INITIAL_DELAY_MAX,
                    SD_INITIAL_DELAY_MIN, SD_MULTICAST_ADDRESS, SD_PORT,
                    SD_REPETITIONS_BASE_DELAY, SD_REPETITIONS_MAX,
                    SD_REQUESTS_BASE_DELAY, SD_SUBSCRIBE_BASE_DELAY,
                    SomeIpError, SdEntryType, SdOptionType, MessageType)
from .wire import Header, Message

SD_SERVICE_ID = 0xFFFF
SD_METHOD_ID = 0x8100
SD_PROTOCOL_VERSION = 0x01
SD_INTERFACE_VERSION = 0x01
ENTRY_SIZE = 16
_U32 = struct.Struct(">I")
_U16 = struct.Struct(">H")


class SdError(SomeIpError):
    """SD-layer failure."""


# ---------------------------------------------------------------- codec ----

def encode_entry(entry_type, service_id, instance_id, major, ttl,
                 minor=0, eventgroup_id=None, idx1=0, idx2=0, opts1=0, opts2=0):
    entry = bytearray(16)
    entry[0] = entry_type & 0xFF
    entry[1] = idx1 & 0xFF
    entry[2] = idx2 & 0xFF
    entry[3] = ((opts2 & 0x0F) << 4) | (opts1 & 0x0F)
    struct.pack_into(">HH", entry, 4, service_id, instance_id)
    entry[8] = major & 0xFF
    entry[9] = (ttl >> 16) & 0xFF
    entry[10] = (ttl >> 8) & 0xFF
    entry[11] = ttl & 0xFF
    if eventgroup_id is not None:
        struct.pack_into(">HH", entry, 12, eventgroup_id, 0x0000)
    else:
        struct.pack_into(">I", entry, 12, minor)
    return bytes(entry)


def parse_entry(data):
    if len(data) < ENTRY_SIZE:
        raise SdError("short SD entry")
    (entry_type, idx1, idx2, counts, svc, inst, major) = struct.unpack(
        ">BBBBHHB", data[0:9])
    ttl = ((data[9] << 16) | (data[10] << 8) | data[11]) & 0xFFFFFF
    tail = data[12:16]
    entry = {
        "type": entry_type,
        "index_first": idx1,
        "index_second": idx2,
        "options1": counts & 0x0F,
        "options2": (counts >> 4) & 0x0F,
        "service_id": svc,
        "instance_id": inst,
        "major_version": major,
        "ttl": ttl,
        "eventgroup_id": None,
        "minor_version": None,
    }
    if entry_type in (SdEntryType.SUBSCRIBE_EVENTGROUP,
                      SdEntryType.SUBSCRIBE_EVENTGROUP_ACK):
        entry["eventgroup_id"] = _U16.unpack(tail[0:2])[0]
    else:
        entry["minor_version"] = _U32.unpack(tail)[0]
    return entry


def encode_option_ipv4(address, port, option_type=SdOptionType.IPV4_ENDPOINT,
                        protocol=0x11):
    value = struct.pack(">4sBH", socket.inet_aton(address), protocol, port)
    return struct.pack(">HBB", 2 + len(value), option_type, 0x00) + value


def parse_options(raw, offset, size):
    options = []
    while offset + 4 <= size:
        length, opt_type, _ = struct.unpack(">HBB", raw[offset:offset + 4])
        if length < 2 or offset + 2 + length > size:
            raise SdError("corrupt SD option")
        options.append({
            "type": opt_type,
            "reserved": _,
            "value": raw[offset + 4:offset + 2 + length],
        })
        offset += 2 + length
    return options


def attach_options(entries, options):
    for e in entries:
        indexes = []
        if e["options1"]:
            indexes += range(e["index_first"], e["index_first"] + e["options1"])
        if e["options2"]:
            indexes += range(e["index_second"], e["index_second"] + e["options2"])
        e["options"] = [options[i] for i in indexes if i < len(options)]


def build_sd_message(entries, options, header_client=0, header_session=1):
    entries_bytes = b"".join(entries)
    options_bytes = b"".join(options)
    pad_e = (-len(entries_bytes)) % 4
    pad_o = (-len(options_bytes)) % 4
    payload = (_U32.pack(len(entries_bytes)) + entries_bytes + b"\x00" * pad_e +
               _U32.pack(len(options_bytes)) + options_bytes + b"\x00" * pad_o)
    h = Header(SD_SERVICE_ID, SD_METHOD_ID, client_id=header_client,
               session_id=header_session, interface_version=SD_INTERFACE_VERSION,
               protocol_version=SD_PROTOCOL_VERSION,
               message_type=MessageType.NOTIFICATION)
    return Message(h, payload)


def parse_sd_message(message):
    payload = message.payload
    if len(payload) < 8:
        raise SdError("SD payload too short")
    entries_len = _U32.unpack(payload[0:4])[0]
    if 4 + entries_len + 4 > len(payload):
        raise SdError("SD entries length overruns payload")
    entries_raw = payload[4:4 + entries_len]
    opts_base = 4 + entries_len
    opts_len = _U32.unpack(payload[opts_base:opts_base + 4])[0]
    if opts_base + 4 + opts_len > len(payload):
        raise SdError("SD options length overruns payload")
    entries = [parse_entry(entries_raw[i:i + ENTRY_SIZE])
               for i in range(0, entries_len - ENTRY_SIZE + 1, ENTRY_SIZE)]
    options = parse_options(payload, opts_base + 4, opts_base + 4 + opts_len)
    attach_options(entries, options)
    return entries


def ipv4_endpoint(option):
    value = option.get("value")
    if option["type"] != SdOptionType.IPV4_ENDPOINT or len(value) < 7:
        return None
    return _inet_ntoa(value[0:4]), _U16.unpack(value[5:7])[0]


def _inet_ntoa(raw):
    return "%d.%d.%d.%d" % tuple(raw)


# --------------------------------------------------------- shared helpers ----

def _backoff_schedule(base_delay, repetitions):
    """Delay sequence: base, 2*base, 4*base ... for ``repetitions`` retries."""
    delays = []
    for i in range(repetitions):
        delays.append(base_delay * (1 << i))
    return delays


# ------------------------------------------------------- client monitor ----

class OfferRecord:
    __slots__ = ("service_id", "instance_id", "major", "minor", "address",
                 "port", "deadline", "available")

    def __init__(self, service_id, instance_id, major, minor, address, port,
                 deadline):
        self.service_id = service_id
        self.instance_id = instance_id
        self.major = major
        self.minor = minor
        self.address = address
        self.port = port
        self.deadline = deadline
        self.available = True


class ServiceMonitor:
    """Client-side discovery: find, track offers, manage subscriptions."""

    def __init__(self, client_id=0x0000, session_base=1, *, now=None,
                 rand=None):
        self.client_id = client_id
        self._session = session_base
        self._now = now or time.monotonic
        self._rand = rand or random.Random()
        self.client_address = "0.0.0.0"
        self.client_port = 0
        # (service,instance) -> targets to locate (with event groups to join)
        self._wanted = {}
        self._offers = {}
        # key (svc,inst) -> subscription schedule
        self._subscribing = {}
        self._subscribed = {}
        self._send_queue = []
        self._phase = {}  # (svc,inst) -> "find"/"cycle"
        self._next_find = {}  # (svc,inst) -> monotonic deadline
        self._find_step = {}
        self._acks = set()   # (svc,inst,eventgroup) acknowledged
        self._nacks = set()  # (svc,inst,eventgroup) rejected
        self.on_available = None
        self.on_unavailable = None
        self.on_subscribe_ok = None
        self.on_subscribe_nack = None

    # -- configuration -----------------------------------------------------
    def find(self, service_id, instance_id, eventgroups=()):
        key = (service_id, instance_id)
        self._wanted[key] = {"eventgroups": set(eventgroups),
                             "major": None}
        self._find_step[key] = 0
        self._phase[key] = "find"
        self._next_find[key] = self._now() + self._init_delay()

    def subscribed_eventgroups(self, service_id, instance_id):
        return set(self._wanted.get((service_id, instance_id), {}).get(
            "eventgroups", ()))

    def offer(self, service_id, instance_id):
        """Return the current :class:`OfferRecord` (or ``None`` if unknown)."""
        return self._offers.get((service_id, instance_id))

    def subscription_state(self, service_id, instance_id, eventgroup_id):
        """'ack' / 'nack' / 'none' tracking for a subscribed event group."""
        key = (service_id, instance_id, eventgroup_id)
        if key in self._acks:
            return "ack"
        if key in self._nacks:
            return "nack"
        return "none"

    def resubscribe(self, service_id, instance_id):
        """(Re)emit SUBSCRIBE if the service is already known and available."""
        key = (service_id, instance_id)
        rec = self._offers.get(key)
        if rec is None or not rec.available:
            return
        if key not in self._wanted or not self._wanted[key]["eventgroups"]:
            return
        if key not in self._subscribed:
            self._init_subscribe(key)

    # -- core --------------------------------------------------------------
    def process(self, now=None):
        now = self._now() if now is None else now
        for key, state in list(self._wanted.items()):
            if key in self._offers and self._offers[key].available:
                continue  # found -> stop advertising
            deadline = self._next_find.get(key)
            if deadline is None or now < deadline:
                continue
            self._emit_find(key)
            self._schedule_next_find(key, now)
        # subscription renewal for active subs (each cycle)
        for key in list(self._subscribed):
            if now >= self._subscribed[key]["renew_at"]:
                self._emit_subscribe(key)
        return self._drain()

    def _init_delay(self):
        lo = SD_INITIAL_DELAY_MIN
        hi = max(lo + 0.001, SD_INITIAL_DELAY_MAX)
        return self._rand.uniform(lo, hi)

    def _schedule_next_find(self, key, now):
        step = self._find_step[key]
        if step < SD_REPETITIONS_MAX:
            delay = SD_REQUESTS_BASE_DELAY * (1 << step)
            self._find_step[key] = step + 1
        else:
            delay = SD_DEFAULT_CYCLE
        self._next_find[key] = now + delay

    def _emit_find(self, key):
        svc, inst = key
        entry = encode_entry(SdEntryType.FIND_SERVICE, svc, inst, 0x01,
                             SD_DEFAULT_TTL, minor=0)
        self._send_queue.append(
            build_sd_message([entry], [], header_client=self.client_id,
                             header_session=next_session(self)))

    def _emit_subscribe(self, key):
        svc, inst = key
        groups = self.subscribed_eventgroups(svc, inst)
        if not groups:
            self._subscribed.pop(key, None)
            return
        offer = self._offers.get(key)
        if offer is None:
            return
        entries = [encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP, svc, inst,
                                offer.major, SD_DEFAULT_TTL,
                                eventgroup_id=gid, idx1=0, opts1=1)
                   for gid in sorted(groups)]
        options = [encode_option_ipv4(self.client_address, self.client_port)]
        self._send_queue.append(
            build_sd_message(entries, options, header_client=self.client_id,
                             header_session=next_session(self)))

    def handle_datagram(self, raw, src_addr):
        msg = _frame_from(raw)
        if msg.header.service_id != SD_SERVICE_ID:
            return
        try:
            entries = parse_sd_message(msg)
        except SdError:
            return
        for e in entries:
            key = (e["service_id"], e["instance_id"])
            if e["type"] == SdEntryType.OFFER_SERVICE:
                self._on_offer(key, e)
            elif e["type"] == SdEntryType.SUBSCRIBE_EVENTGROUP_ACK:
                self._acks.add(key + (e["eventgroup_id"],))
                self._nacks.discard(key + (e["eventgroup_id"],))
                if key in self._subscribed:
                    self._subscribed[key]["renew_at"] = self._now() + SD_DEFAULT_TTL
                if self.on_subscribe_ok:
                    self.on_subscribe_ok(key, e["eventgroup_id"])
            elif e["type"] == SdEntryType.SUBSCRIBE_EVENTGROUP_NACK:
                self._nacks.add(key + (e["eventgroup_id"],))
                self._acks.discard(key + (e["eventgroup_id"],))
                self._subscribed.pop(key, None)
                if self.on_subscribe_nack:
                    self.on_subscribe_nack(key, e["eventgroup_id"])

    def _on_offer(self, key, e):
        ttl = e["ttl"]
        if ttl == 0:  # StopOffer -> unavailable
            rec = self._offers.pop(key, None)
            if rec and rec.available and self.on_unavailable:
                self.on_unavailable(key)
                rec.available = False
            return
        ep = None
        for opt in e["options"]:
            if opt["type"] == SdOptionType.IPV4_ENDPOINT:
                ep = ipv4_endpoint(opt)
                if ep:
                    break
        address, port = ep if ep else ("0.0.0.0", 0)
        deadline = self._now() + ttl
        rec = OfferRecord(e["service_id"], e["instance_id"], e["major_version"],
                          e["minor_version"], address, port, deadline)
        was_available = key in self._offers and self._offers[key].available
        self._offers[key] = rec
        if key not in self._subscribed:
            # start (or restart) the subscription when event groups are wanted
            if self.subscribed_eventgroups(key[0], key[1]):
                self._init_subscribe(key)
        if not was_available:
            if self.on_available:
                self.on_available(key)

    def _init_subscribe(self, key):
        self._subscribed[key] = {"renew_at": self._now() + SD_DEFAULT_TTL}
        self._emit_subscribe(key)

    def tick(self, now=None):
        now = self._now() if now is None else now
        expired = [k for k, rec in self._offers.items()
                   if rec.available and now >= rec.deadline]
        for k in expired:
            self._offers[k].available = False
            if self.on_unavailable:
                self.on_unavailable(k)

    def _drain(self):
        out = self._send_queue
        self._send_queue = []
        return out

    def first_session(self):
        return self._session


def next_session(machine):
    s = machine._session
    machine._session = (s + 1) & 0xFFFF
    return s


def _frame_from(raw):
    return Message.from_bytes(raw, strict=True)


# ------------------------------------------------------- service publisher ----

class OfferedService:
    __slots__ = ("service_id", "instance_id", "major", "minor", "address",
                 "port", "eventgroups")

    def __init__(self, service_id, instance_id, major, minor, address, port,
                 eventgroups=()):
        self.service_id = service_id
        self.instance_id = instance_id
        self.major = major
        self.minor = minor
        self.address = address
        self.port = port
        self.eventgroups = set(eventgroups)


class ServicePublisher:
    """Service-side discovery: advertise offers, answer finds, ack subscribes."""

    def __init__(self, offers=(), *, now=None, rand=None,
                 interface=SD_MULTICAST_ADDRESS):
        self._now = now or time.monotonic
        self._rand = rand or random.Random()
        self._services = {o.service_id: {o.instance_id: o} for o in offers}
        self._offers_by_key = {(o.service_id, o.instance_id): o
                             for o in offers}
        self._send_queue = []
        self._phase = {}       # key -> {"announce": bool, "next": t, "step": n}
        self._subscribers = {}  # key -> set((address, port))
        self._last_client_session = {}  # sawtooth guard
        self.interface = interface
        self._init_schedule()

    def _init_schedule(self):
        now = self._now()
        for key in self._offers_by_key:
            self._phase[key] = {
                "announce": True,
                "next": now + self._rand.uniform(SD_INITIAL_DELAY_MIN,
                                                 SD_INITIAL_DELAY_MAX),
                "step": 0,
            }

    # -- service set -------------------------------------------------------
    def add_service(self, offered):
        self._offers_by_key[(offered.service_id, offered.instance_id)] = offered
        self._services.setdefault(offered.service_id, {})[
            offered.instance_id] = offered
        now = self._now()
        self._phase[(offered.service_id, offered.instance_id)] = {
            "announce": True, "next": now + self._init_delay(), "step": 0}

    def _init_delay(self):
        return self._rand.uniform(SD_INITIAL_DELAY_MIN,
                                  max(SD_INITIAL_DELAY_MAX, SD_INITIAL_DELAY_MIN + 0.001))

    def _offer_entry(self, o):
        return encode_entry(SdEntryType.OFFER_SERVICE, o.service_id,
                            o.instance_id, o.major, SD_DEFAULT_TTL,
                            minor=o.minor,
                            idx1=0, opts1=1)

    def _offer_message(self, o, stop=False):
        entry = self._offer_entry(o) if not stop else encode_entry(
            SdEntryType.OFFER_SERVICE, o.service_id, o.instance_id, o.major, 0,
            minor=o.minor, idx1=0, opts1=1)
        options = [] if stop else [encode_option_ipv4(o.address, o.port)]
        return build_sd_message([entry], options)

    def process(self, now=None):
        now = self._now() if now is None else now
        for key, st in list(self._phase.items()):
            if st["announce"] and now >= st["next"]:
                self._emit_offer(key, now)
        return self._drain()

    def _emit_offer(self, key, now):
        st = self._phase[key]
        if st["step"] < SD_REPETITIONS_MAX:
            delay = SD_REPETITIONS_BASE_DELAY * (1 << st["step"])
            st["step"] += 1
        else:
            delay = SD_DEFAULT_CYCLE  # steady-state periodic announcements
        self._send_queue.append(self._offer_message(self._offers_by_key[key]))
        st["next"] = now + delay

    def handle_datagram(self, raw, src_addr):
        msg = _frame_from(raw)
        if msg.header.service_id != SD_SERVICE_ID:
            return
        # sawtooth: ignore stale/older session ids from the same client
        src = src_addr[0]
        sid = msg.header.session_id
        last = self._last_client_session.get(src, -1)
        if last >= 0 and (sid - last) & 0xFFFF > 0x7FFF:
            return  # out-of-order/replay guard
        self._last_client_session[src] = sid
        try:
            entries = parse_sd_message(msg)
        except SdError:
            return
        for e in entries:
            key = (e["service_id"], e["instance_id"])
            offered = self._offers_by_key.get(key)
            if offered is None:
                continue
            if e["type"] == SdEntryType.FIND_SERVICE:
                self._send_queue.append(self._offer_message(offered))
            elif e["type"] == SdEntryType.SUBSCRIBE_EVENTGROUP:
                if e["eventgroup_id"] in offered.eventgroups:
                    target = None
                    for opt in e["options"]:
                        if opt["type"] == SdOptionType.IPV4_ENDPOINT:
                            ep = ipv4_endpoint(opt)
                            if ep:
                                target = ep
                                break
                    if target is None:
                        target = src_addr  # fall back to datagram source
                    self._subscribers.setdefault(key, set()).add(target)
                    self._send_queue.append(self._subscribe_ack(offered, e,
                                                                target))
                else:
                    self._send_queue.append(self._subscribe_nack(offered, e,
                                                                 src_addr))

    def _subscribe_ack(self, offered, entry, src_addr):
        entry = encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP_ACK,
                             offered.service_id, offered.instance_id,
                             offered.major, SD_DEFAULT_TTL,
                             eventgroup_id=entry["eventgroup_id"])
        return build_sd_message([entry], [], header_client=0x0000,
                                header_session=1)

    def _subscribe_nack(self, offered, entry, src_addr):
        entry = encode_entry(SdEntryType.SUBSCRIBE_EVENTGROUP_NACK,
                             offered.service_id, offered.instance_id,
                             offered.major, SD_DEFAULT_TTL,
                             eventgroup_id=entry["eventgroup_id"])
        return build_sd_message([entry], [], header_client=0x0000,
                                header_session=1)

    def subscribers(self, service_id, instance_id):
        return self._subscribers.get((service_id, instance_id), set())

    def stop(self):
        """Emit StopOffer (TTL=0) for all advertised services."""
        for o in self._offers_by_key.values():
            self._send_queue.append(self._offer_message(o, stop=True))
        return self._drain()

    def _drain(self):
        out = self._send_queue
        self._send_queue = []
        return out


__all__ = [
    "SD_SERVICE_ID", "SD_METHOD_ID", "SD_INTERFACE_VERSION", "ENTRY_SIZE",
    "SdError", "OfferRecord", "OfferedService",
    "ServiceMonitor", "ServicePublisher",
    "encode_entry", "parse_entry", "encode_option_ipv4", "parse_options",
    "attach_options", "build_sd_message", "parse_sd_message", "ipv4_endpoint",
]