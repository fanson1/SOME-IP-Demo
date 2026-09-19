"""Petting watchdog timer (P1 robustness).

A ``Watchdog`` runs a worker thread that fires ``on_expired()`` once when
``timeout`` seconds elapse between ``pet()`` calls, then waits for the next
lapse. This is used to detect stall/death of owned loops (offer publishing,
event stream) and to drive recovery (e.g. re-subscribe). ``stop()`` joins the
worker for a graceful shutdown.
"""

import threading
import time


class Watchdog:
    def __init__(self, timeout, on_expired=None, interval=0.1, now=time.monotonic):
        if timeout <= 0:
            raise ValueError("timeout must be > 0")
        self.timeout = float(timeout)
        self.interval = float(interval)
        self.on_expired = on_expired
        self._now = now
        self._last = None
        self._event = threading.Event()
        self._thread = None
        self._lock = threading.Lock()

    def start(self):
        with self._lock:
            if self._thread is not None:
                return self
            self._last = self._now()
            self._thread = threading.Thread(target=self._run, name="watchdog",
                                            daemon=True)
            self._thread.start()
        return self

    def pet(self):
        with self._lock:
            if self._event.is_set():
                return
            self._last = self._now()

    def stop(self, join_timeout=1.0):
        with self._lock:
            self._event.set()
            t = self._thread
            self._thread = None
        if t is not None:
            t.join(timeout=join_timeout)

    def _run(self):
        while not self._event.is_set():
            with self._lock:
                last = self._last
            now = self._now()
            if now - last >= self.timeout:
                fired = self.on_expired
                if fired is not None:
                    try:
                        fired()
                    except Exception:
                        pass
                with self._lock:
                    self._last = self._now()
            self._event.wait(self.interval)
        self._event.clear()