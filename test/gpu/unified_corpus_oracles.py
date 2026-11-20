#!/usr/bin/env python3
"""Measured checks for the supplemental production-binary corpus.

CPU replays are independent numerical controls, never physical execution proof.
Coherent fusion is also recomputed directly from the observed complex CAF and
the declared array manifold; no expected value is inferred from fused output.
"""
import cmath
import copy
import hashlib
import math
import re
from pathlib import Path

import unified_corpus_fixtures as fixtures
import unified_physical_corpus as corpus


def record(checks, name, value):
    if name in checks.expected:
        checks.record(name, value)


def retained(run, evidence):
    return {"generation": run["processorGeneration"], "case": run["case"],
        "resolvedPlans": evidence.put({lane: sorted({corpus.resolved_plan_identity(run, row)
            for row in run["observer"] if row["stage"] == "products" and row["lane"] == lane})
            for lane in {row["lane"] for row in run["observer"]}}),
        "config": evidence.put(run["config"]), "fixtures": evidence.put(run["scene"]),
        "products": evidence.put(run["products"]), "execution": evidence.put({
            "observer": run["observer"], "events": run["events"], "statuses": run["statuses"],
            "processorPid": run["processorPid"], "generation": run["processorGeneration"]}),
        "log": evidence.put({"text": run["log"], "returncode": run["returncode"]}),
        "calibrations": evidence.put(run["calibrations"] or {"applicable": False})}


def base_checks(run, checks, proof):
    variant = run["case"]["variant"]
    corpus.complete_publications(run)
    lanes = corpus.lane_frames(run)
    plans = {}
    for row in run["observer"]:
        if row["stage"] != "products": continue
        identity = corpus.resolved_plan_identity(run, row)
        assert plans.setdefault(row["lane"], identity) == identity, "native plan drifted within a lane"
    assert len(plans) == variant["lanes"], "missing resolved plan for a configured lane"
    record(checks, "immutable-resolved-plan", {"proof": proof, "plans": plans})
    assert len(lanes) == variant["lanes"]
    for name, frames in lanes.items():
        assert len(frames) == corpus.PUBLISHED_FRAMES, ("missing complete published frames", name)
        positions = [corpus.lineage_key(frame) for frame in frames]
        assert len(set(positions)) == len(positions), "capture interval published twice"
        for epoch, first, end in positions:
            assert all(type(value) is int and 0 <= value < 2**53 for value in (epoch, first, end))
            assert end > first
        for frame in frames:
            for product in ("map", "detections", "tracks"):
                value = frame[product]
                assert value["timestamp"] == frame["timestamp"], "unbound mathematical product"
                assert value.get("capture_epoch") == frame["capture_epoch"], "product epoch differs"
                assert value.get("capture_first_input_sample") == frame["capture_first_input_sample"]
                assert value.get("capture_end_input_sample") == frame["capture_end_input_sample"]
        if run["case"]["mode"] != "cpu":
            final = frames[-2:]
            for frame in final:
                for stage in ("acceleration", "clutterAcceleration"):
                    value = frame[stage]
                    assert value["gpuExecuted"] is True and value["cpuExecuted"] is False, value
                    assert value["active"] in ("vulkan", "vulkan-resident"), value
                    assert value["state"] == "ready" and value["device"], value
            record(checks, "physical-device-execution", {"proof": proof, "lane": name, "steadyFrames": 2})
            if run["case"]["mode"] == "gpu-resident":
                assert all(frame["acceleration"]["active"] == "vulkan-resident" for frame in frames[-2:])
                record(checks, "resident-execution-every-lane", {"proof": proof, "lane": name})
    record(checks, "frame-bound-lineage", {"proof": proof, "lanes": list(lanes),
                                          "framesPerLane": corpus.PUBLISHED_FRAMES})
    assert corpus.event_count(run, "input-gap-discarded") == 1, "capture gap was not exercised"
    gap = next(item for item in run["events"] if item.get("boundary") == "input-gap-discarded")
    reset = [item for item in run["events"] if item.get("boundary") == "history-reset" and
             item["frame"] == gap["frame"] + 1 and item["captureEpoch"] == gap["captureEpoch"] and
             item["captureFirstEpochSample"] == gap["captureEndEpochSample"]]
    assert len(reset) == 1, "gap reset was not observed at the next owned capture interval"
    epochs = {frame["capture_epoch"] for frames in lanes.values() for frame in frames}
    assert len(epochs) >= 2, "second replay epoch was not exercised"
    record(checks, "gap-reset", {"proof": proof, "discardedCpis": 1})
    record(checks, "history-reset", {"proof": proof, "captureGap": gap, "nextCaptureReset": reset[0]})
    record(checks, "epoch-reset", {"proof": proof, "epochs": sorted(epochs)})
    if "aggregate-memory-bound" in checks.expected and run["case"]["mode"] == "gpu-resident":
        allocation_checks(run, checks, proof)
    return lanes


