import random
import socket
import struct
import threading
import time

from .constants import (MessageType, ReturnCode, SD_MULTICAST_ADDRESS,
                        SD_PORT, SD_TTL, SdEntryType)
from .header import SomeIpMessage
from .net import local_ip, make_sd_socket, make_udp_socket
from .sd import (build_entry, build_option_ipv4_endpoint, build_sd_message,
                 parse_sd_message)


class SomeIpClient:
    def __init__(self, service_id, instance_id, major_version=0x01,
                 interface_version=0x01, sd_port=SD_PORT, interface_ip=None,
                 client_id=None):
        self.service_id = service_id
        self.instance_id = instance_id
        self.major_version = major_version
        self.interface_version = interface_version
        self.sd_port = sd_port
        self.interface_ip = interface_ip or local_ip()
        self.client_id = (client_id if client_id is not None
                          else random.randint(1, 0xFFFE))

        self._session = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._discovered = None
        self._event_callbacks = {}
        self._on_subscribe_ack = None

        self.method_sock = make_udp_socket(0)
        self.event_sock = make_udp_socket(0)
        self.sd_sock = make_sd_socket(sd_port, SD_MULTICAST_ADDRESS,
                                      self.interface_ip)

    def on_event(self, event_id, callback):
        self._event_callbacks[event_id] = callback

    @property
    def event_port(self):
        return self.event_sock.getsockname()[1]

    def start(self):
        threading.Thread(target=self._event_loop, name="event",
                         daemon=True).start()
        return self

    def stop(self):
        self._stop.set()
        for sock in (self.method_sock, self.event_sock, self.sd_sock):
            try:
                sock.close()
            except OSError:
                pass

    def _next_session(self):
        with self._lock:
            self._session = (self._session + 1) & 0xFFFF
            return self._session

    def _build_sd(self, entries, options):
        return build_sd_message(entries, options, self._next_session(),
                                self.client_id)

    def _send_find(self):
        entry = build_entry(SdEntryType.FIND_SERVICE, self.service_id,
                            self.instance_id, self.major_version, 0)
        self.sd_sock.sendto(self._build_sd([entry], []),
                            (SD_MULTICAST_ADDRESS, self.sd_port))

    def find_service(self, timeout=5.0):
        self._send_find()
        deadline = time.time() + timeout
        self.sd_sock.settimeout(0.5)
        while time.time() < deadline and not self._stop.is_set():
            try:
                data, _addr = self.sd_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return None
            try:
                _msg, entries, _options = parse_sd_message(data)
            except ValueError:
                continue
            for entry in entries:
                if (entry["type"] == SdEntryType.OFFER_SERVICE
                        and entry["ttl"] > 0
                        and entry["service_id"] == self.service_id
                        and entry["instance_id"] == self.instance_id):
                    for opt in entry["options"]:
                        parsed = opt.endpoint()
                        if parsed and parsed[2] == 0x11:
                            self._discovered = (parsed[0], parsed[1])
                            return self._discovered
        return None

    def request(self, method_id, payload=b"", timeout=3.0):
        if self._discovered is None:
            raise RuntimeError("service not discovered, call find_service first")
        session_id = self._next_session()
        msg = SomeIpMessage(self.service_id, method_id, payload,
                            self.client_id, session_id, MessageType.REQUEST,
                            ReturnCode.E_OK, self.interface_version)
        self.method_sock.settimeout(0.2)
        deadline = time.time() + timeout
        self.method_sock.sendto(msg.serialize(), self._discovered)
        while time.time() < deadline and not self._stop.is_set():
            try:
                data, _addr = self.method_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                resp = SomeIpMessage.deserialize(data)
            except ValueError:
                continue
            if (resp.request_id == (self.client_id << 16) | session_id
                    and resp.service_id == self.service_id):
                return resp.return_code, resp.payload
        raise TimeoutError("SOME/IP request timed out")

    def subscribe(self, eventgroup_id, timeout=3.0):
        if self._discovered is None:
            raise RuntimeError("service not discovered")
        option = build_option_ipv4_endpoint(self.interface_ip,
                                            self.event_port)
        entry = build_entry(SdEntryType.SUBSCRIBE_EVENTGROUP, self.service_id,
                            self.instance_id, self.major_version, SD_TTL,
                            eventgroup_id=eventgroup_id,
                            index_first_option=0, n_options=1)
        self.sd_sock.sendto(self._build_sd([entry], [option]),
                            (SD_MULTICAST_ADDRESS, self.sd_port))
        deadline = time.time() + timeout
        self.sd_sock.settimeout(0.5)
        while time.time() < deadline and not self._stop.is_set():
            try:
                data, _addr = self.sd_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return False
            try:
                _msg, entries, _options = parse_sd_message(data)
            except ValueError:
                continue
            for item in entries:
                if (item["type"] == SdEntryType.SUBSCRIBE_EVENTGROUP_ACK
                        and item["service_id"] == self.service_id
                        and item["instance_id"] == self.instance_id
                        and item["eventgroup_id"] == eventgroup_id):
                    return True
        return False

    def _event_loop(self):
        self.event_sock.settimeout(0.5)
        while not self._stop.is_set():
            try:
                data, _addr = self.event_sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg = SomeIpMessage.deserialize(data)
            except ValueError:
                continue
            if msg.message_type != MessageType.NOTIFICATION:
                continue
            callback = self._event_callbacks.get(msg.method_id)
            if callback:
                try:
                    callback(msg.method_id, msg.payload)
                except Exception:
                    pass
            else:
                generic = self._event_callbacks.get(None)
                if generic:
                    try:
                        generic(msg.method_id, msg.payload)
                    except Exception:
                        pass