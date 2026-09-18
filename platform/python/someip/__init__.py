"""SOME/IP production stack (v2).

Module layout mirrors the protocol layers:
  someip.types   types / constants / exceptions
  someip.wire    SOME/IP header + message framing with strict validation
  someip.ser     AUTOSAR wire-format (de)serialization
  someip.transport   UDP/TCP endpoints with pollable receive and framing
  someip.tpc         SOME/IP-TP segmentation / reassembly

The v1 implementation lives in ``someip.legacy`` and drives the example
demos until the v2 app/transport layers land. It is deprecated and will be
removed once v2 is feature-complete (see docs/v2-architecture.md).
"""

from . import ser, tpc, transport, types, wire
from .ser import Reader, Writer
from .tpc import Reassembler, TpHeader, segment
from .transport import ReceiveTimeout, TcpConnection, TcpListener, UdpEndpoint
from .wire import Header, Message, PartialMessage

__version__ = "0.3.0"

__all__ = [
    "types", "wire", "ser", "transport", "tpc",
    "Header", "Message", "PartialMessage",
    "Writer", "Reader",
    "UdpEndpoint", "TcpConnection", "TcpListener", "ReceiveTimeout",
    "Reassembler", "TpHeader", "segment",
    "__version__",
]