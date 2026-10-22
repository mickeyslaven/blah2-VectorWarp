#!/usr/bin/env python3
"""Replay-only ADS-B-windowed sparse delay-Doppler inversion.

Five coherent CAF channels share delay-Doppler support but retain independent
complex amplitudes. Smooth, zero-Doppler, and zero-delay terms are unpenalized
nuisance atoms. The same 9-by-5 candidate bank is ranked at the ADS-B path and
at every translated control that fits in the retained cutout.
"""

import argparse
import json
import math
import statistics
from pathlib import Path


C_MPS = 299_792_458.0
DEFAULT_DELAY_OFFSETS = tuple(range(-4, 5))
DEFAULT_DOPPLER_OFFSETS_HZ = (-20.0, -10.0, 0.0, 10.0, 20.0)


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def group_soft(values, threshold):
    """Complex l2,1 proximal operator; columns are common-support groups."""
    output = [[0j] * len(values[0]) for _ in values]
    for column in range(len(values[0])):
        norm = math.sqrt(sum(abs(row[column]) ** 2 for row in values))
        scale = max(0.0, 1.0 - threshold / max(norm, 1e-300))
        for channel, row in enumerate(values):
            output[channel][column] = row[column] * scale
    return output


def nuisance_atoms(delay, doppler):
    """Return smooth background plus explicit direct-path/clutter atoms."""
    ds = max(max(delay) - min(delay), 1e-12)
    fs = max(max(doppler) - min(doppler), 1e-12)
    d_mean, f_mean = statistics.mean(delay), statistics.mean(doppler)
    d = [(value - d_mean) / ds for value in delay]
    f = [(value - f_mean) / fs for value in doppler]
    unique_delay, unique_doppler = sorted(set(delay)), sorted(set(doppler))
    delay_step = max(statistics.median(abs(b - a) for a, b in zip(
        unique_delay, unique_delay[1:])), 1e-12)
    doppler_step = max(statistics.median(abs(b - a) for a, b in zip(
        unique_doppler, unique_doppler[1:])), 1e-12)
    atoms = [
        [1.0] * len(d), d, f, [dv * fv for dv, fv in zip(d, f)],
        [math.exp(-0.5 * (value / (2 * delay_step)) ** 2) for value in delay],
        [math.exp(-0.5 * (value / (2 * doppler_step)) ** 2) for value in doppler],
    ]
    output = []
    for atom in atoms:
        norm = math.sqrt(sum(value * value for value in atom))
        if norm > 1e-10:
            output.append([value / norm for value in atom])
    return output


def solve_linear(matrix, vector):
    """Small complex Gaussian elimination with diagonal regularization."""
    size = len(vector)
    augmented = [list(matrix[row]) + [vector[row]] for row in range(size)]
    for column in range(size):
        pivot = max(range(column, size), key=lambda row: abs(augmented[row][column]))
        augmented[column], augmented[pivot] = augmented[pivot], augmented[column]
        denominator = augmented[column][column]
        if abs(denominator) < 1e-12:
            denominator = 1e-12
            augmented[column][column] = denominator
        augmented[column] = [value / denominator for value in augmented[column]]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [value - factor * pivot_value for value, pivot_value in
                              zip(augmented[row], augmented[column])]
    return [row[-1] for row in augmented]


def fit_nuisance(values, atoms):
    gram = [[sum(left * right for left, right in zip(a, b)) +
             (1e-9 if row == column else 0.0)
             for column, b in enumerate(atoms)] for row, a in enumerate(atoms)]
    coefficients = solve_linear(
        gram, [sum(atom[index] * values[index] for index in range(len(values)))
               for atom in atoms])
    return [sum(coefficient * atom[index] for coefficient, atom in
                zip(coefficients, atoms)) for index in range(len(values))]


def group_sparse_solve(observations, delay, doppler, penalty=.8, iterations=10):
    """Solve min .5||Y-X-CB||F^2 + lambda sum_j ||X_j||_2."""
    observations = [[complex(value) for value in row] for row in observations]
    atoms = nuisance_atoms(delay, doppler)
    initial_nuisance = [fit_nuisance(row, atoms) for row in observations]
    initial_residual = [[value - nuisance for value, nuisance in zip(row, fitted)]
                        for row, fitted in zip(observations, initial_nuisance)]
    robust_scale = statistics.median(math.sqrt(sum(abs(row[column]) ** 2
                                                  for row in initial_residual))
                                     for column in range(len(observations[0])))
    threshold = penalty * max(robust_scale, 1e-12)
    coefficients = [[0j] * len(observations[0]) for _ in observations]
    for _ in range(iterations):
        nuisance = [fit_nuisance([value - sparse for value, sparse in zip(row, support)], atoms)
                    for row, support in zip(observations, coefficients)]
        coefficients = group_soft(
            [[value - clutter for value, clutter in zip(row, fitted)]
             for row, fitted in zip(observations, nuisance)], threshold)
    residual = [[value - sparse - clutter for value, sparse, clutter in
                 zip(row, support, fitted)] for row, support, fitted in
                zip(observations, coefficients, nuisance)]
    noise_power = max(statistics.median(sum(abs(row[column]) ** 2 for row in residual)
                                        for column in range(len(observations[0]))), 1e-24)
    return coefficients, noise_power


