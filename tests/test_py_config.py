import json
import os
import tempfile
import unittest

from someip import config

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(ROOT, "config", "someip_demo.json")


class ConfigTest(unittest.TestCase):
    def test_demo_file_loads_and_normalizes(self):
        cfg = config.load_config(DEMO)
        self.assertEqual(cfg["unicast"], "auto")
        self.assertEqual(cfg["sd"]["port"], 30490)
        self.assertEqual(cfg["sd"]["multicast"], "224.244.224.245")
        self.assertEqual(cfg["sd"]["ttl"], 3)
        self.assertEqual(cfg["service"]["service_id"], 0x1234)
        self.assertEqual(cfg["service"]["instance_id"], 0x5678)
        self.assertEqual(cfg["client"]["client_id"], "auto")

    def test_service_kwargs(self):
        cfg = config.load_config(DEMO)
        kw = config.service_kwargs(cfg)
        self.assertEqual(kw["service_id"], 0x1234)
        self.assertEqual(kw["instance_id"], 0x5678)
        self.assertEqual(kw["major_version"], 1)
        self.assertEqual(kw["minor_version"], 1)
        self.assertEqual(kw["method_port"], 30500)
        self.assertEqual(kw["event_port"], 30501)
        self.assertEqual(kw["interface_ip"], None)
        self.assertEqual(kw["sd_port"], 30490)

    def test_client_kwargs(self):
        kw = config.client_kwargs(config.load_config(DEMO))
        self.assertIsNone(kw["client_id"])
        self.assertEqual(kw["sd_port"], 30490)
        self.assertEqual(kw["interface_ip"], None)
        ckw = config.client_kwargs(config.load_config(self._dump({
            "client": {"client_id": 0x0701, "sd_port": 31001,
                       "interface": "10.0.0.9"}})))
        self.assertEqual(ckw["client_id"], 0x0701)

    def _dump(self, obj):
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f)
        return path

    def test_custom_values_and_hex_ids(self):
        path = self._dump({
            "unicast": "10.0.0.9",
            "sd": {"port": 31000, "ttl": 5},
            "service": {
                "service_id": "0x1A2B", "instance_id": 7,
                "major": 2, "minor": 0,
                "method_port": 32100, "event_port": 32101,
            },
            "client": {"client_id": 0x0701, "sd_port": 31001,
                       "interface": "10.0.0.9"},
        })
        cfg = config.load_config(path)
        self.assertEqual(cfg["unicast"], "10.0.0.9")
        self.assertEqual(cfg["sd"]["port"], 31000)
        self.assertEqual(cfg["sd"]["ttl"], 5)
        kw = config.service_kwargs(cfg)
        self.assertEqual(kw["service_id"], 0x1A2B)
        self.assertEqual(kw["instance_id"], 7)
        self.assertEqual(kw["method_port"], 32100)
        self.assertEqual(kw["interface_ip"], "10.0.0.9")
        ckw = config.client_kwargs(cfg)
        self.assertEqual(ckw["client_id"], 0x0701)
        os.unlink(path)

    def test_unknown_keys_tolerated(self):
        path = self._dump({
            "unicast": "auto",
            "some_future_key": {"a": 1},
            "service": {"service_id": 0x1111, "instance_id": 0x2222},
        })
        cfg = config.load_config(path)
        self.assertEqual(cfg["service"]["service_id"], 0x1111)
        os.unlink(path)

    def test_invalid_rejected(self):
        for bad in ({"sd": {"port": "nope"}},
                    {"sd": {"port": -1}},
                    {"sd": {"multicast": 42}},
                    {"service": {"service_id": {"x": 1}}},
                    {"client": {"client_id": 1.5}},
                    []):
            path = self._dump(bad)
            with self.assertRaises(config.ConfigError):
                config.load_config(path)
            os.unlink(path)

    def test_missing_file(self):
        with self.assertRaises(OSError):
            config.load_config("/nonexistent/x.json")


if __name__ == "__main__":
    unittest.main()