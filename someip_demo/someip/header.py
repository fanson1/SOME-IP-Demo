import struct

from .constants import PROTOCOL_VERSION

HEADER_FORMAT = ">HHIHHBBBB"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


class SomeIpMessage:
    def __init__(self, service_id, method_id, payload=b"", client_id=0,
                 session_id=0, message_type=0x00, return_code=0x00,
                 interface_version=0x01, protocol_version=PROTOCOL_VERSION):
        self.service_id = service_id
        self.method_id = method_id
        self.payload = payload
        self.client_id = client_id
        self.session_id = session_id
        self.message_type = message_type
        self.return_code = return_code
        self.interface_version = interface_version
        self.protocol_version = protocol_version

    @property
    def message_id(self):
        return (self.service_id << 16) | self.method_id

    @property
    def request_id(self):
        return (self.client_id << 16) | self.session_id

    @property
    def length(self):
        return 8 + len(self.payload)

    def serialize(self):
        header = struct.pack(
            HEADER_FORMAT,
            self.service_id,
            self.method_id,
            self.length,
            self.client_id,
            self.session_id,
            self.protocol_version,
            self.interface_version,
            self.message_type,
            self.return_code,
        )
        return header + self.payload

    @classmethod
    def deserialize(cls, data):
        if len(data) < HEADER_SIZE:
            raise ValueError("SOME/IP packet too short")
        (service_id, method_id, length, client_id, session_id,
         protocol_version, interface_version, message_type,
         return_code) = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
        payload = data[HEADER_SIZE:8 + length]
        return cls(service_id, method_id, payload, client_id, session_id,
                   message_type, return_code, interface_version,
                   protocol_version)

    def __repr__(self):
        return ("SomeIpMessage(service=0x%04X, method=0x%04X, type=0x%02X, "
                "return=0x%02X, payload=%d bytes)"
                % (self.service_id, self.method_id, self.message_type,
                   self.return_code, len(self.payload)))
