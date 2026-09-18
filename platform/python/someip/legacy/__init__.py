"""Legacy v1 SOME/IP stack (deprecated).

Kept byte-compatible as the reference wire implementation and to run the
example demos until the v2 app layer lands. New code should use the v2
modules directly (``someip.wire`` / ``someip.ser`` / ``someip.types``).
"""

from .client import SomeIpClient
from .service import SomeIpService

__all__ = ["SomeIpService", "SomeIpClient"]