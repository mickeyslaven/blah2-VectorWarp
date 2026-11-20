"""Data-only producer regressions. No fixture is physical qualification."""
import copy
import hashlib
import json
import struct
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import recorded_iq_qualification as measured


def fixture():
    plan = {"sourceSha": "a"*40, "artifacts": {"blah2": "b"*64, "blah2-gpu-vulkan.so": "c"*64}}
    acquisition = {"schema": "vectorwarp-recording-inspection-v1", "recording_sha256": "d"*64,
        "acquisition_sha256": "e"*64, "epoch": "7", "sample_rate_hz": 1000,
        "exact_acquisition": True, "acquisition_hash_trusted": True,
        "acquisition_integrity_valid": True, "acquisition_capable": True}
    pins = {"require_exact": True, "recording_sha256": "d"*64, "acquisition_sha256": "e"*64}
    module = {"path": "/unit-fixture/blah2-gpu-vulkan.so", "sha256": "c"*64}
    device = {"id": "fixture", "name": "fixture", "shaderFloat64": False}
    run = {**plan, "deviceClass": "fp32", "device": device, "acquisition": acquisition,
        "returncode": 0, "truthUsedForInference": False,
        "artifactBinding": {"nonce": "fixture", "artifacts": {"processor": {
            "path": "/unit-fixture/blah2", "sha256": "b"*64}, "portable": module}},
        "artifactReceipt": {"passed": True, "workers": [123], "loaded": [{"nonce": "fixture",
            "role": "portable", "module": module, "driverStatus": {"devices": [device]}}]},
        "config": {"capture": {"replay": {"state": True, "loop": False, "qualification": {
            "enabled": True, "confirmation": "offline-physical-gpu-qualification-v1", "acquisition": pins}}},
            "process": {"performance": {"acceleration": "gpu"}}},
        "statuses": [{"state": "complete", "captureDroppedSamples": [0, 0]}],
        "observer": [], "events": [], "products": {name: [] for name in
            ("map", "detection", "track", "timestamp", "timing", "bearing", "illuminator")}}
    for index in range(2):
        epoch, first, end = 7, index*1000, (index+1)*1000
        public = {"capture_epoch": epoch, "capture_first_input_sample": first,
                  "capture_end_input_sample": end, "timestamp": index}
        execution = {"active": "vulkan", "state": "ready", "gpuExecuted": True,
                     "cpuExecuted": False, "device": "fixture"}
        run["products"]["map"].append(dict(public))
        run["products"]["detection"].append({**public, "delay": [], "doppler": []})
        run["products"]["track"].append({**public, "n": 0, "data": []})
        run["products"]["timestamp"].append({"timestamp": index})
        run["products"]["timing"].append({"timestamp": index, "captureEpoch": epoch,
            "captureFirstEpochSample": first, "captureEndEpochSample": end,
            "acceleration": execution, "clutterAcceleration": execution})
        run["events"].append({"boundary": "publication-complete", "captureEpoch": epoch,
            "captureFirstEpochSample": first, "captureEndEpochSample": end})
        observed = {"schema": "vectorwarp-coherent-observer-v1", "lane": "single",
            "captureEpoch": epoch, "captureFirst": first, "captureEnd": end,
            "sampleRateHz": 1000, "inputSamplesPerOutput": 1}
        run["observer"].extend([{**observed, "stage": "products"}, {**observed, "stage": "detector",
            "measurement": "production-cfar-pre-centroid-v1", "enabled": True, "ready": True,
            "evaluated_cells": 100, "accepted_cells": 1, "nonfinite_cells": 0},
            {**observed, "stage": "clutter", "filter_policy": "refit-weights",
             "gpu_executed": True, "cpu_executed": False, "channel_ids": [1],
             "reference_channel_ids": [0], "delay_min": 0, "delay_max_exclusive": 1,
             "relative_diagonal_loading": 0, "unfiltered_surveillance": [[[0, 0]]],
             "filtered_surveillance": [[[0, 0]]]}])
    baseline = copy.deepcopy(run)
    baseline["deviceClass"] = "cpu"
    baseline["config"]["process"]["performance"]["acceleration"] = "cpu"
    for timing in baseline["products"]["timing"]:
        timing["acceleration"] = {"active": "cpu"}
    window = {"firstSample": 0, "endSample": 2000, "passId": "whole-pass-unit-fixture"}
    adjudication = {"schema": "vectorwarp-recorded-negative-adjudication-v1", "reviewed": True,
        "coverageComplete": True, "wholePassDisjoint": True, "noTargets": True,
        "recordingSha256": "d"*64, "acquisitionSha256": "e"*64, "captureEpoch": 7, **window}
    return plan, run, baseline, window, adjudication