def trajectory_from_document(document, event_id=None, revision=None):
    """Read direct truth or corrected historical manifest rows."""
    if "trajectory" in document:
        return document["trajectory"], {"source": "trajectory_document"}
    rows = document.get("rows")
    if rows is None:
        raise ValueError("truth document has neither trajectory nor manifest rows")
    matches = [row for row in rows
               if (event_id is None or row.get("event_id") == event_id)
               and (revision is None or row.get("capture", {}).get("revision") == revision)]
    if len(matches) != 1:
        raise ValueError(f"expected one corrected manifest row, found {len(matches)}")
    row = matches[0]
    return row["replay_trajectory"], {
        "source": "corrected_historical_manifest",
        "event_id": row.get("event_id"), "revision": row.get("capture", {}).get("revision"),
        "truth_source": row.get("truth_source"), "hex": row.get("hex"),
        "role": row.get("role"), "outcome": row.get("outcome"),
    }


def physical_consistency(trajectory, doppler_values, carrier_hz, tolerance_hz):
    times = [item["timestamp_ms"] / 1000 for item in trajectory]
    ranges = [item["delay_km"] for item in trajectory]
    if len(times) < 2 or max(times) <= min(times):
        return 1.0, 0.0, [None] * len(times)
    rates = []
    for index in range(len(times)):
        left, right = max(0, index - 1), min(len(times) - 1, index + 1)
        rates.append((ranges[right] - ranges[left]) / (times[right] - times[left]))
    predicted = [-carrier_hz * 1000.0 / C_MPS * rate for rate in rates]
    residuals = [observed - expected for observed, expected in zip(doppler_values, predicted)]
    rms = math.sqrt(statistics.mean(value * value for value in residuals))
    return math.exp(-0.5 * (rms / tolerance_hz) ** 2), rms, predicted


def _window(frame, center_row, center_column, delay_offsets, doppler_offsets_hz):
    requested_dopplers = [frame["doppler_hz"][center_row] + offset
                          for offset in doppler_offsets_hz]
    if min(requested_dopplers) < min(frame["doppler_hz"]) or \
            max(requested_dopplers) > max(frame["doppler_hz"]):
        return None
    rows = [nearest(frame["doppler_hz"], value) for value in requested_dopplers]
    columns = [center_column + offset for offset in delay_offsets]
    if min(rows) < 0 or max(rows) >= len(frame["doppler_hz"]):
        return None
    if min(columns) < 0 or max(columns) >= len(frame["delay_km"]):
        return None
    cells = [(row, column, doppler_offset, delay_offset)
             for doppler_offset, row in zip(doppler_offsets_hz, rows)
             for delay_offset, column in zip(delay_offsets, columns)]
    observations = [
        [complex(*frame["channels"][channel][row][column]) for row, column, _, _ in cells]
        for channel in range(5)]
    return cells, observations


def _candidate_bank(cutouts, trajectory, translations, delay_offsets,
                    doppler_offsets_hz, penalty, carrier_hz, tolerance_hz):
    powers = {(doppler, delay): [] for doppler in doppler_offsets_hz
              for delay in delay_offsets}
    dopplers = {key: [] for key in powers}
    selected_truth = []
    for frame, truth, (row_translation, column_translation) in zip(
            cutouts, trajectory, translations):
        base_row = nearest(frame["doppler_hz"], truth["doppler_hz"]) + row_translation
        base_column = nearest(frame["delay_km"], truth["delay_km"]) + column_translation
        if not 0 <= base_row < len(frame["doppler_hz"]):
            return None
        if not 0 <= base_column < len(frame["delay_km"]):
            return None
        window = _window(frame, base_row, base_column, delay_offsets, doppler_offsets_hz)
        if window is None:
            return None
        cells, observations = window
        coefficients, noise_power = group_sparse_solve(
            observations,
            [frame["delay_km"][column] for row, column, _, _ in cells],
            [frame["doppler_hz"][row] for row, column, _, _ in cells], penalty)
        for index, (row, column, doppler_offset, delay_offset) in enumerate(cells):
            key = doppler_offset, delay_offset
            powers[key].append(sum(abs(channel[index]) ** 2 for channel in coefficients) /
                               noise_power)
            dopplers[key].append(frame["doppler_hz"][row])
        # Preserve the continuous ADS-B excess-path trajectory for range rate.
        # Fixed bin translations and candidate offsets change range intercept,
        # not its derivative, and quantizing here would manufacture rate jumps.
        selected_truth.append(truth)
    ranked = []
    for (doppler_offset, delay_offset), values in powers.items():
        consistency, residual, predicted = physical_consistency(
            selected_truth, dopplers[(doppler_offset, delay_offset)], carrier_hz, tolerance_hz)
        sparse_score = math.exp(statistics.mean(math.log(max(value, 1e-300))
                                                for value in values))
        ranked.append({
            "delay_offset_bins": delay_offset, "doppler_offset_hz": doppler_offset,
            "sparse_score": sparse_score, "physical_consistency": consistency,
            "doppler_range_rate_rms_hz": residual,
            "coupled_score": sparse_score * consistency,
            "frame_scores": values, "predicted_doppler_hz": predicted,
        })
    ranked.sort(key=lambda item: item["coupled_score"], reverse=True)
    for rank, item in enumerate(ranked, 1):
        item["rank"] = rank
    return ranked


