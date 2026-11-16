#!/usr/bin/env python3
"""Offline, fail-closed admitted truth -> resident display-order target labels.

No radar values are inspected, no peaks are searched, and no controls are inferred.
The native helper owns snapshot ABI validation and snapshot/lineage identity.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

SPEED_OF_LIGHT_M_S = 299792458.0
MAX_JSON_BYTES = 64 << 20
MAX_FRAMES = 20
# Fixed diagnostic join policy; not fit/tuned against radar output.
MAX_FRAME_OFFSET_MS = 250.0
MAX_TRUTH_DELAY_DIFFERENCE_KM = 1e-6
MAX_TRUTH_DOPPLER_DIFFERENCE_HZ = 1e-6
MAX_FREQUENCY_DIFFERENCE_HZ = 1.0
MAX_CLOCK_ROUNDING_MS = 1.0


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, name):
    if isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]*", value):
        value = int(value)
    require(type(value) is int and 0 <= value < 2**64, f"invalid integer: {name}")
    return value


def finite(value, name):
    require(type(value) in (int, float) and math.isfinite(value), f"nonfinite/missing {name}")
    return float(value)


def sha(value, name):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
            and value != "0" * 64, f"missing/canonical SHA256 required: {name}")
    return value


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(data):
    require(0 < len(data) <= MAX_JSON_BYTES, "JSON input exceeds 64 MiB bound")
    return json.loads(data, object_pairs_hook=unique_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"nonfinite JSON: {value}")))


def read_json(path):
    with Path(path).open("rb") as source:
        data = source.read(MAX_JSON_BYTES + 1)
    return parse_json(data), hashlib.sha256(data).hexdigest()


def nearest(axis, value, name):
    require(isinstance(axis, list) and len(axis) >= 2, f"missing {name} axis support")
    values = [finite(item, f"{name} axis") for item in axis]
    require(all(a < b for a, b in zip(values, values[1:])), f"unordered/duplicate {name} axis")
    require(values[0] <= value <= values[-1], f"truth outside snapshot {name} axis")
    distances = [abs(item - value) for item in values]
    order = sorted(range(len(axis)), key=distances.__getitem__)
    require(not math.isclose(distances[order[0]], distances[order[1]], rel_tol=1e-12, abs_tol=1e-12),
            f"ambiguous equidistant {name} bins")
    return order[0]


def selected_truth(admission, pass_id):
    require(isinstance(pass_id, str) and 0 < len(pass_id.encode()) <= 256,
            "selected pass ID exceeds scorer string bound")
    require(admission.get("schema") == "vectorwarp-capture-admission-v1" and
            admission.get("state") == "admitted" and admission.get("integrity", {}).get("valid") is True,
            "recording admission/integrity is not valid")
    events = admission.get("events", [])
    selected = [item for item in events if item.get("pass_id") == pass_id]
    require(len(selected) == 1, "selected pass identity missing or ambiguous")
    event = selected[0]
    icao = event.get("icao", "").lower()
    require(re.fullmatch(r"[0-9a-f]{6}", icao) is not None, "selected pass ICAO identity missing")
    require(event.get("kind") == "adsb-eligible-aircraft", "selected pass is not aircraft evaluation truth")
    require(sum(item.get("icao", "").lower() == icao for item in events) == 1,
            "ambiguous same-aircraft pass identity")
    capture_ids = event.get("capture_ids")
    require(isinstance(capture_ids, list) and capture_ids and len(set(capture_ids)) == len(capture_ids),
            "selected pass capture identity missing or duplicate")
    points = {}
    for capture_id in capture_ids:
        require(sum(capture_id in item.get("capture_ids", []) for item in events) == 1,
                "capture belongs to ambiguous pass identities")
        capture = admission.get("captures", {}).get(capture_id, {})
        require(capture.get("valid") is True and capture.get("admission_state") == "admitted",
                "selected pass capture lacks admitted support")
        cpi_ms = finite(capture.get("cpi_ms"), "admitted CPI duration")
        require(cpi_ms > 0, "invalid admitted CPI duration")
        cpis = capture.get("cpis", [])
        timestamps = [integer(row.get("timestamp_ms"), "admitted CPI timestamp") for row in cpis]
        require(timestamps and len(set(timestamps)) == len(timestamps), "missing/duplicate admitted CPI support")
        truths = {}
        for row in capture.get("truth_samples", []):
            timestamp = integer(row.get("timestamp_ms"), "admitted truth timestamp")
            require(timestamp not in truths, "duplicate admitted truth endpoint")
            truths[timestamp] = row
        for timestamp in timestamps:
            require(timestamp in truths and timestamp not in points, "missing/duplicate selected pass truth support")
            points[timestamp] = {"truth": truths[timestamp], "capture_id": capture_id,
                                 "cpi_ms": cpi_ms, "decision_grade": capture.get("decision_grade") is True}
    return event, icao, points


def validate_provenance(admission, timeline, metadata, profile, receipt, hashes):
    require(receipt.get("schema") == "vectorwarp-iq-protection-replay-v1",
            "missing authoritative replay receipt")
    for name in ("admission", "timeline", "metadata", "profile"):
        require(sha(receipt.get(name + "_sha256"), name) == hashes[name], f"replay {name} hash differs")
    source = sha(receipt.get("recording_sha256"), "recording")
    require(source == admission["integrity"].get("data_sha256") == metadata.get("dataSha256"),
            "recording/admission/metadata source identity differs")
    require(hashes["metadata"] == admission["integrity"].get("sidecar_sha256"),
            "admission metadata hash differs")
    require(admission["integrity"].get("data_sha256_verified_by") == "independent-read-sha256",
            "admission lacks verified source identity")
    require(timeline.get("schema") in ("vectorwarp-recording-timeline-v1", "vectorwarp-recording-timeline-v2"),
            "unsupported recording timeline")
    require(profile.get("schema") == "vectorwarp-replay-profile-v2" and
            profile.get("configuration_complete") is True and timeline.get("replay_profile") == profile,
            "timeline RF/replay profile identity differs or is incomplete")
    require(profile.get("multi_illuminator", {}).get("enabled") is not True,
            "multi-illuminator sample clock mapping is unsupported")
    require(timeline.get("start_ms") == metadata.get("windowStart") and
            timeline.get("end_ms") == metadata.get("windowEnd"), "timeline recording window differs")
    rate = integer(metadata.get("sampleRate"), "metadata sample rate")
    require(rate > 0 and rate == timeline.get("sample_rate_hz") == profile.get("capture", {}).get("sample_rate_hz"),
            "recording sample geometry differs")
    frequency = finite(metadata.get("frequency"), "metadata frequency")
    require(frequency > 0 and frequency == timeline.get("frequency_hz") == profile.get("capture", {}).get("frequency_hz"),
            "recording RF geometry differs")
    final = receipt.get("final_status", {})
    drops = final.get("captureDroppedSamples")
    require(final.get("state") == "complete" and isinstance(drops, list) and drops
            and all(type(value) is int and value == 0 for value in drops),
            "replay receipt lacks lossless completion")
    return source, rate, frequency


def source_time(frame, metadata, receipt):
    """Use the original source sample clock, never replay's synthetic wall clock."""
    first = integer(frame.get("first_sample"), "snapshot first sample")
    end = integer(frame.get("end_sample"), "snapshot end sample")
    epoch = integer(frame.get("capture_epoch"), "snapshot capture epoch")
    rate = integer(frame.get("sample_rate_hz"), "snapshot sample rate")
    first_recorded = integer(metadata.get("firstEpochSample"), "recording first sample")
    end_recorded = integer(metadata.get("endEpochSample"), "recording end sample")
    recording_epoch = integer(metadata.get("epoch"), "recording epoch")
    require(end_recorded > first_recorded and end_recorded - first_recorded ==
            integer(metadata.get("samplesPerChannel"), "recording sample count"),
            "recording source sample count differs")
    # RecordingReader preserves native MCHT source coordinates when present;
    # the clip sidecar also retains recorder-local coordinates for eventTime.
    # Both origins are bound by the admitted metadata hash. Never guess their
    # offset or relabel a zero epoch when this explicit correspondence is absent.
    source_first, source_epoch = first_recorded, recording_epoch
    mapping = "recording_sample_origin"
    upstream = metadata.get("upstreamTiming")
    if upstream is not None:
        require(isinstance(upstream, dict) and upstream.get("protocol") == "MCHT-v1",
                "unsupported upstream sample-clock provenance")
        source_first = integer(upstream.get("firstSample"), "MCHT source first sample")
        source_epoch = integer(upstream.get("acquisitionEpoch"), "MCHT acquisition epoch")
        mapping = "metadata_bound_native_mcht_to_recording_origin"
    source_end = source_first + end_recorded - first_recorded
    require(source_end < 2**64 and source_first <= first < end <= source_end and epoch == source_epoch,
            "snapshot source sample interval/epoch differs from recording")
    clock_first = first_recorded + first - source_first
    clock_end = first_recorded + end - source_first
    require(end - first == integer(frame.get("clutter_samples"), "snapshot CPI samples"),
            "snapshot source sample support differs from CPI")
    matching = [row for row in receipt.get("frames", {}).get("timing", [])
                if row.get("captureSamplePositionAvailable") is True and
                integer(row.get("captureEpoch"), "receipt capture epoch") == epoch and
                integer(row.get("captureFirstEpochSample"), "receipt first sample") == first and
                integer(row.get("captureEndEpochSample"), "receipt end sample") == end]
    require(len(matching) == 1, "exact snapshot/receipt sample correspondence missing or ambiguous")
    require(matching[0].get("inputDiscontinuity") is not True,
            "snapshot endpoint has replay input discontinuity")
    clock = metadata.get("eventTime", {})
    require(clock.get("schema") == "vectorwarp-event-time-v1" and clock.get("estimateAvailable") is True,
            "recording source-time clock support missing; replay signal timestamp is not recording UTC")
    require(integer(clock.get("epoch"), "clock epoch") == recording_epoch and
            integer(clock.get("firstSample"), "clock first sample") == first_recorded and
            integer(clock.get("endSample"), "clock end sample") == end_recorded and
            integer(clock.get("sampleRateHz"), "clock sample rate") == rate,
            "recording source-time clock provenance differs")
    require(isinstance(clock.get("source"), str) and clock["source"], "recording clock source missing")
    origin_ns = integer(clock.get("epochEstimateNs"), "source clock origin")
    recording_midpoint_ns = origin_ns + (first_recorded + end_recorded) * 500000000 / rate
    require(abs(recording_midpoint_ns - integer(clock.get("estimateNs"), "recording midpoint time")) <= MAX_CLOCK_ROUNDING_MS * 1e6,
            "recording source clock midpoint mismatch")
    require(abs(recording_midpoint_ns / 1e6 - (integer(metadata.get("windowStart"), "recording start") +
                integer(metadata.get("windowEnd"), "recording end")) / 2) <= MAX_CLOCK_ROUNDING_MS,
            "recording source clock/window mismatch")
    return origin_ns / 1e6 + (clock_first + clock_end) * 500.0 / rate, {
        **clock, "sample_mapping": mapping, "recording_clock_first_sample": str(clock_first),
        "recording_clock_end_sample": str(clock_end)}


