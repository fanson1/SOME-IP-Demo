"""High-level application API: threaded service and client.

Wires together the layers below into ready-to-use endpoints:

- offline discovery state machines: :class:`sdm.ServicePublisher` (service
  side) and :class:`sdm.ServiceMonitor` (client side) - driven from owner
  threads via ``process()`` / ``handle_datagram()``;
- datagram transport: :class:`UdpEndpoint` for methods, events and SD
  multicast (v2 uses one client socket for both requests and notifications);
- SOME/IP-TP: large payloads (> 1392 B) are transparently segmented on send
  and reassembled on receive.

Thread model
------------
Each endpoint is owned by exactly one thread (service: method + SD + event
publishing from the app thread; client: one loop for SD + app socket). All
mutations from other threads go through thread-safe helpers (``send`` frame
locks, callback dispatch from the owning thread). ``stop()`` is idempotent.
"""

import random
import socket
import struct
import threading
import time

from . import sdm, tpc, transport, wire
from .sdm import OfferedService
from .transport import ReceiveTimeout, TransportError, UdpEndpoint
from .types import (SD_DEFAULT_TTL, SD_MULTICAST_ADDRESS, SD_PORT,
                    MessageType, ReturnCode, SomeIpError)
from .wire import Header, Message

REQUEST_TYPES = (MessageType.REQUEST, MessageType.REQUEST_NO_RETURN)

MAX_IN_FLIGHT_REQUESTS = 1024


