#!/usr/bin/env python3
"""Build a strict production-replay receipt from a native full-bank grid export.

This program does not form beams or reconstruct radar samples.  The native
export must contain every baseline and candidate beam power for the identical
CUT and every CA-CFAR training cell.  This wrapper verifies the complete grid,
applies the production beam-max and CA-CFAR/trial contract, and binds all input
artifacts into one importable receipt.
"""

import argparse
import hashlib
import json
import math
import os
import subprocess
from pathlib import Path


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def file_bytes(path):
    return path.read_bytes()


def canonical_hash(value):
    # The receipt is imported by JavaScript, whose JSON number rendering turns
    # an integral IEEE-754 value such as 539000000.0 into `539000000`.  Python's
    # json.dumps retains the `.0`, so using it here creates a hash which the
    # importer can never reproduce.  Delegate only canonical serialization to
    # the same Node/JSON.stringify contract used at the trust boundary.
    program = ("const fs=require('fs'); const v=JSON.parse(fs.readFileSync(0,'utf8')); "
               "const c=x=>Array.isArray(x)?'['+x.map(c).join(',')+']':"
               "x&&typeof x==='object'?'{'+Object.keys(x).sort().map(k=>"
               "JSON.stringify(k)+':'+c(x[k])).join(',')+'}':JSON.stringify(x); "
               "process.stdout.write(c(v));")
    try:
        encoded = json.dumps(value, allow_nan=False, separators=(",", ":")).encode()
        result = subprocess.run(["node", "-e", program], input=encoded,
                                check=True, capture_output=True)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise ValueError(f"cannot canonicalize receipt value with Node JSON contract: {error}") from error
    return sha256_bytes(result.stdout)


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def cfar_survival(alpha, training_cells, integration_cpis):
    x = training_cells / (training_cells + alpha)
    if not 0 < x <= 1:
        return 0
    total = training_cells * integration_cpis + integration_cpis - 1
    term = x ** total
    result = term
    ratio = (1 - x) / x
    for index in range(1, integration_cpis):
        term *= (total - index + 1) / index * ratio
        result += term
    return result


def cfar_alpha(pfa, training_cells, integration_cpis):
    low, high = 0.0, 1.0
    while cfar_survival(high, training_cells, integration_cpis) > pfa and high < 1e12:
        high *= 2
    for _ in range(96):
        middle = (low + high) / 2
        if cfar_survival(middle, training_cells, integration_cpis) > pfa:
            low = middle
        else:
            high = middle
    return high


def load_json(path, label):
    try:
        return json.loads(file_bytes(path))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{label} is not one JSON document: {error}") from error


