#!/usr/bin/env python3
"""Validate IQ replay against known radar tracks and inspect pre-fusion complex CAF."""

import argparse
import cmath
import json
import math
import statistics
from pathlib import Path

C = 299792458.0
TAU = 2 * math.pi


def load_truth(path):
    document = json.loads(path.read_text().splitlines()[0])
    if len(document["truth"]) != 1:
        raise ValueError("calibrated replay requires exactly one truth aircraft")
    ident, truth = next(iter(document["truth"].items()))
    return ident, truth


def direct_path_azimuth(config):
    geometry = config["capture"]["device"]["array_geometry"]
    rx, tx = config["location"]["rx"], config["location"]["tx"]
    radians = math.pi / 180
    rx_lat, tx_lat = rx["latitude"] * radians, tx["latitude"] * radians
    delta_lon = (tx["longitude"] - rx["longitude"]) * radians
    true_bearing = math.atan2(math.sin(delta_lon) * math.cos(tx_lat),
        math.cos(rx_lat) * math.sin(tx_lat) - math.sin(rx_lat) *
        math.cos(tx_lat) * math.cos(delta_lon))
    return math.remainder(geometry["x_axis_bearing_deg_true"] * radians - true_bearing, TAU)


def positions(config):
    elements = sorted(config["capture"]["device"]["array_geometry"]["elements"],
                      key=lambda item: item["daq_channel"])
    return [item["position"] for item in elements]


def calibrated_weights(config, reference):
    fusion = config["process"]["surveillance_fusion"]
    count = len(reference)
    norm = sum(abs(value) ** 2 for value in reference)
    if not norm:
        raise ValueError("zero reference-weight norm")
    scale = 1 / math.sqrt(norm)
    wave_number = TAU * config["capture"]["fc"] / C
    direct = direct_path_azimuth(config)
    direct_x, direct_y = math.cos(direct), math.sin(direct)
    null_fraction = 1 - 10 ** (-fusion.get("direct_path_null_db", 0) / 20)
    output = []
    for beam in range(fusion["beams"]):
        angle = TAU * beam / fusion["beams"]
        target_x, target_y = math.cos(angle), math.sin(angle)
        steering = []
        projection = 0j
        for channel, position in enumerate(positions(config)):
            relative_phase = wave_number * (position[0] * (target_x - direct_x) +
                                             position[1] * (target_y - direct_y))
            value = reference[channel] * cmath.exp(-1j * relative_phase)
            steering.append(value)
            projection += reference[channel].conjugate() * value
        projection *= null_fraction / norm
        output.append([scale * (steering[channel] - projection * reference[channel]).conjugate()
                       for channel in range(count)])
    return output


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def best_beam(cutout, config, delay, doppler):
    row = nearest(cutout["doppler_hz"], doppler)
    column = nearest(cutout["delay_km"], delay)
    return best_beam_at(cutout, config, row, column)


def best_beam_at(cutout, config, row, column):
    samples = [complex(*channel[row][column]) for channel in cutout["channels"]]
    reference = [complex(*value) for value in cutout["reference_weights"]]
    candidates = [sum(weight * sample for weight, sample in zip(weights, samples))
                  for weights in calibrated_weights(config, reference)]
    beam = max(range(len(candidates)), key=lambda index: abs(candidates[index]) ** 2)
    return {"row": row, "column": column, "delay_km": cutout["delay_km"][column],
            "doppler_hz": cutout["doppler_hz"][row], "beam": beam,
            "value": candidates[beam]}


def p99(values):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(.99 * len(ordered)))]


def gamma_cfar_survival(alpha, training_cells, cpis):
    x = training_cells / (training_cells + alpha)
    total = training_cells * cpis + cpis - 1
    term = x ** total
    result = term
    ratio = (1 - x) / x
    for k in range(1, cpis):
        term *= (total - k + 1) / k * ratio
        result += term
    return result


def gamma_cfar_alpha(pfa, training_cells, cpis):
    low, high = 0.0, 1.0
    while gamma_cfar_survival(high, training_cells, cpis) > pfa:
        high *= 2
    for _ in range(96):
        middle = (low + high) / 2
        if gamma_cfar_survival(middle, training_cells, cpis) > pfa:
            low = middle
        else:
            high = middle
    return high


