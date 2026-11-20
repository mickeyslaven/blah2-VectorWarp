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
        cls.algorithm = (ROOT / "src/process/pipeline/RadarPipeline.h").read_text()
        cls.tests = (ROOT / "test/unit/process/tracker/TestTracker.cpp").read_text()
        cls.integration = (ROOT / "src/process/detection/DetectorIntegration.h").read_text()
        cls.policy = (ROOT / "src/process/tracker/AssociationContract.h").read_text()

    def test_disabled_default_and_shared_wiring(self):
        self.assertIn("bool stationaryClutter = false", self.header)
        self.assertIn("result.configureRecovery(config_.recovery)", self.algorithm)
        self.assertIn("associationMaximumNis, birthExclusivity, recovery", self.source)
        self.assertIn("track.promote(index, m, n)", self.source)

    def test_zero_epoch_and_exact_disjoint_credit(self):
        self.assertIn("evidenceEpochInitialized", self.source)
        self.assertNotRegex(self.source, r"!value\.epoch|value\.epoch\s*!=\s*0")
        self.assertIn("value.newestSequence<=found->second.highWaterSequence", self.source)
        self.assertIn("gap<8 ? 1u<<uint32_t(gap-1) : 0", self.source)
        self.assertIn("tracker_confirmation::confirmationDisjoint(value.depth,recent)", self.source)
        self.assertIn("ledger.highWaterSequence = value->newestSequence", self.source)
        self.assertIn("if (epochReset) reset(true)", self.source)
        self.assertIn("tracker_policy::frameClockNoOp(frameOrdinal>0,currentTime,timestamp,sequence,frameSequence)", self.source)
        self.assertIn("evidence.empty() && evidenceEpochInitialized", self.source)
        self.assertIn("validEvidence(item,false)", self.source)
        self.assertIn("accepted&&!clockAfter(low,high,previousLow,previousHigh)", self.policy)
        self.assertIn("diagnostics.lineageResets=pendingLineageReset", self.source)
        self.assertIn("if (integration && integration->historyReset) tracker.reset(true)", self.integration)

    def test_canonical_recovery_is_bounded(self):
        for retired in ("guidedRecovery", "TrackerGuidedCandidate", "guidedPredictions"):
            self.assertNotIn(retired, self.header + self.source + self.integration)
        self.assertIn("return tracker.process(&detections, signalTimeMs);", self.integration)
        self.assertIn("votes.size() > n", self.source)
        self.assertIn("stationaryCells.size() >= recovery.maximumStationaryCells", self.source)
        self.assertIn("track.get_n()+accInit.size()>65535", self.source)

    def test_stationary_capacity_is_not_clutter_evidence(self):
        classifier=self.source.split("bool Tracker::stationary(",1)[1].split(
            "bool Tracker::stationaryCapacityBlocked",1)[0]
        self.assertIn("found==stationaryCells.end())continue", classifier)
        self.assertIn("return false", classifier)
        self.assertNotIn("maximumStationaryCells",classifier)
        process=self.source.split("std::unique_ptr<Track> Tracker::process",1)[1]
        self.assertLess(process.index("pruneStationary();"),process.index("initiate(detection);"))
        self.assertIn("++diagnostics.stationaryBirthCapacityRejected",self.source)

    def test_cpp_acceptance_cases_cover_boundaries(self):
        for label in ["one disjoint confirmation vote", "Malformed temporal lineage",
                      "stationary occupancy has an explicit bounded capacity",
                      "Epoch zero is a real reset destination",
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
