import socket
import struct
import threading

from .constants import (MessageType, ReturnCode, SD_CYCLE, SD_MULTICAST_ADDRESS,
                        SD_PORT, SD_TTL, SdEntryType)
from .header import SomeIpMessage
from .net import local_ip, make_sd_socket, make_udp_socket
from .sd import (build_entry, build_option_ipv4_endpoint, build_sd_message,
                 parse_sd_message)


class SomeIpService:
    def __init__(self, service_id, instance_id, major_version=0x01,
                 minor_version=0x00000001, method_port=30500,
                 event_port=30501, sd_port=SD_PORT, interface_ip=None):
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
        self.events = {}
        self.eventgroups = {}
        self.subscribers = {}

        self._session = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads = []
        self._method_sock = None
        self._event_sock = None
        self._sd_sock = None

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

    def publish_event(self, event_id, payload):
        eventgroup_id = self.events.get(event_id)
        if eventgroup_id is None or self._event_sock is None:
            return
        msg = SomeIpMessage(self.service_id, event_id, payload, 0x0000,
                            self._next_session(), MessageType.NOTIFICATION,
                            ReturnCode.E_OK, self.major_version)
        data = msg.serialize()
        targets = list(self.subscribers.get(eventgroup_id, ()))
        for addr in targets:
            try:
                self._event_sock.sendto(data, addr)
            except OSError:
                pass

    def start(self):
        self._method_sock = make_udp_socket(self.method_port)
        self._event_sock = make_udp_socket(self.event_port)
        self._sd_sock = make_sd_socket(self.sd_port,
                                       SD_MULTICAST_ADDRESS, self.interface_ip)
        for sock in (self._method_sock, self._event_sock, self._sd_sock):
            sock.settimeout(0.5)
        self._threads = [
            threading.Thread(target=self._method_loop, name="method",
                             daemon=True),
            threading.Thread(target=self._sd_loop, name="sd", daemon=True),
            threading.Thread(target=self._offer_loop, name="offer",
                             daemon=True),
        ]
        for t in self._threads:
            t.start()
        return self

    def stop(self):
        self._stop.set()
        for sock in (self._method_sock, self._event_sock, self._sd_sock):
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass

    def _next_session(self):
        with self._lock:
            self._session = (self._session + 1) & 0xFFFF
            return self._session

    def _build_offer(self):
        option = build_option_ipv4_endpoint(self.interface_ip, self.method_port)
        entry = build_entry(SdEntryType.OFFER_SERVICE, self.service_id,
                            self.instance_id, self.major_version, SD_TTL,
                            self.minor_version, index_first_option=0,
                            n_options=1)
        return build_sd_message([entry], [option], self._next_session())

    def _send_offer(self):
        if self._sd_sock is None:
            return
        self._sd_sock.sendto(self._build_offer(),
                             (SD_MULTICAST_ADDRESS, self.sd_port))

    def _offer_loop(self):
        while not self._stop.is_set():
            try:
                self._send_offer()
            except OSError:
                break
            self._stop.wait(SD_CYCLE)

    def _method_loop(self):
        while not self._stop.is_set():
            try:
                data, addr = self._method_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self._handle_request(SomeIpMessage.deserialize(data), addr)
            except ValueError:
                continue

    def _handle_request(self, msg, addr):
        if msg.service_id != self.service_id:
            return
        if msg.message_type not in (MessageType.REQUEST,
                                    MessageType.REQUEST_NO_RETURN):
            return
        handler = self.methods.get(msg.method_id)
        if handler is None:
            rc, response = ReturnCode.E_UNKNOWN_METHOD, b""
        else:
            try:
                rc, response = handler(msg.payload, addr)
            except Exception:
                rc, response = ReturnCode.E_NOT_OK, b""
        if msg.message_type == MessageType.REQUEST:
            ack = SomeIpMessage(self.service_id, msg.method_id, response,
                                msg.client_id, msg.session_id,
                                MessageType.RESPONSE, rc, self.major_version)
            try:
                self._method_sock.sendto(ack.serialize(), addr)
            except OSError:
                pass

    def _sd_loop(self):
        while not self._stop.is_set():
            try:
                data, _addr = self._sd_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                _msg, entries, _options = parse_sd_message(data)
            except ValueError:
                continue
            for entry in entries:
                if (entry["service_id"] != self.service_id
                        or entry["instance_id"] != self.instance_id):
                    continue
                if entry["type"] == SdEntryType.FIND_SERVICE:
                    self._send_offer()
                elif entry["type"] == SdEntryType.SUBSCRIBE_EVENTGROUP:
                    self._on_subscribe(entry)

    def _on_subscribe(self, entry):
        endpoint = None
        for opt in entry["options"]:
            parsed = opt.endpoint()
            if parsed and parsed[2] == 0x11:
                endpoint = (parsed[0], parsed[1])
                break
        if endpoint is None:
            return
        eventgroup_id = entry["eventgroup_id"]
        self.subscribers.setdefault(eventgroup_id, set()).add(endpoint)
        ack = build_entry(SdEntryType.SUBSCRIBE_EVENTGROUP_ACK,
                          self.service_id, self.instance_id,
                          self.major_version, SD_TTL,
                          eventgroup_id=eventgroup_id)
        data = build_sd_message([ack], [], self._next_session())
        try:
            self._sd_sock.sendto(data, (SD_MULTICAST_ADDRESS, self.sd_port))
        except OSError:
            pass