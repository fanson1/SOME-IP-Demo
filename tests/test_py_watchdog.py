import threading
import time
import unittest

from someip.watchdog import Watchdog


class FakeClock:
    def __init__(self):
        self.t = 0.0
        self.lock = threading.Lock()

    def __call__(self):
        with self.lock:
            return self.t

    def advance(self, dt):
        with self.lock:
            self.t += dt


class WatchdogTest(unittest.TestCase):
    def test_fires_when_starved(self):
        clock = FakeClock()
        fired = []
        wd = Watchdog(timeout=1.0, on_expired=lambda: fired.append(1),
                      interval=0.01, now=clock)
        wd.start()
        clock.advance(1.2)
        time.sleep(0.05)
        self.assertEqual(len(fired), 1)
        clock.advance(1.0)
        time.sleep(0.05)
        self.assertEqual(len(fired), 2)
        wd.stop()

    def test_not_fired_when_petted(self):
        clock = FakeClock()
        fired = []
        wd = Watchdog(timeout=1.0, on_expired=lambda: fired.append(1),
                      interval=0.01, now=clock)
        wd.start()
        for _ in range(50):
            clock.advance(0.5)   # keep under the 1.0 threshold
            wd.pet()
        time.sleep(0.05)
        self.assertEqual(fired, [])
        wd.stop()

    def test_verbose_death_stops_firing_after_pet(self):
        clock = FakeClock()
        fired = []
        wd = Watchdog(timeout=0.5, on_expired=lambda: fired.append(1),
                      interval=0.01, now=clock)
        wd.start()
        clock.advance(1.0)
        time.sleep(0.05)
        self.assertEqual(len(fired), 1)
        wd.pet()
        clock.advance(0.2)
        time.sleep(0.05)
        self.assertEqual(len(fired), 1)
        wd.stop()

    def test_stop_idempotent(self):
        clock = FakeClock()
        wd = Watchdog(1.0, interval=0.01, now=clock)
        wd.start()
        wd.stop()
        wd.stop()

    def test_bad_timeout(self):
        with self.assertRaises(ValueError):
            Watchdog(0.0)


if __name__ == "__main__":
    unittest.main()