def golden_fixture():
    plan, run, baseline, _, _ = fixture()
    native = {"schema": "vectorwarp-resolved-products-v1", "stages.clutter": True,
        "stages.detection": True, "stages.tracking": True, "trackerM": 2, "trackerN": 9,
        "trackerDelete": 7, "integrationCpis": 8, "integrationMotion": 1, "pfa": 1e-5}
    for stream in (run, baseline):
        for row in stream["observer"]:
            row.update(resolved_product_plan=json.dumps(native), frequencyHz=100000000,
                channel_ids=[1], reference_channel_ids=[0], rows=1, columns=2,
                delay_bins=[0, 1], doppler_hz=[100],
                geometry={"fusion_mode": "noncoherent", "positions_m": [],
                    "detection_manifold_file": "", "bearing_manifold_file": ""},
                reference_config={"analysis_samples": 1000})
        for frame in stream["products"]["track"]:
            frame.update(n=1, data=[{"id": "0001", "state": "ACTIVE", "delay": 1.2,
                "doppler": 100, "n": 3, "measured": True, "associated_delay": [1.1, 1.2]}])
        for frame in stream["products"]["detection"]:
            frame.update(delay=[1], doppler=[100])
    for frame in baseline["products"]["timing"]:
        for stage in ("acceleration", "clutterAcceleration"):
            frame[stage] = {"active": "cpu", "cpuExecuted": True, "gpuExecuted": False}
    identity = measured.corpus.resolved_plan_identity(run, run["observer"][0])
    pair = {"id": "fixture-pair", "baseline": "cpu", "candidate": "gpu",
        "planSha256ByLane": {"single": identity}, "requiredPlanFields": {
            "integrationCpis": 8, "trackerM": 2, "trackerN": 9, "trackerDelete": 7},
        "requiredTrackStates": ["ACTIVE"], "minimumCounts": {"detections": 1, "operationalTrackFrames": 1}}
    return plan, run, baseline, pair


def checkpoint_fixture(mode="fixed", kind="fp32", doppler=100):
    """Fabricated data contract only; never execution/physical evidence."""
    plan, control, _, pair, _ = fixture()
    declared = {"mode": mode, "path": "/unit-fixture/weights", "source_sha": plan["sourceSha"],
        "warm_frame": 1, "epoch": 7, "first_sample": 0, "warm_first_sample": 0, "warm_end_sample": 1000,
        "injection_frame": 2, "injection_first_sample": 1000, "injected": False}
    body = b"VWCLUTTER1" + b"a"*64 + b"b"*64 + struct.pack("<5Q3Ii", 1, 7, 0, 0, 1000, 1000, 1, 1, 0)
    body += struct.pack("<2d2f", 1, 0, .01, 0)
    declared["sha256"] = hashlib.sha256(body).hexdigest()
    control["deviceClass"] = kind;control["device"]["shaderFloat64"] = kind == "fp64"
    control["artifactReceipt"]["loaded"][0]["driverStatus"]["devices"][0]["shaderFloat64"] = kind == "fp64"
    control["config"]["capture"].update(fs=1000)
    control["config"]["capture"]["replay"].update(format="blah2",
        inject_echo={"state": True, "delay_km": 1, "doppler_hz": doppler})
    control["config"]["capture"]["replay"]["qualification"]["clutter_checkpoint"] = declared
    control["config"]["process"].update(data={"cpi": 1}, clutter={"enable": True})
    control["clutterCheckpoint"] = {"sha256": declared["sha256"], "bytesHex": body.hex()}
    pair.update(firstSample=1000, checkpointSha256=declared["sha256"], target={"lane": "single", "delayKm": 1,
        "dopplerHz": doppler, "delayToleranceKm": .1, "dopplerToleranceHz": 1})
    for row in list(control["observer"]):
        index = row["captureFirst"]//1000+1
        if row["stage"] != "clutter": continue
        fixed = mode == "fixed" and index == 2
        text = f"{index} 7 {row['captureFirst']} {row['captureEnd']} {int(fixed)} {declared['sha256']} {'b'*64} "
        text += "0 0 0" if fixed else "1 1 2"
        row.update(checkpoint_proof=text, clutter_backend="vulkan", reference_iq=[[1,0]]*1000,
            filter_policy="fixed-weights" if fixed else "refit-weights",
            unfiltered_surveillance=[[[0,0]]*1000], filtered_surveillance=[[[0,0]]*1000])
        control["events"].append({"boundary": "clutter-checkpoint", "route": "portable", "lane": 0,
            "workerPid": 123, "frame": index, "captureEpoch": 7, "captureFirstEpochSample": row["captureFirst"],
            "captureEndEpochSample": row["captureEnd"], "clutterCheckpoint": text})
        control["observer"].append({**row, "stage": "input", "channelized_iq": [[[0,0]]*1000]})
    injected = copy.deepcopy(control)
    injected["config"]["capture"]["replay"]["qualification"]["clutter_checkpoint"]["injected"] = True
    for row in injected["observer"]:
        if row["captureFirst"] == 1000 and row["stage"] == "clutter":
            row.update(unfiltered_surveillance=[[[1,0]]*1000], filtered_surveillance=[[[.9,0]]*1000])
    injected["products"]["detection"][1].update(delay=[1], doppler=[doppler])
    injected["products"]["track"][1].update(n=1, data=[{"id": 3, "state": "ACTIVE",
        "delay": 1000000/299792458, "doppler": doppler}])
    return plan, control, injected, pair