def config_identity(config):
    capture = config.get("capture", {})
    device = capture.get("device", {})
    channels = device.get("surveillance_channels")
    heimdall = device.get("heimdall", {})
    gain = {"requested_common_gain": heimdall.get("gain", "keep"),
            "requested_channel_gains": heimdall.get("channel_gains")}
    if (not isinstance(channels, list) or not 2 <= len(channels) <= 8 or
            any(not isinstance(channel, int) or isinstance(channel, bool) or channel < 0
                for channel in channels) or len(set(channels)) != len(channels)):
        raise ValueError("config requires 2-8 explicit surveillance channels")
    if not finite(gain["requested_common_gain"]):
        raise ValueError("config requires an explicit fixed requested common gain")
    vector = gain["requested_channel_gains"]
    if vector is not None:
        if not isinstance(vector, list) or not all(finite(value) for value in vector):
            raise ValueError("config has no exact requested channel gain vector")
        if len(vector) == device.get("channel_count") and all(channel < len(vector) for channel in channels):
            gain["requested_capture_channel_gains"] = list(vector)
            gain["requested_channel_gains"] = [vector[channel] for channel in channels]
        elif len(vector) != len(channels):
            raise ValueError("config has no exact requested surveillance gain vector")
    process = config.get("process", {})
    detector = process.get("detection", {})
    fusion = process.get("surveillance_fusion", {})
    integration = detector.get("integration_cpis", 1)
    motion_bins = detector.get("integration_motion_bins", 0)
    tbd = detector.get("track_before_detect", {})
    geometry = device.get("array_geometry", {})
    beam_count = fusion.get("beams")
    direct_null = fusion.get("direct_path_null_db", 0)
    if not finite(capture.get("fc")) or not finite(capture.get("fs")):
        raise ValueError("config requires exact frequency and sample rate")
    if (fusion.get("mode") != "coherent_beam_bank" or integration != 1 or motion_bins != 0 or
            tbd.get("enable", False) is not False):
        raise ValueError("native replay currently requires single-CPI coherent_beam_bank without motion/TBD")
    if (not isinstance(detector.get("nGuard"), int) or detector["nGuard"] < 0 or
            detector["nGuard"] > 127 or not isinstance(detector.get("nTrain"), int) or
            not 1 <= detector["nTrain"] <= 127):
        raise ValueError("config requires exact CA-CFAR guard and training geometry")
    if (not isinstance(beam_count, int) or isinstance(beam_count, bool) or beam_count < 1 or
            not finite(direct_null) or not 0 <= direct_null <= 40):
        raise ValueError("config requires exact beam count and direct-path null")
    if (geometry.get("mapping_confirmed") is not True or
            geometry.get("geometry_confirmed") is not True or
            geometry.get("orientation_confirmed") is not True or
            not finite(geometry.get("x_axis_bearing_deg_true"))):
        raise ValueError("config requires confirmed installed array geometry and orientation")
    by_channel = {element.get("daq_channel"): element.get("position")
                  for element in geometry.get("elements", []) if isinstance(element, dict)}
    selected_positions = [by_channel.get(channel) for channel in channels]
    if any(not isinstance(position, list) or len(position) != 3 or
           not all(finite(value) for value in position) for position in selected_positions):
        raise ValueError("config lacks exact selected surveillance element positions")
    location = config.get("location", {})
    receiver, transmitter = location.get("rx", {}), location.get("tx", {})
    receiver_location = [receiver.get(key) for key in ("latitude", "longitude", "altitude")]
    transmitter_location = [transmitter.get(key) for key in ("latitude", "longitude", "altitude")]
    if not all(finite(value) for value in receiver_location + transmitter_location):
        raise ValueError("config requires exact receiver and fixed-transmitter coordinates")
    if receiver_location[:2] == transmitter_location[:2]:
        raise ValueError("receiver and fixed transmitter cannot share a horizontal location")
    radians = math.pi / 180
    rx_lat, tx_lat = receiver_location[0] * radians, transmitter_location[0] * radians
    delta_lon = (transmitter_location[1] - receiver_location[1]) * radians
    true_bearing = math.atan2(math.sin(delta_lon) * math.cos(tx_lat),
        math.cos(rx_lat) * math.sin(tx_lat) - math.sin(rx_lat) * math.cos(tx_lat) * math.cos(delta_lon))
    direct_azimuth = math.remainder(geometry["x_axis_bearing_deg_true"] * radians - true_bearing,
                                    2 * math.pi)
    return {"frequency_hz": capture["fc"], "sample_rate_hz": capture["fs"],
            "channels": len(channels), "channel_layout": channels, "requested_gain": gain,
            "fusion_mode": fusion["mode"], "guard_cells": detector["nGuard"],
            "training_cells_per_side": detector["nTrain"], "integration_cpis": integration,
            "motion_trials": 1, "beam_count": beam_count,
            "positions_m": selected_positions,
            "x_axis_bearing_deg_true": geometry["x_axis_bearing_deg_true"],
            "receiver_location": receiver_location, "transmitter_location": transmitter_location,
            "direct_path_azimuth_rad": direct_azimuth, "direct_path_null_db": direct_null}


def exact_offsets(detector):
    guard, training = detector["guard_cells"], detector["training_cells_per_side"]
    if not isinstance(guard, int) or guard < 0 or not isinstance(training, int) or training < 1:
        raise ValueError("native export has invalid CA-CFAR guard/training geometry")
    return list(range(-guard - training, -guard)) + list(range(guard + 1, guard + training + 1))


def validate_power_vector(value, beams, label):
    if not isinstance(value, list) or len(value) != beams or not all(finite(item) and item >= 0 for item in value):
        raise ValueError(f"{label} does not contain every finite nonnegative beam power")
    return value


def summarize_arm(arm, bank, offsets, alpha, cut_id):
    if not isinstance(arm, dict) or arm.get("bank_sha256") != bank["sha256"]:
        raise ValueError(f"{cut_id} arm is not bound to its exact beam bank")
    cut = validate_power_vector(arm.get("cut_power_by_beam"), bank["beams"], f"{cut_id} CUT")
    training = arm.get("training_power_by_offset_beam")
    if not isinstance(training, list) or len(training) != len(offsets):
        raise ValueError(f"{cut_id} does not contain the complete CA-CFAR training grid")
    by_offset = {}
    for row in training:
        if not isinstance(row, dict) or row.get("offset") not in offsets or row.get("offset") in by_offset:
            raise ValueError(f"{cut_id} has duplicate or unexpected CA-CFAR training offsets")
        by_offset[row["offset"]] = validate_power_vector(row.get("power_by_beam"), bank["beams"],
                                                          f"{cut_id} training offset")
    if set(by_offset) != set(offsets):
        raise ValueError(f"{cut_id} is missing CA-CFAR training offsets")
    cut_power = max(cut)
    training_maxima = [max(by_offset[offset]) for offset in offsets]
    training_mean = sum(training_maxima) / len(training_maxima)
    threshold = alpha * training_mean
    return {"bank_sha256": bank["sha256"], "beams_evaluated": bank["beams"],
            "cut_power": cut_power, "training_mean_power": training_mean,
            "alpha": alpha, "threshold": threshold, "detected": cut_power > threshold}