def map_labels(geometry, admission, timeline, metadata, profile, receipt, hashes, pass_id):
    event, icao, truth = selected_truth(admission, pass_id)
    require(pass_id in metadata.get("passIds", []) and event.get("event_id") in metadata.get("eventIds", []),
            "selected pass identity absent from source metadata")
    source, rate, frequency = validate_provenance(admission, timeline, metadata, profile, receipt, hashes)
    require(geometry.get("schema") == "vectorwarp-resident-snapshot-geometry-v1" and
            geometry.get("evaluation_only") is True, "native validated geometry missing")
    frames = geometry.get("frames", [])
    require(isinstance(frames, list) and 0 < len(frames) <= MAX_FRAMES, "snapshot frame count outside 1..20")
    endpoints = timeline.get("frames", [])
    require(isinstance(endpoints, list) and endpoints, "timeline endpoint support missing")
    timeline_times = [integer(row.get("timestamp_ms"), "timeline timestamp") for row in endpoints]
    require(all(a < b for a, b in zip(timeline_times, timeline_times[1:])),
            "timeline duplicate, unordered, or ambiguous illuminator endpoints")
    entries = receipt.get("resident_snapshot_export", {}).get("files", [])
    require(isinstance(entries, list) and entries, "replay receipt snapshot export identity missing")
    identities, joined_times, output = set(), set(), []
    for frame in frames:
        identity = sha(frame.get("snapshot_and_lineage_sha256"), "snapshot and lineage")
        require(identity not in identities, "duplicate snapshot/lineage identity")
        identities.add(identity)
        require(frame.get("source_sha256") == source and frame.get("rf_sha256") == hashes["profile"],
                "snapshot source/RF profile identity differs")
        require(integer(frame.get("sample_rate_hz"), "snapshot sample rate") == rate and
                abs(integer(frame.get("frequency_hz"), "snapshot RF") - frequency) <= MAX_FREQUENCY_DIFFERENCE_HZ,
                "snapshot RF/sample geometry differs")
        snapshot_hash = sha(frame.get("snapshot_sha256"), "snapshot")
        export = [row for row in entries if row.get("sha256") == snapshot_hash]
        require(len(export) == 1, "snapshot missing/duplicated in authoritative replay receipt")
        export = export[0]
        require(integer(export.get("logical_frame"), "export logical frame") == integer(frame.get("logical_frame"), "logical frame") and
                integer(export.get("consumed_sequence"), "export sequence") == integer(frame.get("consumed_sequence"), "snapshot sequence") and
                export.get("lineage_sha256") == frame.get("lineage_file_sha256"),
                "replay snapshot lineage/sequence identity differs")
        signal_timestamp = integer(frame.get("signal_timestamp_ms"), "snapshot signal timestamp")
        require(signal_timestamp > 0, "snapshot signal identity missing")
        timestamp, clock = source_time(frame, metadata, receipt)
        nearest_times = sorted(timeline_times, key=lambda value: abs(value - timestamp))
        selected = nearest_times[0]
        require(len(nearest_times) == 1 or not math.isclose(abs(selected - timestamp), abs(nearest_times[1] - timestamp),
                                                        rel_tol=0, abs_tol=1e-6),
                "ambiguous nearest timeline endpoint")
        require(selected in truth, "nearest timeline endpoint lacks selected pass admission support")
        support = truth[selected]
        require(abs((integer(frame["end_sample"], "end sample") - integer(frame["first_sample"], "first sample")) *
                    1000.0 / rate - support["cpi_ms"]) <= MAX_CLOCK_ROUNDING_MS,
                "admission/snapshot CPI duration differs")
        require(abs(selected - timestamp) <= min(MAX_FRAME_OFFSET_MS, support["cpi_ms"] / 2),
                "snapshot source-time/timeline mismatch exceeds 250 ms or half CPI")
        require(selected not in joined_times, "duplicate timeline endpoint selected for snapshots")
        joined_times.add(selected)
        endpoint = endpoints[timeline_times.index(selected)]
        require(endpoint.get("illuminator") in (None, ""), "unsupported illuminator endpoint")
        predictions = endpoint.get("delay_doppler")
        require(isinstance(predictions, dict), "timeline evaluation truth missing")
        matches = [row for key, row in predictions.items() if key.lower() == icao or
                   (isinstance(row, dict) and row.get("hex", "").lower() == icao)]
        require(len(matches) == 1 and isinstance(matches[0], dict), "selected pass timeline truth missing or ambiguous")
        predicted, admitted = matches[0], support["truth"]
        require(predicted.get("hex", "").lower() == icao, "timeline truth ICAO identity differs")
        for row in (predicted, admitted):
            require(row.get("delay_definition") == "excess_bistatic_path_km" and
                    abs(finite(row.get("frequency_hz"), "truth RF") - frequency) <= MAX_FREQUENCY_DIFFERENCE_HZ,
                    "truth RF/delay definition differs or is missing")
            require(isinstance(row.get("provenance"), dict) and row["provenance"].get("source_id"),
                    "truth observation provenance missing")
        require(predicted["provenance"]["source_id"] == admitted["provenance"]["source_id"] and
                abs(finite(predicted.get("timestamp"), "timeline truth position time") -
                    finite(admitted.get("position_timestamp_s"), "admitted truth position time")) <= 1e-6,
                "admission/timeline truth observation identity differs")
        delay = finite(admitted.get("delay_km"), "admitted delay")
        doppler = finite(admitted.get("doppler_hz"), "admitted Doppler")
        require(delay >= 0 and abs(delay - finite(predicted.get("delay"), "timeline delay")) <= MAX_TRUTH_DELAY_DIFFERENCE_KM and
                abs(doppler - finite(predicted.get("doppler"), "timeline Doppler")) <= MAX_TRUTH_DOPPLER_DIFFERENCE_HZ,
                "admission/timeline truth endpoint mismatch")
        # Excess bistatic path has NO monostatic factor of two. delayAxis is
        # stored in samples (possibly negative/offset), not zero-based columns.
        delay_samples = delay * 1000.0 * rate / SPEED_OF_LIGHT_M_S
        column = nearest(frame.get("delay_axis_samples"), delay_samples, "delay")
        row = nearest(frame.get("doppler_axis_hz"), doppler, "Doppler")
        evidence = f"admission:{hashes['admission']};timeline:{hashes['timeline']};t_ms:{selected}"
        output.append({"snapshot_and_lineage_sha256": identity,
                       "points": [{"kind": "target", "id": pass_id, "evidence": evidence, "row": row, "column": column}],
                       "join": {"capture_id": support["capture_id"], "icao": icao,
                                "signal_timestamp_ms": str(signal_timestamp),
                                "capture_epoch": frame["capture_epoch"],
                                "first_sample": frame["first_sample"], "end_sample": frame["end_sample"],
                                "sample_clock_mapping": clock["sample_mapping"],
                                "recording_clock_first_sample": clock["recording_clock_first_sample"],
                                "recording_clock_end_sample": clock["recording_clock_end_sample"],
                                "recording_sample_midpoint_ms": timestamp, "timeline_timestamp_ms": selected,
                                "timeline_offset_ms": selected - timestamp,
                                "clock_source": clock.get("source"), "clock_exact": clock.get("exact") is True,
                                "clock_uncertainty_bounded": clock.get("uncertaintyBounded") is True,
                                "admission_decision_grade": support["decision_grade"],
                                "truth_delay_km": delay, "truth_doppler_hz": doppler,
                                "truth_delay_samples": delay_samples,
                                "selected_delay_samples": frame["delay_axis_samples"][column],
                                "selected_doppler_hz": frame["doppler_axis_hz"][row]}})
    return {"version": 1, "evaluation_only": True, "inference_features": False,
            "promotion_authorized": False, "control_labels_inferred": False,
            "provenance": {**hashes, "recording_sha256": source},
            "thresholds": {"maximum_frame_offset_ms": MAX_FRAME_OFFSET_MS,
                           "maximum_cpi_fraction": .5, "maximum_rf_difference_hz": MAX_FREQUENCY_DIFFERENCE_HZ,
                           "truth_delay_agreement_km": MAX_TRUTH_DELAY_DIFFERENCE_KM,
                           "truth_doppler_agreement_hz": MAX_TRUTH_DOPPLER_DIFFERENCE_HZ}, "frames": output}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("admission", "timeline", "metadata", "profile", "replay-receipt", "geometry-helper", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--pass-id", required=True)
    parser.add_argument("snapshots", nargs="+", type=Path)
    args = parser.parse_args(argv)
    require(not args.output.exists(), "output already exists; refusing to replace labels")
    documents, hashes = {}, {}
    for name in ("admission", "timeline", "metadata", "profile", "replay_receipt"):
        documents[name], hashes[name] = read_json(getattr(args, name))
    require(0 < len(args.snapshots) <= MAX_FRAMES and len(set(path.resolve() for path in args.snapshots)) == len(args.snapshots),
            "duplicate snapshot paths or count outside 1..20")
    helper = args.geometry_helper.resolve(strict=True)
    hashes["geometry_helper_sha256"] = file_sha256(helper)
    sidecars = {str(path.resolve()): file_sha256(Path(str(path) + ".vwrl")) for path in args.snapshots}
    completed = subprocess.run([str(helper), *map(str, args.snapshots)], capture_output=True, timeout=60, check=False)
    require(completed.returncode == 0, "native snapshot validation failed: " + completed.stderr.decode(errors="replace")[-1000:])
    require(file_sha256(helper) == hashes["geometry_helper_sha256"], "geometry helper changed during validation")
    geometry = parse_json(completed.stdout)
    require({frame.get("snapshot") for frame in geometry.get("frames", [])} == set(sidecars),
            "native geometry helper returned different snapshot paths")
    for frame in geometry["frames"]:
        path = frame["snapshot"]
        require(file_sha256(Path(path + ".vwrl")) == sidecars[path], "lineage file changed during validation")
        frame["lineage_file_sha256"] = sidecars[path]
    hashes["mapper_sha256"] = file_sha256(Path(__file__))
    result = map_labels(geometry, documents["admission"], documents["timeline"], documents["metadata"],
                        documents["profile"], documents["replay_receipt"], hashes, args.pass_id)
    encoded = json.dumps(result, indent=2, allow_nan=False) + "\n"
    require(len(encoded.encode()) <= 1 << 20, "evaluation output exceeds scorer 1 MiB bound")
    with args.output.open("x") as output:
        output.write(encoded)
    print(json.dumps({"output": str(args.output), "frames": len(result["frames"]), "evaluation_only": True}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError, AttributeError, OverflowError, subprocess.SubprocessError) as error:
        print(f"resident truth labels rejected: {error}", file=sys.stderr)
        sys.exit(1)