def allocation_checks(run, checks, proof):
    grouped = {}
    for item in run["observer"]:
        if item.get("stage") != "products": continue
        allocation = item.get("resident_allocation")
        assert isinstance(allocation, dict), "actual resident allocation proof is absent"
        assert re.fullmatch(r"[0-9a-f]{32}", allocation.get("session_id", ""))
        fields = ("limit_bytes", "used_bytes", "peak_bytes", "live_allocations",
                  "shared_reservation_bytes", "aggregate_cap_bytes", "session_reservation_bytes",
                  "aggregate_session_cap")
        assert all(type(allocation.get(name)) is int and allocation[name] > 0 for name in fields)
        assert allocation["used_bytes"] <= allocation["peak_bytes"] <= allocation["limit_bytes"]
        assert allocation["limit_bytes"] + allocation["shared_reservation_bytes"] <= allocation["session_reservation_bytes"]
        key = (item["captureEpoch"], item["captureFirst"], item["captureEnd"])
        grouped.setdefault(key, []).append((item["lane"], allocation))
    observations = []
    for window, allocations in grouped.items():
        if len(allocations) != run["case"]["variant"]["lanes"]:
            continue  # Complete-frame stop may drain a final partial observer set.
        assert len({lane for lane, _ in allocations}) == len(allocations)
        assert len({value["session_id"] for _, value in allocations}) == len(allocations), \
            "independent lanes reused a worker-session nonce"
        caps = {(value["aggregate_cap_bytes"], value["aggregate_session_cap"]) for _, value in allocations}
        assert len(caps) == 1
        cap, session_cap = next(iter(caps))
        used = sum(value["peak_bytes"] + value["shared_reservation_bytes"] for _, value in allocations)
        reserved = sum(value["session_reservation_bytes"] for _, value in allocations)
        assert len(allocations) <= session_cap and used <= reserved <= cap
        observations.append({"capture": window, "sessions": len(allocations),
                             "measuredPeakAndMappingBytes": used, "reservedBytes": reserved, "capBytes": cap})
    assert len(observations) >= corpus.PUBLISHED_FRAMES, "aggregate physical session evidence is incomplete"
    record(checks, "aggregate-memory-bound", {"proof": proof, "allocations": observations})


def target_match(delay_km, doppler_hz, expected):
    bins = delay_km * fixtures.LANE_RATE * 1000 / fixtures.C
    return abs(bins - expected["delayBins"]) <= 1.75 and abs(doppler_hz - expected["dopplerHz"]) <= 75


def null_control_checks(lanes):
    frames = [frame for values in lanes.values() for frame in values]
    operational = sum(track.get("state") in ("ACTIVE", "COASTING") for frame in frames
                      for track in frame["tracks"].get("data", []))
    # This short deterministic fixture is a strict synthetic negative control,
    # not enough exposure to estimate operational PFA or real-world false tracks.
    assert operational == 0, "synthetic absent-target control produced an operational false track"
    return {"syntheticOnly": True, "maximumOperationalFalseTrackFrames": 0,
            "operationalFalseTrackFrames": operational,
            "falseTrackFramesIncludingTentative": sum(frame["tracks"]["n"] for frame in frames),
            "falseDetections": sum(len(frame["detections"]["delay"]) for frame in frames),
            "laneCpis": len(frames)}


def target_checks(run, lanes, checks, proof):
    scene = run["scene"]
    found, found_tracks = {}, {}
    false_tracks = 0
    for lane, (name, frames) in enumerate(lanes.items()):
        matches, track_matches = set(), set()
        for frame in frames:
            relative = (frame["capture_first_input_sample"] - 1000) // scene["samplesPerFrame"]
            source_frame = int(relative % fixtures.EPOCH_FRAMES)
            expected = scene["truth"][source_frame][lane]
            detections = frame["detections"]
            for delay, doppler in zip(detections["delay"], detections["doppler"]):
                if source_frame < scene["recipe"]["targetFrames"] and target_match(delay, doppler, expected):
                    matches.add(source_frame)
            for track in frame["tracks"].get("data", []):
                # Tracker JSON deliberately keeps delay bins; detection JSON
                # exports kilometres. Do not silently apply the same units.
                if source_frame < scene["recipe"]["targetFrames"] and \
                    track.get("state") in ("ACTIVE", "COASTING") and \
                    target_match(track["delay"] * fixtures.C / fixtures.LANE_RATE / 1000,
                                 track["doppler"], expected):
                    track_matches.add(source_frame)
            if not run["case"]["target"]:
                false_tracks += frame["tracks"]["n"]
        if run["case"]["target"]:
            assert len(matches) >= 2, ("known target absent from published detections", name)
            assert len(track_matches) >= 2, ("known target absent from operational tracks", name)
            found[name] = sorted(matches)
            found_tracks[name] = sorted(track_matches)
    if run["case"]["target"]:
        record(checks, "target-present", {"proof": proof, "matchedDetectionFrames": found,
                                         "matchedOperationalTrackFrames": found_tracks})
        if len(lanes) > 1:
            expected_targets = [scene["truth"][0][lane] for lane in range(len(lanes))]
            assert len({(x["delayBins"], x["dopplerHz"]) for x in expected_targets}) == len(lanes)
            record(checks, "distinct-lane-signals", {"proof": proof, "targets": expected_targets})
            record(checks, "lane-isolation", {"proof": proof, "matchedIndependentLaneTargets": found})
    else:
        assert run["scene"]["recipe"]["target"] is False
        record(checks, "absent-target-control", {"proof": proof, **null_control_checks(lanes)})
    return false_tracks


def observer_pairs(cpu, gpu, stage):
    def indexed(run):
        result = {}
        for item in run["observer"]:
            if item.get("stage") != stage: continue
            key = (item["lane"], item["captureEpoch"], item["captureFirst"], item["captureEnd"])
            assert key not in result, "duplicate observer record"
            result[key] = item
        return result
    expected, actual = indexed(cpu), indexed(gpu)
    common = expected.keys() & actual.keys()
    assert len(common) >= 2 * gpu["case"]["variant"]["lanes"], "complex observer lacks paired frames"
    # Process stop may drain additional complete frames. Compare all common
    # positioned observations; they retain exact epoch, interval and lane.
    return [(expected[key], actual[key]) for key in sorted(common)]


