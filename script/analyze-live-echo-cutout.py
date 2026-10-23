#!/usr/bin/env python3
"""Audit an exact-truth trajectory in replay complex CAF cutouts.

The replay cutouts retain one complex CAF value per physical surveillance
channel before spatial fusion.  This tool interpolates the admitted ADS-B
truth to each recorded CPI, scores every channel and the noncoherent
five-channel sum against same-frame local controls, and reports persistence.
It is deliberately an offline validation aid; ADS-B never enters detection.
"""

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


def longest_run(values):
    best = current = 0
    for value in values:
        current = current + 1 if value else 0
        best = max(best, current)
    return best


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--admission", required=True, type=Path)
    parser.add_argument("--arm", default="whitened_null_6db")
    parser.add_argument("--guard", type=int, default=1)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    replay = json.loads(args.replay.read_text())
    admission = json.loads(args.admission.read_text())
    if admission.get("state") != "admitted":
        raise ValueError("capture was not admitted")
    capture = next(value for value in admission["captures"].values() if value["valid"])
    cpis = capture["cpis"]
    truth = capture["truth_samples"]
    frames = replay["arms"][args.arm]["complex_cutouts"]
    count = min(len(frames), len(cpis))
    if not count:
        raise ValueError("no overlapping replay frames and admitted CPIs")

    frame_results = []
    for index in range(count):
        frame = frames[index]
        timestamp = cpis[index]["timestamp_ms"]
        target_delay = interpolate(truth, timestamp, "delay_km")
        target_doppler = interpolate(truth, timestamp, "doppler_hz")
        row = nearest(frame["doppler_hz"], target_doppler)
        column = nearest(frame["delay_km"], target_delay)
        inside = (frame["doppler_hz"][0] <= target_doppler <= frame["doppler_hz"][-1] and
                  frame["delay_km"][0] <= target_delay <= frame["delay_km"][-1])
        channel_results = []
        channel_powers = []
        for channel_index, channel in enumerate(frame["channels"]):
            target = power(channel[row][column])
            controls = [power(channel[rr][cc]) for rr in range(len(channel))
                        for cc in range(len(channel[rr]))
                        if abs(rr - row) > args.guard or abs(cc - column) > args.guard]
            threshold = percentile(controls, .99)
            rank = 1 + sum(value > target for value in controls)
            channel_results.append({"channel": channel_index,
                                    "margin_p99_db": 10 * math.log10(max(target, 1e-300) /
                                                                      max(threshold, 1e-300)),
                                    "rank": rank,
                                    "control_cells": len(controls)})
            channel_powers.append((target, controls))
        target_sum = sum(value[0] for value in channel_powers)
        control_sum = [sum(value[1][cell] for value in channel_powers)
                       for cell in range(len(channel_powers[0][1]))]
        threshold_sum = percentile(control_sum, .99)
        frame_results.append({"frame": index, "timestamp_ms": timestamp,
                              "truth_delay_km": target_delay,
                              "truth_doppler_hz": target_doppler,
                              "sampled_delay_km": frame["delay_km"][column],
                              "sampled_doppler_hz": frame["doppler_hz"][row],
                              "truth_inside_cutout": inside,
                              "sum_margin_p99_db": 10 * math.log10(max(target_sum, 1e-300) /
                                                                    max(threshold_sum, 1e-300)),
                              "sum_rank": 1 + sum(value > target_sum for value in control_sum),
                              "channels": channel_results})

    valid = [value for value in frame_results if value["truth_inside_cutout"]]
    channel_summary = []
    for channel in range(len(frames[0]["channels"])):
        margins = [value["channels"][channel]["margin_p99_db"] for value in valid]
        channel_summary.append({"channel": channel,
                                "frames": len(margins),
                                "positive_frames": sum(value > 0 for value in margins),
                                "three_db_frames": sum(value >= 3 for value in margins),
                                "longest_positive_run": longest_run(value > 0 for value in margins),
                                "best_margin_p99_db": max(margins) if margins else None})
    sum_margins = [value["sum_margin_p99_db"] for value in valid]
    result = {"schema": "vectorwarp-live-echo-cutout-audit-v1",
              "replay": str(args.replay), "admission": str(args.admission),
              "arm": args.arm, "frames": frame_results,
              "summary": {"overlapping_frames": count,
                          "truth_inside_cutout_frames": len(valid),
                          "sum_positive_frames": sum(value > 0 for value in sum_margins),
                          "sum_three_db_frames": sum(value >= 3 for value in sum_margins),
                          "sum_longest_positive_run": longest_run(value > 0 for value in sum_margins),
                          "sum_best_margin_p99_db": max(sum_margins) if sum_margins else None,
                          "channels": channel_summary}}
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
