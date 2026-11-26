"""Replay-only temporal score evidence wiring contract."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class TemporalScoreEvidenceContract(unittest.TestCase):
    def test_full_precision_candidate_evidence_is_not_a_display_payload(self):
        header = (ROOT / "src/data/Detection.h").read_text()
        source = (ROOT / "src/data/Detection.cpp").read_text()
        self.assertIn("to_calibration_evidence_json", header)
        body = source.split("Detection::to_calibration_evidence_json", 1)[1].split(
            "Detection::delay_bin_to_km", 1)[0]
        self.assertIn('"canonical-temporal-score-evidence-v1"', body)
        self.assertNotIn("SetMaxDecimalPlaces", body)
        for field in ("delay_bins", "doppler_hz", "score", "depth",
                      "delay_motion_bins", "support_sequences_exact"):
            self.assertIn('"' + field + '"', body)

    def test_observer_is_bounded_completed_replay_only_sink(self):
        observer = (ROOT / "src/process/analysis/ReplayCoherentObserver.h").read_text()
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("void temporalScores", observer)
        self.assertIn("candidate_output_complete", observer)
        self.assertIn("candidate_output_truncated", observer)
        self.assertIn("Qualification temporal-score observation has no exact completed lineage", observer)
        self.assertIn("config_.maxBytes-bytes_", observer)
        self.assertIn("O_EXCL|O_NOFOLLOW|O_CLOEXEC", observer)
        staged, completion = radar.split("RadarFrame complete_processed_cpi(StagedCpi staged)", 1)
        completion = completion.split("void queue_processed_cpi", 1)[0]
        self.assertIn("staged.qualification = qualificationFrame(first);", staged)
        self.assertIn("if (staged.qualification)", completion)
        self.assertIn("const auto& identity = *staged.qualification;", completion)
        self.assertIn("qualificationObserver->temporalScores(identity,*processed.detections)", completion)
        self.assertNotIn("qualificationFrame(", completion,
                         "Async completion must retain the captured CPI identity")


if __name__ == "__main__":
    unittest.main()