def paired_checks(cpu, gpu, checks, cpu_proof, proof):
    errors = []
    for left, right in observer_pairs(cpu, gpu, "input"):
        errors.append(corpus.complex_error(left["channelized_iq"], right["channelized_iq"]))
    record(checks, "channelizer-complex-phase-parity", {"oracle": cpu_proof, "actual": proof, "errors": errors})
    raw_errors = []
    for left, right in observer_pairs(cpu, gpu, "products"):
        if gpu["case"]["variant"]["channels"] == 2:
            assert corpus.resolved_plan_identity(cpu, left) == corpus.resolved_plan_identity(gpu, right), \
                "CPU/GPU resolved radar plan differs"
        assert left["rows"] == right["rows"] and left["columns"] == right["columns"]
        assert left["delay_bins"] == right["delay_bins"] and left["doppler_hz"] == right["doppler_hz"]
        raw_errors.append({"reference": corpus.complex_error(left["reference_iq"], right["reference_iq"]),
                           "caf": corpus.complex_error(left["caf"], right["caf"])})
    record(checks, "reference-complex-parity", {"oracle": cpu_proof, "actual": proof, "errors": raw_errors})
    record(checks, "numerical-oracle", {"oracle": cpu_proof, "actual": proof, "complexErrors": raw_errors})
    if gpu["case"]["variant"]["channels"] == 2:
        left, right = corpus.lane_frames(cpu), corpus.lane_frames(gpu)
        assert left.keys() == right.keys()
        for lane in left:
            for first, second in zip(left[lane], right[lane]):
                assert corpus.lineage_key(first) == corpus.lineage_key(second)
                for product in ("map", "detections", "tracks"):
                    corpus.parity.compare(corpus.semantic(first[product]), corpus.semantic(second[product]),
                                          f"{lane}.{product}")
        record(checks, "map-detection-track-parity", {"oracle": cpu_proof, "actual": proof})


def temporal_cell_power(history, row, column, maximum, fractional=False, beam_tolerance=None):
    """Independent scalar power-path maximum; history is newest first.

    Coordinates use the public signed Doppler axis and exact consumed sample
    stride. Every path uses one distinct CPI per age; edges invalidate a whole
    path. Beam IDs here are the native zero-based fused IDs.
    """
    newest = history[0]
    rows, columns = newest["rows"], newest["columns"]
    cell = row * columns + column
    current = newest["fused_amplitude"][cell] ** 2
    if len(history) < 2:
        return current
    scale = -(newest["captureEnd"] - newest["captureFirst"]) / \
        (newest["inputSamplesPerOutput"] * newest["frequencyHz"]) if fractional else 0.
    beam_count = newest["geometry"]["beam_count"]

    def compatible(older, at):
        if beam_tolerance is None:
            return True
        left, right = newest["fused_beam_id"][cell], older["fused_beam_id"][at]
        assert int(left) == left and int(right) == right and 0 <= left < beam_count and 0 <= right < beam_count
        distance = abs(left - right)
        return min(distance, beam_count - distance) <= beam_tolerance

    selected = .5 * (current + history[1]["fused_amplitude"][cell] ** 2) \
        if compatible(history[1], cell) else 0.
    for depth in (2, 4, 8):
        if depth > maximum or depth > len(history):
            continue
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                values = []
                for age, older in enumerate(history[:depth]):
                    rr = row - dr * age
                    if not 0 <= rr < rows:
                        break
                    displacement = scale * .5 * (newest["doppler_hz"][row] + newest["doppler_hz"][rr]) * age
                    fixed = math.floor(abs(displacement) * 65536 + .5)
                    nearest = (fixed + 32768) // 65536
                    offset = -nearest if displacement < 0 else nearest
                    cc = column - dc * age - offset
                    if not 0 <= cc < columns or not compatible(older, rr * columns + cc):
                        break
                    value = older["fused_amplitude"][rr * columns + cc]
                    assert math.isfinite(value) and value >= 0
                    values.append(value ** 2)
                if len(values) == depth:
                    selected = max(selected, sum(values) / depth)
    return selected