def policy_fixture(root, minimum=2):
    identity, template, _, _ = golden_fixture()
    plan = {**identity, "schema": "vectorwarp-recorded-policy-comparison-plan-v1", "phase": "development",
        "runs": {}, "runPolicies": {}, "matchingGates": {"delayKm": .1, "dopplerHz": 2},
        "policyPairs": [{"id": "paired", "baseline": "depth1", "candidate": "depth8"}]}
    def pin(name, value):
        body = measured.canonical(value); (root / name).write_bytes(body)
        return {"path": name, "sha256": hashlib.sha256(body).hexdigest()}
    labels = {"schema": "vectorwarp-policy-labels-v1", "source": "receiver-A", "partition": "fit",
        "recordingSha256": "d"*64, "acquisitionSha256": "e"*64, "captureEpoch": 7,
        "timingQualified": True, "passes": [{"id": "pass-1", "lane": "single", "entrySample": 0,
            "exitSample": 20000, "wholePass": True}], "frames": []}
    runs = {}
    for depth in (1, 8):
        run = copy.deepcopy(template)
        run["config"]["capture"]["replay"].update(file="/retained/recording")
        run["config"]["capture"]["replay"]["qualification"]["observer"] = {"directory": f"/private/{depth}/observer"}
        run["config"].update(network={"ip": "127.0.0.1", "ports": {"api": 3000+depth}}, save={"path": f"/private/{depth}"})
        run["config"]["process"].update(detection={"enable": True, "integration_cpis": 1, "integration_motion_bins": 0,
            "track_before_detect": {"enable": depth == 8, "max_cpis": 8, "motion_bins": 1,
                "delay_motion_bins": 1, "doppler_motion_bins": 1, "fractional_delay": True}},
            tracker={"enable": True, "initiate": {"M": minimum, "N": 1+8*(minimum-1)}, "delete": 7})
        native = json.loads(template["observer"][0]["resolved_product_plan"])
        native.update(integrationCpis=depth, integrationMotion=0 if depth == 1 else 1,
            trackerM=minimum, trackerN=1+8*(minimum-1))
        run["products"] = {key: [] for key in template["products"]};run["observer"] = [];run["events"] = []
        for index in range(20):
            for name in ("map", "detection", "track", "timestamp", "timing"):
                row = copy.deepcopy(template["products"][name][0]);row["timestamp"] = index
                if name in ("map", "detection", "track"):
                    row.update(capture_first_input_sample=index*1000, capture_end_input_sample=(index+1)*1000)
                if name == "detection":
                    row.update(delay=[1.] if index in (1, 2, 14, 15) else [], doppler=[100.] if index in (1, 2, 14, 15) else [])
                if name == "track":
                    tracks = []
                    if index in (1, 2) or 3 <= index <= 9:
                        tracks = [{"id": "A", "state": "ACTIVE" if index < 3 else "COASTING", "delay": 1000000/299792458,
                                   "doppler": 100., "measured": index < 3}]
                    if 11 <= index <= 13:
                        tracks = [{"id": "U", "state": "ACTIVE" if index < 13 else "COASTING", "delay": 10000000/299792458,
                                   "doppler": 100., "measured": index < 13}]
                    if index >= 14:
                        tracks = [{"id": "B", "state": "ACTIVE" if index < 16 else "COASTING", "delay": 1000000/299792458,
                                   "doppler": 100., "measured": index < 16}]
                    row.update(n=len(tracks)+(index == 0), nTentative=int(index == 0), data=tracks)
                if name == "timing":
                    row.update(captureFirstEpochSample=index*1000, captureEndEpochSample=(index+1)*1000,
                        trackerDiagnostics={"inactivity_deleted": int(index in (10, 14)), "tentative_expired": 0})
                run["products"][name].append(row)
            event = copy.deepcopy(template["events"][0]);event.update(captureFirstEpochSample=index*1000, captureEndEpochSample=(index+1)*1000)
            run["events"].append(event)
            for original in template["observer"][:3]:
                row = copy.deepcopy(original);row.update(captureFirst=index*1000, captureEnd=(index+1)*1000,
                    resolved_product_plan=json.dumps(native));run["observer"].append(row)
            if depth == 1:
                labels["frames"].append({"lane": "single", "firstSample": index*1000, "endSample": (index+1)*1000,
                    "exposureValid": True, "truthEligible": True, "targets": [{"passId": "pass-1", "delayKm": 1., "dopplerHz": 100.}]})
        name = f"depth{depth}";runs[name] = run
    cohort = {"schema": "vectorwarp-policy-cohort-v1", "reviewed": True,
        "basis": [pin("inventory.json", {"retained": "independent unit fixture, never RF evidence"})],
        "recordings": [{"recordingSha256": "d"*64, "source": "receiver-A", "partition": "fit", "passIds": ["pass-1"], "wholePass": True}]}
    def refresh():
        for name, run in runs.items():
            run["configSha256"] = hashlib.sha256(measured.canonical(run["config"])).hexdigest()
            plan["runs"][name] = pin(name + ".json", run)
            plan["runPolicies"][name] = {"configSha256": run["configSha256"], "resolvedConfigSha256": run["configSha256"],
                "harnessBindings": {field: measured.field_at(run["config"], field) for field in measured.HARNESS_FIELDS},
                "fields": {field: measured.field_at(run["config"]["process"], field) for field in measured.POLICY_FIELDS},
                "planSha256ByLane": {"single": measured.corpus.resolved_plan_identity(run, run["observer"][0])}}
        plan["cohortManifest"] = pin("cohort.json", cohort)
        plan["policyPairs"][0]["labels"] = pin("labels.json", labels)
    refresh()
    return plan, runs, labels, cohort, refresh, pin


