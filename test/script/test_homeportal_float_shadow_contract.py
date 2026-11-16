"""Wiring checks for ATSC float ingestion and bounded observation-only ridge work."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HomePortalFloatShadowContract(unittest.TestCase):
    def test_pilot_does_not_force_double_staging(self):
        radar = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertNotIn("!fmReferenceFilter && !atscPilot", radar)
        direct = radar.split("std::optional<RadarFrame> process_float_cpi(", 1)[1]
        direct = direct.split("void process_borrowed_ambiguity", 1)[0]
        self.assertIn("atscPilotProjector->process(rawReference", direct)
        self.assertIn("projectedReferenceContiguous.data()", direct)
        self.assertIn("stages->clutter_float(\n          rawReference", direct)
        self.assertIn("process_borrowed_ambiguity(nullptr, projected)", direct)
        self.assertNotIn("assign_complex", direct)
        self.assertNotIn("resize_for_write", direct)

    def test_projection_uses_existing_contiguous_ambiguity_without_cpu_copy(self):
        radar = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        borrowed = radar.split("void process_borrowed_ambiguity", 1)[1]
        borrowed = borrowed.split("RadarFrame finish_processed_cpi", 1)[0]
        self.assertIn("ambiguity_borrowed_reference(contiguousQualifiedReference", borrowed)
        self.assertIn("if (acceleration && qualifiedReference)", borrowed)
        self.assertIn("(qualifiedReference || contiguousQualifiedReference) ? 0", borrowed)

    def test_shadow_completion_only_releases_observation_state(self):
        radar = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertIn('scrubberMode == "active" ||', radar)
        self.assertIn('scrubberMode == "shadow" && ridgeShadowWindow.available()', radar)
        completion = radar.split('if (scrubberMode == "shadow" && compare && !ridgeShadowWindow.available())', 1)[1]
        completion = completion.split("\n      }", 1)[0]
        self.assertIn("comparisonTracker.reset();", completion)
        self.assertIn("ridgeScrubber.reset();", completion)
        self.assertNotIn("tracker =", completion)
        self.assertNotIn("config.", completion)
        self.assertIn('clear("input_discontinuity")', radar)
        self.assertIn("ridgeShadowWindow.reset(reason)", radar)

    def test_cost_and_lifecycle_metadata_reach_api(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        for key in ["staging_widened_samples", "staging_copied_samples",
                    "pilot_quality_ms", "pilot_projection_ms", "comparison_available",
                    "observed_cpis", "maximum_cpis", "comparison_ms", "reset_reason"]:
            self.assertIn(f'writer.Key("{key}")', main)
        self.assertIn('else writer.Null();', main)
        api = (ROOT / "api/server.js").read_text()
        self.assertIn("input_processing: live?.input_processing ?? null", api)
        self.assertIn("doppler_ridge_scrubber: live?.doppler_ridge_scrubber ?? null", api)

    def test_window_is_configured_documented_and_bounded(self):
        header = (ROOT / "src/process/detection/DopplerRidgeScrubber.h").read_text()
        self.assertIn("shadowMaximumCpis = 256", header)
        window = (ROOT / "src/process/detection/DopplerRidgeShadowWindow.h").read_text()
        self.assertIn("observed_ < maximum_", window)
        self.assertIn("maximum_ > 65535", window)
        for relative in ["src/blah2.cpp", "api/config-manager.js", "api/config-rules.js",
                         "html/js/config_ui.js"]:
            self.assertIn("shadow_maximum_cpis", (ROOT / relative).read_text())
        # An omitted optional default must not get injected on save/reload.
        self.assertNotIn("shadow_maximum_cpis", (ROOT / "api/config-store.js").read_text())


if __name__ == "__main__":
    unittest.main()
