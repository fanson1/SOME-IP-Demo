"""SOME/IP v2 types, constants and exceptions (production-oriented stack)."""

PROTOCOL_VERSION = 0x01

# UDP/IPv4 safe payload limit for a single unfragmented SOME/IP message.
MAX_PAYLOAD_NO_TP = 1392


class MessageType:
    REQUEST = 0x00
    REQUEST_NO_RETURN = 0x01
    NOTIFICATION = 0x02
    RESPONSE = 0x80
    ERROR = 0x81


class ReturnCode:
    E_OK = 0x00
    E_NOT_OK = 0x01
    E_UNKNOWN_SERVICE = 0x02
    E_UNKNOWN_METHOD = 0x03
    E_NOT_READY = 0x04
    E_NOT_REACHABLE = 0x05
    E_TIMEOUT = 0x06
    E_WRONG_PROTOCOL_VERSION = 0x07
    E_WRONG_INTERFACE_VERSION = 0x08
    E_MALFORMED_MESSAGE = 0x09
    E_WRONG_MESSAGE_TYPE = 0x0A
    E_E2E_REPEATED = 0x0B
    E_E2E_WRONG_SEQUENCE = 0x0C
    E_E2E = 0x0D
    E_E2E_NOT_AVAILABLE = 0x0E
    E_E2E_NO_NEW_DATA = 0x0F


class TpFlag:
    TP_REQUEST = 0x20
    TP_RESPONSE = 0x21
    TP_ERROR = 0x22
    TP_NOTIFICATION = 0x23
    TP_REQUEST_NO_RETURN = 0x24


# SD (synchronous with spec 2.2.2)
SD_SERVICE_ID = 0xFFFF
SD_METHOD_ID = 0x8100
SD_PROTOCOL_VERSION = 0x01
SD_INTERFACE_VERSION = 0x01
SD_MULTICAST_ADDRESS = "224.244.224.245"
SD_PORT = 30490
SD_DEFAULT_TTL = 3
SD_DEFAULT_CYCLE = 2.0
SD_INITIAL_DELAY_MIN = 0.0
SD_INITIAL_DELAY_MAX = 0.1
SD_REPETITIONS_BASE_DELAY = 0.2
SD_REPETITIONS_MAX = 3
SD_REQUESTS_BASE_DELAY = 0.2
SD_SUBSCRIBE_BASE_DELAY = 0.2


class SdEntryType:
    FIND_SERVICE = 0x00
    OFFER_SERVICE = 0x01
    STOP_OFFER_SERVICE = 0x01  # encoded same as OFFER with ttl==0
    SUBSCRIBE_EVENTGROUP = 0x06
    SUBSCRIBE_EVENTGROUP_ACK = 0x07
    SUBSCRIBE_EVENTGROUP_NACK = 0x08


class SdOptionType:
    CONFIGURATION = 0x00
    IPV4_ENDPOINT = 0x04
    IPV6_ENDPOINT = 0x06
    IPV4_MULTICAST = 0x14
    IPV6_MULTICAST = 0x16


class SomeIpError(Exception):
    """Base error for the v2 stack."""


class MalformedMessage(SomeIpError):
    """Wire-level parse failure."""


class OversizedMessage(SomeIpError):
    """Payload exceeds transport limits and no TP was used."""