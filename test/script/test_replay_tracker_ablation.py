import importlib.util
from pathlib import Path
import unittest


PATH = Path(__file__).parents[2] / "script" / "replay-fusion-ab-cass.py"
SPEC = importlib.util.spec_from_file_location("replay_fusion", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TrackerAblationTest(unittest.TestCase):
    def test_stationary_clutter_override_is_narrow_and_supports_missing_block(self):
        config = {"process": {"tracker": {"initiate": {"M": 3, "N": 5}}}}
        MODULE.set_stationary_clutter(config, "off")
        self.assertEqual(config, {"process": {"tracker": {"initiate": {
            "M": 3, "N": 5, "stationary_clutter": {"enable": False}}}}})
        MODULE.set_stationary_clutter(config, "on")
        self.assertEqual(config["process"]["tracker"]["initiate"][
            "stationary_clutter"], {
                "enable": True,
                "persistence_cpis": 60,
                "max_doppler_hz": 15.0,
            })

    def test_stationary_clutter_none_preserves_config(self):
        config = {"process": {"tracker": {"initiate": {"stationary_clutter": {
            "enable": True, "persistence_cpis": 91}}}}}
        MODULE.set_stationary_clutter(config, None)
        self.assertEqual(config["process"]["tracker"]["initiate"][
            "stationary_clutter"], {"enable": True, "persistence_cpis": 91})

    def test_stationary_clutter_on_preserves_native_default_when_block_missing(self):
        config = {"process": {"tracker": {"initiate": {"M": 3, "N": 5}}}}
        MODULE.set_stationary_clutter(config, "on")
        self.assertNotIn("stationary_clutter",
                         config["process"]["tracker"]["initiate"])


if __name__ == "__main__":
    unittest.main()
