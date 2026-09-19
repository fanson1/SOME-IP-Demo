"""Graded stderr logger (P1, mirrors vsomeip logging levels).

Levels: debug < info < warn < error. A process-wide default level applies to
every logger; configure with ``SOMEIP_LOG_LEVEL`` env var (``debug``/``info``/
``warn``/``error``, default ``info``) or ``log.level`` in the JSON config.
All writes go to stderr so program stdout keeps machine-readable output.
"""

import os
import sys
import threading
import time

LEVELS = {"debug": 10, "info": 20, "warn": 30, "error": 40}

_LEVEL_NAMES = {v: k for k, v in LEVELS.items()}

_lock = threading.Lock()
_default_level = LEVELS.get(os.environ.get("SOMEIP_LOG_LEVEL", "").lower(),
                            LEVELS["info"])


def set_default_level(name):
    global _default_level
    name = name.lower()
    if name not in LEVELS:
        raise ValueError("unknown log level: %r" % name)
    _default_level = LEVELS[name]


def get_default_level():
    return _LEVEL_NAMES[_default_level]


class Logger:
    def __init__(self, component, level=None):
        self.component = component
        self.level = _default_level if level is None else _level(level)

    def debug(self, msg):
        self._emit(LEVELS["debug"], msg)

    def info(self, msg):
        self._emit(LEVELS["info"], msg)

    def warn(self, msg):
        self._emit(LEVELS["warn"], msg)

    def error(self, msg):
        self._emit(LEVELS["error"], msg)

    def _emit(self, level, msg):
        if level < self.level:
            return
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        with _lock:
            sys.stderr.write("[%s][%s][%s] %s\n" % (_LEVEL_NAMES[level], ts,
                                                    self.component, msg))
            sys.stderr.flush()


def _level(name):
    name = name.lower()
    if name not in LEVELS:
        raise ValueError("unknown log level: %r" % name)
    return LEVELS[name]


def level_from_config(cfg):
    """Extract an explicit log level from a config dict, or None."""
    log = cfg.get("log") or {}
    if not isinstance(log, dict):
        return None
    lvl = log.get("level")
    return lvl if isinstance(lvl, str) else None