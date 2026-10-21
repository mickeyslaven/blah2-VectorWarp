#!/usr/bin/env python3
"""Score sub-threshold radar energy along bounded delay/Doppler trajectories.

This is an offline oracle: ADS-B supplies only the trajectory anchor used to
measure whether recorded radar energy is recoverable.  Significance is
estimated with the identical motion search at nearby control anchors, so the
motion-bank look-elsewhere gain is included in the comparison.
"""

import argparse
import json
import math
from pathlib import Path


def values(specification):
    start, stop, step = (float(value) for value in specification.split(":"))
    count = int(round((stop - start) / step))
    return [start + index * step for index in range(count + 1)]


def nearest(axis, value):
    if not axis:
        raise ValueError("empty map axis")
    low, high = 0, len(axis) - 1
    while low < high:
        middle = (low + high) // 2
        if axis[middle] < value:
            low = middle + 1
        else:
            high = middle
    if low and abs(axis[low - 1] - value) <= abs(axis[low] - value):
        return low - 1
    return low


class MapScores:
    def __init__(self, frames, delay_radius, doppler_radius, guard, train):
        self.frames = frames
        self.delay_radius = delay_radius
        self.doppler_radius = doppler_radius
        self.guard = guard
        self.train = train
        self.cache = {}

    def score(self, frame_index, delay, doppler):
        frame = self.frames[frame_index]
        row = nearest(frame["doppler"], doppler)
        column = nearest(frame["delay"], delay)
        key = frame_index, row, column
        if key in self.cache:
            return self.cache[key]
        rows = range(max(0, row - self.doppler_radius),
                     min(len(frame["data"]), row + self.doppler_radius + 1))
        ratios = []
        for current_row in rows:
            data = frame["data"][current_row]
            patch_begin = max(0, column - self.delay_radius)
            patch_end = min(len(data), column + self.delay_radius + 1)
            left_begin = max(0, column - self.guard - self.train)
            left_end = max(0, column - self.guard)
            right_begin = min(len(data), column + self.guard + 1)
            right_end = min(len(data), column + self.guard + self.train + 1)
            training = list(range(left_begin, left_end)) + list(range(right_begin, right_end))
            if not training or patch_begin == patch_end:
                continue
            # VectorWarp's display map stores 10*log10(amplitude) with a
            # common scale.  Squaring amplitude therefore uses /5 here.
            patch = sum(10 ** (data[index] / 5.0)
                        for index in range(patch_begin, patch_end)) / (patch_end - patch_begin)
            noise = sum(10 ** (data[index] / 5.0) for index in training) / len(training)
            ratios.append(patch / max(noise, 1e-300))
        result = sum(ratios) / len(ratios) if ratios else 0.0
        self.cache[key] = result
        return result


def path_score(scores, times, delay, doppler, delay_rate, doppler_rate,
               min_doppler):
    coordinates = [(delay + delay_rate * time, doppler + doppler_rate * time)
                   for time in times]
    delay_axis = scores.frames[0]["delay"]
    doppler_axis = scores.frames[0]["doppler"]
    if any(candidate_delay < delay_axis[0] or candidate_delay > delay_axis[-1] or
           candidate_doppler < doppler_axis[0] or candidate_doppler > doppler_axis[-1] or
           abs(candidate_doppler) < min_doppler
           for candidate_delay, candidate_doppler in coordinates):
        return -math.inf, []
    samples = [scores.score(index, candidate_delay, candidate_doppler)
               for index, (candidate_delay, candidate_doppler) in enumerate(coordinates)]
    return sum(samples) / len(samples), samples


def coordinate_score(scores, coordinates, min_doppler):
    delay_axis = scores.frames[0]["delay"]
    doppler_axis = scores.frames[0]["doppler"]
    if len(coordinates) != len(scores.frames) or any(
            delay < delay_axis[0] or delay > delay_axis[-1] or
            doppler < doppler_axis[0] or doppler > doppler_axis[-1] or
            abs(doppler) < min_doppler for delay, doppler in coordinates):
        return None
    samples = [scores.score(index, delay, doppler)
               for index, (delay, doppler) in enumerate(coordinates)]
    return sum(samples) / len(samples), samples


def best_path(scores, times, delay, doppler, delay_rates, doppler_rates,
              min_doppler):
    best = None
    for delay_rate in delay_rates:
        for doppler_rate in doppler_rates:
            score, samples = path_score(scores, times, delay, doppler,
                                        delay_rate, doppler_rate, min_doppler)
            candidate = score, delay_rate, doppler_rate, samples
            if best is None or candidate[0] > best[0]:
                best = candidate
    return best


