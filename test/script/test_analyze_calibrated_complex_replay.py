#!/usr/bin/env python3
import importlib.util
import unittest
import copy

SPEC = importlib.util.spec_from_file_location(
    "calibrated", "script/analyze-calibrated-complex-replay.py")
MODULE = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(MODULE)


class CalibratedReplayTest(unittest.TestCase):
    def test_whitening_requires_exact_applied_weights(self):
        config = {"process": {"surveillance_fusion": {
            "mode": "noise_whitened_beam_bank", "beams": 1}}}
        frame = {"channels": [[[[1, 0]]], [[[2, 0]]]], "reference_weights": [[1, 0], [1, 0]],
                 "delay_km": [1], "doppler_hz": [2]}
        report = MODULE.fusion_provenance([frame], config)
        self.assertFalse(report["production_equivalent"])
        self.assertEqual(report["statistic"], "forensic_unwhitened_coherent_reconstruction")
        with self.assertRaisesRegex(ValueError, "requires native applied"):
            MODULE.fusion_provenance([frame], config, require_production=True)
        frame["fusion_state"] = {"schema": "vectorwarp-fusion-weights-v1",
            "mode": "noise_whitened_beam_bank", "convention": "multiply_then_sum",
            "source": "native_committed", "weights": [[[.5, 0], [2, 0]]]}
        self.assertTrue(MODULE.fusion_provenance([frame], config, True)["production_equivalent"])
        self.assertEqual(MODULE.best_beam_at(frame, config, 0, 0)["value"], 4.5)
        for field, value in [("mode", "coherent_beam_bank"), ("source", "reconstructed"),
                             ("convention", "unknown"), ("weights", [[[float("nan"), 0]]])]:
            bad = copy.deepcopy(frame); bad["fusion_state"][field] = value
            with self.assertRaisesRegex(ValueError, "applied fusion-weight"):
                MODULE.fusion_provenance([bad], config)
        self.assertFalse(MODULE.fusion_provenance([frame, {"channels": frame["channels"]}], config)
                         ["production_equivalent"])

    def test_fractional_delay_uses_production_cpi_geometry(self):
        config = {"capture": {"fs": 2400000, "fc": 509000000},
                  "process": {"data": {"cpi": .5}}}
        scale = MODULE.fractional_delay_scale(config)
        self.assertAlmostEqual(scale, -1200000 / 509000000)
        # A 292-Hz target moves about -0.688 delay bins per CPI. Production
        # nearest-coordinate sampling therefore reads one higher column from
        # the preceding CPI when the residual integer motion is zero.
        self.assertEqual(MODULE.delay_path_column(20, 0, 1, scale, 292, 292), 21)
        self.assertEqual(MODULE.delay_path_column(20, 0, 0, scale, 292, 292), 20)

    def test_fractional_delay_rejects_missing_geometry(self):
        with self.assertRaisesRegex(ValueError, "geometry"):
            MODULE.fractional_delay_scale({"capture": {"fs": 0, "fc": 1},
                "process": {"data": {"cpi": .5}}})

    def test_nearby_detection_uses_the_closest_candidate(self):
        truth = {"delay": 8.6, "doppler": -30}
        documents = [{"delay": [8.7, 8.61], "doppler": [-20, -29], "snr": [9, 4]}]
        matches = MODULE.nearby_detections(documents, truth, .5, 25)
        self.assertEqual(len(matches), 1)
        self.assertAlmostEqual(matches[0]["delay_bias_km"], .01)
        self.assertAlmostEqual(matches[0]["doppler_bias_hz"], 1)

    def test_track_delay_bins_convert_to_kilometres(self):
        truth = {"delay": MODULE.C / 2400000 / 1000 * 10, "doppler": 20}
        tracks = [{"data": [{"id": "1", "state": "ACTIVE",
            "associated_delay": [9, 11, 10], "associated_doppler": [20, 20, 20],
            "associated_state": ["ACTIVE", "ACTIVE", "ACTIVE"],
            "associated_measured": [True, True, True]}]}]
        evidence = MODULE.track_evidence(tracks, truth, 2400000, .1, 1)
        self.assertEqual(evidence["1"]["near_samples"], 1)
        self.assertTrue(evidence["1"]["active"])

    def test_track_evidence_deduplicates_histories_and_excludes_predictions(self):
        truth = {"delay": MODULE.C / 2400000 / 1000 * 10, "doppler": 20}
        history = {"id": "1", "state": "ACTIVE", "measured": False,
            "associated_delay": [10, 10, 10], "associated_doppler": [20, 20, 20],
            "associated_state": ["ACTIVE", "COASTING", "ACTIVE"],
            "associated_measured": [True, False, False]}
        evidence = MODULE.track_evidence([{"data": [history]}, {"data": [history]}],
            truth, 2400000, .1, 1)
        self.assertEqual(evidence["1"]["near_samples"], 0)
        self.assertTrue(evidence["1"]["active"])

    def test_track_evidence_uses_final_presence_not_ever_active(self):
        truth = {"delay": MODULE.C / 2400000 / 1000 * 10, "doppler": 20}
        active = {"id": "1", "state": "ACTIVE", "associated_delay": [10],
            "associated_doppler": [20], "associated_state": ["ACTIVE"],
            "associated_measured": [True]}
        evidence = MODULE.track_evidence([{"data": [active]}, {"data": []}],
            truth, 2400000, .1, 1)
        self.assertFalse(evidence["1"]["active"])

    def test_p99_uses_the_empirical_upper_tail(self):
        self.assertEqual(MODULE.p99(list(range(100))), 99)


if __name__ == "__main__":
    unittest.main()
