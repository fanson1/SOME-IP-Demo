import socket
import struct

from .constants import (MessageType, ReturnCode, SD_INTERFACE_VERSION,
                        SD_METHOD_ID, SD_PROTOCOL_VERSION, SD_SERVICE_ID,
                        SdEntryType, SdOptionType)
from .header import SomeIpMessage

ENTRY_SIZE = 16


class SdOption:
    def __init__(self, option_type, value, length=None):
        self.option_type = option_type
        self.value = value
        self.length = 2 + len(value) if length is None else length

    def serialize(self):
        return struct.pack(">HBB", self.length, self.option_type, 0x00) + self.value

    @classmethod
    def deserialize(cls, data, offset):
        length, option_type, _reserved = struct.unpack(">HBB", data[offset:offset + 4])
        value = data[offset + 4:offset + 2 + length]
        return cls(option_type, value, length), offset + 2 + length

    def endpoint(self):
        if self.option_type != SdOptionType.IPV4_ENDPOINT:
            return None
        address = socket.inet_ntoa(self.value[0:4])
        protocol = self.value[4]
        port = struct.unpack(">H", self.value[5:7])[0]
        return address, port, protocol


def build_option_ipv4_endpoint(address, port, protocol=0x11):
    value = struct.pack(">4sBH", socket.inet_aton(address), protocol, port)
    return SdOption(SdOptionType.IPV4_ENDPOINT, value)


def build_entry(entry_type, service_id, instance_id, major_version, ttl,
                minor_version=0, index_first_option=0, index_second_option=0,
                n_options=0, n_options2=0, eventgroup_id=None):
    counts = ((n_options2 & 0x0F) << 4) | (n_options & 0x0F)
    head = struct.pack(">BBBBHHB", entry_type, index_first_option,
                       index_second_option, counts, service_id,
                       instance_id, major_version)
    ttl_bytes = struct.pack(">I", ttl & 0xFFFFFF)[1:]
    if eventgroup_id is not None:
        tail = struct.pack(">HH", eventgroup_id, 0x0000)
    else:
        tail = struct.pack(">I", minor_version)
    return head + ttl_bytes + tail


def parse_entry(data):
    (entry_type, index_first, index_second, counts, service_id,
     instance_id, major_version) = struct.unpack(">BBBBHHB", data[0:9])
    ttl = struct.unpack(">I", b"\x00" + data[8:11])[0]
    tail = data[12:16]
    entry = {
        "type": entry_type,
        "index_first_option": index_first,
        "index_second_option": index_second,
        "n_options": counts & 0x0F,
        "n_options2": (counts >> 4) & 0x0F,
        "service_id": service_id,
        "instance_id": instance_id,
        "major_version": major_version,
        "ttl": ttl,
        "minor_version": None,
        "eventgroup_id": None,
        "options": [],
    }
    if entry_type in (SdEntryType.SUBSCRIBE_EVENTGROUP,
                      SdEntryType.SUBSCRIBE_EVENTGROUP_ACK):
        entry["eventgroup_id"] = struct.unpack(">H", tail[0:2])[0]
    else:
        entry["minor_version"] = struct.unpack(">I", tail)[0]
    return entry


def build_sd_message(entries, options, session_id=1, client_id=0x0000):
    entries_bytes = b"".join(entries)
    options_bytes = b"".join(opt.serialize() for opt in options)
    pad_entries = (-len(entries_bytes)) % 4
    pad_options = (-len(options_bytes)) % 4
    payload = struct.pack(">I", len(entries_bytes)) + entries_bytes
    payload += b"\x00" * pad_entries
    payload += struct.pack(">I", len(options_bytes)) + options_bytes
    payload += b"\x00" * pad_options
    msg = SomeIpMessage(SD_SERVICE_ID, SD_METHOD_ID, payload, client_id,
                        session_id, MessageType.NOTIFICATION, ReturnCode.E_OK,
                        SD_INTERFACE_VERSION, SD_PROTOCOL_VERSION)
    return msg.serialize()


def parse_sd_message(data):
    msg = SomeIpMessage.deserialize(data)
    payload = msg.payload
    entries_len = struct.unpack(">I", payload[0:4])[0]
    entries_raw = payload[4:4 + entries_len]
    offset = 4 + entries_len
    options_len = struct.unpack(">I", payload[offset:offset + 4])[0]
    options_raw = payload[offset + 4:offset + 4 + options_len]

    options = []
    cursor = 0
    while cursor < options_len:
        option, cursor = SdOption.deserialize(options_raw, cursor)
        options.append(option)

    entries = []
    for i in range(0, entries_len, ENTRY_SIZE):
        entry = parse_entry(entries_raw[i:i + ENTRY_SIZE])
        indices = []
        if entry["n_options"]:
            indices += list(range(entry["index_first_option"],
                                  entry["index_first_option"] + entry["n_options"]))
        if entry["n_options2"]:
            indices += list(range(entry["index_second_option"],
                                  entry["index_second_option"] + entry["n_options2"]))
        entry["options"] = [options[i] for i in indices if i < len(options)]
        entries.append(entry)
    return msg, entries, options
