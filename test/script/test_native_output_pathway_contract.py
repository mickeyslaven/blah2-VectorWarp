"""Source wiring checks only; native numerical/lifecycle tests require compilation."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeOutputPathwayContract(unittest.TestCase):
    def test_every_tracker_publication_uses_the_canonical_diagnostics(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        lane = main.split("std::string multi_track_json(", 1)[1].split("template <typename Frame>", 1)[0]
        self.assertIn("annotateTrackerFrameDiagnostics(", lane)
        self.assertIn("frame.trackerDiagnostics", lane)
        publish = main.split("const auto publishRadarFrame =", 1)[1].split(
            "// Finish first-use CPU/GPU qualification", 1)[0]
        self.assertIn("socket_track->sendData(tracks)", publish)
        self.assertIn("publication.timestampMs, map, detections, tracks", publish)
        self.assertEqual(main.count("socket_track->sendData("), 1)
        self.assertNotIn("cpuProducts", main)
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("result.residentProducts && !result.residentProducts->trackerOnHost ?", radar)
        self.assertIn("residentProducts->trackerDiagnostics : products->tracker.frame_diagnostics()", radar)

    def test_resident_diagnostics_layout_keeps_all_status_and_counter_words(self):
        tracker = (ROOT / "src/process/tracker/Tracker.h").read_text()
        struct = tracker.split("struct TrackerFrameDiagnostics {", 1)[1].split("};", 1)[0]
        members = set(re.findall(r"\b(\w+)=0\b", struct))
        registry = tracker.split("trackerDiagnosticFields[]={", 1)[1].split("};", 1)[0]
        fields = re.findall(r'TRACKER_DIAGNOSTIC\((\w+),"([^"]+)"\)', registry)
        self.assertEqual(members, {member for member, _ in fields})
        self.assertEqual(len(fields), len({key for _, key in fields}))
        layout = (ROOT / "src/process/ambiguity/GpuResidentEvidenceLayout.h").read_text()
        layout = layout.split("namespace prepared {", 1)[1].split("}", 1)[0]
        offsets = {key: int(value) for key, value in re.findall(r"\b(\w+)=(\d+)", layout)}
        self.assertLessEqual(offsets["trackerPreparedStatus"]+66*4, offsets["trackerDiagnostics"])
        self.assertLessEqual(8+len(fields)*8, offsets["trackerDiagnosticsCapacity"])
        self.assertLessEqual(offsets["trackerDiagnostics"]+offsets["trackerDiagnosticsCapacity"], offsets["times"])
        self.assertLessEqual(offsets["times"]+16+14*8, offsets["nativeBatches"])
        self.assertEqual(offsets["submitContext"]+256, offsets["bytes"])

    def test_ordinary_output_attaches_the_existing_native_plan_owner(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        identity = (ROOT / "src/process/analysis/NativeProductIdentity.h").read_text()
        self.assertIn("result.resolvedProductPlan = products->planIdentity();", radar)
        self.assertIn("result.resolvedProductPlan = frame.resolvedProductPlan;", main)
        self.assertNotIn("identity.resolvedProductPlan = radarPlan.identity();", main)
        self.assertEqual(main.count("result.resolvedProductPlan = frame.resolvedProductPlan;"), 1)
        self.assertIn('writer.Key("resolved_product_plan"); writer.String(identity.resolvedProductPlan.c_str());', identity)
        # The native serializer is authoritative; attachment never parses or
        # rewrites its derived values or the scientific payload.
        self.assertNotIn(".Parse(", identity)

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
        self.assertIn("milliseconds(outputEnd,pipelineStart)});", main)
        self.assertIn("if (!framePublications.empty())\n              timing->record_processing_cycle", main)

    def test_timing_and_timestamp_follow_each_returned_publication_token(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        publish = main.split("const auto publishRadarTiming =", 1)[1].split(
            "const auto drainMultiPublications =", 1)[0]
        self.assertIn("for (const auto& [sequence, framePublication] : publications)", publish)
        self.assertIn("aggregate_status(frames, sequence)", publish)
        for field in ("timestampMs", "capturePrepareMs", "channelizerMs", "captureBacklogSamples",
                      "captureDroppedSamples", "samplePositionAvailable", "captureEpoch",
                      "captureFirst", "captureEnd", "inputDiscontinuity", "acceleration",
                      "widebandGpuExecuted", "widebandCpuExecuted", "eventTime"):
            self.assertIn("framePublication." + field, publish)
        self.assertEqual(main.count("timing->update("), 1)
        self.assertEqual(main.count("socket_timestamp->sendData("), 1)
        self.assertIn("std::to_string(framePublication.timestampMs)", publish)
        self.assertNotRegex(publish, r"\bpublication(?:->|\.)|frames\.front\(\)")
        self.assertEqual(main.count("multiProductPublications.snapshotsFor(frames)"), 2)
        self.assertEqual(main.count("publishRadarTiming(frames, framePublications, laneHealth,"), 2)
        steady = main.split("auto frames = processMultiFrames(std::move(channelized)", 1)[1]
        ordered = steady.index("publish_then_advance(")
        self.assertLess(steady.index("snapshotsFor(frames)"), ordered)
        self.assertLess(ordered, steady.index("const auto pipelineEnd=", ordered))
        ordered_call = steady[ordered:steady.index("const auto pipelineEnd=", ordered)]
        self.assertLess(ordered_call.index("publishRadarTiming("), ordered_call.index("}, finishCurrent);"))
        self.assertIn("publication.capturePrepareMs = capturePrepareMs;", main)
        fifo = (ROOT / "src/process/analysis/MultiIlluminatorPipelineState.h").read_text()
        snapshot = fifo.split("snapshotsFor(", 1)[1].split("  void retain(", 1)[0]
        self.assertIn("(void)at(frame.publicationSequence)", snapshot)
        self.assertIn("frame.publicationSequence == entry.first", snapshot)
        self.assertIn("result.push_back(entry)", snapshot)

    def test_replay_is_not_gated_on_a_live_device(self):
        capture = (ROOT / "src/capture/Capture.cpp").read_text()
        self.assertIn("if ((replay || device) && !inputStopped.load())", capture)
        self.assertIn('if (device && payload.HasMember("recording")', capture)
        self.assertIn('else if (device && request && request->status == 404)', capture)
        self.assertIn("replayControl.publish({", capture)
        self.assertIn("return replayControl.snapshot();", capture)
        self.assertNotIn("replayControlRevision", capture)

    def test_resident_spectrum_shape_uses_one_resolved_bandwidth(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        start = source.index("const double spectrumBandwidth = resolveReferenceSpectrumBandwidth(")
        block = source[start:source.index("frontend.reference.centreHz", start)]
        self.assertIn("resolveReferenceSpectrumBins(samples,\n          definition.outputRateHz, spectrumBandwidth)", block)
        self.assertIn("frontend.reference.bandwidthHz = spectrumBandwidth;", block)

    def test_replay_revision_is_not_reset_by_loop_and_eof_remains_controlled(self):
        replay = (ROOT / "src/capture/Replay.cpp").read_text()
        self.assertLess(replay.index("uint64_t controlRevision=0"), replay.index("  do {"))
        self.assertIn("progress.controlsAvailable=awaitControlsAtEof", replay)
        self.assertIn("if(!awaitControlsAtEof) return", replay)
        capture = (ROOT / "src/capture/Capture.cpp").read_text()
        # EOF controls remain enabled; the newer trailing callback reports queue
        # position without replacing the control mailbox or its true EOF flag.
        self.assertRegex(capture, r"return replayControl\.snapshot\(\);\s*"
            r"\},\s*true,\s*\[&\]\(uint64_t samples\)\s*\{\s*"
            r"replayQueuedSamples\.store\(samples, std::memory_order_release\);")
        header = (ROOT / "src/capture/Replay.h").read_text()
        self.assertEqual(header.count("std::lock_guard<std::mutex> lock(mutex_)"), 2)

    def test_native_saves_and_cutouts_cover_normal_and_drain_paths(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        self.assertEqual(main.count("processMultiFrames(std::move("), 2)
        self.assertEqual(main.count("saveMultiProducts(frame,"), 1)
        self.assertEqual(main.count("publishRadarFrame(frame,"), 2)
        save = main.split("const auto saveMultiProducts =", 1)[1].split(
            "// Finish first-use CPU/GPU qualification", 1)[0]
        self.assertIn("multi_map_json(frame, publication.timestampMs, true, false)", save)
        map_serializer = main.split("std::string multi_map_json(", 1)[1].split(
            "std::string multi_detection_json(", 1)[0]
        self.assertIn("frame.map->to_json_km", map_serializer)
        self.assertIn("multi_detection_json(frame, publication.timestampMs)", save)
        detection = main.split("std::string multi_detection_json(", 1)[1].split(
            "std::string multi_track_json(", 1)[0]
        self.assertIn("frame.detections->to_json_km", detection)
        self.assertIn("frame.sampleRateHz", detection)
        self.assertNotIn("to_json_presentation", save)
        self.assertIn("if (multiRadar && complexCutout.enabled())", main)
        self.assertIn("identity.channelIds = view.channelIds", main)
        self.assertIn("complexCutout.appendNative", main)
        self.assertIn("view.maps, view.referenceWeights, view.directPathPilot, &identity", main)
        self.assertIn("view.pilotAnchorChannel", main)

    def test_live_maps_are_bounded_presentation_not_radar_deadline_products(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        coordinator = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        # Live resident DSP retains its owned map but does not stringify the
        # full raster before the publisher can apply display cadence/limits.
        self.assertIn("bool serializeMap=true", coordinator)
        self.assertIn("if(serializeMap)result.map=json::map", coordinator)
        self.assertIn("config.offlineReplay);", radar)
        # Direct displays receive a bounded raster every CPI because that UI
        # does not cache metadata-only frames. Channelized views own a cache.
        self.assertIn("const bool publishFullMap = !multiIlluminatorEnabled ||", main)
        self.assertIn("(!recoveringBacklog && fullMapCadence);", main)
        self.assertEqual(main.count("framePublication.publishFullMap, !state)"), 2)
        # Research saves remain full-resolution and are not presentation JSON.
        self.assertIn("publication.timestampMs, true, false", main)

    def test_direct_and_channelized_products_share_input_bound_publication(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        submit = main.split("const auto processMultiFrames =", 1)[1].split(
            "const auto resetMultiRadar =", 1)[0]
        self.assertEqual(main.count("multiRadar->processPipelined("), 1)
        self.assertNotIn("multiRadar->process(", main)
        self.assertIn("multiProductPublications.append(publication)", submit)
        self.assertIn("publication.inputGainDb, sequence, publication.signalTimestampMs", submit)
        self.assertNotRegex(submit, r"if\s*\([^)]*\b(?:state|multiIlluminatorEnabled)\b")
        # Each serialized frame retains its own token even if one lane resets
        # or rejects the newer CPI. Both normal and drain publication use it.
        self.assertEqual(len(re.findall(
            r"const auto& framePublication\s*=\s*multiProductPublications.at\(frame.publicationSequence\)",
            main)), 4)
        publish = main.split("const auto publishRadarFrame =", 1)[1].split(
            "// Finish first-use CPU/GPU qualification", 1)[0]
        for field in ("frame.referenceSpectrum", "frame.manifoldEvidence", "frame.residentDiagnostics"):
            self.assertIn(field, publish)
        for forbidden in ("processFrontend(", "fuseMaps(", "->process(", "->format("):
            self.assertNotIn(forbidden, publish)
        drain = main.split("const auto drainMultiPublications =", 1)[1].split(
            "uint64_t inputGeneration", 1)[0]
        self.assertIn("multiPendingPublication.has_value()", drain)
        self.assertIn("multiRadar->pendingPublicationSequences().empty()", drain)
        self.assertIn("multiRadar->drain()", drain)
        self.assertIn("publishRadarFrame(frame, framePublication", drain)

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
        finish = radar.index("StagedCpi stage_processed_cpi")
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
        # Only coherent resident products need this template. Noncoherent
        # resident frontends feed the canonical shared FP64 host products.
        self.assertIn("if (coherentFusion && residentMode) multiConfig.residentProductTemplate = residentConfig", main)
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("if (residentRequired && !residentProductRoute)", radar)
        self.assertIn("requires a qualified resident backend", radar)
        self.assertIn('throw std::runtime_error("No selectable FP64 resident device")', radar)
        # The shared intent now supplies backend policy to both ingress routes;
        # a second lane-only assignment would reinstate the removed duplicate.
        self.assertLess(main.index("radarIntent.accelerationMode = accelerationMode;"),
            main.index("auto multiConfig = radarIntent;"))
        self.assertEqual(main.count("aggregate_status(frames, sequence)"), 1)
        self.assertEqual(main.count("aggregate.ambiguityGpuExecuted, aggregate.ambiguityCpuExecuted"), 1)

    def test_required_resident_auto_preserves_request_and_fails_closed(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn("selectRequiredResidentRoute(accelerationMode,", main)
        self.assertIn("requiresResidentProducts(requiredResidentFeatures)", main)
        self.assertIn("causalEnabled || residentSnapshotEnabled", main)
        self.assertNotIn('accelerationMode = "gpu-resident"', main)
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("residentStatus.requested = config.accelerationMode;", radar)
        self.assertIn("result.acceleration = residentStatus;", radar)
        self.assertIn("result.clutterAcceleration = residentStatus;", radar)
        self.assertIn("if (residentRequired && !residentBuilt)", radar)
        self.assertIn('if (config_.accelerationMode == "gpu-resident" || residentRequired) throw;', radar)


if __name__ == "__main__":
    unittest.main()
