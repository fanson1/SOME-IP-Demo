"""JSON configuration loader (vsomeip schema subset).

Supported keys (a deliberately small surface, mirroring the C++ side):

    unicast          "auto" | IPv4            -> interface_ip for service/client
    sd.port          int                     -> SD unicast/multicast port
    sd.multicast     IPv4                    -> SD multicast address
    sd.ttl           int                     -> SD multicast TTL
    sd_json optional                       (reserved for full vsomeip parity)
    service.service_id   int | "0x1234"     -> service identifier
    service.instance_id  int | "0x5678"     -> instance identifier
    service.major/major_version   int
    service.minor/minor_version   int
    service.method_port/test_port/etc -> uint16 endpoints
    service.event_port           uint16
    service.interface  "auto" | IPv4
    client.client_id    int | "auto"
    client.sd_port      uint16
    client.interface    "auto" | IPv4

Unknown keys are ignored so a full vsomeip file can be handed over.
"""

import json
import os


class ConfigError(Exception):
    pass


def _u16(value, key):
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigError("%s: expected integer, got %r" % (key, value))
    if value < 0 or value > 0xFFFF:
        raise ConfigError("%s: %d out of uint16 range" % (key, value))
    return value


def _euid(value, key):
    """Accept a plain int or a 0x/16-style hex string."""
    if isinstance(value, bool):
        raise ConfigError("%s: expected int or hex string, got bool" % key)
    if isinstance(value, int):
        if value < 0 or value > 0xFFFFFFFF:
            raise ConfigError("%s: %d out of range" % (key, value))
        return value
    if isinstance(value, str):
        try:
            parsed = int(value.strip(), 0)
        except ValueError:
            raise ConfigError("%s: not an integer: %r" % (key, value))
        return _euid(parsed, key)
    raise ConfigError("%s: expected int or hex string, got %r" % (key, value))


def load_config(path):
    """Load + validate a config file; returns a normalized dict."""
    if not isinstance(path, os.PathLike):
        path = os.fspath(path)
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return normalize_config(raw)


def normalize_config(raw):
    """Validate/normalize a parsed JSON config object."""
    if not isinstance(raw, dict):
        raise ConfigError("config root must be a JSON object")
    cfg = {
        "unicast": "auto",
        "sd": {"port": 30490, "multicast": "224.244.224.245", "ttl": 3},
        "log": {"level": None},
        "service": None,
        "client": {"client_id": "auto", "sd_port": 30490, "interface": "auto"},
    }
    sd = raw.get("sd", {}) or {}
    if not isinstance(sd, dict):
        raise ConfigError("sd must be an object")
    if "port" in sd:
        cfg["sd"]["port"] = _u16(sd["port"], "sd.port")
    if "multicast" in sd:
        if not isinstance(sd["multicast"], str):
            raise ConfigError("sd.multicast must be an IPv4 string")
        cfg["sd"]["multicast"] = sd["multicast"]
    if "ttl" in sd:
        cfg["sd"]["ttl"] = _u16(sd["ttl"], "sd.ttl") or cfg["sd"]["ttl"]

    log = raw.get("log") or {}
    if not isinstance(log, dict):
        raise ConfigError("log must be an object")
    if "level" in log:
        if not isinstance(log["level"], str):
            raise ConfigError("log.level must be a string")
        cfg["log"]["level"] = log["level"].lower()

    unicast = raw.get("unicast", "auto")
    if isinstance(unicast, bool) or not isinstance(unicast, str):
        raise ConfigError("unicast must be 'auto' or an IPv4 string")
    cfg["unicast"] = unicast

    svc = raw.get("service")
    if svc is not None:
        if not isinstance(svc, dict):
            raise ConfigError("service must be an object")
        out = {}
        out["service_id"] = _euid(svc.get("service_id", 0x1234), "service.service_id")
        out["instance_id"] = _euid(svc.get("instance_id", 0x5678), "service.instance_id")
        out["major"] = svc.get("major", svc.get("major_version", 1))
        out["minor"] = svc.get("minor", svc.get("minor_version", 1))
        out["method_port"] = _u16(svc.get("method_port", 30500), "service.method_port")
        out["event_port"] = _u16(svc.get("event_port", 30501), "service.event_port")
        iface = svc.get("interface", cfg["unicast"])
        if not isinstance(iface, str):
            raise ConfigError("service.interface must be a string")
        out["interface"] = iface
        cfg["service"] = out

    cli = raw.get("client") or {}
    if not isinstance(cli, dict):
        raise ConfigError("client must be an object")
    if "client_id" in cli:
        if cli["client_id"] == "auto":
            cfg["client"]["client_id"] = "auto"
        else:
            cfg["client"]["client_id"] = _euid(cli["client_id"], "client.client_id")
    if "sd_port" in cli:
        cfg["client"]["sd_port"] = _u16(cli["sd_port"], "client.sd_port")
    if "interface" in cli:
        if not isinstance(cli["interface"], str):
            raise ConfigError("client.interface must be a string")
        cfg["client"]["interface"] = cli["interface"]
    return cfg


def _iface(v):
    return None if v == "auto" else v


def service_kwargs(cfg):
    """Build SomeipServiceV2 constructor kwargs from a normalized config."""
    kw = {}
    if cfg["service"]:
        s = cfg["service"]
        kw["service_id"] = s["service_id"]
        kw["instance_id"] = s["instance_id"]
        kw["major_version"] = s["major"]
        kw["minor_version"] = s["minor"]
        kw["method_port"] = s["method_port"]
        kw["event_port"] = s["event_port"]
        kw["interface_ip"] = _iface(s["interface"])
    kw.setdefault("sd_port", cfg["sd"]["port"])
    return kw


def client_kwargs(cfg):
    """Build ClientV2 constructor kwargs from a normalized config."""
    kw = {
        "client_id": cfg["client"]["client_id"],
        "sd_port": cfg["client"]["sd_port"],
        "interface_ip": cfg["client"]["interface"],
    }
    if kw["client_id"] == "auto":
        kw["client_id"] = None
    kw["interface_ip"] = _iface(kw["interface_ip"])
    return kw