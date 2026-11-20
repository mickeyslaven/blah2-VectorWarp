"""Source wiring checks only; native numerical/lifecycle tests require compilation."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeOutputPathwayContract(unittest.TestCase):
    def test_cycle_timing_closes_after_publication_and_next_loop_cleanup(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        start = main.index("const auto residentWholeStart=")
        close = main.index("timing->complete_processing_cycle(residentWholeStart);", start)
        self.assertLess(close, main.index("bool ready = false;", start))
        pipeline_end = main.index("const auto pipelineEnd=", close)
        publication = main.index('event("publication-complete")', pipeline_end)
        record = main.index("timing->record_processing_cycle(", publication)
        self.assertEqual(main.count("timing->record_processing_cycle("), 1)
        self.assertLess(record, main.index("continue;", publication))
        self.assertIn("milliseconds(pipelineEnd,pipelineStart)};", main)

    def test_replay_is_not_gated_on_a_live_device(self):
        capture = (ROOT / "src/capture/Capture.cpp").read_text()
        self.assertIn("if ((replay || device) && !inputStopped.load())", capture)
        self.assertIn('if (device && payload.HasMember("recording")', capture)
        self.assertIn('else if (device && request && request->status == 404)', capture)
        self.assertIn("replayControl.publish({", capture)
        self.assertIn("return replayControl.snapshot();", capture)
        self.assertNotIn("replayControlRevision", capture)

    def test_replay_revision_is_not_reset_by_loop_and_eof_remains_controlled(self):
        replay = (ROOT / "src/capture/Replay.cpp").read_text()
        self.assertLess(replay.index("uint64_t controlRevision=0"), replay.index("  do {"))
        self.assertIn("progress.controlsAvailable=awaitControlsAtEof", replay)
        self.assertIn("if(!awaitControlsAtEof) return", replay)
        capture = (ROOT / "src/capture/Capture.cpp").read_text()
        self.assertIn("return replayControl.snapshot();\n        }, true)", capture)
        header = (ROOT / "src/capture/Replay.h").read_text()
        self.assertEqual(header.count("std::lock_guard<std::mutex> lock(mutex_)"), 2)

    def test_native_saves_and_cutouts_cover_normal_and_drain_paths(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        self.assertEqual(main.count("processMultiFrames(std::move("), 2)
        self.assertEqual(main.count("saveMultiProducts(frame,"), 2)
        save = main.split("const auto saveMultiProducts =", 1)[1].split(
            "// Finish first-use CPU/GPU qualification", 1)[0]
        self.assertIn("frame.map->to_json_km", save)
        self.assertIn("multi_detection_json(frame, publication.timestampMs)", save)
        detection = main.split("std::string multi_detection_json(", 1)[1].split(
            "std::string multi_track_json(", 1)[0]
        self.assertIn("frame.detections->to_json_km", detection)
        self.assertIn("frame.sampleRateHz", detection)
        self.assertNotIn("to_json_presentation", save)
        self.assertIn("if (multiRadar && complexCutout.enabled())", main)
        self.assertIn("identity.channelIds = view.channelIds", main)
        self.assertIn("complexCutout.appendNative", main)

    def test_native_metadata_does_not_round_scientific_payload(self):
        identity = (ROOT / "src/process/analysis/NativeProductIdentity.h").read_text()
        self.assertNotIn("annotate_event_time(", identity)
        self.assertNotIn(".Parse(", identity)
        self.assertIn("event_time_json(identity.eventTime", identity)
        cutout = (ROOT / "src/process/analysis/ComplexCutoutRecorder.cpp").read_text()
        self.assertNotIn("SetMaxDecimalPlaces", cutout)
        header = (ROOT / "src/process/analysis/ComplexCutoutRecorder.h").read_text()
        self.assertIn("return capacityReached_ || frames_ >= config_.maxFrames", header)
        self.assertEqual(cutout.count("if (!config_.enabled || complete()) return;"), 2)
        self.assertIn("restricted to replay input", cutout)

    def test_observer_is_only_at_success_boundary_and_failure_is_latched(self):
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        finish = radar.index("RadarFrame finish_processed_cpi")
        callback = radar.index("nativeObserver(view)")
        self.assertLess(finish, callback)
        self.assertLess(callback, radar.index("pipeline::fuseMaps", finish))
        self.assertEqual(radar.count("nativeObserver(view)"), 1)
        self.assertIn("nativeObserverFailed = true;\n      nativeObserver(view);", radar)
        self.assertIn('throw std::logic_error("Cannot reuse a lane', radar)

    def test_optional_wideband_fallback_and_heterogeneous_lane_status(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn("multiPipelineState.startup_fallback(", main)
        self.assertIn("Optional GPU channelizer unavailable", main)
        # The unified multi-lane route now supports qualified resident lanes;
        # retain explicit capability/refusal checks, not the deleted blanket
        # rejection of every multi-illuminator resident configuration.
        self.assertIn("This cohort does not provide the current resident GPU route", main)
        resident_guard = main.split("#ifndef VECTORWARP_RESIDENT_GPU", 1)[1].split("#endif", 1)[0]
        self.assertTrue("if(residentMode)throw" in resident_guard)
        self.assertIn("if (residentMode) multiConfig.residentProductTemplate = residentConfig", main)
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("if (residentRequired && !residentProductRoute)", radar)
        self.assertIn("requires a qualified resident backend", radar)
        self.assertIn('throw std::runtime_error("No selectable FP64 resident device")', radar)
        self.assertIn("multiConfig.accelerationMode = accelerationMode;", main)
        self.assertEqual(main.count("aggregate_status(frames)"), 2)
        self.assertEqual(main.count("aggregate.ambiguityGpuExecuted, aggregate.ambiguityCpuExecuted"), 2)


if __name__ == "__main__":
    unittest.main()