def analyze(cutouts, trajectory, penalty=.8, carrier_hz=563e6,
            consistency_tolerance_hz=40.0, delay_offsets=DEFAULT_DELAY_OFFSETS,
            doppler_offsets_hz=DEFAULT_DOPPLER_OFFSETS_HZ):
    frames = min(len(cutouts), len(trajectory))
    cutouts, trajectory = cutouts[:frames], trajectory[:frames]
    if not frames or any(len(frame.get("channels", [])) != 5 for frame in cutouts):
        raise ValueError("analysis requires at least one replay frame with five coherent channels")
    target = _candidate_bank(cutouts, trajectory, [(0, 0)] * frames,
                             delay_offsets, doppler_offsets_hz, penalty,
                             carrier_hz, consistency_tolerance_hz)
    if target is None:
        raise ValueError("ADS-B corridor does not fit in retained replay cutout")
    translations = []
    for row_offset in range(-len(cutouts[0]["doppler_hz"]) + 1,
                            len(cutouts[0]["doppler_hz"])):
        for column_offset in range(-len(cutouts[0]["delay_km"]) + 1,
                                   len(cutouts[0]["delay_km"])):
            if row_offset == column_offset == 0:
                continue
            candidate = _candidate_bank(
                cutouts, trajectory, [(row_offset, column_offset)] * frames,
                delay_offsets, doppler_offsets_hz, penalty, carrier_hz,
                consistency_tolerance_hz)
            if candidate is not None:
                compact = [{key: item[key] for key in (
                    "delay_offset_bins", "doppler_offset_hz", "sparse_score",
                    "physical_consistency", "doppler_range_rate_rms_hz",
                    "coupled_score", "rank")} for item in candidate]
                translations.append({"doppler_offset_bins": row_offset,
                                     "delay_offset_bins": column_offset,
                                     "top_candidate": compact[0],
                                     "candidates": compact})
    target_score = target[0]["coupled_score"]
    control_scores = sorted(item["top_candidate"]["coupled_score"] for item in translations)
    exceedances = sum(value >= target_score for value in control_scores)
    return {
        "schema": "vectorwarp-adsb-sparse-delay-doppler-v1",
        "candidate_only": True, "replay_only": True, "frames": frames,
        "channels": 5,
        "configuration": {
            "solver": "complex_group_l21_common_support",
            "penalty_noise_scale": penalty,
            "delay_offset_bins": list(delay_offsets),
            "doppler_offset_hz": list(doppler_offsets_hz),
            "candidates_per_path": len(delay_offsets) * len(doppler_offsets_hz),
            "nuisance_atoms": ["constant", "delay_slope", "doppler_slope",
                               "delay_doppler_cross", "zero_delay_direct_path",
                               "zero_doppler_clutter"],
            "carrier_hz": carrier_hz,
            "doppler_range_rate_tolerance_hz": consistency_tolerance_hz,
        },
        "target_candidates": target,
        "target_score": target_score,
        "translated_controls": translations,
        "controls": len(translations),
        "controls_exceeding_target": exceedances,
        "target_rank_among_paths": 1 + exceedances,
        "empirical_survival_p": (1 + exceedances) / (1 + len(translations)),
        "margin_over_control_median_db": 10 * math.log10(
            max(target_score, 1e-300) / max(statistics.median(control_scores), 1e-300))
            if control_scores else None,
        "gating": "none; every target and control candidate is ranked",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--truth", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--event-id")
    parser.add_argument("--revision", type=int)
    parser.add_argument("--penalty", type=float, default=.8)
    parser.add_argument("--carrier-hz", type=float, default=563e6)
    parser.add_argument("--consistency-tolerance-hz", type=float, default=40.0)
    args = parser.parse_args()
    replay = json.loads(args.replay.read_text())
    trajectory, provenance = trajectory_from_document(
        json.loads(args.truth.read_text()), args.event_id, args.revision)
    result = analyze(replay["arms"]["coherent_beam_bank"]["complex_cutouts"],
                     trajectory, args.penalty, args.carrier_hz,
                     args.consistency_tolerance_hz)
    result.update({"replay": str(args.replay), "truth": str(args.truth),
                   "truth_provenance": provenance})
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in (
        "target_score", "controls", "controls_exceeding_target",
        "target_rank_among_paths", "empirical_survival_p",
        "margin_over_control_median_db")}, indent=2))


if __name__ == "__main__":
    main()