def control_anchors(frames, target_delay, target_doppler):
    delay_axis = frames[0]["delay"]
    doppler_axis = frames[0]["doppler"]
    anchors = set()
    delay_offsets = (-12, -8, -5, -3, -2, 2, 3, 5, 8, 12)
    doppler_offsets = (-320, -240, -160, -100, -60, 60, 100, 160, 240, 320)
    for delay_offset in delay_offsets:
        for doppler_offset in doppler_offsets:
            delay = target_delay + delay_offset
            doppler = target_doppler + doppler_offset
            if delay_axis[0] + 1 <= delay <= delay_axis[-1] - 1 and \
                    doppler_axis[0] + 20 <= doppler <= doppler_axis[-1] - 20 and \
                    abs(doppler) >= 12:
                anchors.add((delay, doppler))
    return sorted(anchors)


def percentile(controls, score):
    return sum(value < score for value in controls) / len(controls) if controls else None


def gamma_survival(alpha, training_cells, cpis):
    x = training_cells / (training_cells + alpha)
    total = training_cells * cpis + cpis - 1
    term = x ** total
    result = term
    ratio = (1 - x) / x
    for index in range(1, cpis):
        term *= ((total - index + 1) / index) * ratio
        result += term
    return result


def gamma_cfar_alpha(pfa, training_cells, cpis):
    low, high = 0.0, 1.0
    while gamma_survival(high, training_cells, cpis) > pfa:
        high *= 2
    for _ in range(96):
        middle = (low + high) * .5
        if gamma_survival(middle, training_cells, cpis) > pfa:
            low = middle
        else:
            high = middle
    return high


def interpolate(points, timestamp):
    if not points:
        return None
    if timestamp <= points[0][0]:
        return points[0][1], points[0][2]
    if timestamp >= points[-1][0]:
        return points[-1][1], points[-1][2]
    for left, right in zip(points, points[1:]):
        if left[0] <= timestamp <= right[0]:
            span = right[0] - left[0]
            fraction = 0 if span <= 0 else (timestamp - left[0]) / span
            return (left[1] + fraction * (right[1] - left[1]),
                    left[2] + fraction * (right[2] - left[2]))
    return None