def local_ip():
    """Best-effort default-route IPv4, falling back to loopback."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"
    finally:
        s.close()


class _Pending:
    __slots__ = ("service_id", "deadline", "done", "result")

    def __init__(self, service_id, deadline, done):
        self.service_id = service_id
        self.deadline = deadline
        self.done = done
        self.result = None


def _send_message(endpoint, message, addr):
    """Send ``message``, transparently applying SOME/IP-TP framing."""
    for frame in tpc.segment(message):
        endpoint.send(frame.to_bytes(), addr)


class _ReassemblePass:
    """Complete a Message from wire bytes; returns None while TP is partial."""

    def __init__(self):
        self._reasm = tpc.Reassembler()

    def feed(self, data):
        try:
            msg = wire.Message.from_bytes(data, strict=True)
        except (wire.MalformedMessage, wire.PartialMessage, Exception):
            return None
        try:
            return self._reasm.add(msg)
        except tpc.TpError:
            return None


# ================================================================== service =

class SomeipServiceV2:
    """SOME/IP service: RPC methods, fields and eventgroup notifications.

    Handlers have the legacy signature ``handler(payload, addr) -> (rc, payload)``.
    """

    def __init__(self, service_id, instance_id, major_version=0x01,
                 minor_version=0x00000001, method_port=30500, event_port=30501,
                 sd_port=SD_PORT, interface_ip=None, client_id=0x0000):
        self.service_id = service_id
        self.instance_id = instance_id
        self.major_version = major_version
        self.minor_version = minor_version
        self.method_port = method_port
        self.event_port = event_port
        self.sd_port = sd_port
        self.interface_ip = interface_ip or local_ip()

        self.methods = {}
        self.fields = {}
        self.events = {}          # event_id -> eventgroup_id
        self.eventgroups = {}     # eventgroup_id -> set(event_id)

        self._publisher = None
        self._method_ep = None
        self._event_ep = None
        self._sd_ep = None
        self._session = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads = []

    # -- configuration -----------------------------------------------------
    def add_method(self, method_id, handler):
        self.methods[method_id] = handler

    def add_event(self, event_id, eventgroup_id):
        self.events[event_id] = eventgroup_id
        self.eventgroups.setdefault(eventgroup_id, set()).add(event_id)

    def add_uint32_field(self, field_id, eventgroup_id, initial=0):
        self.fields[field_id] = initial & 0xFFFFFFFF

        def getter(_payload, _addr):
            return ReturnCode.E_OK, struct.pack(">I", self.fields[field_id])

        def setter(payload, _addr):
            if len(payload) != 4:
                return ReturnCode.E_MALFORMED_MESSAGE, b""
            value = struct.unpack(">I", payload)[0]
            self.fields[field_id] = value
            self.publish_event(field_id + 2, payload)
            return ReturnCode.E_OK, payload

        self.add_method(field_id, getter)
        self.add_method(field_id + 1, setter)
        self.add_event(field_id + 2, eventgroup_id)

    def set_field(self, field_id, value):
        self.fields[field_id] = value & 0xFFFFFFFF
        self.publish_event(field_id + 2, struct.pack(">I", self.fields[field_id]))

    # -- lifecycle ---------------------------------------------------------
    def start(self):
        self._method_ep = UdpEndpoint.unicast(self.method_port, reuse=True)
        self._event_ep = UdpEndpoint.unicast(self.event_port, reuse=True)
        self._sd_ep = UdpEndpoint.multicast(self.sd_port, SD_MULTICAST_ADDRESS,
                                            self.interface_ip)
        self.method_port = self._method_ep.local_port
        self.event_port = self._event_ep.local_port
        offered = OfferedService(self.service_id, self.instance_id,
                                 self.major_version, self.minor_version,
                                 self.interface_ip, self.method_port,
                                 eventgroups=tuple(self.eventgroups))
        self._publisher = sdm.ServicePublisher([offered],
                                               interface=SD_MULTICAST_ADDRESS)
        self._threads = [
            threading.Thread(target=self._method_loop, name="method",
                             daemon=True),
            threading.Thread(target=self._sd_loop, name="sd", daemon=True),
        ]
        for t in self._threads:
            t.start()
        return self

    def stop(self):
        self._stop.set()
        self._flush_sd(self._publisher.stop() if self._publisher else ())
        for ep in (self._method_ep, self._event_ep, self._sd_ep):
            if ep is not None:
                try:
                    ep.close()
                except Exception:
                    pass
        for t in self._threads:
            if t is not None:
                t.join(timeout=1.0)
        self._threads = []

    # -- notifications -----------------------------------------------------
    def publish_event(self, event_id, payload):
        eventgroup_id = self.events.get(event_id)
        if eventgroup_id is None or self._event_ep is None:
            return
        msg = Message(
            Header(self.service_id, event_id, 0x0000, self._next_session(),
                   message_type=MessageType.NOTIFICATION,
                   interface_version=self.major_version, return_code=0x00),
            payload)
        targets = list(self._publisher.subscribers(self.service_id,
                                                   self.instance_id))
        for addr in targets:
            try:
                _send_message(self._event_ep, msg, addr)
            except TransportError:
                pass

    # -- internal ----------------------------------------------------------
    def _next_session(self):
        with self._lock:
            self._session = (self._session + 1) & 0xFFFF
            return self._session

    def _method_loop(self):
        recon = _ReassemblePass()
        while not self._stop.is_set():
            try:
                data, addr = self._method_ep.recv(timeout=0.2)
            except ReceiveTimeout:
                continue
            except (TransportError, OSError):
                break
            msg = recon.feed(data)
            if msg is None:
                continue
            self._handle_request(msg, addr)

    def _handle_request(self, msg, addr):
        header = msg.header
        if header.service_id != self.service_id:
            return
        if header.message_type not in REQUEST_TYPES:
            return
        handler = self.methods.get(header.method_id)
        if handler is None:
            rc, response = ReturnCode.E_UNKNOWN_METHOD, b""
        else:
            try:
                rc, response = handler(msg.payload, addr)
            except Exception:
                rc, response = ReturnCode.E_NOT_OK, b""
        if header.message_type == MessageType.REQUEST:
            resp = Message(
                Header(self.service_id, header.method_id, header.client_id,
                       header.session_id, message_type=MessageType.RESPONSE,
                       interface_version=self.major_version, return_code=rc),
                response)
            try:
                _send_message(self._method_ep, resp, addr)
            except TransportError:
                pass

    def _sd_loop(self):
        while not self._stop.is_set():
            try:
                data, addr = self._sd_ep.recv(timeout=0.1)
            except ReceiveTimeout:
                data = None
            except (TransportError, OSError):
                break
            if data is not None:
                try:
                    self._publisher.handle_datagram(data, addr)
                except (sdm.SdError, wire.MalformedMessage,
                        wire.PartialMessage):
                    pass
            self._flush_sd(self._publisher.process())

    def _flush_sd(self, messages):
        for message in messages:
            try:
                self._sd_ep.send(message.to_bytes(),
                                 (SD_MULTICAST_ADDRESS, self.sd_port))
            except TransportError:
                pass


# ==================================================================== client =

class ClientV2:
    """SOME/IP client: discovery, RPC, subscriptions and notifications."""

    def __init__(self, client_id=None, sd_port=SD_PORT, interface_ip=None):
        self.client_id = client_id or random.randint(1, 0xFFFE)
        self.sd_port = sd_port
        self.interface_ip = interface_ip or local_ip()

        self._monitor = sdm.ServiceMonitor(client_id=self.client_id)
        self._session = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._pending = {}
        self._event_callbacks = {}
        self._avail = []
        self._unavail = []
        self._sub_ok = None
        self._sub_nack = None

        self._endpoint = None   # requests + notifications, single UDP socket
        self._sd_ep = None
        self._thread = None

    # -- configuration -----------------------------------------------------
    def on_event(self, event_id, callback):
        """Callback ``fn(event_id, payload)`` for a notification on ``event_id``."""
        self._event_callbacks[event_id] = callback

    def on_available(self, fn):
        self._avail.append(fn)

    def on_unavailable(self, fn):
        self._unavail.append(fn)

    def on_subscribe_ok(self, fn):
        self._sub_ok = fn

    def on_subscribe_nack(self, fn):
        self._sub_nack = fn

    # -- lifecycle ---------------------------------------------------------
    def start(self):
        self._endpoint = UdpEndpoint.unicast(0)
        self._sd_ep = UdpEndpoint.multicast(self.sd_port, SD_MULTICAST_ADDRESS,
                                            self.interface_ip)
        self._monitor.client_address = self.interface_ip
        self._monitor.client_port = self._endpoint.local_port
        self._monitor.on_available = self._dispatch_available
        self._monitor.on_unavailable = self._dispatch_unavailable
        self._monitor.on_subscribe_ok = self._dispatch_sub_ok
        self._monitor.on_subscribe_nack = self._dispatch_sub_nack
        self._thread = threading.Thread(target=self._loop, name="client",
                                        daemon=True)
        self._thread.start()
        return self

    def _dispatch_available(self, key):
        for fn in self._avail:
            try:
                fn(key[0], key[1])
            except Exception:
                pass

    def _dispatch_unavailable(self, key):
        for fn in self._unavail:
            try:
                fn(key[0], key[1])
            except Exception:
                pass

    def _dispatch_sub_ok(self, key, gid):
        if self._sub_ok is not None:
            try:
                self._sub_ok(key[0], key[1], gid)
            except Exception:
                pass

    def _dispatch_sub_nack(self, key, gid):
        if self._sub_nack is not None:
            try:
                self._sub_nack(key[0], key[1], gid)
            except Exception:
                pass

    def stop(self):
        self._stop.set()
        for ep in (self._endpoint, self._sd_ep):
            if ep is not None:
                try:
                    ep.close()
                except Exception:
                    pass
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

    # -- high-level ops ----------------------------------------------------
    def wait_for_service(self, service_id, instance_id, eventgroups=(),
                         timeout=10.0):
        """Start discovery and block until the service is available."""
        self._monitor.find(service_id, instance_id, eventgroups)
        marker = threading.Event()
        record = self._monitor.offer(service_id, instance_id)
        if record is not None and record.available:
            marker.set()

        def on_avail(s, i):
            if s == service_id and i == instance_id and self._monitor.offer(
                    s, i) is not None and self._monitor.offer(s, i).available:
                marker.set()

        self.on_available(on_avail)
        return marker.wait(timeout)

    def discovered_endpoint(self, service_id, instance_id):
        """Return ``(address, port)`` of a located service, or ``None``."""
        record = self._monitor.offer(service_id, instance_id)
        if record is None or not record.available:
            return None
        return (record.address, record.port)

    def request(self, service_id, instance_id, method_id, payload=b"",
                timeout=3.0):
        offer = self._monitor.offer(service_id, instance_id)
        if offer is None or not offer.available:
            raise RuntimeError("service 0x%04X/0x%04X not discovered"
                               % (service_id, instance_id))
        session = self._next_session()
        done = threading.Event()
        with self._lock:
            if len(self._pending) >= MAX_IN_FLIGHT_REQUESTS:
                raise SomeIpError(
                    "in-flight request limit reached (backpressure)")
            self._pending[session] = _Pending(service_id, time.monotonic() + timeout, done)
        pend = self._pending[session]
        msg = Message(
            Header(service_id, method_id, self.client_id, session,
                   message_type=MessageType.REQUEST,
                   interface_version=0x01),
            payload)
        _send_message(self._endpoint, msg, (offer.address, offer.port))
        if not done.wait(timeout):
            with self._lock:
                self._pending.pop(session, None)
            raise TimeoutError("SOME/IP request 0x%04X timed out" % method_id)
        return pend.result if pend.result is not None else (ReturnCode.E_NOT_OK, b"")

    def subscribe(self, service_id, instance_id, eventgroups=(),
                  ack_timeout=5.0):
        """Join event groups (blocking until Ack/Nack or timeout)."""
        if not self.wait_for_service(service_id, instance_id, eventgroups,
                                     timeout=ack_timeout):
            return False
        self._monitor.resubscribe(service_id, instance_id)
        result = [None]
        done = threading.Event()

        def on_ok(s, i, g):
            if s == service_id and i == instance_id:
                result[0] = True
                done.set()

        def on_nack(s, i, g):
            if s == service_id and i == instance_id:
                result[0] = False
                done.set()

        self._sub_ok = on_ok
        self._sub_nack = on_nack
        # the Ack may already have been processed before we registered
        for gid in eventgroups:
            state = self._monitor.subscription_state(service_id, instance_id,
                                                     gid)
            if state == "ack":
                result[0] = True
                done.set()
            elif state == "nack":
                result[0] = False
                done.set()
        if not done.wait(ack_timeout):
            return False
        return True if result[0] else False

    def resubscribe(self, service_id, instance_id):
        """Re-emit SUBSCRIBE for an already-discovered service (recovery)."""
        self._monitor.resubscribe(service_id, instance_id)

    # -- internal ----------------------------------------------------------
    def _next_session(self):
        with self._lock:
            self._session = (self._session + 1) & 0xFFFF
            return self._session

    def _loop(self):
        recon = _ReassemblePass()
        while not self._stop.is_set():
            self._poll(self._sd_ep, recon, sd=True)
            self._poll(self._endpoint, recon, sd=False)
            try:
                for message in self._monitor.process():
                    self._sd_ep.send(message.to_bytes(),
                                     (SD_MULTICAST_ADDRESS, self.sd_port))
            except SomeIpError:
                pass
            except TransportError:
                pass
            self._monitor.tick()
            recon._reasm.tick()
            self._expire_pending()

    def _poll(self, endpoint, recon, sd):
        while True:
            try:
                data, addr = endpoint.recv(timeout=0.02)
            except ReceiveTimeout:
                return
            except (TransportError, OSError):
                return
            if sd:
                try:
                    self._monitor.handle_datagram(data, addr)
                except (sdm.SdError, wire.MalformedMessage,
                        wire.PartialMessage, SomeIpError):
                    pass
                continue
            msg = recon.feed(data)
            if msg is None:
                continue
            self._dispatch(msg)

    def _dispatch(self, msg):
        header = msg.header
        if header.message_type in (MessageType.RESPONSE, MessageType.ERROR):
            with self._lock:
                pend = self._pending.pop(header.session_id, None)
            if pend is not None and pend.service_id == header.service_id:
                pend.result = (header.return_code, msg.payload)
                pend.done.set()
            return
        if header.message_type == MessageType.NOTIFICATION:
            cb = self._event_callbacks.get(header.method_id)
            if cb is not None:
                try:
                    cb(header.method_id, msg.payload)
                except Exception:
                    pass
            return

    def _expire_pending(self):
        now = time.monotonic()
        stale = [s for s, p in list(self._pending.items())
                 if p.deadline < now]
        for s in stale:
            with self._lock:
                p = self._pending.pop(s, None)
            if p is not None:
                p.done.set()


__all__ = [
    "SomeipServiceV2", "ClientV2", "local_ip",
]