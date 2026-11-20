#!/usr/bin/env python3

import importlib.util
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

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
    def packet(self, raw, *, phase=4, noise=0, retune=0, metadata=None, timing=None):
        channels, samples = raw.shape[:2]
        encoded = struct.pack(">8I", MODULE.MAGIC if timing is None else MODULE.MCHT_MAGIC,
                              channels, samples, phase, noise, 1, 0, retune)
        encoded += struct.pack("<" + "f" * (channels * 2),
                               *(metadata or [527e6, 2.7] * channels))
        if timing is not None: encoded += struct.pack(">4I10Q", *timing)
        return encoded + raw.tobytes()

    def analyze_bytes(self, data, **kwargs):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.mchq"
            path.write_bytes(data)
            result = MODULE.analyze(path, **kwargs)
        json.dumps(result, allow_nan=False)
        self.assertEqual(result["input"]["sha256"], hashlib.sha256(data).hexdigest())
        return result

    def variable_iq(self, channels=2, samples=16):
        return np.random.default_rng(2).integers(80, 170, size=(channels, samples, 2), dtype=np.uint8)

    def timing(self, **changes):
        fields = dict(version=1, size=96, flags=1, reference=0, epoch=7, first=100,
                      transfer=9, monotonic=10, tai=0, realtime=1000, span=1,
                      error=1, buffer=9, anchor=116)
        fields.update(changes)
        return tuple(fields.values())

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
            result = MODULE.analyze(path, analysis_stride=2, contiguous_samples=samples,
                                    sample_count=samples, channel_order=["reference", "surveillance"],
                                    noise_range=(0, samples), direct_anchor_range=(0, samples),
                                    sample_rate_hz=2_400_000)
            self.assertEqual(result["input"]["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(result["input"]["sample_rate_hz"], 2_400_000)
            self.assertEqual(result["input"]["sample_rate_provenance"], "caller_supplied")
        self.assertEqual(result["channels"], 2)
        self.assertGreater(result["array"]["principal_coherent_fraction"], .99)
        self.assertEqual(result["input"]["channel_order"], ["reference", "surveillance"])
        self.assertTrue(result["noise_correlation"]["available"])
        self.assertGreater(result["noise_correlation"]["normalized_complex_correlation_matrix"][0][1][0], .9)
        self.assertTrue(result["equal_weight_array_gain"]["available"])
        self.assertLess(result["equal_weight_array_gain"]["effective_gain_linear"], 1.1)
        self.assertTrue(result["direct_anchor_phase"]["available"])
        self.assertLess(result["direct_anchor_phase"]["per_channel"][1]["circular_standard_deviation_rad"], .1)

    def test_explicit_unavailable_evidence_and_bounded_range(self):
        samples = 16
        raw = np.full((2, samples, 2), 127, dtype=np.uint8)
        raw[:, :, 0] = np.arange(samples, dtype=np.uint8) + 120
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "range.mchq"
            with path.open("wb") as stream:
                stream.write(struct.pack(">8I", MODULE.MAGIC, 2, samples, 4, 0, 1, 0, 0))
                stream.write(struct.pack("<4f", 527e6, 2.7, 527e6, 2.7))
                stream.write(raw.tobytes())
            result = MODULE.analyze(path, sample_start=4, sample_count=8)
        self.assertIsNone(result["input"]["sample_rate_hz"])
        self.assertEqual(result["input"]["sample_rate_provenance"], "not_encoded_in_mchq")
        self.assertEqual(result["input"]["analysis_range"]["observed_sample_count"], 8)
        self.assertEqual(result["noise_correlation"]["reason"], "noise_segment_not_supplied")
        self.assertEqual(result["equal_weight_array_gain"]["reason"], "noise_segment_not_supplied")
        self.assertEqual(result["direct_anchor_phase"]["reason"],
                         "direct_anchor_or_reference_segment_not_supplied")

    def test_independent_noise_has_near_ideal_equal_weight_gain(self):
        rng, samples = np.random.default_rng(11), 16384
        channels = rng.normal(size=(2, samples)) + 1j * rng.normal(size=(2, samples))
        raw = np.clip(np.stack((channels.real, channels.imag), axis=2) * 16 + 127.5,
                      0, 255).astype(np.uint8)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "noise.mchq"
            with path.open("wb") as stream:
                stream.write(struct.pack(">8I", MODULE.MAGIC, 2, samples, 4, 0, 1, 0, 0))
                stream.write(struct.pack("<4f", 527e6, 2.7, 527e6, 2.7))
                stream.write(raw.tobytes())
            result = MODULE.analyze(path, sample_count=samples, noise_range=(0, samples))
        self.assertTrue(result["equal_weight_array_gain"]["available"])
        self.assertAlmostEqual(result["equal_weight_array_gain"]["effective_gain_linear"], 2, delta=.08)

    def test_ranges_span_packets_in_channel_major_wire_order(self):
        raw = self.variable_iq(samples=32)
        data = self.packet(raw[:, :7]) + self.packet(raw[:, 7:19]) + self.packet(raw[:, 19:])
        result = self.analyze_bytes(data, sample_start=5, sample_count=17,
                                    noise_range=(6, 19), direct_anchor_range=(14, 12))
        values = (raw[..., 0].astype(float) - 127.5 + 1j * (raw[..., 1].astype(float) - 127.5)) / 127.5
        centered = values[:, 5:22] - values[:, 5:22].mean(axis=1, keepdims=True)
        cov = centered @ centered.conj().T / 17
        expected = cov / np.sqrt(np.outer(cov.diagonal().real, cov.diagonal().real))
        actual = np.asarray(result["observed_centered_iq_correlation"]["matrix"])
        np.testing.assert_allclose(actual[..., 0] + 1j * actual[..., 1], expected, atol=1e-14)
        np.testing.assert_allclose([channel["mean_power_linear"] for channel in result["channel_power"]],
                                   np.mean(abs(values[:, 5:22]) ** 2, axis=1))
        self.assertEqual(result["noise_correlation"]["selection"]["observed_sample_count"], 19)
        self.assertEqual(result["direct_anchor_phase"]["selection"]["observed_sample_count"], 12)
        self.assertEqual(result["recording"]["packets"], 3)
        self.assertFalse(result["processing"]["production_preprocessing_applied"])
        self.assertIn("not production-equivalent", result["processing"]["basis"])

    def test_recorder_channel_and_packet_limits(self):
        result = self.analyze_bytes(self.packet(self.variable_iq(channels=1)))
        self.assertEqual(result["channels"], 1)
        result = self.analyze_bytes(self.packet(self.variable_iq(channels=1, samples=65536)), sample_count=2)
        self.assertEqual(result["recording"]["samples_per_channel"], 65536)
        for channels, samples in ((0, 1), (9, 1), (2, 0), (2, 65537), (2, 2 ** 32 - 1)):
            with self.subTest(channels=channels, samples=samples), self.assertRaisesRegex(ValueError, "header"):
                self.analyze_bytes(struct.pack(">8I", MODULE.MAGIC, channels, samples, 4, 0, 1, 0, 0))

    def test_rejects_nonfinite_metadata_and_truncation(self):
        raw = self.variable_iq()
        for value in (float("nan"), float("inf"), -float("inf")):
            for metadata in ([value, 2.7, 527e6, 2.7], [527e6, value, 527e6, 2.7]):
                with self.subTest(metadata=metadata), self.assertRaisesRegex(ValueError, "non-finite"):
                    self.analyze_bytes(self.packet(raw, metadata=metadata))
        packet = self.packet(raw)
        for size in (1, 31, 33, 47, len(packet) - 1):
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, "truncated"):
                self.analyze_bytes(packet[:size])

    def test_mcht_metadata_validation_and_clipped_sample_lineage(self):
        raw = self.variable_iq()
        first = self.timing(first=103, anchor=132)
        second = self.timing(first=119, anchor=140, transfer=10, buffer=10, monotonic=11)
        data = self.packet(raw, timing=first) + self.packet(raw, timing=second)
        result = self.analyze_bytes(data, sample_count=32, noise_range=(0, 32))
        self.assertEqual(result["recording"]["sample_continuity"], "verified_from_mcht")
        self.assertEqual(result["recording"]["first_packet_metadata"]["timing"]["first_sample"], 103)
        self.assertTrue(result["noise_correlation"]["available"])
        for changes in (dict(version=2), dict(size=95), dict(flags=8), dict(reference=2),
                        dict(epoch=0), dict(realtime=0), dict(realtime=2 ** 63),
                        dict(first=2 ** 64 - 16, anchor=2 ** 64 - 1), dict(anchor=115)):
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, "timing extension"):
                self.analyze_bytes(self.packet(raw, timing=self.timing(**changes)))
        with self.assertRaisesRegex(ValueError, "truncated"):
            self.analyze_bytes(self.packet(raw, timing=first)[:143])

    def test_mcht_discontinuity_disables_evidence(self):
        raw = self.variable_iq()
        for changes in (dict(first=117), dict(epoch=8), dict(transfer=11), dict(buffer=11),
                        dict(reference=1), dict(monotonic=10)):
            second = dict(first=116, anchor=150, transfer=10, buffer=10, monotonic=11)
            second.update(changes)
            with self.subTest(changes=changes):
                result = self.analyze_bytes(self.packet(raw, timing=self.timing()) +
                                            self.packet(raw, timing=self.timing(**second)),
                                            sample_count=32, noise_range=(0, 32), direct_anchor_range=(0, 32))
                self.assertEqual(result["recording"]["sample_continuity"], "discontinuous")
                for key in ("noise_correlation", "equal_weight_array_gain", "direct_anchor_phase"):
                    self.assertFalse(result[key]["available"])
                    self.assertEqual(result[key]["reason"], "recording_timing_discontinuity")

    def test_receiver_state_changes_and_unsynchronized_input_disable_evidence(self):
        raw = self.variable_iq()
        for changes in (dict(phase=3), dict(phase=0x204), dict(phase=0x404), dict(phase=0x804),
                        dict(noise=1), dict(retune=1), dict(metadata=[527e6, 3., 527e6, 2.7])):
            with self.subTest(changes=changes):
                result = self.analyze_bytes(self.packet(raw) + self.packet(raw, **changes),
                                            noise_range=(0, 32), direct_anchor_range=(0, 32))
                self.assertFalse(result["recording"]["metadata_stable"])
                for key in ("noise_correlation", "equal_weight_array_gain", "direct_anchor_phase"):
                    self.assertFalse(result[key]["available"])
                self.assertIsNone(result["noise_correlation"]["normalized_complex_correlation_matrix"])
        result = self.analyze_bytes(self.packet(raw, phase=3), noise_range=(0, 16))
        self.assertEqual(result["noise_correlation"]["reason"], "recording_contains_unsynchronized_iq")
        result = self.analyze_bytes(self.packet(raw, phase=0x104), noise_range=(0, 16))
        self.assertTrue(result["noise_correlation"]["available"])

    def test_incomplete_and_absent_ranges_do_not_publish_noise_evidence(self):
        data = self.packet(self.variable_iq())
        for selected, count, start in (((12, 8), 4, 12), ((40, 8), 0, None)):
            result = self.analyze_bytes(data, sample_start=12, sample_count=8,
                                        noise_range=selected, direct_anchor_range=selected)
            self.assertFalse(result["input"]["analysis_range"]["complete"])
            for key in ("noise_correlation", "direct_anchor_phase"):
                self.assertFalse(result[key]["available"])
                self.assertEqual(result[key]["selection"]["observed_sample_count"], count)
                self.assertEqual(result[key]["selection"]["observed_start_sample"], start)
            self.assertIsNone(result["noise_correlation"]["normalized_complex_correlation_matrix"])
            self.assertFalse(result["equal_weight_array_gain"]["available"])

    def test_zero_centered_power_does_not_claim_anchor_or_noise(self):
        data = self.packet(np.full((2, 16, 2), 127, dtype=np.uint8))
        result = self.analyze_bytes(data, noise_range=(0, 16), direct_anchor_range=(0, 16))
        for key in ("observed_centered_iq_correlation", "noise_correlation", "direct_anchor_phase"):
            self.assertFalse(result[key]["available"])

    def test_constant_multiframe_adc_does_not_acquire_roundoff_noise(self):
        data = self.packet(np.full((2, 16, 2), 200, dtype=np.uint8)) * 1000
        result = self.analyze_bytes(data, sample_count=16000, noise_range=(0, 16000),
                                    direct_anchor_range=(0, 16000))
        for key in ("observed_centered_iq_correlation", "noise_correlation", "equal_weight_array_gain", "direct_anchor_phase"):
            self.assertFalse(result[key]["available"])

    def test_phase_circular_statistics_are_not_labelled_array_efficiency(self):
        raw = np.asarray([[[128, 128], [127, 127], [128, 128], [127, 127]],
                          [[127, 128], [128, 127], [128, 127], [127, 128]]], dtype=np.uint8)
        result = self.analyze_bytes(self.packet(raw), direct_anchor_range=(0, 4))
        phase = result["direct_anchor_phase"]["per_channel"][1]
        self.assertEqual(phase["mean_resultant_length"], 0)
        self.assertEqual(phase["mean_phasor_power_fraction"], 0)
        self.assertIsNone(phase["circular_standard_deviation_rad"])
        self.assertIsNone(phase["mean_relative_phase_rad"])
        self.assertNotIn("phase_rms_rad", phase)
        self.assertNotIn("coherent_efficiency", phase)

    def test_array_gain_is_conditional_and_zero_noise_sum_is_unavailable(self):
        raw = self.variable_iq(channels=1)
        correlated = np.concatenate((raw, raw))
        result = self.analyze_bytes(self.packet(correlated), noise_range=(0, 16))
        self.assertAlmostEqual(result["equal_weight_array_gain"]["effective_gain_linear"], 1)
        self.assertIn("conditional model", result["equal_weight_array_gain"]["interpretation"])
        anticorrelated = np.concatenate((raw, 255 - raw))
        result = self.analyze_bytes(self.packet(anticorrelated), noise_range=(0, 16))
        self.assertFalse(result["equal_weight_array_gain"]["available"])
        self.assertEqual(result["equal_weight_array_gain"]["reason"], "invalid_noise_correlation_sum")

    def test_mutated_recording_cannot_get_a_post_analysis_hash(self):
        data = self.packet(self.variable_iq())
        original = MODULE.iter_mchq
        def mutate(path, digest):
            yield from original(path, digest)
            with path.open("ab") as stream: stream.write(b"changed")
        with patch.object(MODULE, "iter_mchq", mutate), self.assertRaisesRegex(ValueError, "changed during"):
            self.analyze_bytes(data)


if __name__ == "__main__":
    unittest.main()