def temporal_result_checks(run, lanes):
    """Compare exported integrated cells with the independent path oracle.

    Sample target neighborhoods, corners and interior cells on every published
    frame. This bounds Python work without using readiness/truth as a result.
    """
    variant = run["case"]["variant"]
    maximum = variant["window"]
    records = [item for item in run["observer"] if item.get("stage") == "products"]
    published = {(name if len(lanes) > 1 else "primary", corpus.lineage_key(frame)): frame
                 for name, frames in lanes.items() for frame in frames}
    histories, compared, deep_effect = {}, {}, {}
    maximum_error = 0.
    maximum_limit = .025
    for item in records:
        lane = item["lane"]
        key = (item["captureEpoch"], item["captureFirst"], item["captureEnd"])
        history = histories.setdefault(lane, [])
        if history and (history[0]["captureEpoch"] != key[0] or history[0]["captureEnd"] != key[1]):
            history.clear()
        assert item["fused_stage"] == "pre_temporal"
        if history:
            assert item["delay_bins"] == history[0]["delay_bins"] and item["doppler_hz"] == history[0]["doppler_hz"]
        history.insert(0, item)
        del history[maximum:]
        frame = published.get((lane, key))
        if frame is None:
            continue  # The bounded stop can drain a final unpublished record.
        actual = frame["map"]
        precision = actual.get("presentation_precision_db", 0.)
        assert precision in (0., .25) and actual.get("presentationStride", 1) == 1, "temporal oracle requires native map cells"
        limit = .025 + precision / 2
        maximum_limit = max(maximum_limit, limit)
        rows, columns = item["rows"], item["columns"]
        assert actual["nRows"] == rows and actual["nCols"] == columns
        assert len(actual["data"]) == rows and all(len(values) == columns for values in actual["data"])
        cells = {(0, 0), (0, columns - 1), (rows - 1, 0), (rows - 1, columns - 1),
                 (rows // 2, columns // 2), (rows // 3, columns // 3), (2 * rows // 3, 2 * columns // 3)}
        source = (key[1] - 1000) // run["scene"]["samplesPerFrame"] % fixtures.EPOCH_FRAMES
        lane_index = int(lane.split("-")[-1]) if len(lanes) > 1 else 0
        target = run["scene"]["truth"][source][lane_index]
        row = min(range(rows), key=lambda at: abs(item["doppler_hz"][at] - target["dopplerHz"]))
        column = min(range(columns), key=lambda at: abs(item["delay_bins"][at] - target["delayBins"]))
        cells.update((row, at) for at in range(max(0, column - 1), min(columns, column + 2)))
        tolerance = run["config"]["process"]["tracker"].get("beam_tolerance") \
            if item["product_backend"] == "vulkan-resident" else None
        fractional = variant["motion"] == "fractional-delay"
        for row, column in cells:
            power = temporal_cell_power(history, row, column, maximum, fractional, tolerance)
            expected = 10 * math.log10(max(math.sqrt(power), 1e-30))
            observed = actual["data"][row][column] + actual["noisePower"]
            error = abs(observed - expected)
            assert math.isfinite(observed) and error <= limit, ("integrated temporal cell differs", lane, key, row, column, expected, observed)
            maximum_error = max(maximum_error, error)
            if len(history) == maximum:
                shallow = temporal_cell_power(history, row, column, maximum // 2, fractional, tolerance)
                if power > shallow * 10 ** (2 * limit / 5):
                    deep_effect[lane] = deep_effect.get(lane, 0) + 1
        compared[lane] = compared.get(lane, 0) + 1
    assert len(compared) == len(lanes) and all(count == len(lanes[name if len(lanes) > 1 else "single"])
                                              for name, count in compared.items()), "temporal output lacks complete observer lineage"
    assert set(deep_effect) == set(compared), "configured deepest temporal bank made no verified contribution"
    return {"comparedFrames": compared, "deepestBankContributions": deep_effect,
            "maximumAbsoluteDisplayDbError": maximum_error, "limitDb": maximum_limit}


def temporal_checks(run, lanes, checks, proof):
    variant = run["case"]["variant"]
    if variant["window"] <= 2: return
    numeric = temporal_result_checks(run, lanes)
    record(checks, "motion-trajectory-oracle", {"proof": proof, "motion": variant["motion"], "numeric": numeric})
    if variant["channels"] > 2:
        if run["case"]["mode"] not in ("gpu-resident", "auto"):
            return  # CPU control qualifies complex values, not resident history.
        products = [item for item in run["observer"] if item.get("stage") == "products"]
        # Single AUTO legitimately selects portable; its raw CAF remains a
        # numerical control but cannot provide resident temporal proof.
        if run["case"]["mode"] == "auto" and variant["lanes"] == 1:
            return
        assert products and all(item.get("resident_proof") for item in products), \
            "coherent history readiness proof is absent"
        ready = [item for item in products if item["resident_proof"]["postprocess_status"][28] != 0]
        warm = [item for item in products if item["resident_proof"]["postprocess_status"][28] == 0]
        assert ready and warm, "coherent TBD never exercised both warmup and ready states"
        record(checks, "history-warmup", {"proof": proof, "cold": len(warm), "ready": len(ready)})
        assert all(item["geometry"]["integration_cpis"] == variant["window"] for item in products)
        record(checks, "four-cpi-support", {"proof": proof, "window": variant["window"], "ready": len(ready)})
        if variant["window"] == 8:
            record(checks, "eight-cpi-support", {"proof": proof, "ready": len(ready)})
        # Each ready native row must carry source support in its detector flags;
        # observing a readiness bit alone cannot establish beam conditioning.
        supported = [item for item in ready if any(item.get("flags", []))]
        assert supported, "coherent temporal detector did not retain flagged cell support"
        record(checks, "beam-conditioned-support", {"proof": proof, "supportedFrames": len(supported)})
        previous, resets = {}, []
        for item in products:
            lane, native = item["lane"], item["resident_proof"]
            prior = previous.get(lane)
            discontinuity = prior and (item["captureEpoch"] != prior["captureEpoch"] or
                                        item["captureFirst"] != prior["captureEnd"])
            if discontinuity:
                assert native["postprocess_status"][28] == 0, "coherent support survived capture reset"
                # Input sequence is deliberately monotonic across resetInput;
                # only retained history becomes cold. Never reinterpret it as
                # a per-epoch sequence or require a fabricated restart at one.
                assert native["consumed_sequence"] > prior["resident_proof"]["consumed_sequence"]
                resets.append([lane, item["captureEpoch"], item["captureFirst"]])
            previous[lane] = item
        assert len(resets) >= 2 * variant["lanes"], "coherent gap and epoch resets not observed"
        record(checks, "no-cross-epoch-support", {"proof": proof, "coldResets": resets})
        return
    for lane, frames in lanes.items():
        metrics = [frame.get("detector_integration") for frame in frames]
        assert all(isinstance(value, dict) and value["cpis"] == variant["window"] for value in metrics)
        assert any(value["available_cpis"] == 1 and not value["ready"] for value in metrics)
        assert any(value["available_cpis"] == variant["window"] for value in metrics)
        assert any(value.get("bank_ready", [])[2:3] == [True] for value in metrics)
        record(checks, "history-warmup", {"proof": proof, "lane": lane})
        record(checks, "four-cpi-support", {"proof": proof, "lane": lane})
        if variant["window"] == 8:
            assert any(value.get("bank_ready", [])[3:4] == [True] for value in metrics)
            record(checks, "eight-cpi-support", {"proof": proof, "lane": lane})
        prior = None
        resets = 0
        for frame, value in zip(frames, metrics):
            if prior is not None and frame["capture_epoch"] != prior:
                assert value["available_cpis"] == 1 and not value["ready"]
                assert value["support_sequences"] == [1]
                resets += 1
            prior = frame["capture_epoch"]
        assert resets, "temporal history did not cross a replay epoch boundary"
        record(checks, "no-cross-epoch-support", {"proof": proof, "lane": lane, "resets": resets})
    if run["case"]["target"]:
        assert variant["motion"] in ("integer", "fractional-delay", "stationary")
        if variant["motion"] == "fractional-delay":
            assert all(value["delay_drift_bins_per_hz"] < 0 for value in metrics)


def covariance(channels, analysis_samples, loading):
    values = [[complex(*sample) for sample in channel] for channel in channels]
    count, samples = len(values), len(values[0])
    stride = max(1, samples // analysis_samples)
    selected = range(0, samples, stride)
    matrix = [[sum(values[left][index] * values[right][index].conjugate() for index in selected) /
               len(selected) for right in range(count)] for left in range(count)]
    load = loading * sum(matrix[channel][channel].real for channel in range(count)) / count
    for channel in range(count): matrix[channel][channel] += load
    return matrix


def whitening_scales(matrix, weights, loading):
    count = len(weights)
    trace = sum(matrix[c][c].real for c in range(count))
    reference_load = loading * trace / ((1 + loading) * count)
    principal = max(0.0, sum(weights[a].conjugate() * matrix[a][b] * weights[b]
                            for a in range(count) for b in range(count)).real - reference_load)
    noise = [max(0.0, matrix[c][c].real - principal * abs(weights[c]) ** 2) for c in range(count)]
    floor = max(reference_load, .05 * sum(noise) / count)
    noise = [max(value, floor) for value in noise]
    median = sorted(noise)[count // 2]
    return [min(2., max(.5, median / value)) for value in noise] if median > 0 else [1.] * count


def bearing_result_checks(run, lanes):
    """Bind bearing truth to its own lane, capture interval and radar track."""
    single = {}
    if len(lanes) == 1:
        for item in run["products"]["bearing"]:
            key = corpus.lineage_key(item)
            assert key not in single, "duplicate bearing capture interval"
            single[key] = item
    found = {}
    scene = run["scene"]
    for lane, (name, frames) in enumerate(lanes.items()):
        matches = []
        for frame in frames:
            bearing = single.get(corpus.lineage_key(frame)) if len(lanes) == 1 else frame.get("bearing")
            assert isinstance(bearing, dict), "missing lane-bound bearing publication"
            assert corpus.lineage_key(bearing) == corpus.lineage_key(frame), "unbound bearing lineage"
            assert bearing["timestamp"] == frame["timestamp"], "unbound bearing timestamp"
            relative = (frame["capture_first_input_sample"] - 1000) // scene["samplesPerFrame"]
            source_frame = int(relative % fixtures.EPOCH_FRAMES)
            if source_frame >= scene["recipe"]["targetFrames"]:
                continue
            expected = scene["truth"][source_frame][lane]
            tracks = {track["id"]: track for track in frame["tracks"].get("data", [])}
            for item in bearing.get("data", []):
                track = tracks.get(item.get("id"))
                if not track or track.get("state") not in ("ACTIVE", "COASTING"):
                    continue
                if not target_match(track["delay"] * fixtures.C / fixtures.LANE_RATE / 1000,
                                    track["doppler"], expected):
                    continue
                # Bearing rows carry the same current track coordinates in
                # native bins/Hz; an ID alone cannot authenticate association.
                assert item.get("state") == track["state"]
                assert item.get("delay") == track["delay"] and item.get("doppler") == track["doppler"], \
                    "bearing coordinates differ from its operational track"
                value = item.get("current_bearing_deg_true")
                if type(value) not in (int, float) or not math.isfinite(value):
                    continue
                error = abs((value - (90 - expected["bearingLocalDeg"]) + 180) % 360 - 180)
                if error <= 11.25:
                    matches.append({"capture": corpus.lineage_key(frame), "track": track["id"],
                                    "angularErrorDeg": error})
        assert matches, ("known lane target produced no track-bound calibrated bearing", name)
        found[name] = matches
    return found


def coherent_checks(run, lanes, checks, proof):
    if run["case"]["variant"]["channels"] <= 2: return
    inputs = {(item["lane"], item["captureEpoch"], item["captureFirst"]): item
              for item in run["observer"] if item.get("stage") == "input"}
    errors, scale_ranges = [], []
    for item in (value for value in run["observer"] if value.get("stage") == "products"):
        source = inputs[(item["lane"], item["captureEpoch"], item["captureFirst"])]
        weights = [complex(*value) for value in item["reference_weights"]]
        count = len(weights)
        assert count == run["case"]["variant"]["channels"]
        scales = [1.] * count
        if run["case"]["variant"]["fusion"] == "noise-whitened":
            config = item["reference_config"]
            matrix = covariance(source["channelized_iq"], config["analysis_samples"], config["diagonal_loading"])
            scales = whitening_scales(matrix, weights, config["diagonal_loading"])
            scale_ranges.append([min(scales), max(scales)])
        maps = [[complex(*value) for value in channel] for channel in item["caf"]]
        geometry = item["geometry"]
        positions = geometry["positions_m"]
        beams = geometry["beam_count"]
        direct = geometry["direct_path_azimuth_rad"]
        assert geometry["direct_path_null_db"] == 0, "corpus oracle requires explicit zero null"
        expected = [0.] * (item["rows"] * item["columns"])
        expected_beams = [0] * len(expected)
        beam_amplitudes = []
        for beam in range(beams):
            angle = 2 * math.pi * beam / beams
            steering = [weights[c] * scales[c] * cmath.exp(-2j * math.pi * item["frequencyHz"] / fixtures.C *
                (position[0] * (math.cos(angle) - math.cos(direct)) +
                 position[1] * (math.sin(angle) - math.sin(direct)))) for c, position in enumerate(positions)]
            norm = math.sqrt(sum(abs(value) ** 2 for value in steering))
            assert norm > 0
            amplitudes = []
            for cell in range(len(expected)):
                amplitude = abs(sum(steering[c].conjugate() * maps[c][cell] for c in range(count))) / norm
                amplitudes.append(amplitude)
                if amplitude > expected[cell]:
                    expected[cell], expected_beams[cell] = amplitude, beam
            beam_amplitudes.append(amplitudes)
        actual = item["fused_amplitude"]
        assert len(actual) == len(expected)
        error = max(abs(a - b) for a, b in zip(expected, actual)) / max(max(expected), 1e-30)
        assert error <= 5e-5, ("independent coherent statistic oracle", error)
        if item["fused_beam_id"] is not None:
            assert len(item["fused_beam_id"]) == len(expected)
            for cell, beam in enumerate(item["fused_beam_id"]):
                assert int(beam) == beam and 0 <= beam < beams
                assert abs(beam_amplitudes[int(beam)][cell] - expected[cell]) <= max(max(expected) * 5e-5, 1e-30), \
                    "published beam is not an independently measured maximum"
        errors.append(error)
        assert item.get("actual_gain_db") == [20.] * count, "manifold actual gain identity unavailable"
        assert item["channel_ids"] == list(range(count))
    assert errors
    record(checks, "numerical-oracle", {"proof": proof, "independentBeamMaximumErrors": errors, "limit": 5e-5})
    if scale_ranges:
        assert any(high - low > 1e-3 for low, high in scale_ranges), "whitening fixture did not excite unequal scales"
        record(checks, "whitened-statistic-calibration", {"proof": proof, "scaleRanges": scale_ranges, "errors": errors})
    assert run["calibrations"]
    record(checks, "calibration-identity", {"proof": proof, "calibrations": run["calibrations"]})
    record(checks, "manifold-channel-order", {"proof": proof, "channels": count})
    if run["case"]["target"] and run["case"]["mode"] == "gpu-resident":
        matches = bearing_result_checks(run, lanes)
        record(checks, "known-bearing-recovery", {"proof": proof, "laneMatches": matches, "limitDeg": 11.25})
        record(checks, "bearing-support", {"proof": proof,
            "matchedBearingsPerLane": {lane: len(values) for lane, values in matches.items()}})


def refusal_checks(run, checks, proof):
    kind = run["case"]["refusal"]
    assert run["exited"] or any(value.get("state") == "error" for value in run["statuses"])
    assert not any(run["products"][name] for name in ("map", "detection", "track", "illuminator", "bearing")), \
        "refused calibration published numerical products"
    if kind == "bad-manifold":
        assert "identity differs" in run["log"] or "mapping differs" in run["log"]
        record(checks, "bad-manifold-refusal", {"proof": proof})
    elif kind == "missing-calibration":
        assert "Cannot open bearing manifold_file" in run["log"]
        record(checks, "missing-calibration-refusal", {"proof": proof})
    else:
        raise AssertionError("unknown refusal oracle")


def fault_checks(run, checks, proof, cpu):
    fault = run["case"]["fault"]
    injected = [event for event in run["events"] if event.get("boundary") == fault]
    assert len(injected) == 1, ("requested physical fault did not execute exactly once", fault, run["log"][-3000:])
    event = injected[0]
    assert event["frame"] == (0 if fault in ("startup", "allocation") else 16)
    requested = run["config"]["capture"]["replay"]["qualification"]["fault"]
    assert event["route"] == requested["route"] and event["lane"] == requested["lane"], "fault reached a different route/lane"
    public_lanes = corpus.complete_publications(run)
    if not run["case"]["target"]:
        null_control_checks(public_lanes)
    if fault == "allocation":
        allocation = [item for item in run["events"] if item.get("boundary") == "allocation-budget-rejected"]
        assert len(allocation) == 1 and allocation[0]["allocationLimit"] == 1 and allocation[0]["allocationBytes"] > 1
        assert allocation[0]["workerPid"] == event["workerPid"]
    record(checks, "fault-observed-at-requested-boundary", {"proof": proof, "injection": event})
    worker = event["workerPid"]
    assert worker > 0 and any(item.get("boundary") == "worker-reaped" and item["workerPid"] == worker
                              for item in run["events"]), "injected physical worker was not reaped"
    record(checks, "worker-reaped", {"proof": proof, "workerPid": worker})
    published = [item for item in run["events"] if item.get("boundary") == "publication-complete"]
    assert len({item["frame"] for item in published}) == len(published), "fault duplicated publication"
    published_windows = {(item["captureEpoch"], item["captureFirstEpochSample"], item["captureEndEpochSample"])
                         for item in published}
    for lane, values in public_lanes.items():
        keys = [corpus.lineage_key(frame) for frame in values]
        assert len(keys) == len(set(keys)), "fault replay repeated capture interval"
        assert set(keys) <= published_windows, "unacknowledged partial publication"
        for frame in values:
            for name in ("map", "detections", "tracks"):
                assert corpus.lineage_key(frame[name]) == corpus.lineage_key(frame), "stale fault product"
    if len(public_lanes) > 1:
        assert len({tuple(corpus.lineage_key(frame) for frame in values)
                    for values in public_lanes.values()}) == 1, "fault published only some lanes"
    if run["exited"]:
        assert event["route"] == "resident" and (run["case"]["variant"]["lanes"] == 1 or
            run["case"]["variant"]["statistic"] == "resident-only-coherent"), "recoverable route terminated instead of producing CPU recovery"
        assert not any(item["frame"] >= event["frame"] for item in published), "failed resident frame was published"
        if event["route"] == "resident" and run["case"]["variant"]["statistic"] == "resident-only-coherent":
            record(checks, "resident-only-fail-closed", {"proof": proof, "failure": event})
    else:
        assert not (event["route"] == "resident" and
                    run["case"]["variant"]["statistic"] == "resident-only-coherent"), \
            "resident-only coherent statistic silently fell back"
        frames = [frame for values in public_lanes.values() for frame in values]
        recovered = [frame for frame in frames if frame["acceleration"]["active"] == "cpu" and
                     frame["acceleration"]["cpuExecuted"] is True and
                     frame["acceleration"]["gpuExecuted"] is False]
        assert recovered, "fault produced no measured CPU recovery"
        if event["route"] == "resident" and event["frame"] > 0:
            bound = lambda item: all(item.get(field) == event.get(field) for field in
                ("route", "lane", "frame", "captureEpoch", "captureFirstEpochSample", "captureEndEpochSample"))
            resets = [item for item in run["events"] if item.get("boundary") == "history-reset" and bound(item)]
            recoveries = [item for item in run["events"] if item.get("boundary") == "cpu-recovery-complete" and bound(item)]
            assert len(resets) == len(recoveries) == 1, "resident reset/recovery was not bound to the exact fault interval"
            publication = next(item for item in published if item["frame"] == event["frame"])
            assert run["events"].index(event) < run["events"].index(resets[0]) < \
                run["events"].index(recoveries[0]) < run["events"].index(publication), "resident recovery boundaries are out of order"
            assert cpu["case"].get("fault") == "cpu-oracle-reset" and cpu["case"]["mode"] == "cpu", \
                "resident CPU oracle lacks the same exact-lane reset"
            control_resets = [item for item in cpu["events"] if item.get("boundary") == "history-reset"]
            assert len(control_resets) == 1 and control_resets[0]["route"] == "host" and \
                control_resets[0]["fault"] == "cpu-oracle-reset" and all(
                    control_resets[0].get(field) == event.get(field) for field in
                    ("lane", "frame", "captureEpoch", "captureFirstEpochSample", "captureEndEpochSample")), \
                "CPU reset did not match the resident failure's exact lane and interval"
            control_publication = next(item for item in cpu["events"] if
                item.get("boundary") == "publication-complete" and item["frame"] == event["frame"])
            assert cpu["events"].index(control_resets[0]) < cpu["events"].index(control_publication)
            record(checks, "history-reset", {"proof": proof, "fault": event, "reset": resets[0], "recovery": recoveries[0]})
        else:
            assert cpu["case"].get("noGap") is True, "portable/startup CPU oracle introduced an unrelated capture gap"
        expected = {(frame["id"], corpus.lineage_key(frame)): frame
                    for values in corpus.complete_publications(cpu).values() for frame in values}
        fault_window = (event["captureEpoch"], event["captureFirstEpochSample"], event["captureEndEpochSample"])
        exact = [frame for frame in recovered if corpus.lineage_key(frame) == fault_window]
        if event["frame"] > 0:
            assert exact, "runtime fault did not recover its exact owned capture interval"
        else:
            exact = recovered[:1]
        if event["frame"] > 0:
            following = [item for item in published if item["frame"] == event["frame"] + 1]
            assert len(following) == 1, "fault recovery omitted the next capture frame"
            next_window = tuple(following[0][field] for field in
                                ("captureEpoch", "captureFirstEpochSample", "captureEndEpochSample"))
            next_frames = [frame for frame in recovered if corpus.lineage_key(frame) == next_window]
            assert next_frames, "next capture frame did not report truthful CPU recovery"
        else:
            next_frames = recovered[1:2]
            assert next_frames, "startup recovery omitted its second frame"
        # Exact and next are required; every other paired recovered interval,
        # including the drained tail, must also agree numerically.
        compared = [frame for frame in recovered if (frame["id"], corpus.lineage_key(frame)) in expected]
        assert all(frame in compared for frame in exact + next_frames)
        for frame in compared:
            oracle = expected[(frame["id"], corpus.lineage_key(frame))]
            for product in ("map", "detections", "tracks"):
                corpus.parity.compare(corpus.semantic(oracle[product]), corpus.semantic(frame[product]), f"fault.cpu.{product}")
        if not run["case"]["target"]:
            actual_false = sum(frame["tracks"]["n"] for frame in recovered
                               if (frame["id"], corpus.lineage_key(frame)) in expected)
            cpu_false = sum(expected[(frame["id"], corpus.lineage_key(frame))]["tracks"]["n"]
                            for frame in recovered if (frame["id"], corpus.lineage_key(frame)) in expected)
            assert actual_false <= cpu_false, "fault recovery increased false-track exposure"
            record(checks, "no-false-track-increase", {"proof": proof, "fault": fault,
                "cpuFalseTrackFrames": cpu_false, "actualFalseTrackFrames": actual_false})
        record(checks, "exact-frame-cpu-recovery", {"proof": proof, "captureIntervals": [corpus.lineage_key(f) for f in exact]})
        if run["case"]["variant"]["statistic"] == "power":
            record(checks, "equivalent-power-cpu-recovery", {"proof": proof, "captureIntervals": [corpus.lineage_key(f) for f in exact]})
        record(checks, "truthful-fallback-execution", {"proof": proof, "recoveredFrames": len(recovered)})
        record(checks, "next-frame-parity", {"proof": proof,
            "captureIntervals": [corpus.lineage_key(frame) for frame in next_frames]})
    record(checks, "no-double-consumption", {"proof": proof, "publishedCaptureFrames": [item["frame"] for item in published]})
    record(checks, "no-partial-publication", {"proof": proof, "publishedCaptureFrames": [item["frame"] for item in published]})
    record(checks, "no-stale-products", {"proof": proof, "publicationCount": len(published)})
    # Portable worker recovery consumes the same canonical CPI and retains its
    # host reference/tracker history. That policy is checked against a no-gap
    # CPU oracle, never described as a reset. Capture-gap resets are measured
    # separately by base_checks, and resident reset events are bound above.


def qualify_scenario(contract, device_class, binary, device, root, cache, evidence):
    checks = corpus.Checks(contract, evidence)
    summaries, oracles, observed_frames = [], [], 0
    generation = None
    cases = list(corpus.planned_cases(contract, device_class))
    for variant in corpus.variants(contract, device_class):
        # Scope each CPU control and its GPU comparisons to this variant so
        # completed high-volume observer arrays are released promptly.
        for target in (True, False):
            recipe = fixtures.scene_recipe(variant, target)
            key = fixtures.fingerprint(recipe)
            recording = root / f"{key}.blah2iq"
            if key not in cache: cache[key] = fixtures.write_scene(recording, recipe)
            scene = cache[key]
            cpu_variant = copy.deepcopy(variant)
            if cpu_variant["fusion"] == "noise-whitened": cpu_variant["fusion"] = "coherent"
            cpu = corpus.run_case(binary, device, {"variant": cpu_variant, "mode": "cpu", "target": target}, recording, scene)
            cpu_proof = retained(cpu, evidence)
            cpu_lanes = base_checks(cpu, checks, cpu_proof)
            cpu_false = target_checks(cpu, cpu_lanes, checks, cpu_proof)
            oracles.append(cpu_proof)
            fault_controls = {}
            for mode in variant["modes"]:
                if mode == "cpu":
                    run, proof, lanes = cpu, cpu_proof, cpu_lanes
                else:
                    run = corpus.run_case(binary, device, {"variant": variant, "mode": mode, "target": target}, recording, scene)
                    proof = retained(run, evidence)
                    lanes = base_checks(run, checks, proof)
                    paired_checks(cpu, run, checks, cpu_proof, proof)
                actual_false = target_checks(run, lanes, checks, proof)
                if not target:
                    assert actual_false <= cpu_false, ("synthetic false tracks increased", actual_false, cpu_false)
                    record(checks, "no-false-track-increase", {"oracle": cpu_proof, "actual": proof,
                        "cpuFalseTrackFrames": cpu_false, "actualFalseTrackFrames": actual_false})
                temporal_checks(run, lanes, checks, proof)
                if mode != "cpu": coherent_checks(run, lanes, checks, proof)
                summaries.append(proof)
                generation = generation or run["processorGeneration"]
                observed_frames += sum(map(len, lanes.values()))
                for case in cases:
                    if case["variant"] != variant or case["mode"] != mode or case["target"] != target or not case.get("fault"):
                        continue
                    failed = corpus.run_case(binary, device, case, recording, scene)
                    failed_proof = retained(failed, evidence)
                    route = failed["config"]["capture"]["replay"]["qualification"]["fault"]["route"]
                    reset_control = route == "resident" and case["fault"] not in ("startup", "allocation")
                    if not failed["exited"] and reset_control not in fault_controls:
                        control_case = {"variant": cpu_variant, "mode": "cpu", "target": target,
                                        "noGap": True, **({"fault": "cpu-oracle-reset"} if reset_control else {})}
                        control = corpus.run_case(binary, device, control_case, recording, scene)
                        corpus.complete_publications(control)
                        control_proof = retained(control, evidence)
                        fault_controls[reset_control] = control
                        oracles.append(control_proof)
                    fault_checks(failed, checks, failed_proof, fault_controls.get(reset_control, cpu))
                    summaries.append(failed_proof)
            if target:
                for case in cases:
                    if case["variant"] == variant and case.get("refusal"):
                        refused = corpus.run_case(binary, device, case, recording, scene)
                        proof = retained(refused, evidence)
                        refusal_checks(refused, checks, proof)
                        summaries.append(proof)
    record(checks, "artifact-unchanged", {"binarySha256": corpus.parity.digest(binary)})
    # Every check above has a numerical, execution or rejection predicate. A
    # missing interface/oracle is a failure here, never an inferred pass.
    return {"id": contract["id"], "settings": contract["settings"], "passed": True,
        "processorGeneration": generation, "observedFrames": observed_frames,
        "configSha256": evidence.put({"configs": [item["config"] for item in summaries]}),
        "fixtureSha256": evidence.put({"fixtures": [item["fixtures"] for item in summaries]}),
        "lineageSha256": evidence.put({"lineageRecipes": [item["fixtures"] for item in summaries]}),
        "executionSha256": evidence.put({"runs": summaries}),
        "productsSha256": evidence.put({"products": [item["products"] for item in summaries]}),
        "oracleSha256": evidence.put({"oracles": oracles}), "checks": checks.finish()}