def generate(corpus_path, candidate_path, iq_path, config_path, native_export_path,
             native_generator_path, expected_generator_sha256, pass_id, label):
    if not native_generator_path.is_file() or not os.access(native_generator_path, os.X_OK):
        raise ValueError("native generator path is not an executable file")
    corpus_bytes, candidate_bytes = file_bytes(corpus_path), file_bytes(candidate_path)
    iq_bytes, config_bytes = file_bytes(iq_path), file_bytes(config_path)
    export_bytes, generator_bytes = file_bytes(native_export_path), file_bytes(native_generator_path)
    candidate = load_json(candidate_path, "candidate")
    config = load_json(config_path, "config")
    native = load_json(native_export_path, "native grid export")
    identity = config_identity(config)
    if not isinstance(pass_id, str) or not pass_id.strip() or label not in ("positive", "miss", "subthreshold"):
        raise ValueError("receipt requires an explicit pass identity and supported outcome label")
    if (candidate.get("schema") != "vectorwarp-detection-manifold-runtime-v1" or
            candidate.get("runtime_compatible") is not True):
        raise ValueError("candidate is not a runtime-compatible dense beam bank")
    provenance = native.get("provenance", {})
    hashes = {"corpus_sha256": sha256_bytes(corpus_bytes),
              "candidate_model_sha256": sha256_bytes(candidate_bytes),
              "iq_sha256": sha256_bytes(iq_bytes), "config_sha256": sha256_bytes(config_bytes),
              "native_export_sha256": sha256_bytes(export_bytes),
              "generator_sha256": sha256_bytes(generator_bytes),
              "receipt_generator_sha256": sha256_bytes(Path(__file__).read_bytes())}
    replay_policy = config.get("process", {}).get("surveillance_fusion", {}).get(
        "detection_manifold_replay", {})
    allowlist = replay_policy.get("trusted_native_generator_sha256")
    if (not isinstance(expected_generator_sha256, str) or len(expected_generator_sha256) != 64 or
            any(character not in "0123456789abcdef" for character in expected_generator_sha256) or
            not isinstance(allowlist, list) or not allowlist or
            any(not isinstance(value, str) or len(value) != 64 or
                any(character not in "0123456789abcdef" for character in value) for value in allowlist) or
            hashes["generator_sha256"] != expected_generator_sha256 or
            hashes["generator_sha256"] not in allowlist):
        raise ValueError("native generator is not operator-pinned and config-allowlisted")
    hashes["generator_trust_policy_sha256"] = canonical_hash(allowlist)
    hashes.update({"frequency_sha256": canonical_hash(identity["frequency_hz"]),
                   "sample_rate_sha256": canonical_hash(identity["sample_rate_hz"]),
                   "channel_layout_sha256": canonical_hash(identity["channel_layout"])})
    if (native.get("schema") != "vectorwarp-native-detection-full-bank-grid-v1" or
            native.get("production_equivalent") is not True or native.get("same_iq") is not True or
            native.get("truth_selected_beam") is not False):
        raise ValueError("native export is not a production-equivalent same-IQ full-bank grid")
    for key in ("candidate_model_sha256", "iq_sha256", "config_sha256", "generator_sha256",
                "frequency_sha256", "sample_rate_sha256", "channel_layout_sha256"):
        if provenance.get(key) != hashes[key]:
            raise ValueError(f"native export {key} does not match the exact input artifact")
    exported = native.get("configuration", {})
    if not isinstance(exported.get("config_fingerprint"), str) or not exported["config_fingerprint"]:
        raise ValueError("native export has no exact config fingerprint")
    for key in ("frequency_hz", "sample_rate_hz", "channels", "channel_layout"):
        if exported.get(key) != identity[key]:
            raise ValueError(f"native export {key} does not match config")
    if exported.get("fusion_mode") != identity["fusion_mode"]:
        raise ValueError("native export fusion mode does not match config")
    for key in ("positions_m", "x_axis_bearing_deg_true", "receiver_location",
                "transmitter_location", "direct_path_azimuth_rad", "direct_path_null_db"):
        if exported.get(key) != identity[key]:
            raise ValueError(f"native export {key} does not match installed config")
    gain = exported.get("receiver_gain_v2", {})
    if ({key: gain.get(key) for key in identity["requested_gain"]} != identity["requested_gain"] or
            not isinstance(gain.get("actual_channel_gains_db"), list) or
            len(gain["actual_channel_gains_db"]) != identity["channels"] or
            not all(finite(value) for value in gain["actual_channel_gains_db"])):
        raise ValueError("native export does not attest exact requested and actual channel gains")
    hashes["gain_sha256"] = canonical_hash(gain)
    if provenance.get("gain_sha256") != hashes["gain_sha256"]:
        raise ValueError("native export gain hash does not match its exact gain receipt")
    detector, banks = native.get("detector", {}), native.get("banks", {})
    if detector.get("contract") != "vectorwarp-production-ca-cfar-full-bank-v1":
        raise ValueError("native export does not use the production detector contract")
    configured_pfa = config.get("process", {}).get("detection", {}).get("pfa")
    trials = config.get("process", {}).get("surveillance_fusion", {}).get("effective_beam_trials")
    if (not finite(configured_pfa) or not 0 < configured_pfa < 1 or
            not finite(trials) or trials < 1 or detector.get("configured_pfa") != configured_pfa or
            detector.get("effective_beam_trials") != trials):
        raise ValueError("native detector PFA/trial accounting does not match config")
    motion_trials = detector.get("motion_trials")
    integration_cpis = detector.get("integration_cpis")
    if (motion_trials != identity["motion_trials"] or
            integration_cpis != identity["integration_cpis"] or
            detector.get("guard_cells") != identity["guard_cells"] or
            detector.get("training_cells_per_side") != identity["training_cells_per_side"]):
        raise ValueError("native export has unsupported motion/integration accounting")
    expected_pfa = configured_pfa / (trials * motion_trials)
    if not math.isclose(detector.get("detector_pfa", -1), expected_pfa, rel_tol=1e-12, abs_tol=1e-15):
        raise ValueError("native detector PFA omits beam or motion trials")
    for name in ("baseline", "candidate"):
        bank = banks.get(name, {})
        if (not isinstance(bank.get("beams"), int) or bank["beams"] < 1 or
                not isinstance(bank.get("sha256"), str) or len(bank["sha256"]) != 64 or
                any(character not in "0123456789abcdef" for character in bank["sha256"])):
            raise ValueError(f"native export has no exact {name} bank identity")
    if banks["baseline"]["beams"] != identity["beam_count"]:
        raise ValueError("native beam count does not match configured fusion bank")
    baseline_descriptor = banks["baseline"].get("descriptor")
    if (not isinstance(baseline_descriptor, dict) or
            baseline_descriptor.get("frequency_hz") != identity["frequency_hz"] or
            baseline_descriptor.get("beams") != banks["baseline"]["beams"] or
            baseline_descriptor.get("positions_m") != exported.get("positions_m") or
            baseline_descriptor.get("direct_path_azimuth_rad") != exported.get("direct_path_azimuth_rad") or
            baseline_descriptor.get("direct_path_null_db") != exported.get("direct_path_null_db") or
            canonical_hash(baseline_descriptor) != banks["baseline"]["sha256"]):
        raise ValueError("native ideal baseline bank identity is not bound to its exact geometry")
    responses = candidate.get("responses")
    native_responses = banks.get("candidate", {}).get("responses")
    if (not isinstance(responses, list) or len(responses) != banks["candidate"]["beams"] or
            any(not isinstance(response, list) or len(response) != identity["channels"] or
                any(not isinstance(value, list) or len(value) != 2 or
                    not all(finite(component) for component in value) for value in response)
                for response in responses) or
            native_responses != responses or
            banks["candidate"]["sha256"] != canonical_hash(native_responses) or
            candidate.get("bank_sha256") != banks["candidate"]["sha256"] or
            candidate.get("beams") != banks["candidate"]["beams"]):
        raise ValueError("native candidate bank is not the exact dense runtime candidate contents")
    support = banks["candidate"].get("elevation_range_deg")
    if (banks["candidate"].get("runtime_compatible") is not True or
            not isinstance(support, list) or len(support) != 2 or
            not all(finite(value) for value in support) or support[0] > support[1]):
        raise ValueError("candidate bank is not runtime-compatible over an explicit elevation range")
    if candidate.get("elevation_range_deg") != support:
        raise ValueError("native candidate elevation support differs from the runtime candidate")
    if banks["baseline"]["beams"] != banks["candidate"]["beams"] or trials > banks["baseline"]["beams"]:
        raise ValueError("baseline/candidate beam banks or effective trials are inconsistent")
    offsets = exact_offsets(detector)
    alpha = cfar_alpha(expected_pfa, len(offsets), integration_cpis)
    if not math.isclose(detector.get("native_alpha", -1), alpha,
                        rel_tol=1e-12, abs_tol=1e-15):
        raise ValueError("native emitter CA-CFAR alpha disagrees with the production primitive")
    cuts, cut_ids, target_count, nuisance_count = [], set(), 0, 0
    for source in native.get("cuts", []):
        role = source.get("role")
        if (role not in ("target", "nuisance") or not isinstance(source.get("id"), str) or
                not source["id"] or source["id"] in cut_ids):
            raise ValueError("native export has an invalid CUT role or identity")
        cut_ids.add(source["id"])
        target_count += role == "target"
        nuisance_count += role == "nuisance"
        cuts.append({"id": source["id"], "role": role,
                     "input_grid_sha256": hashes["native_export_sha256"],
                     "training_cells": len(offsets),
                     "baseline": summarize_arm(source.get("baseline"), banks["baseline"], offsets,
                                                alpha, source["id"]),
                     "candidate": summarize_arm(source.get("candidate"), banks["candidate"], offsets,
                                                 alpha, source["id"])})
    truth = native.get("truth_label", {})
    target_ids = [cut["id"] for cut in native.get("cuts", []) if cut.get("role") == "target"]
    if (target_count != 1 or nuisance_count < 1 or
            truth.get("selection") != "target-cut-only-no-beam-selection" or
            truth.get("target_cut_id") != target_ids[0] or
            not finite(truth.get("local_azimuth_deg")) or not finite(truth.get("elevation_deg"))):
        raise ValueError("truth may label exactly one target CUT/pass but may not select a beam")
    if not support[0] <= truth["elevation_deg"] <= support[1]:
        raise ValueError("truth elevation is outside candidate bank support")
    replay_id = sha256_bytes("\0".join([hashes[key] for key in ("corpus_sha256",
        "candidate_model_sha256", "iq_sha256", "native_export_sha256", "generator_sha256")] +
        [pass_id]).encode())
    return {"schema": "vectorwarp-detection-manifold-production-replay-v1",
            "pass": {"id": pass_id, "label": label, "truth": truth},
            "provenance": {"source": "vectorwarp-native-full-bank-replay",
                "independent_from_positive_corpus": True, "replay_id": replay_id,
                "input_grid_sha256": hashes["native_export_sha256"], **hashes},
            "configuration": {"config_fingerprint": exported.get("config_fingerprint"),
                "frequency_hz": identity["frequency_hz"],
                "sample_rate_hz": identity["sample_rate_hz"], "channels": identity["channels"],
                "channel_layout": identity["channel_layout"], "fusion_mode": identity["fusion_mode"],
                "beam_count": identity["beam_count"], "positions_m": identity["positions_m"],
                "x_axis_bearing_deg_true": identity["x_axis_bearing_deg_true"],
                "receiver_location": identity["receiver_location"],
                "transmitter_location": identity["transmitter_location"],
                "direct_path_azimuth_rad": identity["direct_path_azimuth_rad"],
                "direct_path_null_db": identity["direct_path_null_db"],
                "receiver_gain_v2": gain},
            "detector": {"contract": detector["contract"], "configured_pfa": configured_pfa,
                "detector_pfa": expected_pfa, "effective_beam_trials": trials,
                "motion_trials": motion_trials, "integration_cpis": integration_cpis,
                "guard_cells": detector["guard_cells"],
                "training_cells_per_side": detector["training_cells_per_side"]},
            "banks": banks, "replay": {"same_iq": True, "complete_grid": True,
                "truth_selected_beam": False, "support_complete": True, "cuts": cuts}}


def main():
    parser = argparse.ArgumentParser()
    for name in ("corpus", "candidate", "iq", "config", "native-export", "native-generator", "output"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--pass-id", required=True)
    parser.add_argument("--expected-generator-sha256", required=True)
    parser.add_argument("--label", required=True, choices=("positive", "miss", "subthreshold"))
    args = parser.parse_args()
    receipt = generate(args.corpus, args.candidate, args.iq, args.config, args.native_export,
                       args.native_generator, args.expected_generator_sha256, args.pass_id, args.label)
    temporary = args.output.with_name(f"{args.output.name}.tmp")
    temporary.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    temporary.replace(args.output)


if __name__ == "__main__":
    main()