def tbd_cfar_evidence(cutouts, config, delay, doppler):
    """Recreate the resident fixed/moving-path CFAR decision on a wide cutout."""
    detection = config["process"]["detection"]
    tbd = detection["track_before_detect"]
    maximum = tbd.get("max_cpis", 8)
    lengths = [value for value in (2, 4, 8) if value <= maximum]
    delay_motion = tbd.get("delay_motion_bins", tbd.get("motion_bins", 1))
    doppler_motion = tbd.get("doppler_motion_bins", tbd.get("motion_bins", 1))
    trajectories = [(dr, dc) for dr in range(-doppler_motion, doppler_motion + 1)
                    for dc in range(-delay_motion, delay_motion + 1)]
    trials = len(lengths) * len(trajectories)
    effective_pfa = detection["pfa"] / config["process"]["surveillance_fusion"].get(
        "effective_beam_trials", 1) / trials
    rows, columns = len(cutouts[0]["doppler_hz"]), len(cutouts[0]["delay_km"])
    powers = [[[abs(best_beam_at(frame, config, row, column)["value"]) ** 2
                for column in range(columns)] for row in range(rows)]
              for frame in cutouts]
    target_row = nearest(cutouts[0]["doppler_hz"], doppler)
    target_column = nearest(cutouts[0]["delay_km"], delay)
    guard, train = detection["nGuard"], detection["nTrain"]
    best = {length: None for length in lengths}
    for length in lengths:
        alpha = None
        for end in range(length - 1, len(cutouts)):
            surface = [[0.0] * columns for _ in range(rows)]
            for row in range(rows):
                for column in range(columns):
                    path_values = []
                    for dr, dc in trajectories:
                        cells = [(row - dr * age, column - dc * age)
                                 for age in range(length)]
                        if all(0 <= rr < rows and 0 <= cc < columns for rr, cc in cells):
                            path_values.append(sum(powers[end - age][rr][cc]
                                for age, (rr, cc) in enumerate(cells)) / length)
                    surface[row][column] = max(path_values, default=0.0)
            training_columns = list(range(max(0, target_column - guard - train),
                                          max(0, target_column - guard)))
            training_columns += list(range(min(columns, target_column + guard + 1),
                                           min(columns, target_column + guard + train + 1)))
            if not training_columns:
                continue
            if alpha is None:
                alpha = gamma_cfar_alpha(effective_pfa, len(training_columns), length)
            noise = sum(surface[target_row][column] for column in training_columns) / len(training_columns)
            target_power = surface[target_row][target_column]
            training_power = sorted(surface[target_row][column] for column in training_columns)
            ratio = target_power / max(alpha * noise, 1e-300)
            item = {"ending_frame": end, "margin_db": 10 * math.log10(max(ratio, 1e-300)),
                    "target_over_mean_training_db": 10 * math.log10(max(
                        target_power / max(noise, 1e-300), 1e-300)),
                    "target_over_median_training_db": 10 * math.log10(max(
                        target_power / max(statistics.median(training_power), 1e-300), 1e-300)),
                    "alpha": alpha, "training_cells": len(training_columns)}
            if best[length] is None or item["margin_db"] > best[length]["margin_db"]:
                best[length] = item
    return {"motion_trials": trials, "effective_pfa_per_integrated_pass": effective_pfa,
            "selected_delay_km": cutouts[0]["delay_km"][target_column],
            "selected_doppler_hz": cutouts[0]["doppler_hz"][target_row],
            "best_by_cpis": {str(length): best[length] for length in lengths}}


def integrated_cell_evidence(cutouts, config, delay, doppler, guard=1):
    if not cutouts:
        return None
    center_row = nearest(cutouts[0]["doppler_hz"], doppler)
    center_column = nearest(cutouts[0]["delay_km"], delay)
    cpi = config["process"]["data"]["cpi"]
    paths = []
    for row in range(len(cutouts[0]["doppler_hz"])):
        for column in range(len(cutouts[0]["delay_km"])):
            values = [best_beam_at(frame, config, row, column)["value"] for frame in cutouts]
            noncoherent = sum(abs(value) ** 2 for value in values) / len(values)
            coherent = []
            for sign in (-1, 1):
                aligned = [value * cmath.exp(sign * 1j * TAU *
                    cutouts[index]["doppler_hz"][row] * index * cpi)
                    for index, value in enumerate(values)]
                coherent.append(abs(sum(aligned)) ** 2 / len(aligned))
            paths.append({"row": row, "column": column, "noncoherent": noncoherent,
                          "coherent": max(coherent)})
    target = next(item for item in paths if item["row"] == center_row and
                  item["column"] == center_column)
    controls = [item for item in paths if abs(item["row"] - center_row) > guard or
                abs(item["column"] - center_column) > guard]
    noncoherent_p99 = p99([item["noncoherent"] for item in controls])
    coherent_p99 = p99([item["coherent"] for item in controls])
    def margin(value, control):
        return 10 * math.log10(max(value, 1e-300) / max(control, 1e-300))
    return {
        "frames": len(cutouts), "control_paths": len(controls),
        "selected_delay_km": cutouts[0]["delay_km"][center_column],
        "selected_doppler_hz": cutouts[0]["doppler_hz"][center_row],
        "noncoherent_margin_over_control_p99_db": margin(target["noncoherent"], noncoherent_p99),
        "phase_compensated_margin_over_control_p99_db": margin(target["coherent"], coherent_p99),
    }