class MeasuredQualificationTest(unittest.TestCase):
    def test_policy_comparison_measures_exposure_latency_splits_coast_and_deletion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, _, _, _, _, _ = policy_fixture(root)
            report = measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
            actual = report["pairs"][0]["candidate"];lane = actual["lanes"]["single"];passed = actual["passes"][0]
            self.assertEqual((lane["admittedCpis"], lane["eligibleCpis"], lane["exposureSeconds"]), (20, 20, 20))
            self.assertEqual((passed["detectionCpis"], passed["activeCpis"], passed["coastedCpis"]), (4, 4, 11))
            self.assertEqual((passed["splits"], passed["coverageGaps"], passed["longestCoveredCpis"]), (1, 1, 9))
            self.assertEqual((passed["firstActiveFromEntrySeconds"], passed["firstActiveFromMeasuredEvidenceSeconds"]), (2, 0))
            self.assertEqual((lane["unmatchedActiveBirths"], lane["unmatchedActiveTrackSeconds"], lane["unmatchedCoastedTrackSeconds"]), (1, 2, 1))
            self.assertEqual(lane["unmatchedActiveBirthsPerLabelledHour"], 180)
            self.assertEqual(lane["deleteBoundaryViolations"], 1)
            removed = [row for row in actual["lifecycle"] if row["event"] == "removed"]
            self.assertEqual([(row["id"], row["missedCpis"], row["configuredDeleteBoundary"]) for row in removed], [("A", 8, True), ("U", 2, False)])
            self.assertIsNone(actual["falseTracks"]);self.assertFalse(report["releaseQualified"])
            self.assertIsNone(report["policySelected"])
            self.assertTrue((root/"result/policy-comparison.json").is_file())
            with self.assertRaises(FileExistsError):
                measured.produce_policy_comparison(plan, root, root/"result", "9"*64)

    def test_policy_grid_allows_only_declared_cohorts_and_preserves_golden(self):
        for minimum in (2, 3, 4):
            with self.subTest(minimum=minimum), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary);plan, runs, _, _, _, _ = policy_fixture(root, minimum)
                report = measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
                self.assertEqual(report["pairs"][0]["policy"]["N"], 1+8*(minimum-1))
                lanes, _, _, policy = measured.policy_run(runs["depth8"], plan["runPolicies"]["depth8"], plan)
                self.assertEqual(policy["depth"], 8);self.assertEqual(len(lanes["single"]), 20)

    def test_policy_refuses_rehashed_nonpolicy_config_native_or_device_drift(self):
        mutations = [lambda run: run["config"]["process"].update(clutter={"loading": .3}),
            lambda run: run["config"].update(location={"rx": {"latitude": 1}}),
            lambda run: run["config"]["process"]["performance"].update(acceleration="auto"),
            lambda run: run["config"]["process"]["tracker"]["initiate"].update(M=4, N=6),
            lambda run: run["config"]["process"]["detection"].update(integration_cpis=2),
            lambda run: run.update(truthUsedForInference=True)]
        for mutate in mutations:
            with self.subTest(mutation=mutate), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary);plan, runs, _, _, refresh, _ = policy_fixture(root)
                mutate(runs["depth8"]);refresh()
                with self.assertRaises((AssertionError, ValueError)):
                    measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, runs, _, _, refresh, _ = policy_fixture(root)
            for row in runs["depth8"]["observer"]:
                native = json.loads(row["resolved_product_plan"]);native["pfa"] = .01;row["resolved_product_plan"] = json.dumps(native)
            refresh()
            with self.assertRaisesRegex(ValueError, "non-policy drift"):
                measured.produce_policy_comparison(plan, root, root/"result", "9"*64)

    def test_policy_grid_refuses_drift_hidden_in_both_arms_of_another_cohort(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, runs, _, _, refresh, _ = policy_fixture(root)
            for name, run in list(runs.items()):
                other = copy.deepcopy(run)
                other["config"]["process"]["tracker"]["initiate"].update(M=3, N=17)
                for row in other["observer"]:
                    native = json.loads(row["resolved_product_plan"]);native.update(trackerM=3, trackerN=17)
                    row["resolved_product_plan"] = json.dumps(native)
                runs[name + "-m3"] = other
            plan["policyPairs"].append({**plan["policyPairs"][0], "id": "m3", "baseline": "depth1-m3", "candidate": "depth8-m3"})
            refresh()
            self.assertEqual(len(measured.produce_policy_comparison(plan, root, root/"good", "9"*64)["pairs"]), 2)
            for name in ("depth1-m3", "depth8-m3"):
                for row in runs[name]["observer"]:
                    native = json.loads(row["resolved_product_plan"]);native["pfa"] = .01
                    row["resolved_product_plan"] = json.dumps(native)
            refresh()
            with self.assertRaisesRegex(ValueError, "cross-cohort non-policy drift"):
                measured.produce_policy_comparison(plan, root, root/"bad", "9"*64)

    def test_policy_exact_pins_missing_intervals_and_population_are_not_optional(self):
        for defect in ("run-pin", "config-pin", "plan-pin", "artifact", "labels", "population", "prediction", "identity-reuse", "control-motion"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary);plan, runs, labels, _, refresh, _ = policy_fixture(root)
                if defect == "labels": labels["frames"].pop()
                if defect == "population": runs["depth8"]["products"]["track"][0]["n"] = 999
                if defect == "prediction": runs["depth8"]["products"]["track"][3]["data"][0]["measured"] = True
                if defect == "identity-reuse": runs["depth8"]["products"]["track"][14]["data"][0]["id"] = "A"
                if defect == "control-motion":
                    for row in runs["depth1"]["observer"]:
                        native = json.loads(row["resolved_product_plan"]);native["integrationMotion"] = 1
                        row["resolved_product_plan"] = json.dumps(native)
                if defect == "artifact": runs["depth8"]["artifacts"]["blah2"] = "f"*64
                refresh()
                if defect == "run-pin": plan["runs"]["depth8"]["sha256"] = "f"*64
                if defect == "config-pin": plan["runPolicies"]["depth8"]["configSha256"] = "f"*64
                if defect == "plan-pin": plan["runPolicies"]["depth8"]["planSha256ByLane"]["single"] = "f"*64
                with self.assertRaises((AssertionError, ValueError)):
                    measured.produce_policy_comparison(plan, root, root/"result", "9"*64)

    def test_policy_truth_gaps_and_short_clips_are_censored_not_misses(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, runs, labels, cohort, refresh, _ = policy_fixture(root, 4)
            labels["frames"][5].update(truthEligible=False, targets=[])
            labels["passes"][0]["wholePass"] = False;cohort["recordings"][0]["wholePass"] = False
            for run in runs.values():
                for frame in run["products"]["track"]: frame.update(n=0, nTentative=0, data=[])
            refresh();report = measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
            actual = report["pairs"][0]["candidate"];passed = actual["passes"][0]
            self.assertEqual(actual["lanes"]["single"]["eligibleCpis"], 19)
            self.assertEqual(passed["confirmationOutcome"], "censored")
            self.assertIsNone(passed["wholePassDetected"]);self.assertIsNone(actual["passActiveRate"])
            self.assertEqual(passed["unscorableCpis"], 1)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, _, labels, _, refresh, _ = policy_fixture(root)
            labels["frames"][5]["exposureValid"] = False
            refresh();report = measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
            passed = report["pairs"][0]["candidate"]["passes"][0]
            self.assertEqual(passed["unscorableCpis"], 1)
            self.assertIsNone(passed["wholePassDetected"])

    def test_policy_partitions_holdout_and_duplicate_exposure_fail_closed(self):
        for defect in ("reserved", "lineage", "pass-overlap", "duplicate-pair", "timing"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary);plan, _, labels, cohort, refresh, _ = policy_fixture(root)
                if defect in ("reserved", "lineage"):
                    partition = "reserved_holdout" if defect == "reserved" else "lineage_review"
                    cohort["recordings"][0]["partition"] = partition;labels["partition"] = partition
                if defect == "pass-overlap":
                    cohort["recordings"].append({**cohort["recordings"][0], "recordingSha256": "f"*64, "partition": "reserved_holdout"})
                if defect == "duplicate-pair":
                    plan["policyPairs"].append({**plan["policyPairs"][0], "id": "duplicate"})
                if defect == "timing": labels["timingQualified"] = False
                refresh()
                with self.assertRaises(ValueError): measured.produce_policy_comparison(plan, root, root/"result", "9"*64)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan, _, labels, cohort, refresh, pin = policy_fixture(root)
            plan["phase"] = "holdout";labels["partition"] = cohort["recordings"][0]["partition"] = "reserved_holdout";refresh()
            plan["selectionReceipt"] = pin("selection.json", {"schema": "vectorwarp-frozen-policy-selection-v1", "frozen": True,
                "sourceSha": plan["sourceSha"], "artifacts": plan["artifacts"], "policy": {"depth": 8, "M": 2, "N": 9, "delete": 7},
                "policyFields": plan["runPolicies"]["depth8"]["fields"]})
            plan["holdoutAudit"] = pin("audit.json", {"schema": "vectorwarp-holdout-exposure-audit-v1", "untouched": True,
                "cohortManifestSha256": plan["cohortManifest"]["sha256"], "passIds": ["pass-1"]})
            self.assertFalse(measured.produce_policy_comparison(plan, root, root/"good", "9"*64)["releaseQualified"])
            selection = measured.pinned(plan["selectionReceipt"], root)
            selection["policyFields"]["detection.track_before_detect.fractional_delay"] = False
            plan["selectionReceipt"] = pin("changed-selection.json", selection)
            with self.assertRaisesRegex(ValueError, "changed after selection"):
                measured.produce_policy_comparison(plan, root, root/"changed-policy", "9"*64)
            plan["holdoutAudit"] = pin("audit-bad.json", {"schema": "vectorwarp-holdout-exposure-audit-v1", "untouched": False})
            with self.assertRaisesRegex(ValueError, "Uncertified"):
                measured.produce_policy_comparison(plan, root, root/"bad", "9"*64)

    def test_golden_checks_physical_products_exact_policy_and_full_track_lifecycle(self):
        plan, run, baseline, pair = golden_fixture()
        result = measured.golden_pair(baseline, run, pair, plan)
        self.assertEqual(result["counts"], {"frames": 2, "detections": 2, "operationalTrackFrames": 2})
        self.assertEqual(result["lifecycle"][0]["event"], "first-publication")
        self.assertEqual(result["trackerDelayUnit"], "km")
        for field, value in (("delay", 1.23), ("doppler", 100.03), ("id", "0002"),
                             ("state", "COASTING"), ("associated_delay", [1.1, 1.23]), ("n", 4)):
            bad = copy.deepcopy(run);bad["products"]["track"][-1]["data"][0][field] = value
            with self.assertRaises((AssertionError, ValueError), msg=field):
                measured.golden_pair(baseline, bad, pair, plan)

    def test_golden_plan_identity_covers_native_frontend_geometry_and_manifold_changes(self):
        plan, run, baseline, pair = golden_fixture()
        row = run["observer"][0]
        original = measured.corpus.resolved_plan_identity(run, row)
        native = json.loads(row["resolved_product_plan"])
        for field in native.keys() - {"schema"}:
            changed = copy.deepcopy(row);policy = dict(native)
            policy[field] = not policy[field] if type(policy[field]) is bool else policy[field] + 1
            changed["resolved_product_plan"] = json.dumps(policy)
            self.assertNotEqual(original, measured.corpus.resolved_plan_identity(run, changed), field)
        for field in ("reference_synthesis", "clutter", "ambiguity", "detection", "tracker", "bearing", "surveillance_fusion", "multi_illuminator"):
            bad = copy.deepcopy(run);bad["config"]["process"][field] = {"changed": True}
            self.assertNotEqual(original, measured.corpus.resolved_plan_identity(bad, row), field)
        for field, value in (("sampleRateHz", 2000), ("frequencyHz", 200000000), ("reference_channel_ids", [1]),
                             ("channel_ids", [0]), ("doppler_hz", [101]), ("delay_bins", [0, 2]),
                             ("geometry", {"positions_m": [[1, 2, 3]]}), ("reference_config", {"analysis_samples": 999})):
            bad = copy.deepcopy(row);bad[field] = value
            self.assertNotEqual(original, measured.corpus.resolved_plan_identity(run, bad), field)
        row["geometry"]["bearing_manifold_file"] = "/example/manifold.json"
        with self.assertRaisesRegex(AssertionError, "manifold"):
            measured.corpus.resolved_plan_identity(run, row)
        run["calibrations"] = {"manifold.json": {"sha256": "1"*64}}
        calibrated = measured.corpus.resolved_plan_identity(run, row)
        run["calibrations"]["manifold.json"]["sha256"] = "2"*64
        self.assertNotEqual(calibrated, measured.corpus.resolved_plan_identity(run, row))

    def test_golden_rejects_missing_plan_drift_fallback_and_unexercised_features(self):
        plan, run, baseline, pair = golden_fixture()
        for stage, field, value in (("products", "resolved_product_plan", "{}"),
                                    ("products", "frequencyHz", 200000000),
                                    ("detector", "enabled", False)):
            bad = copy.deepcopy(run)
            next(row for row in bad["observer"] if row["stage"] == stage)[field] = value
            with self.assertRaises((AssertionError, ValueError)):
                measured.golden_pair(baseline, bad, pair, plan)
        bad = copy.deepcopy(run);bad["products"]["timing"][-1]["acceleration"]["cpuExecuted"] = True
        with self.assertRaisesRegex(ValueError, "physical GPU"):
            measured.golden_pair(baseline, bad, pair, plan)
        pair["requiredTrackStates"].append("COASTING")
        with self.assertRaisesRegex(ValueError, "lifecycle"):
            measured.golden_pair(baseline, run, pair, plan)

    def test_golden_manifest_requires_measured_feature_coverage_and_retains_full_evidence(self):
        plan, run, baseline, pair = golden_fixture()
        policy = measured.gate.POLICY_PATH.read_bytes()
        plan.update(schema="vectorwarp-recorded-iq-qualification-plan-v1",
            policySha256=hashlib.sha256(policy).hexdigest(), goldenPairs=[pair],
            featureManifest={"depth8-and-tracking": {"pairs": [pair["id"]],
                "requiredPlanFields": {"integrationCpis": 8, "stages.tracking": True},
                "requiredTrackStates": ["ACTIVE"], "minimumCounts": {"operationalTrackFrames": 2}}})
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);plan["runs"] = {}
            for name, value in (("cpu", baseline), ("gpu", run)):
                body = measured.canonical(value);(root / f"{name}.json").write_bytes(body)
                plan["runs"][name] = {"path": f"{name}.json", "sha256": hashlib.sha256(body).hexdigest()}
            report = measured.produce(plan, root, root / "good", policy, golden_only=True)
            self.assertTrue(report["passed"]);self.assertFalse(report["releaseQualified"])
            digest = report["pairs"][0]["evidenceSha256"]
            retained = json.loads((root / "good/pre-soak-evidence" / f"{digest}.json").read_text())
            self.assertEqual(retained["candidate"]["products"], run["products"])
            plan["featureManifest"]["depth8-and-tracking"]["requiredTrackStates"].append("COASTING")
            with self.assertRaisesRegex(ValueError, "lifecycle"):
                measured.produce(plan, root, root / "bad", policy, golden_only=True)

    def test_counts_actual_precentroid_population_and_unique_operational_tracks(self):
        plan, run, baseline, window, adjudication = fixture()
        for stream in (run, baseline):
            for frame in stream["products"]["track"]:
                frame.update(n=1, data=[{"id": 4, "state": "ACTIVE"}])
        result = measured.negative_window(run, baseline, window, adjudication, plan)
        self.assertEqual((result["evaluatedCells"], result["falseAlarmCells"], result["falseTracks"]), (200, 2, 1))
        self.assertEqual(result["baselineFalseTracks"], 1)
        # Empty final detections do not erase the observed raw CFAR positives.
        self.assertEqual(run["products"]["detection"][0]["delay"], [])

    def test_missing_cut_population_warmup_nonfinite_and_gaps_are_refused(self):
        plan, run, baseline, window, adjudication = fixture()
        for field, value in (("ready", False), ("evaluated_cells", 0), ("nonfinite_cells", 1)):
            bad = copy.deepcopy(run);bad["observer"][1][field] = value
            with self.assertRaisesRegex(ValueError, "CUT|population"):
                measured.negative_window(bad, baseline, window, adjudication, plan)
        bad = copy.deepcopy(run);bad["observer"] = [x for x in bad["observer"] if x["stage"] != "detector"]
        with self.assertRaisesRegex(ValueError, "CUT"):
            measured.negative_window(bad, baseline, window, adjudication, plan)
        window["endSample"] += 1;adjudication["endSample"] += 1
        with self.assertRaisesRegex(ValueError, "incomplete"):
            measured.negative_window(run, baseline, window, adjudication, plan)

    def test_adjudication_execution_and_candidate_are_not_summary_booleans(self):
        plan, run, baseline, window, adjudication = fixture()
        bad = copy.deepcopy(adjudication);bad["coverageComplete"] = False
        with self.assertRaisesRegex(ValueError, "adjudicated"):
            measured.negative_window(run, baseline, window, bad, plan)
        run["artifactReceipt"]["loaded"] = []
        with self.assertRaisesRegex(ValueError, "loaded-module"):
            measured.negative_window(run, baseline, window, adjudication, plan)
        run["sourceSha"] = "f"*40
        with self.assertRaisesRegex(ValueError, "another exact candidate"):
            measured.validate_run(run, plan)

    def test_echo_requires_real_detections_tracks_and_fixed_weight_control(self):
        plan, control, _, pair, _ = fixture()
        pair["target"] = {"lane": "single", "delayKm": 1, "dopplerHz": 100,
            "delayToleranceKm": .1, "dopplerToleranceHz": 1}
        injected = copy.deepcopy(control)
        injected["config"]["capture"]["replay"]["inject_echo"] = {"state": True, "delay_km": 1, "doppler_hz": 100}
        for row in injected["observer"]:
            if row["stage"] == "clutter":
                row["unfiltered_surveillance"] = [[[1,0]]]
                row["filtered_surveillance"] = [[[.9,0]]]
        with self.assertRaisesRegex(ValueError, "detections or operational tracks"):
            measured.echo_pair(control, injected, pair, plan)
        for detection, track in zip(injected["products"]["detection"], injected["products"]["track"]):
            detection.update(delay=[1], doppler=[100])
            track.update(n=1, data=[{"id": 3, "state": "ACTIVE", "delay": 1000000/299792458,
                                   "doppler": 100}])
        item = measured.echo_pair(control, injected, pair, plan)
        self.assertAlmostEqual(item["echoRetention"], .81)
        self.assertEqual((item["matchedDetections"], item["matchedOperationalTracks"]), (2, 1))
        for run in (control, injected):
            for row in run["observer"]:
                if row["stage"] == "clutter": row["filter_policy"] = "fixed-weights"
        with self.assertRaisesRegex(ValueError, "supported native"):
            measured.echo_pair(control, injected, pair, plan)

    def test_bounded_checkpoint_fixed_refit_both_classes_and_signed_doppler(self):
        for mode in ("fixed", "refit"):
            for kind in ("fp32", "fp64"):
                for sign in (-1, 1):
                    plan, control, injected, pair = checkpoint_fixture(mode, kind, sign*100)
                    item = measured.echo_pair(control, injected, pair, plan)
                    self.assertAlmostEqual(item["echoRetention"], .81)
                    self.assertEqual(item["filterPolicy"], mode+"-weights")
                    self.assertEqual(item["qualificationScope"], "bounded-clutter-replay-v1")
                    self.assertEqual(item["checkpointSha256"], pair["checkpointSha256"])

    def test_checkpoint_refuses_drift_missing_receipts_premature_injection_and_fitting(self):
        mutations = [
            lambda r: r["config"]["process"]["performance"].update(acceleration="cpu"),
            lambda r: r["config"]["process"]["performance"].update(acceleration="gpu-resident"),
            lambda r: r["config"]["process"].update(multi_illuminator={"enabled": True}),
            lambda r: r["config"]["capture"]["replay"].update(state=False),
            lambda r: r["config"]["capture"]["replay"]["qualification"]["clutter_checkpoint"].update(injection_first_sample=0),
            lambda r: r["config"]["capture"]["replay"]["qualification"]["clutter_checkpoint"].update(sha256=""),
            lambda r: r["clutterCheckpoint"].update(bytesHex="00"),
            lambda r: r["events"].pop(),
            lambda r: r["events"][-1].update(workerPid=999),
            lambda r: r["observer"][2].update(reference_iq=[[2,0]]*1000),
            lambda r: r["observer"][5].update(checkpoint_proof=r["observer"][5]["checkpoint_proof"][:-1]+"1"),
            lambda r: r["observer"][5].update(checkpoint_proof=""),
            lambda r: r["observer"][5].update(channel_ids=[2,1]),
            lambda r: r["observer"][5].update(cpu_executed=True),
            lambda r: r["config"]["capture"]["replay"]["inject_echo"].update(doppler_hz=-100),
        ]
        for mutate in mutations:
            plan, control, injected, pair = checkpoint_fixture()
            mutate(injected)
            with self.assertRaises((ValueError, KeyError)):
                measured.echo_pair(control, injected, pair, plan)
        plan, control, injected, pair = checkpoint_fixture()
        pair["firstSample"] = 0
        with self.assertRaisesRegex(ValueError, "boundary"):
            measured.echo_pair(control, injected, pair, plan)

    def test_capture_routes_bounded_complex_without_changing_ordinary_golden(self):
        for bounded in (False, True):
            plan, control, _, _ = checkpoint_fixture()
            config = copy.deepcopy(control["config"])
            config["capture"].update(fc=100000000, device={"channel_count": 2})
            replay = config["capture"]["replay"]
            if not bounded:
                replay["inject_echo"]["state"] = False
                del replay["qualification"]["clutter_checkpoint"]
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                if bounded:
                    path = root / "checkpoint";path.write_bytes(bytes.fromhex(control["clutterCheckpoint"]["bytesHex"]))
                    replay["qualification"]["clutter_checkpoint"]["path"] = str(path)
                spec = {"config": {}, "binary": {"path": "blah2", "sha256": "f"*64},
                    "inspector": {"path": "inspect", "sha256": "f"*64},
                    "recording": {"path": "recording", "sha256": "d"*64}, "deviceClass": "fp32", "maximumFrames": 2}
                if bounded: spec["qualificationKind"] = "clutter-echo-v1"
                plan["captureRuns"] = {"fixture": spec}
                def fake_run(*args, **kwargs):
                    actual = kwargs["configuration"]["capture"]["replay"]["qualification"]["observer"]
                    self.assertEqual(actual["summary_only"], not bounded)
                    self.assertEqual(actual.get("clutter", {}).get("enable", False), bounded)
                    self.assertEqual(actual["max_samples_per_channel"], 4096 if bounded else 16777216)
                    result = copy.deepcopy(control);result["config"] = kwargs["configuration"]
                    return result
                with mock.patch.object(measured, "pinned", return_value=config), \
                     mock.patch.object(measured.corpus.parity, "digest", return_value="f"*64), \
                     mock.patch.object(measured.subprocess, "check_output", side_effect=[
                         json.dumps(control["acquisition"]).encode(), b"{}"]), \
                     mock.patch.object(measured.corpus.parity, "select_device", return_value=control["device"]), \
                     mock.patch.object(measured.corpus, "run_case", side_effect=fake_run), \
                     mock.patch.object(measured.corpus, "resolved_plan_identity", return_value="a"*64):
                    measured.capture_run(plan, "fixture", root, root / "run.json")
                    retained = json.loads((root / "run.json").read_bytes())
                    if bounded:
                        self.assertEqual(retained["qualificationScope"], "bounded-clutter-replay-v1")
                        self.assertEqual(retained["clutterCheckpoint"], control["clutterCheckpoint"])

    def test_unapproved_policy_cannot_emit_release_receipt_and_output_is_exclusive(self):
        policy = measured.gate.POLICY_PATH.read_bytes()
        plan = {"schema": "vectorwarp-recorded-iq-qualification-plan-v1", "sourceSha": "a"*40,
            "artifacts": {"blah2": "b"*64}, "policySha256": hashlib.sha256(policy).hexdigest(), "runs": {}}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);output = root / "evidence"
            report = measured.produce(plan, root, output, policy)
            self.assertFalse(report["releaseQualified"])
            self.assertIn("not been approved", report["blockingReasons"][0])
            self.assertFalse((output / "pre-soak-radar-evidence.json").exists())
            with self.assertRaises(FileExistsError): measured.produce(plan, root, output, policy)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);(root / "evidence.json").write_text('{"tampered":true}')
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                measured.pinned({"path": "evidence.json", "sha256": "a"*64}, root)


if __name__ == "__main__":
    unittest.main()
