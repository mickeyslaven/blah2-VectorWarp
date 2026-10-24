#!/usr/bin/env python3
"""Score a fixed truth trajectory against offset trajectories in complex CAF cutouts."""

import argparse
import json
import math
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def interpolate(samples, timestamp_ms, key):
    samples = sorted(samples, key=lambda value: value["timestamp_ms"])
    before = max((value for value in samples if value["timestamp_ms"] <= timestamp_ms),
                 key=lambda value: value["timestamp_ms"], default=samples[0])
    after = min((value for value in samples if value["timestamp_ms"] >= timestamp_ms),
                key=lambda value: value["timestamp_ms"], default=samples[-1])
    if after["timestamp_ms"] == before["timestamp_ms"]:
        return before[key]
    fraction = ((timestamp_ms - before["timestamp_ms"]) /
                (after["timestamp_ms"] - before["timestamp_ms"]))
    return before[key] + fraction * (after[key] - before[key])


def nearest(axis, value):
    return min(range(len(axis)), key=lambda index: abs(axis[index] - value))


def power(value):
    return value[0] * value[0] + value[1] * value[1]


def margin(value, reference):
    return 10 * math.log10(max(value, 1e-300) / max(reference, 1e-300))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--admission", required=True, type=Path)
    parser.add_argument("--arm", default="whitened_null_6db")
    parser.add_argument("--guard", type=int, default=1)
    parser.add_argument("--search-row-radius", type=int, default=0,
                        help="bounded Doppler-bin bias searched around truth")
    parser.add_argument("--search-column-radius", type=int, default=0,
                        help="bounded delay-bin bias searched around truth")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    replay = json.loads(args.replay.read_text())
    admission = json.loads(args.admission.read_text())
    capture = next(value for value in admission["captures"].values() if value["valid"])
    cpis = capture["cpis"]
    truth = capture["truth_samples"]
    cutouts = replay["arms"][args.arm]["complex_cutouts"]
    count = min(len(cpis), len(cutouts))
    if count < 3:
        raise ValueError("at least three aligned frames are required")

    anchors = []
    for index in range(count):
        frame = cutouts[index]
        timestamp = cpis[index]["timestamp_ms"]
        delay = interpolate(truth, timestamp, "delay_km")
        doppler = interpolate(truth, timestamp, "doppler_hz")
        anchors.append((nearest(frame["doppler_hz"], doppler),
                        nearest(frame["delay_km"], delay)))

    row_radius = min(anchor[0] for anchor in anchors)
    row_radius = min(row_radius, min(len(cutouts[index]["doppler_hz"]) - 1 - anchor[0]
                                     for index, anchor in enumerate(anchors)))
    column_radius = min(anchor[1] for anchor in anchors)
    column_radius = min(column_radius, min(len(cutouts[index]["delay_km"]) - 1 - anchor[1]
                                           for index, anchor in enumerate(anchors)))
    channels = len(cutouts[0]["channels"])

    def score(row_offset, column_offset):
        per_channel = [0.0] * channels
        for index, (row, column) in enumerate(anchors):
            for channel in range(channels):
                per_channel[channel] += power(
                    cutouts[index]["channels"][channel]
                    [row + row_offset][column + column_offset])
        return {"sum": sum(per_channel), "channels": per_channel}

    target = score(0, 0)
    scored = {}
    for row_offset in range(-row_radius, row_radius + 1):
        for column_offset in range(-column_radius, column_radius + 1):
            scored[(row_offset, column_offset)] = score(row_offset, column_offset)
    controls = [{"row_offset": row_offset, "column_offset": column_offset, **value}
                for (row_offset, column_offset), value in scored.items()
                if abs(row_offset) > args.guard or abs(column_offset) > args.guard]
    if not controls:
        raise ValueError("no valid offset trajectory controls")

    sum_threshold = percentile([value["sum"] for value in controls], .99)
    channel_results = []
    for channel in range(channels):
        threshold = percentile([value["channels"][channel] for value in controls], .99)
        channel_results.append({
            "channel": channel,
            "margin_p99_db": margin(target["channels"][channel], threshold),
            "rank": 1 + sum(value["channels"][channel] >= target["channels"][channel]
                              for value in controls)})
    result = {
        "schema": "vectorwarp-exact-trajectory-stack-v1",
        "frames": count,
        "control_trajectories": len(controls),
        "guard_bins": args.guard,
        "target_anchors": [{"row": row, "column": column} for row, column in anchors],
        "fusion_margin_p99_db": margin(target["sum"], sum_threshold),
        "fusion_rank": 1 + sum(value["sum"] >= target["sum"] for value in controls),
        "channels": channel_results,
    }
    if args.search_row_radius or args.search_column_radius:
        search_offsets = [(row, column)
                          for row in range(-args.search_row_radius,
                                           args.search_row_radius + 1)
                          for column in range(-args.search_column_radius,
                                              args.search_column_radius + 1)]

        def best_at(center_row, center_column, key, channel=None):
            candidates = []
            for row, column in search_offsets:
                offset = (center_row + row, center_column + column)
                if offset not in scored:
                    return None
                value = scored[offset][key] if channel is None else scored[offset][key][channel]
                candidates.append((value, offset))
            return max(candidates)

        target_best = best_at(0, 0, "sum")
        null_centers = []
        for row in range(-row_radius + args.search_row_radius,
                         row_radius - args.search_row_radius + 1):
            for column in range(-column_radius + args.search_column_radius,
                                column_radius - args.search_column_radius + 1):
                if (abs(row) <= args.search_row_radius + args.guard and
                        abs(column) <= args.search_column_radius + args.guard):
                    continue
                candidate = best_at(row, column, "sum")
                if candidate is not None:
                    null_centers.append(candidate[0])
        if not null_centers:
            raise ValueError("no valid search-corrected null windows")
        threshold = percentile(null_centers, .99)
        searched_channels = []
        for channel in range(channels):
            channel_target = best_at(0, 0, "channels", channel)
            channel_null = []
            for row in range(-row_radius + args.search_row_radius,
                             row_radius - args.search_row_radius + 1):
                for column in range(-column_radius + args.search_column_radius,
                                    column_radius - args.search_column_radius + 1):
                    if (abs(row) <= args.search_row_radius + args.guard and
                            abs(column) <= args.search_column_radius + args.guard):
                        continue
                    candidate = best_at(row, column, "channels", channel)
                    if candidate is not None:
                        channel_null.append(candidate[0])
            channel_threshold = percentile(channel_null, .99)
            searched_channels.append({
                "channel": channel,
                "best_offset": {"row": channel_target[1][0],
                                "column": channel_target[1][1]},
                "margin_p99_db": margin(channel_target[0], channel_threshold),
                "rank": 1 + sum(value >= channel_target[0] for value in channel_null),
            })
        result["bounded_bias_search"] = {
            "row_radius": args.search_row_radius,
            "column_radius": args.search_column_radius,
            "target_trials": len(search_offsets),
            "null_windows": len(null_centers),
            "best_offset": {"row": target_best[1][0], "column": target_best[1][1]},
            "fusion_margin_p99_db": margin(target_best[0], threshold),
            "fusion_rank": 1 + sum(value >= target_best[0] for value in null_centers),
            "channels": searched_channels,
        }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
