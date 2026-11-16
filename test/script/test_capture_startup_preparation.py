"""Keep cold qualification and consumer setup ahead of producer admission."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StartupPreparation(unittest.TestCase):
    def test_all_lanes_and_legacy_acceleration_prepare_before_threads(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        declaration = source.index("std::thread t1;")
        self.assertLess(source.index("multiRadar->prepareStartup();"), declaration)
        self.assertLess(source.index("acceleration.prepareStartup("), declaration)
        self.assertLess(source.index("resident->prepareStartup("), declaration)
        self.assertLess(source.index("referenceConfig, isClutter && !multiIlluminatorEnabled"), declaration)
        self.assertEqual(source.count("capture->process("), 1)
        consumer = source.index("std::thread t2(")
        producer = source.index("t1 = std::thread(")
        self.assertLess(consumer, source.index("publishTerminalCaptureAccounting = [&]"))
        self.assertLess(source.index("publishTerminalCaptureAccounting = [&]"), producer)
        self.assertLess(producer, source.index("capture->process("))
        self.assertRegex(source, r"t2\.join\(\);\s*if \(t1\.joinable\(\)\) t1\.join\(\);")

    def test_synthetic_dsp_stops_before_products(self):
        source = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        warm = source.split("  void prepareStartup()", 1)[1].split("  std::optional<RadarFrame> process_float_cpi", 1)[0]
        self.assertIn("startup::fillIq", warm)
        self.assertIn("if (!process_loaded_dsp())", warm)
        self.assertNotIn("finish_processed_cpi", warm)
        self.assertIn("stages->resetReference();", warm)
        self.assertIn("channel->clear();", warm)
        self.assertIn("for (auto& context : contexts_) context->prepareStartup();", source)

    def test_lane_profile_inherits_the_resolved_native_model(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        resolved = source.index('trackerAccelerationModel = trackerProfile == "balanced"')
        override = source.index('["acceleration_model"] >> trackerAccelerationModel', resolved)
        inherit = source.index('multiConfig.advanced.trackerAccelerationModel = trackerAccelerationModel;')
        self.assertLess(resolved, override)
        self.assertLess(override, inherit)
        self.assertLess(source.index('multiConfig.advanced = multiAdvanced;'), inherit)

    def test_centroid_and_tracker_use_actual_caf_geometry(self):
        source = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertIn('*config.centroidDopplerRadius, 1.0 / ambiguity.front()->get_cpi()', source)
        self.assertIn('config.trackerDelete, ambiguity.front()->get_cpi()', source)
        native = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn('new Centroid(nCentroid, nCentroidDoppler,', native)
        self.assertIn('residentConfig.detector = blah2::radar_contract::residentDetector(', native)

    def test_resident_startup_oracle_supports_dedicated_reference_roles(self):
        native = (ROOT / "src/blah2.cpp").read_text()
        startup = native.split('else if (resident && !state)', 1)[1].split(
            'else if (!resident)', 1)[0]
        self.assertIn('if (arrayReference)', startup)
        self.assertIn('captureData[referenceChannel]->drain_front(nSamples)', startup)
        self.assertIn('ArrayReferenceSynthesizer::Metrics dedicatedReferenceMetrics;', startup)

    def test_rejected_cpi_cannot_reach_observer_or_publication(self):
        source = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        loaded = source.split('  std::optional<RadarFrame> process_loaded_cpi', 1)[1].split('  void prepareStartup', 1)[0]
        self.assertRegex(loaded, r'if \(!process_loaded_dsp\(\)\)\s*\{\s*reject\([^;]+;\s*resetAdaptiveState\(\);\s*return std::nullopt;')
        self.assertLess(loaded.index('return std::nullopt'), loaded.index('finish_processed_cpi'))
        reset = source.split('  void resetAdaptiveState(', 1)[1].split('  void resetTracker()', 1)[0]
        self.assertIn('stages->resetReference();', reset)
        self.assertIn('temporalShadow->reset();', reset)
        self.assertNotIn('pendingFirstInputSample =', reset)

    def test_experimental_fm_filter_is_explicit_and_replay_only(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn('bool experimentalFmReferenceQualityFilter = false;', source)
        self.assertIn('experimentalFmReferenceQualityFilter && (!state || !multiIlluminatorEnabled)', source)
        self.assertIn('multiConfig.offlineReplay = state;', source)
        lane = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertIn('config.experimentalFmReferenceQualityFilter && definition.standard == "FM"', lane)
        self.assertIn('!config_.offlineReplay', lane)

    def test_resident_only_tracker_settings_fail_closed_natively(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        self.assertLess(source.index('tree["process"]["tracker"]["enable"] >> isTracker;'),
                        source.index('bool stationaryClutter=residentMode && isTracker;'))
        self.assertIn('bool stationaryClutter=residentMode && isTracker;', source)
        self.assertIn('stationaryClutter && (!residentMode || !isTracker)', source)
        self.assertIn('trackGuidedShadow && (!residentMode || !isTracker || !trackBeforeDetect)', source)
        self.assertIn('trackGuidedPromote && !trackGuidedShadow', source)

    def test_resident_complex_cutout_is_rejected_before_recorder_construction(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        rejection = source.index('if (residentMode && complexCutoutConfig.enabled)')
        construction = source.index('ComplexCutoutRecorder complexCutout(')
        self.assertLess(rejection, construction)
        self.assertIn('Complex cutout recording is not supported by the GPU-resident pathway',
                      source[rejection:construction])

    def test_direct_yaml_cannot_silently_enable_unimplemented_processing(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        for admission in (
            'Multi-illuminator input phase correction is not available',
            'Multi-illuminator Doppler windows other than rectangular are not available',
            'Per-illuminator input_role_mode must be manual or automatic',
        ):
            self.assertIn(admission, source)
        self.assertIn('definition.atscReferenceMode = "pilot_aided"', source)
        radar = (ROOT / "src/process/analysis/MultiIlluminatorRadar.cpp").read_text()
        self.assertIn('conditioning::AtscPilotProjector', radar)
        self.assertIn('ambiguityReference = &projectedReference', radar)
        self.assertIn('ridgeScrubber.process', radar)
        self.assertIn('config.trackerDopplerRidge', radar)

    def test_preparation_does_not_set_ready_or_skip_the_numerical_oracle(self):
        source = (ROOT / "src/process/ambiguity/Acceleration.cpp").read_text()
        warm = source.split("void Acceleration::prepareStartup(", 1)[1].split("double Acceleration::now_ms", 1)[0]
        self.assertIn("startupPending()", warm)
        self.assertIn("TotalQualificationFrames", warm)
        self.assertIn("processFrame(frame);", warm)
        self.assertNotRegex(warm, r"(?:checks_|combinedChecks_|clutterChecks_|status_)\s*=")

    def test_capture_discontinuity_keeps_resident_worker_and_drop_accounting(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        reset = source.split("if (extractedInputGeneration != inputGeneration)", 1)[1]
        reset = reset.split("delete tracker;", 1)[0]
        self.assertIn("resident->resetInput();", reset)
        self.assertNotIn("resident.reset()", reset)
        self.assertNotIn("make_unique", reset)
        self.assertNotRegex(reset, r"(?:residentLogical|residentConsumed|residentConfig\.epoch|captureDroppedSamples)\s*(?:=|\+\+)")
        self.assertIn("capture_drop_counters_retained=1", reset)

    def test_resident_qualification_keeps_strict_numerical_checks(self):
        source = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        self.assertIn("qualification_{Acceleration::TotalQualificationFrames}", source)
        self.assertIn("std::abs(value-expected)<1e-12", source)
        for stage in ("reference", "filtered", "maps", "products"):
            self.assertIn(f"qualification_.checked(Qualification::{stage});", source)
        self.assertIn("qualification_.requireComplete();", source)
        reset = source.split(" void resetInput()", 1)[1].split(" const OutputViews&", 1)[0]
        self.assertIn("session_->resetInput();", reset)
        self.assertIn("clearBorrowed();qualification_.resetInput();", reset)
        self.assertNotIn("published()", reset)
        self.assertNotIn("status_", reset)

    def test_live_resident_preparation_runs_every_oracle_without_publication(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        warm = source.split("else if (resident && !state)", 1)[1].split("else if (!resident)", 1)[0]
        self.assertIn("startup::fillIq(startupInput, nSamples, frame);", warm)
        self.assertIn("resident->begin(captureData, {++residentLogical, ++residentConsumed, timestamp});", warm)
        for call in ("referenceSynthesizer.process_into", "cpuStages.clutter",
                     "cpuStages.ambiguity", "resident->checkReference",
                     "resident->checkFiltered", "resident->checkMaps",
                     "resident->checkProducts", "resident->commit()"):
            self.assertIn(call, warm)
        self.assertIn("auto startupFusion = coherentMapFusion ?", warm)
        self.assertIn("std::make_unique<CoherentBeamBank>(*coherentMapFusion)", warm)
        self.assertIn("startupTracker = std::make_unique<Tracker>", warm)
        self.assertIn("cpuStages.resetReference();", warm)
        self.assertIn("referenceData->clear();", warm)
        self.assertIn("for (auto* channel : startupInput) channel->clear();", warm)
        self.assertIn("for (auto* channel : surveillancePointers) channel->clear();", warm)
        self.assertIn("if (resident->qualifying())", warm)
        for forbidden in ("capture->", "socket_", "complexCutout.", "liveTemporalShadow",
                          "timing->", "tracker->process", "coherentMapFusion->calibrate",
                          "resident->published", "saveMap", "manifoldRecorder.append"):
            self.assertNotIn(forbidden, warm)
        self.assertNotRegex(warm, r"(?:residentLogical|residentConsumed|residentConfig\.epoch)\s*=")
        self.assertLess(source.index("resident->prepareStartup("),
                        source.index('[Startup] DSP ready for capture'))

    def test_resident_preparation_fails_closed_and_resets_before_ready(self):
        source = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        warm = source.split(" void prepareStartup(", 1)[1].split(" const OutputViews&", 1)[0]
        self.assertIn("qualification_.prepareStartup(prepare,", warm)
        self.assertIn("session_->published();", warm)  # transport settlement, no sink
        self.assertIn("resetInput();resetCpu();", warm)
        self.assertLess(warm.index("resetInput();resetCpu();"), warm.index('status_.state="ready"'))
        self.assertIn('stop();status_.state="failed";', warm)
        self.assertIn("throw;", warm)
        gate = (ROOT / "src/process/ambiguity/GpuResidentQualification.h").read_text()
        gate = gate.split(" void prepareStartup(", 1)[1]
        self.assertIn("if(completed_||verified_)", gate)
        self.assertIn("while(pending())", gate)
        self.assertIn("begin();prepare(completed_);requireComplete();settle();published();", gate)
        self.assertIn("reset();resetInput();", gate)
        self.assertNotRegex(gate, r"(?:completed_|required_)\s*=")

    def test_resident_history_reset_is_device_side_and_not_reconstruction(self):
        pipeline = (ROOT / "core/GpuResidentFrame.private.h").read_text()
        reset = pipeline.split(" void resetInput()", 1)[1].split(" bool probePositiveLog", 1)[0]
        for stage in ("referenceStage->reset()", "postprocessStage->appendInputReset",
                      "postprocessStage->completeInputReset", "tracker_->resetInput()"):
            self.assertIn(stage, reset)
        self.assertNotIn("make_unique", reset)
        post = (ROOT / "core/GpuPostprocess.private.h").read_text()
        reset = post.split(" void appendInputReset", 1)[1].split(" void append(", 1)[0]
        for history in ("integration_", "currentPower_", "angleHistory_", "tbdBanks_", "replayStatistics_"):
            self.assertIn(history, reset)
        self.assertIn("0x7ff80000u", reset)
        self.assertIn("79*4", reset)
        tracker = (ROOT / "core/GpuTracker.private.h").read_text()
        reset = tracker.split(" void resetInput()", 1)[1].split(" TrackerToken preparedToken", 1)[0]
        self.assertIn("clutterPersistence_->handle", reset)
        self.assertNotRegex(reset, r"(?:serial_|guidedBudgetExceeded_)\s*=")


if __name__ == "__main__":
    unittest.main()