def nearby_detections(detections, truth, delay_gate, doppler_gate):
    matches = []
    for frame, document in enumerate(detections):
        candidates = []
        for delay, doppler, snr in zip(document.get("delay", []),
                                       document.get("doppler", []),
                                       document.get("snr", [])):
            delay_error = delay - truth["delay"]
            doppler_error = doppler - truth["doppler"]
            if abs(delay_error) <= delay_gate and abs(doppler_error) <= doppler_gate:
                distance = (delay_error / delay_gate) ** 2 + (doppler_error / doppler_gate) ** 2
                candidates.append((distance, delay, doppler, snr, delay_error, doppler_error))
        if candidates:
            _, delay, doppler, snr, delay_error, doppler_error = min(candidates)
            matches.append({"frame": frame, "delay_km": delay, "doppler_hz": doppler,
                            "snr_db": snr, "delay_bias_km": delay_error,
                            "doppler_bias_hz": doppler_error})
    return matches


def track_evidence(tracks, truth, sample_rate, delay_gate, doppler_gate):
    evidence = {}
    for document in tracks:
        for track in document.get("data", []):
            item = evidence.setdefault(track["id"], {"near_samples": 0, "active": False})
            item["active"] = item["active"] or track.get("state") == "ACTIVE"
            for delay_bin, doppler in zip(track.get("associated_delay", []),
                                           track.get("associated_doppler", [])):
                delay = delay_bin * C / sample_rate / 1000
                if (abs(delay - truth["delay"]) <= delay_gate and
                        abs(doppler - truth["doppler"]) <= doppler_gate):
                    item["near_samples"] += 1
    return evidence


def coherence(trace, cpi, sign):
    if not trace:
        return None
    aligned = [item["value"] * cmath.exp(sign * 1j * TAU * item["doppler_hz"] *
               item["frame"] * cpi) for item in trace]
    denominator = len(aligned) * sum(abs(value) ** 2 for value in aligned)
    return abs(sum(aligned)) ** 2 / denominator if denominator else 0


def analyze(replay, config, truth, delay_gate=.5, doppler_gate=25,
            delay_bias=0, doppler_bias=0):
    arm = replay["arms"]["coherent_beam_bank"]
    matches = nearby_detections(arm["detections"], truth, delay_gate, doppler_gate)
    tracks = track_evidence(arm["tracks"], truth, config["capture"]["fs"],
                            delay_gate, doppler_gate)
    cutouts = arm.get("complex_cutouts", [])
    trace = []
    for match in matches:
        if match["frame"] >= len(cutouts):
            continue
        sample = best_beam(cutouts[match["frame"]], config,
                           match["delay_km"], match["doppler_hz"])
        sample["frame"] = match["frame"]
        trace.append(sample)
    measured_delay_bias = statistics.median(item["delay_bias_km"] for item in matches) if matches else None
    measured_doppler_bias = statistics.median(item["doppler_bias_hz"] for item in matches) if matches else None
    active_track = any(item["active"] and item["near_samples"] >= 3 for item in tracks.values())
    accepted = len(matches) >= 3 and active_track and len(cutouts) >= 3
    cell_evidence = integrated_cell_evidence(cutouts, config,
        truth["delay"] + delay_bias, truth["doppler"] + doppler_bias)
    tbd_evidence = (tbd_cfar_evidence(cutouts, config,
        truth["delay"] + delay_bias, truth["doppler"] + doppler_bias)
        if cutouts and len(cutouts[0]["delay_km"]) >= 2 *
        (config["process"]["detection"]["nGuard"] +
         config["process"]["detection"]["nTrain"]) + 1 else None)
    return {
        "schema": "vectorwarp-calibrated-complex-replay-v1",
        "accepted_positive_control": accepted,
        "truth": truth,
        "near_detection_frames": len(matches),
        "median_delay_bias_km": measured_delay_bias,
        "median_doppler_bias_hz": measured_doppler_bias,
        "track_evidence": tracks,
        "complex_cutout_frames": len(cutouts),
        "complex_trace_frames": len(trace),
        "applied_delay_bias_km": delay_bias,
        "applied_doppler_bias_hz": doppler_bias,
        "calibrated_cell_evidence": cell_evidence,
        "resident_tbd_cfar_evidence": tbd_evidence,
        "phase_coherence_uncompensated": coherence(trace, config["process"]["data"]["cpi"], 0),
        "phase_coherence_negative": coherence(trace, config["process"]["data"]["cpi"], -1),
        "phase_coherence_positive": coherence(trace, config["process"]["data"]["cpi"], 1),
        "detections": matches,
        "complex_trace": [{**{key: value for key, value in item.items() if key != "value"},
                            "real": item["value"].real, "imag": item["value"].imag,
                            "power": abs(item["value"]) ** 2} for item in trace],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--truth", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--delay-gate-km", type=float, default=.5)
    parser.add_argument("--doppler-gate-hz", type=float, default=25)
    parser.add_argument("--delay-bias-km", type=float, default=0)
    parser.add_argument("--doppler-bias-hz", type=float, default=0)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    config = json.loads(args.config.read_text())
    ident, truth = load_truth(args.truth)
    result = analyze(replay, config, truth, args.delay_gate_km, args.doppler_gate_hz,
                     args.delay_bias_km, args.doppler_bias_hz)
    result["hex"] = ident
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")


if __name__ == "__main__":
    main()
