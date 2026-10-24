#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

try:
    import numpy as np
except ModuleNotFoundError:
    np = None


SCRIPT = Path(__file__).parents[2] / "script" / "analyze-mchq-loss-budget.py"
if np is not None:
    SPEC = importlib.util.spec_from_file_location("analyze_mchq_loss_budget", SCRIPT)
    MODULE = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(MODULE)


@unittest.skipIf(np is None, "numpy is unavailable")
class LossBudgetTest(unittest.TestCase):
    def test_coherent_recording_and_bandwidth_limit(self):
        rng = np.random.default_rng(7)
        samples = 32768
        source = rng.normal(size=samples) + 1j * rng.normal(size=samples)
        channels = np.vstack((source, source * np.exp(.4j)))
        scaled = np.clip(np.stack((channels.real, channels.imag), axis=2) * 20 + 127.5,
                         0, 255).astype(np.uint8)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.mchq"
            with path.open("wb") as stream:
                stream.write(struct.pack(">8I", MODULE.MAGIC, 2, samples, 4, 0, 1, 0, 0))
                stream.write(struct.pack("<4f", 527e6, 2.7, 527e6, 2.7))
                stream.write(scaled.tobytes())
            result = MODULE.analyze(path, analysis_stride=2, contiguous_samples=samples)
        self.assertEqual(result["channels"], 2)
        self.assertGreater(result["array"]["principal_coherent_fraction"], .99)
        self.assertGreater(result["array"]["block_pair_stability"][0]
                           ["normalized_correlation_median"], .99)
        self.assertFalse(result["waveform_reconstruction"]
                         ["full_channel_reconstruction_possible"])


if __name__ == "__main__":
    unittest.main()
