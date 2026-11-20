"""Regression checks for canonical ATSC float ingestion and staging telemetry."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CanonicalFloatIngressContract(unittest.TestCase):
    def test_pilot_does_not_force_double_staging(self):
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        # The projector is the zero-copy conditioning path.  Reject only the
        # former quality-observer gate, not its `atscPilotProjector` prefix.
        self.assertNotRegex(radar,
                            r"!fmReferenceFilter\s*&&\s*!atscPilot(?:\s|&&|\))")
        direct = radar.split("std::optional<RadarFrame> process_float_cpi(", 1)[1]
        direct = direct.split("void process_borrowed_ambiguity", 1)[0]
        self.assertIn("atscPilotProjector->process(rawReference", direct)
        self.assertIn("projectedReferenceContiguous.data()", direct)
        self.assertIn("stages->clutter_float(\n          rawReference", direct)
        self.assertIn("process_borrowed_ambiguity(nullptr, projected)", direct)
        self.assertNotIn("assign_complex", direct)
        self.assertNotIn("resize_for_write", direct)

    def test_projection_uses_existing_contiguous_ambiguity_without_cpu_copy(self):
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        borrowed = radar.split("void process_borrowed_ambiguity", 1)[1]
        borrowed = borrowed.split("RadarFrame finish_processed_cpi", 1)[0]
        self.assertIn("ambiguity_borrowed_reference(contiguousQualifiedReference", borrowed)
        self.assertIn("if (acceleration && qualifiedReference)", borrowed)
        self.assertIn("(qualifiedReference || contiguousQualifiedReference) ? 0", borrowed)

    def test_cost_and_lifecycle_metadata_reach_api(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        for key in ["staging_widened_samples", "staging_copied_samples",
                    "pilot_quality_ms", "pilot_projection_ms"]:
            self.assertIn(f'writer.Key("{key}")', main)
        api = (ROOT / "api/server.js").read_text()
        self.assertIn("input_processing: live?.input_processing ?? null", api)


if __name__ == "__main__":
    unittest.main()
