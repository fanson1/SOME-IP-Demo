import contextlib
import io
import os
import unittest

from someip import config, log


class _LevelChanger:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.old = log.get_default_level()
        log.set_default_level(self.name)
        return self

    def __exit__(self, *exc):
        log.set_default_level(self.old)
        return False


class LogTest(unittest.TestCase):
    def _emit(self, logger):
        buf = io.StringIO()
        with contextlib.redirect_stderr(buf):
            logger.debug("dbg-line")
            logger.info("info-line")
            logger.warn("warn-line")
            logger.error("err-line")
        return buf.getvalue()

    def test_leveL_filtering(self):
        with _LevelChanger("info"):
            out = self._emit(log.Logger("t"))
        self.assertIn("info-line", out)
        self.assertIn("warn-line", out)
        self.assertIn("err-line", out)
        self.assertNotIn("dbg-line", out)

    def test_error_level(self):
        with _LevelChanger("error"):
            out = self._emit(log.Logger("t"))
        self.assertIn("err-line", out)
        self.assertNotIn("warn-line", out)
        self.assertNotIn("info-line", out)

    def test_component_and_format(self):
        with _LevelChanger("warn"):
            out = self._emit(log.Logger("sdm.x"))
        self.assertIn("[warn]", out)
        self.assertIn("sdm.x", out)

    def test_unknown_level_rejected(self):
        with self.assertRaises(ValueError):
            log.set_default_level("verbose")
        with self.assertRaises(ValueError):
            log.Logger("t", "noisy")

    def test_default_from_env(self):
        import importlib
        os.environ["SOMEIP_LOG_LEVEL"] = "warn"
        try:
            reloaded = importlib.reload(log)
            self.assertEqual(reloaded.get_default_level(), "warn")
        finally:
            os.environ.pop("SOMEIP_LOG_LEVEL")
            importlib.reload(log)

    def test_config_level_normalized(self):
        cfg = config.normalize_config({"log": {"level": "WARN"}})
        self.assertEqual(cfg["log"]["level"], "warn")
        self.assertIsNone(log.level_from_config(
            config.normalize_config({})))
        self.assertEqual(log.level_from_config(cfg), "warn")

    def test_demo_config_log(self):
        cfg = config.load_config(
            os.path.join(os.path.dirname(os.path.dirname(
                os.path.abspath(__file__))), "config", "someip_demo.json"))
        self.assertIsNone(cfg["log"]["level"])


if __name__ == "__main__":
    unittest.main()