#!/usr/bin/env python3
"""Lightweight corpus-source contracts. These NEVER produce physical receipts."""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

import unified_corpus_fixtures as fixtures
import unified_corpus_oracles as oracles
import unified_physical_corpus as corpus


class UnifiedCorpusContractTest(unittest.TestCase):
    def test_interleaved_evidence_rows_preserve_full_publication_alignment(self):
        run = self.publication_unit_run(corpus.PUBLISHED_FRAMES)
        published = copy.deepcopy(run["products"]["timing"])
        for sequence, row in enumerate(published, 1):
            row["nCpi"] = sequence
        stream = b"".join(fixtures.canonical(value) for row in published for value in
                          (row, {**row, "cycleEvidenceOnly": True}))
        run["products"]["timing"] = corpus.parity.publication_timings(stream)
        self.assertEqual(run["products"]["timing"], published)
        self.assertEqual(len(corpus.complete_publications(run)["corpus-0"]), corpus.PUBLISHED_FRAMES)
        run["products"]["illuminator"].pop()
        with self.assertRaisesRegex(AssertionError, "partial trailing lane"):
            corpus.complete_publications(run)

    def test_map_stream_retains_every_frame_and_pins_full_arrays(self):
        values = [{"id": "lane", "map": {"nRows": 1, "nCols": 2, "data": [[index+.01, index+.02]]},
                   "detections": {"delay": [index]}, "tracks": {"data": [{"n": index}]}}
                  for index in range(1, 13)]
        with tempfile.TemporaryDirectory() as temporary:
            stream = corpus.MapEvidenceStream(Path(temporary) / "maps", limit=512, total=8192)
            body = b"".join(fixtures.canonical(value) for value in values)
            self.assertGreater(len(body), stream.limit)
            chunks = [stream.feed("illuminator", body[i:i+13]) for i in range(0, len(body), 13)]
            stream.finish()
            actual = [json.loads(line) for line in b"".join(chunks).splitlines()]
            self.assertEqual(len(actual), 12)
            self.assertEqual([value["tracks"]["data"][0]["n"] for value in actual], list(range(1, 13)))
            for expected, value in zip(values, actual):
                reference = value["map"].pop("dataArtifact")
                data = Path(reference["path"]).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), reference["sha256"])
                value["map"]["data"] = json.loads(data)
                self.assertEqual(value, expected)
            self.assertLess(len(b"".join(chunks)), 64 << 20)

    def test_map_stream_bounds_and_partial_eof_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stream = corpus.MapEvidenceStream(root / "bounded", limit=80, total=10)
            with self.assertRaisesRegex(AssertionError, "1 GiB"):
                stream.feed("map", b'{"data":[[1.01,2.02,3.03]]}')
            stream = corpus.MapEvidenceStream(root / "oversized", limit=10)
            with self.assertRaisesRegex(AssertionError, "64 MiB"):
                stream.feed("map", b'{"data":[[1.01,2.02,3.03]]}')
            stream = corpus.MapEvidenceStream(root / "partial")
            stream.feed("map", b'{"data":[')
            with self.assertRaisesRegex(AssertionError, "EOF"):
                stream.finish()

    def publication_unit_run(self, count=2):
        run = {"case": {"variant": {"lanes": 2, "statistic": "power"}, "mode": "auto",
                        "target": False, "fault": "commit"},
               "config": {"process": {"multi_illuminator": {"enabled": True,
                   "illuminators": [{"id": "corpus-0"}, {"id": "corpus-1"}]}}, "capture": {"replay": {"qualification": {
                   "fault": {"kind": "commit", "route": "resident", "lane": 0, "frame": 16}}}}},
               "products": {name: [] for name in ("map", "detection", "track", "timestamp", "timing", "illuminator", "bearing")},
               "events": [], "exited": False, "log": ""}
        for offset in range(count):
            number, first = 16 + offset, 1000 + offset * 20
            stamp = {"timestamp": number}
            event = {"frame": number, "captureEpoch": 7, "captureFirstEpochSample": first,
                     "captureEndEpochSample": first + 20, "route": "capture", "lane": 0,
                     "workerPid": 0, "boundary": "publication-complete"}
            run["events"].append(event)
            run["products"]["timing"].append({**stamp, "nCpi": offset + 1})
            run["products"]["timestamp"].append(dict(stamp))
            for lane in range(2):
                lineage = {**stamp, "capture_epoch": 7, "capture_first_input_sample": first,
                           "capture_end_input_sample": first + 20}
                run["products"]["illuminator"].append({**lineage, "id": f"corpus-{lane}",
                    "acceleration": {"active": "cpu" if lane == 0 else "vulkan-resident",
                                     "cpuExecuted": lane == 0, "gpuExecuted": lane != 0},
                    "map": {**lineage, "data": [[1.]]},
                    "detections": {**lineage, "delay": [], "doppler": []},
                    "tracks": {**lineage, "n": 0, "data": []}})
        return run

    def fault_unit_runs(self):
        run = self.publication_unit_run()
        event = {**run["events"][0], "boundary": "commit", "route": "resident", "workerPid": 100}
        run["events"][0:0] = [event, {**event, "boundary": "history-reset"},
                              {**event, "boundary": "cpu-recovery-complete"}]
        run["events"].append({**event, "boundary": "worker-reaped"})
        cpu = self.publication_unit_run()
        cpu["case"].update(mode="cpu", fault="cpu-oracle-reset", noGap=True)
        cpu["events"].insert(0, {**event, "route": "host", "boundary": "history-reset", "fault": "cpu-oracle-reset"})
        return run, cpu

    def test_all_trailing_publications_are_checked(self):
        run = self.publication_unit_run(corpus.PUBLISHED_FRAMES + 1)
        self.assertEqual(len(corpus.complete_publications(run)["corpus-0"]), corpus.PUBLISHED_FRAMES + 1)
        for value in (None, True, 1, 3, 2.0):
            bad = copy.deepcopy(run)
            bad["products"]["timing"][1]["nCpi"] = value
            with self.subTest(nCpi=value), self.assertRaisesRegex(AssertionError, "nonsequential publication nCpi"):
                corpus.complete_publications(bad)
        bad = copy.deepcopy(run)
        bad["products"]["illuminator"].pop()
        with self.assertRaisesRegex(AssertionError, "partial trailing lane"):
            corpus.complete_publications(bad)
        bad = copy.deepcopy(run)
        bad["products"]["illuminator"][-1]["tracks"]["capture_first_input_sample"] -= 20
        with self.assertRaisesRegex(AssertionError, "unbound mathematical product"):
            corpus.complete_publications(bad)
        bad = copy.deepcopy(run)
        bad["products"]["track"].append({"timestamp": 0})
        with self.assertRaisesRegex(AssertionError, "partial trailing primary"):
            corpus.complete_publications(bad)

    def test_fault_requires_exact_reset_order_and_detector_tracker_results(self):
        class UnitChecks:
            expected = set()
        run, cpu = self.fault_unit_runs()
        oracles.fault_checks(run, UnitChecks(), {"unitFixtureOnly": True}, cpu)
        for boundary in ("history-reset", "cpu-recovery-complete"):
            bad = copy.deepcopy(run)
            next(event for event in bad["events"] if event["boundary"] == boundary)["frame"] -= 1
            with self.assertRaisesRegex(AssertionError, "exact fault interval"):
                oracles.fault_checks(bad, UnitChecks(), {}, cpu)
        bad = copy.deepcopy(run)
        bad["events"][1], bad["events"][2] = bad["events"][2], bad["events"][1]
        with self.assertRaisesRegex(AssertionError, "out of order"):
            oracles.fault_checks(bad, UnitChecks(), {}, cpu)
        for product, field, value in (("map", "data", [[2.]]), ("detections", "delay", [10.]),
                                      ("tracks", "n", 1)):
            bad = copy.deepcopy(run)
            bad["products"]["illuminator"][0][product][field] = value
            with self.assertRaises(AssertionError):
                oracles.fault_checks(bad, UnitChecks(), {}, cpu)
        bad_cpu = copy.deepcopy(cpu)
        bad_cpu["events"][0]["lane"] = 1
        with self.assertRaisesRegex(AssertionError, "CPU reset did not match"):
            oracles.fault_checks(run, UnitChecks(), {}, bad_cpu)
        bad_cpu = copy.deepcopy(cpu)
        bad_cpu["case"].update(fault="input-gap", gapFrame=15)
        with self.assertRaisesRegex(AssertionError, "same exact-lane reset"):
            oracles.fault_checks(run, UnitChecks(), {}, bad_cpu)

    def test_null_control_fails_even_if_cpu_and_gpu_share_the_same_false_track(self):
        run = self.publication_unit_run()
        result = oracles.null_control_checks(corpus.lane_frames(run))
        self.assertEqual(result["operationalFalseTrackFrames"], 0)
        self.assertEqual(result["falseDetections"], 0)
        self.assertTrue(result["syntheticOnly"])
        run["products"]["illuminator"][-1]["tracks"].update(n=1, data=[{"state": "ACTIVE"}])
        with self.assertRaisesRegex(AssertionError, "operational false track"):
            oracles.null_control_checks(corpus.lane_frames(run))

    def test_corpus_delay_window_fits_actual_correlation_block(self):
        contract = corpus.verify.required_scenarios("fp64")["resident-multilane-2"]
        variant = next(corpus.variants(contract, "fp64"))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = fixtures.configure(corpus.replay, variant, "cpu", root / "input.blah2iq",
                {"sampleRate": 400000, "frequencies": fixtures.frequencies(2), "dataSha256": "a" * 64}, root, 3000, {})
        geometry = config["process"]["ambiguity"]
        samples = int(fixtures.LANE_RATE * config["process"]["data"]["cpi"])
        rows = 2 * math.floor((geometry["dopplerMax"] - geometry["dopplerMin"]) /
                              (2 * fixtures.LANE_RATE / samples)) + 1
        self.assertLess(max(abs(geometry["delayMin"]), abs(geometry["delayMax"])), samples // rows)
        # The prior +/-1000 Hz window yielded 48-sample blocks, rejecting lag 59.
        self.assertGreaterEqual(geometry["delayMax"], samples // 41)

    def test_temporal_fixtures_have_feasible_disjoint_initiation_and_coast(self):
        contract = corpus.verify.required_scenarios("fp64")["coherent-tbd-4-8"]
        for depth in (4, 8):
            variant = next(value for value in corpus.variants(contract, "fp64") if value["window"] == depth)
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                config = fixtures.configure(corpus.replay, variant, "cpu", root / "input.blah2iq",
                    {"sampleRate": 400000, "frequencies": fixtures.frequencies(variant["lanes"]),
                     "dataSha256": "a" * 64}, root, 3000, {})
            tracker = config["process"]["tracker"]
            self.assertEqual(tracker["initiate"]["M"], 2)
            self.assertEqual(tracker["initiate"]["N"], 1 + depth)
            self.assertEqual(tracker["delete"], depth - 1)
            self.assertEqual(config["process"]["detection"]["pfa"], .001)

    def test_target_oracle_rejects_reversed_doppler_and_wrong_range_units(self):
        for sign in (-1, 1):
            truth = {"delayBins": 10, "dopplerHz": sign * 200}
            delay = 10 * fixtures.C / fixtures.LANE_RATE / 1000
            self.assertTrue(oracles.target_match(delay, sign * 200, truth))
            self.assertFalse(oracles.target_match(delay, -sign * 200, truth))
            self.assertFalse(oracles.target_match(delay * 1000, sign * 200, truth))

    def test_physical_delay_rate_matches_signed_doppler_and_lane_gates_are_disjoint(self):
        for motion in ("stationary", "integer", "fractional-delay"):
            targets = []
            for lane, frequency in enumerate(fixtures.frequencies(8)):
                start = fixtures.trajectory(lane, 10, motion, frequency)
                end = fixtures.trajectory(lane, 11, motion, frequency)
                recovered = -(end["delayBins"] - start["delayBins"]) * frequency / (fixtures.LANE_RATE * fixtures.CPI)
                self.assertAlmostEqual(recovered, start["dopplerHz"])
                half = fixtures.trajectory(lane, 10.5, motion, frequency)
                self.assertAlmostEqual(half["delayBins"], .5 * (start["delayBins"] + end["delayBins"]))
                targets.append(start)
            self.assertEqual({math.copysign(1, target["dopplerHz"]) for target in targets}, {-1, 1})
            for lane, target in enumerate(targets):
                for other_lane, other in enumerate(targets):
                    if lane == other_lane: continue
                    self.assertFalse(oracles.target_match(other["delayBins"] * fixtures.C / fixtures.LANE_RATE / 1000,
                                                          other["dopplerHz"], target))

    def test_bearing_publication_uses_same_lineage_annotation_as_radar_products(self):
        source = (Path(corpus.__file__).parents[2] / "src/blah2.cpp").read_text()
        publisher = source.split("const auto publishRadarFrame =", 1)[1].split(
            "// Finish first-use CPU/GPU qualification", 1)[0]
        self.assertIn("if (bearingEnabled && frame.residentProducts)", publisher)
        self.assertIn("socket_bearing->sendData(blah2::detection::annotateIntegration(", publisher)
        self.assertIn("blah2::analysis::annotate_native_product(std::move(bearing),", publisher)
        self.assertIn("native_product_identity(frame, publication)), frame.detectorIntegration)", publisher)
        self.assertEqual(source.count("socket_bearing->sendData("), 1)

    def test_bearing_oracle_rejects_wrong_lane_track_coordinates_and_lineage(self):
        run = self.publication_unit_run()
        truths = [fixtures.trajectory(lane, 0, "integer", frequency)
                  for lane, frequency in enumerate(fixtures.frequencies(2))]
        run["scene"] = {"samplesPerFrame": 20, "recipe": {"targetFrames": 1}, "truth": [truths]}
        lanes = {name: values[:1] for name, values in corpus.lane_frames(run).items()}
        for lane, frames in enumerate(lanes.values()):
            frame = frames[0]
            target = truths[lane]
            track = {"id": "0001", "state": "ACTIVE", "delay": target["delayBins"],
                     "doppler": target["dopplerHz"]}
            frame["tracks"]["data"] = [track]
            frame["bearing"] = {key: frame[key] for key in ("timestamp", "capture_epoch",
                "capture_first_input_sample", "capture_end_input_sample")}
            frame["bearing"]["data"] = [{**track, "current_bearing_deg_true": 90 - target["bearingLocalDeg"]}]
        self.assertEqual(set(oracles.bearing_result_checks(run, lanes)), set(lanes))
        mutations = [lambda row: row.update(current_bearing_deg_true=90 - truths[1]["bearingLocalDeg"]),
                     lambda row: row.update(id="unrelated"),
                     lambda row: row.update(delay=row["delay"] + 1),
                     lambda row: row.update(doppler=-row["doppler"])]
        for mutate in mutations:
            bad = copy.deepcopy(lanes)
            mutate(bad["corpus-0"][0]["bearing"]["data"][0])
            with self.assertRaises(AssertionError):
                oracles.bearing_result_checks(run, bad)
        bad = copy.deepcopy(lanes)
        bad["corpus-1"][0]["bearing"]["capture_epoch"] += 1
        with self.assertRaisesRegex(AssertionError, "unbound bearing lineage"):
            oracles.bearing_result_checks(run, bad)

    def temporal_unit_run(self):
        # Uniform maps admit only the obvious mean: the fourth CPI must retain
        # the old impulse through the four-CPI bank, with no two-CPI substitute.
        run = {"case": {"variant": {"window": 4, "motion": "integer"}},
               "config": {"process": {"tracker": {}}}, "observer": [],
               "scene": {"samplesPerFrame": 20, "truth": [[{"delayBins": 2, "dopplerHz": 100}]] * 32}}
        frames = []
        for index, (amplitude, expected_power) in enumerate(((4, 16), (1, 8.5), (1, 1), (1, 4.75))):
            first = 1000 + index * 20
            run["observer"].append({"stage": "products", "lane": "primary", "captureEpoch": 7,
                "captureFirst": first, "captureEnd": first + 20, "rows": 3, "columns": 5,
                "frequencyHz": 10000, "inputSamplesPerOutput": 1, "fused_stage": "pre_temporal",
                "product_backend": "cpu", "geometry": {"beam_count": 0}, "delay_bins": list(range(5)),
                "doppler_hz": [-100, 0, 100], "fused_amplitude": [amplitude] * 15, "fused_beam_id": None})
            level = 5 * math.log10(expected_power)
            frames.append({"capture_epoch": 7, "capture_first_input_sample": first,
                "capture_end_input_sample": first + 20,
                "map": {"nRows": 3, "nCols": 5, "noisePower": 0, "data": [[level] * 5 for _ in range(3)]}})
        return run, {"single": frames}

    def test_temporal_oracle_requires_actual_deep_result_and_complete_lineage(self):
        run, lanes = self.temporal_unit_run()
        result = oracles.temporal_result_checks(run, lanes)
        self.assertEqual(result["comparedFrames"], {"primary": 4})
        self.assertGreater(result["deepestBankContributions"]["primary"], 0)
        bad = copy.deepcopy(lanes)
        bad["single"][-1]["map"]["data"] = [[0.] * 5 for _ in range(3)]
        # Truth, readiness and frame metadata are unchanged; discarding the
        # actual four-CPI contribution must still fail acceptance.
        with self.assertRaisesRegex(AssertionError, "integrated temporal cell"):
            oracles.temporal_result_checks(run, bad)
        bad_run = copy.deepcopy(run)
        bad_run["observer"].pop()
        with self.assertRaisesRegex(AssertionError, "complete observer lineage"):
            oracles.temporal_result_checks(bad_run, lanes)

    def test_temporal_oracle_resets_on_gap_and_rejects_cross_epoch_power(self):
        for epoch, shift in ((8, 0), (7, 20)):
            run, lanes = self.temporal_unit_run()
            item, frame = run["observer"][-1], lanes["single"][-1]
            item["captureEpoch"] = frame["capture_epoch"] = epoch
            item["captureFirst"] += shift
            item["captureEnd"] += shift
            frame["capture_first_input_sample"] += shift
            frame["capture_end_input_sample"] += shift
            with self.assertRaisesRegex(AssertionError, "integrated temporal cell"):
                oracles.temporal_result_checks(run, lanes)

    def test_native_coherent_beam_ids_are_zero_based(self):
        class UnitChecks:
            expected = set()
        source = {"stage": "input", "lane": "primary", "captureEpoch": 7, "captureFirst": 1000}
        product = {**source, "stage": "products", "reference_weights": [[1, 0], [0, 0], [0, 0]],
            "caf": [[[1, 0]], [[0, 0]], [[0, 0]]], "frequencyHz": fixtures.CENTER,
            "geometry": {"positions_m": [[0, 0, 0]] * 3, "beam_count": 4,
                         "direct_path_azimuth_rad": 0, "direct_path_null_db": 0},
            "rows": 1, "columns": 1, "fused_amplitude": [1], "fused_beam_id": [0],
            "actual_gain_db": [20.] * 3, "channel_ids": [0, 1, 2]}
        run = {"case": {"variant": {"channels": 3, "fusion": "coherent"}, "target": False},
               "observer": [source, product], "calibrations": {"unitFixtureOnly": True}}
        for beam in (0, 3):
            product["fused_beam_id"] = [beam]
            oracles.coherent_checks(run, {}, UnitChecks(), {"unitFixtureOnly": True})
        for beam in (-1, 4, .5):
            product["fused_beam_id"] = [beam]
            with self.assertRaises(AssertionError):
                oracles.coherent_checks(run, {}, UnitChecks(), {"unitFixtureOnly": True})

    def test_plan_expands_all_required_scenarios_without_device_access(self):
        for feature in ("fp32", "fp64"):
            result = subprocess.run([sys.executable, str(Path(corpus.__file__)), "--plan",
                                     "--device-class", feature], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            plan = json.loads(result.stdout)
            required = corpus.verify.required_scenarios(feature)
            self.assertEqual(set(plan["scenarios"]), set(required))
            for name, cases in plan["scenarios"].items():
                self.assertTrue(cases, name)
                self.assertEqual({case["target"] for case in cases}, {True, False})
                if feature == "fp32":
                    self.assertFalse(any(case["mode"] == "gpu-resident" for case in cases))

    def test_resident_fault_statistics_are_reachable_variants(self):
        contract = corpus.verify.required_scenarios("fp64")["failure-injection-resident"]
        variants = list(corpus.variants(contract, "fp64"))
        cases = list(corpus.planned_cases(contract, "fp64"))
        self.assertEqual({variant["statistic"] for variant in variants}, {"power", "resident-only-coherent"})
        for case in cases:
            self.assertIn(case["variant"], variants)
        for statistic in ("power", "resident-only-coherent"):
            self.assertEqual({case["fault"] for case in cases if case.get("fault") and
                              case["variant"]["statistic"] == statistic},
                             set(contract["settings"]["faults"]) - {"input-gap"})
        self.assertFalse(any(case.get("fault") in ("prepare", "commit") and case["mode"] == "auto"
                             and case["variant"]["lanes"] == 1 for case in cases))

    def test_coherent_temporal_motion_and_channel_matrix(self):
        required = corpus.verify.required_scenarios("fp64")
        arrays = list(corpus.variants(required["coherent-array-manifold-bearing"], "fp64"))
        self.assertEqual({value["channels"] for value in arrays}, {3, 5, 8})
        temporal = list(corpus.variants(required["coherent-tbd-4-8"], "fp64"))
        self.assertEqual({value["window"] for value in temporal}, {4, 8})
        self.assertEqual({value["motion"] for value in temporal}, {"integer", "fractional-delay"})
        self.assertEqual({value["fusion"] for value in temporal}, {"coherent", "noise-whitened"})

    def test_cpu_or_empty_observation_cannot_complete_receipt(self):
        contract = next(iter(corpus.verify.required_scenarios("fp32").values()))
        with tempfile.TemporaryDirectory() as temporary:
            checks = corpus.Checks(contract, corpus.Evidence(Path(temporary)))
            with self.assertRaises(AssertionError): checks.finish()
            with self.assertRaises(AssertionError): checks.record("numerical-oracle", {})
            checks.record("numerical-oracle", {"unitFixtureOnly": True})
            with self.assertRaises(AssertionError): checks.finish()

    def test_retained_evidence_validates_transitive_measured_payloads(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            evidence = corpus.Evidence(root / "gpu-feature-evidence")
            raw = evidence.put({"unitFixtureOnly": True, "complexSample": [1., 0.]})
            manifest = evidence.put({"raw": raw})
            receipt = {"scenarios": [{"executionSha256": manifest, "productsSha256": manifest,
                "oracleSha256": manifest, "checks": {"unit": {"evidenceSha256": manifest}}}]}
            path = root / "unit-only.json"
            corpus.verify.validate_evidence_files(receipt, path)
            (evidence.directory / f"{raw}.json").unlink()
            with self.assertRaisesRegex(AssertionError, "missing retained"):
                corpus.verify.validate_evidence_files(receipt, path)

    def test_complex_numeric_oracle_rejects_phase_error_and_nonfinite(self):
        self.assertEqual(corpus.complex_error([[1., 0.]], [[1., 0.]])["relativeRms"], 0)
        for values in ([[0., 1.]], [[float("nan"), 0.]], []):
            with self.assertRaises(AssertionError): corpus.complex_error([[1., 0.]], values)

    def test_independent_whitening_excites_unequal_noise(self):
        scales = oracles.whitening_scales([[1 + 0j, 0j, 0j], [0j, 4 + 0j, 0j],
                                           [0j, 0j, 9 + 0j]], [1 + 0j, 0j, 0j], .001)
        self.assertGreater(max(scales) - min(scales), 1)
        self.assertTrue(all(.5 <= value <= 2 for value in scales))

    def test_aggregate_allocation_requires_real_unique_bounded_session_proofs(self):
        # Tiny data-only predicate fixture; not execution evidence or receipt.
        run = {"case": {"variant": {"lanes": 8}}, "observer": []}
        for frame in range(corpus.PUBLISHED_FRAMES):
            for lane in range(8):
                run["observer"].append({"stage": "products", "lane": str(lane),
                    "captureEpoch": 1, "captureFirst": frame, "captureEnd": frame + 1,
                    "resident_allocation": {"session_id": f"{lane + 1:032x}", "limit_bytes": 90,
                        "used_bytes": 60, "peak_bytes": 70, "live_allocations": 2,
                        "shared_reservation_bytes": 10, "aggregate_cap_bytes": 800,
                        "session_reservation_bytes": 100, "aggregate_session_cap": 8}})
        class UnitChecks:
            expected = {"aggregate-memory-bound"}
            def record(self, name, value): self.observation = (name, value)
        checks = UnitChecks()
        oracles.allocation_checks(run, checks, {"unitFixtureOnly": True})
        self.assertEqual(len(checks.observation[1]["allocations"]), corpus.PUBLISHED_FRAMES)
        for field, value in (("peak_bytes", 100), ("session_id", "0" * 31 + "2"),
                             ("aggregate_cap_bytes", None)):
            bad = copy.deepcopy(run)
            bad["observer"][0]["resident_allocation"][field] = value
            with self.assertRaises(AssertionError): oracles.allocation_checks(bad, checks, {"unitFixtureOnly": True})

    def test_tiny_fixture_is_deterministic_completed_and_gain_bound(self):
        variant = next(corpus.variants(corpus.verify.required_scenarios("fp64")["resident-multilane-2"], "fp64"))
        recipe = fixtures.scene_recipe(variant)
        recipe.update(epochFrames=2, targetFrames=1, laneRate=1000)
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary) / name for name in ("one.blah2iq", "two.blah2iq")]
            scenes = [fixtures.write_scene(path, recipe) for path in paths]
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())
            self.assertEqual(scenes[0], scenes[1])
            self.assertEqual(scenes[0]["lineage"]["perChannelGainDb"], [20., 20.])
            self.assertEqual(paths[0].read_bytes()[48:56], b"\x01" + b"\x00" * 7)

    def test_array_lane_config_preserves_recorded_array_topology(self):
        contract = corpus.verify.required_scenarios("fp64")["coherent-array-manifold-bearing"]
        variant = next(value for value in corpus.variants(contract, "fp64") if value["lanes"] == 2)
        scene = {"sampleRate": 400000, "frequencies": fixtures.frequencies(2), "dataSha256": "a" * 64}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            value = fixtures.configure(corpus.replay, variant, "gpu-resident", root / "input.blah2iq",
                                       scene, root, 3000, {})
            qualification = value["capture"]["replay"]["qualification"]
            self.assertEqual(value["capture"]["replay"]["recording_sha256"], scene["dataSha256"])
            self.assertEqual(qualification["confirmation"], fixtures.CONFIRMATION)
            self.assertEqual(qualification["observer"]["max_frames"], 64)
            for lane in value["process"]["multi_illuminator"]["illuminators"]:
                self.assertNotIn("input_role_mode", lane)
                self.assertTrue(Path(lane["bearing_manifold_file"]).is_file())

    def test_recorded_sites_use_production_reader_and_external_pins(self):
        inspector = Path(os.environ.get("VECTORWARP_TEST_RECORDING_INSPECTOR",
            Path(__file__).resolve().parents[2] / "bin" / "inspectRecordingProvenance"))
        self.assertTrue(inspector.is_file(), "Build inspectRecordingProvenance before corpus contract tests")
        variant = next(corpus.variants(corpus.verify.required_scenarios("fp64")["coherent-array-manifold-bearing"], "fp64"))
        recipe = fixtures.scene_recipe(variant)
        recipe.update(epochFrames=2, targetFrames=1, laneRate=1000)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "array.blah2iq"
            scene = fixtures.write_scene(path, recipe)
            def inspect(file, anchor):
                return subprocess.run([str(inspector), str(file), "blah2", str(recipe["channels"]),
                    str(scene["sampleRate"]), str(fixtures.CENTER), anchor, "", "0"],
                    capture_output=True, text=True, timeout=5)
            checked = inspect(path, scene["dataSha256"])
            self.assertEqual(checked.returncode, 0, checked.stderr)
            proof = json.loads(checked.stdout)
            self.assertTrue(proof["geographic_sites_available"])
            self.assertFalse(proof["exact_acquisition"], "Synthetic fixtures must not fabricate hardware gain readback")
            self.assertFalse(json.loads(inspect(path, "").stdout)["geographic_sites_available"])
            raw = path.read_bytes()
            length = struct.unpack_from("<I", raw, 32)[0]
            def changed(name, metadata):
                encoded = fixtures.canonical(metadata)
                header = bytearray(raw[:128]); struct.pack_into("<I", header, 32, len(encoded))
                header[64:128] = hashlib.sha256(encoded).hexdigest().encode()
                file = path.with_name(name)
                file.write_bytes(header + encoded + raw[128 + length:])
                return file, hashlib.sha256(file.read_bytes()).hexdigest()
            missing = copy.deepcopy(scene["acquisition"])
            del missing["geographicSites"]; del missing["geographicSitesSha256"]
            file, digest = changed("missing.blah2iq", missing)
            self.assertFalse(json.loads(inspect(file, digest).stdout)["geographic_sites_available"])
            tampered = copy.deepcopy(scene["acquisition"])
            tampered["geographicSites"]["rx"]["latitude"] = 42
            file, digest = changed("tampered.blah2iq", tampered)
            self.assertNotEqual(inspect(file, digest).returncode, 0)
            tampered["geographicSitesSha256"] = fixtures.fingerprint(tampered["geographicSites"])
            file, _ = changed("rehashed.blah2iq", tampered)
            self.assertNotEqual(inspect(file, scene["dataSha256"]).returncode, 0)

    def test_manifold_bad_order_is_real_identity_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            good = fixtures.write_manifold(Path(temporary) / "good.json", 3, fixtures.CENTER, fixtures.LANE_RATE)
            bad = fixtures.write_manifold(Path(temporary) / "bad.json", 3, fixtures.CENTER, fixtures.LANE_RATE, bad_order=True)
            self.assertNotEqual(good["identity"]["channel_mapping"], bad["identity"]["channel_mapping"])
            self.assertEqual(good["identity"]["compatibility_fingerprint"], bad["identity"]["compatibility_fingerprint"])

    def test_optimization_cannot_remove_predicates(self):
        result = subprocess.run([sys.executable, "-O", str(Path(corpus.__file__)), "--help"],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertions disabled", result.stderr)


if __name__ == "__main__":
    unittest.main()
