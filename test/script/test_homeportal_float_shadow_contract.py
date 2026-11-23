"""Regression checks for canonical ATSC float ingestion and staging telemetry."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CanonicalFloatIngressContract(unittest.TestCase):
    def test_native_float_selection_is_capability_not_device_specific(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        assignment = "directResidentFloatCapture = !multiIlluminatorEnabled"
        selection = main.split(assignment, 1)[1].split(
            "if (nativeFloatGpuCapture)", 1)[0]
        self.assertIn("multiRadar->prefersNativeFloatCapture()", selection)
        self.assertIn("bool(multiGpuChannelizer) || directResidentFloatCapture", selection)
        for identity in ("Kraken", "Usrp", "HackRf", "RspDuo", "linux", "APPLE"):
            self.assertNotIn(identity, selection)
        self.assertIn("multiGpuStagingOwner", main)
        self.assertLess(main.index("multiRadar->prepareStartup();"),
                        main.index(assignment))
        bank = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        capability = bank.split("bool RadarBank::prefersNativeFloatCapture() const", 1)[1]
        capability = capability.split("void RadarBank::reset()", 1)[0]
        self.assertIn("startupPrepared_", capability)

    def test_native_float_survives_wideband_gpu_fallback(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        cpu = main.split("}else{\n              auto current=directFloatCapture?", 1)[1]
        cpu = cpu.split("const double channelizerMs", 1)[0]
        self.assertIn("multiChannelizer->process(multiGpuStagingInput,firstInputSample)", cpu)
        self.assertIn("multiChannelizer->process(channelizerInput,firstInputSample)", cpu)

    def test_pilot_does_not_force_double_staging(self):
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        # The projector is the zero-copy conditioning path.  Reject only the
        # former quality-observer gate, not its `atscPilotProjector` prefix.
        self.assertNotRegex(radar,
                            r"!fmReferenceFilter\s*&&\s*!atscPilot(?:\s|&&|\))")
        direct = radar.split("void process_float_cpi(", 1)[1]
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
        borrowed = borrowed.split("StagedCpi stage_processed_cpi", 1)[0]
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

    def test_pilot_and_ingestion_snapshots_precede_asynchronous_products(self):
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        stage = radar.split("StagedCpi stage_processed_cpi(", 1)[1].split(
            "RadarFrame complete_processed_cpi(", 1)[0]
        for snapshot in ("result.inputProcessing = inputProcessing;",
                         "result.atscPilot = atscPilot->metrics();",
                         "result.atscPilotProjection = atscPilotProjectionMetrics;"):
            self.assertIn(snapshot, stage)
        self.assertLess(stage.index("result.atscPilot ="), stage.index("atscPilot->reset();"))
        self.assertLess(stage.index("result.inputProcessing ="), stage.index("inputProcessing = {};"))
        complete = radar.split("RadarFrame complete_processed_cpi(", 1)[1].split(
            "void queue_processed_cpi(", 1)[0]
        self.assertNotIn("atscPilot->", complete)
        self.assertNotIn("inputProcessing =", complete)


if __name__ == "__main__":
    unittest.main()
