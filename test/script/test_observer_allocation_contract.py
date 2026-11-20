"""Source wiring checks only; native byte fixtures separately test decoding.

Neither test substitutes for the physical eight-lane allocation receipt.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ObserverAllocationContract(unittest.TestCase):
    def test_native_counters_and_session_identity_are_not_estimated(self):
        views = (ROOT / "src/process/analysis/ReplayCoherentObserverViews.h").read_text()
        self.assertIn("resident::allocationProof(resident::proof_detail::sub(", views)
        self.assertIn("out.preparedEvidence,resident::evidence::prepared::allocation,832)", views)
        self.assertIn("a.session={identity.u64(16),identity.u64(24)}", views)
        for field in ("limit", "used", "peak", "live"):
            self.assertIn(f"=allocation.{field}", views)
        for field in ("totalBytes", "bytesPerSession", "sessions"):
            self.assertIn(f"=laneBudget->{field}", views)

    def test_actual_session_mapping_is_replay_guarded(self):
        coordinator = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        body = coordinator.split("uint64_t observedSharedReservationBytes()const{", 1)[1].split("}", 1)[0]
        self.assertIn('wire::require(replayObserver_,"Resident observer is not enabled")', body)
        self.assertIn("return session_->layout().bytes", body)
        self.assertNotIn("makeLayout", body)

    def test_single_and_multi_routes_pass_the_actual_reservation(self):
        single = (ROOT / "src/blah2.cpp").read_text()
        multi = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertEqual(single.count("resident->observedSharedReservationBytes()"), 2)
        self.assertIn("residentFrontend->observedSharedReservationBytes(),residentBudget.get()", multi)
        self.assertIn("if(qualificationObserver){", multi)

    def test_host_or_missing_resident_evidence_cannot_be_labeled_measured(self):
        observer = (ROOT / "src/process/analysis/ReplayCoherentObserver.h").read_text()
        self.assertIn("p.residentProofAvailable==p.residentAllocation.available", observer)
        self.assertIn("if(p.residentAllocation.available)p.residentAllocation.validate()", observer)
        self.assertIn('w.Key("resident_allocation");if(p.residentAllocation.available)', observer)
        self.assertIn("sessionReservationBytes-sharedReservationBytes", observer)


if __name__ == "__main__":
    unittest.main()