def truth_coordinates(statistics, event_id, absolute_times):
    if not statistics:
        return None
    events = statistics.get("eligibility_events", statistics.get("eligibilityEvents", []))
    event = next((item for item in events if item.get("id") == event_id), None)
    if not event:
        return None
    clip_low, clip_high = min(absolute_times) - 5, max(absolute_times) + 5
    points = [(event["observed_at"] / 1000.0,
               event["predicted"]["delay_km"], event["predicted"]["doppler_hz"])]
    for sample in event.get("truth_samples", []):
        timestamp = sample.get("timestamp_ms", 0) / 1000.0
        if clip_low <= timestamp <= clip_high:
            points.append((timestamp, sample["delay_km"], sample["doppler_hz"]))
    points = sorted(set(points))
    # The trigger point by itself is an anchor, not a measured trajectory.
    if len(points) < 2 or points[-1][0] - points[0][0] < .25:
        return None
    return [interpolate(points, timestamp) for timestamp in absolute_times]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--truth", type=Path,
                        help="Trigger truth JSONL; may be derived from --statistics and the sidecar")
    parser.add_argument("--sidecar", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--arm", default="coherent_beam_bank")
    parser.add_argument("--cpi", type=float, default=.5)
    parser.add_argument("--delay-rates", default="-0.5:0.5:0.05")
    parser.add_argument("--doppler-rates", default="-100:100:5")
    parser.add_argument("--delay-radius", type=int, default=1)
    parser.add_argument("--doppler-radius", type=int, default=1)
    parser.add_argument("--guard", type=int, default=2)
    parser.add_argument("--train", type=int, default=6)
    parser.add_argument("--min-doppler", type=float, default=15)
    parser.add_argument("--effective-pfa", type=float, default=.001)
    parser.add_argument("--statistics", type=Path,
                        help="Evidence-statistics snapshot containing trigger-time truth samples")
    args = parser.parse_args()

    replay = json.loads(args.replay.read_text())
    frames = replay["arms"][args.arm]["maps"]
    if not frames:
        raise SystemExit("replay contains no map frames")
    sidecar = json.loads(args.sidecar.read_text())
    statistics = json.loads(args.statistics.read_text()) if args.statistics else None
    trigger_seconds = sidecar["requestedEventTime"] / 1000.0
    if args.truth:
        truth_document = json.loads(args.truth.read_text().splitlines()[0])
    elif statistics:
        events = statistics.get("eligibility_events", statistics.get("eligibilityEvents", []))
        wanted = set(sidecar.get("eventIds", []))
        truth = {}
        for event in events:
            if event.get("id") not in wanted:
                continue
            predicted = event.get("predicted", {})
            truth[event["hex"]] = {
                "delay": predicted["delay_km"], "doppler": predicted["doppler_hz"],
                "timestamp": event["observed_at"] / 1000.0,
                "flight": event.get("flight", ""), "outcome": event.get("outcome", ""),
                "event_id": event["id"]}
        if not truth:
            raise SystemExit("sidecar event IDs are absent from the statistics snapshot")
        truth_document = {"observed_at": trigger_seconds, "truth": truth}
    else:
        raise SystemExit("provide --truth or --statistics")
    window_start_seconds = sidecar["windowStart"] / 1000.0
    times = [window_start_seconds + (index + 1) * args.cpi - trigger_seconds
             for index in range(len(frames))]
    absolute_times = [window_start_seconds + (index + 1) * args.cpi
                      for index in range(len(frames))]
    delay_rates = values(args.delay_rates)
    doppler_rates = values(args.doppler_rates)
    scores = MapScores(frames, args.delay_radius, args.doppler_radius,
                       args.guard, args.train)
    motion_trials = len(delay_rates) * len(doppler_rates)
    # The exact Gamma threshold applies to a single CUT per CPI.  Spatial
    # kernels are deliberately judged only against empirical control paths
    # because neighboring map cells are correlated.
    theoretical_threshold = None
    if args.delay_radius == 0 and args.doppler_radius == 0:
        theoretical_threshold = gamma_cfar_alpha(
            args.effective_pfa / motion_trials, 2 * args.train, len(frames))
    aircraft = {}
    for ident, truth in sorted(truth_document["truth"].items()):
        delay = truth["delay"]
        doppler = truth["doppler"]
        best = best_path(scores, times, delay, doppler, delay_rates, doppler_rates,
                         args.min_doppler)
        stationary = path_score(scores, times, delay, doppler, 0.0, 0.0,
                                args.min_doppler)[0]
        if best is None or not math.isfinite(best[0]):
            raise ValueError(f"No valid motion path for {ident}")
        controls = []
        for control_delay, control_doppler in control_anchors(frames, delay, doppler):
            control = best_path(scores, times, control_delay, control_doppler,
                                delay_rates, doppler_rates, args.min_doppler)
            if control is not None and math.isfinite(control[0]):
                controls.append(control[0])
        controls.sort()
        score_db = 10 * math.log10(max(best[0], 1e-300))
        stationary_db = 10 * math.log10(max(stationary, 1e-300))
        control_99 = controls[min(len(controls) - 1, int(.99 * len(controls)))] if controls else None
        exact_coordinates = truth_coordinates(
            statistics, truth.get("event_id"), absolute_times)
        exact = coordinate_score(scores, exact_coordinates, args.min_doppler) \
            if exact_coordinates else None
        exact_db = 10 * math.log10(max(exact[0], 1e-300)) if exact else None
        single_path_threshold = None
        blind_threshold = None
        if args.delay_radius == 0 and args.doppler_radius == 0:
            single_path_threshold = gamma_cfar_alpha(
                args.effective_pfa, 2 * args.train, len(frames))
            blind_threshold = theoretical_threshold
        aircraft[ident] = {
            "flight": truth.get("flight", ""),
            "outcome": truth.get("outcome", ""),
            "frames": len(frames),
            "anchor_delay_km": delay,
            "anchor_doppler_hz": doppler,
            "best_delay_rate_km_s": best[1],
            "best_doppler_rate_hz_s": best[2],
            "integrated_local_ratio_db": score_db,
            "zero_motion_local_ratio_db": stationary_db,
            "motion_search_gain_db": score_db - stationary_db,
            "control_paths": len(controls),
            "empirical_percentile": percentile(controls, best[0]),
            "control_p99_local_ratio_db": (10 * math.log10(max(control_99, 1e-300))
                                             if control_99 is not None else None),
            "margin_over_control_p99_db": (score_db - 10 * math.log10(max(control_99, 1e-300))
                                            if control_99 is not None else None),
            "theoretical_gamma_threshold_db": (
                10 * math.log10(theoretical_threshold)
                if theoretical_threshold is not None else None),
            "margin_over_theoretical_threshold_db": (
                score_db - 10 * math.log10(theoretical_threshold)
                if theoretical_threshold is not None else None),
            "per_frame_local_ratio_db": [10 * math.log10(max(value, 1e-300))
                                          for value in best[3]],
            "truth_guided": ({
                "available": True,
                "integrated_local_ratio_db": exact_db,
                "margin_over_single_path_threshold_db": (
                    exact_db - 10 * math.log10(single_path_threshold)
                    if single_path_threshold is not None else None),
                "margin_over_blind_bank_threshold_db": (
                    exact_db - 10 * math.log10(blind_threshold)
                    if blind_threshold is not None else None),
                "per_frame_local_ratio_db": [10 * math.log10(max(value, 1e-300))
                                              for value in exact[1]],
            } if exact else {"available": False}),
        }
    result = {
        "schema": "vectorwarp-motion-integrated-truth-v1",
        "replay": str(args.replay),
        "frames": len(frames),
        "cpi_seconds": args.cpi,
        "relative_frame_times_seconds": times,
        "search": {"delay_rates_km_s": args.delay_rates,
                   "doppler_rates_hz_s": args.doppler_rates,
                   "min_doppler_hz": args.min_doppler,
                   "effective_pfa": args.effective_pfa,
                   "motion_trials": motion_trials,
                   "delay_radius_bins": args.delay_radius,
                   "doppler_radius_bins": args.doppler_radius},
        "aircraft": aircraft,
    }
    encoded = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n")
    print(encoded)


if __name__ == "__main__":
    main()
