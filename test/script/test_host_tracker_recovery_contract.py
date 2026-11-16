"""Source-only admission checks; these do not claim C++ execution or RF parity."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HostTrackerRecoveryContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (ROOT / "src/process/tracker/Tracker.h").read_text()
        cls.source = (ROOT / "src/process/tracker/Tracker.cpp").read_text()
        cls.algorithm = (ROOT / "src/process/analysis/RadarAlgorithmContract.h").read_text()
        cls.tests = (ROOT / "test/unit/process/tracker/TestTracker.cpp").read_text()
        cls.integration = (ROOT / "src/process/detection/DetectorIntegration.h").read_text()

    def test_disabled_default_and_shared_wiring(self):
        self.assertIn("bool stationaryClutter = false", self.header)
        self.assertIn("bool guidedShadow = false, guidedPromote = false, guidedSustain = false", self.header)
        self.assertIn("output.trackerRecovery = hostTrackerRecovery(value)", self.algorithm)
        self.assertIn("associationMaximumNis, birthExclusivity, recovery", self.source)
        self.assertIn("track.promote(index, m, n)", self.source)

    def test_zero_epoch_and_exact_disjoint_credit(self):
        self.assertIn("evidenceEpochInitialized", self.source)
        self.assertNotRegex(self.source, r"!value\.epoch|value\.epoch\s*!=\s*0")
        self.assertIn("value.sequences[value.depth-1] > found->second.highWaterSequence", self.source)
        self.assertIn("shared.highWaterSequence=value.newestSequence", self.source)
        self.assertIn("if (epochReset) reset(true)", self.source)
        self.assertIn("sequence<=frameSequence", self.source)
        self.assertIn("evidence.empty() && evidenceEpochInitialized", self.source)
        self.assertIn("validEvidence(item,false)", self.source)
        self.assertIn("frameOrdinal > 0 && currentTime <= timestamp", self.source)
        self.assertIn("diagnostics.lineageResets=pendingLineageReset", self.source)
        self.assertIn("if (integration && integration->historyReset) tracker.reset(true)", self.integration)

    def test_guided_support_is_prediction_only_and_bounded(self):
        body = self.source.split("if (guidedRecovery(i,prediction,guidedCredit))", 1)[1].split(
            "// update state if no detections associated", 1)[0]
        self.assertIn("set_predicted_current", body)
        self.assertNotIn("set_current(", body)
        self.assertNotIn("set_acceleration(", body)
        self.assertNotIn("++diagnostics.associations", body)
        self.assertIn("std::min(guided.size(),guidedLimit)", self.source)
        self.assertIn("output.size()<recovery.guidedMaximumTracks", self.source)
        self.assertIn("votes.size() > n", self.source)
        self.assertIn("stationaryCells.size() >= recovery.maximumStationaryCells", self.source)
        self.assertIn("track.get_n()+accInit.size()>65535", self.source)

    def test_stationary_capacity_is_not_clutter_evidence(self):
        classifier=self.source.split("bool Tracker::stationary(",1)[1].split(
            "bool Tracker::stationaryCapacityBlocked",1)[0]
        self.assertIn("if (found == stationaryCells.end()) return false", classifier)
        self.assertNotIn("maximumStationaryCells",classifier)
        process=self.source.split("std::unique_ptr<Track> Tracker::process",1)[1]
        self.assertLess(process.index("pruneStationary();"),process.index("initiate(detection);"))
        self.assertIn("++diagnostics.stationaryBirthCapacityRejected",self.source)

    def test_cpp_acceptance_cases_cover_boundaries(self):
        for label in ["one disjoint confirmation vote", "Malformed temporal lineage",
                      "stationary occupancy has an explicit bounded capacity",
                      "Guided shadow never changes", "Guided 2 4 and 8 CPI",
                      "Weak support accepts epoch zero",
                      "Missing or malformed lineage cannot bypass",
                      "Disabled recovery preserves ordinary seed",
                      "Mixed depth evidence shares one disjoint ledger",
                      "Stationary capacity pressure cannot quarantine",
                      "Stationary decay frees admission capacity",
                      "Repeated empty-track timestamps cannot teach",
                      "Explicit lineage reset reports the next frame once"]:
            self.assertIn(label, self.tests)
        self.assertNotRegex(self.algorithm, r"#if.*(?:WIN32|APPLE|linux)")


if __name__ == "__main__":
    unittest.main()
