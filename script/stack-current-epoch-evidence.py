#!/usr/bin/env python3
"""Trial-corrected offline stacks for normalized WWAT recovery vectors.

Inputs are producer vectors with a target evidence_log10 and aligned
off-trajectory controls.  We use equal-weight Stouffer-like sums only after
rank-normalizing each method against its own controls; every single, pair and
all-method combination is charged in the final empirical threshold.
"""
import argparse
import itertools
import json
import math
from pathlib import Path


def percentile(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1)]


def survival(value, controls):
    return (1 + sum(item >= value for item in controls)) / (len(controls) + 1)


def normal_score(p):
    # Bounded monotone evidence scale; avoids an unavailable scipy dependency.
    return -math.log10(max(p, 1e-300))


def load_vector(path):
    document = json.loads(Path(path).read_text())
    methods = document.get("methods", {})
    result = {}
    for name, item in methods.items():
        target = item.get("evidence_log10", item.get("margin_db"))
        controls = item.get("control_evidence_log10", item.get("controls"))
        if target is None or not isinstance(controls, list) or not controls:
            continue
        result[name] = {"target": float(target), "controls": [float(x) for x in controls]}
    return document, result


def stack(methods):
    names = sorted(methods)
    rows = []
    for size in range(1, len(names) + 1):
        for chosen in itertools.combinations(names, size):
            target = sum(normal_score(survival(methods[name]["target"], methods[name]["controls"]))
                         for name in chosen) / math.sqrt(size)
            control_count = min(len(methods[name]["controls"]) for name in chosen)
            controls = [sum(normal_score(survival(methods[name]["controls"][index],
                                                   methods[name]["controls"]))
                            for name in chosen) / math.sqrt(size)
                        for index in range(control_count)]
            rows.append({"combination": list(chosen), "target": target,
                         "controls": controls, "single": size == 1})
    # All wrappers share the same sorted control offsets.  Family-wise control
    # is the maximum over every predeclared combination at each offset, not a
    # pooled mixture that would undercharge the seven combination trials.
    control_count = min(len(row["controls"]) for row in rows)
    family_maxima = [max(row["controls"][index] for row in rows)
                      for index in range(control_count)]
    threshold = percentile(family_maxima, .99)
    for row in rows:
        row["control_p99_all_trials"] = threshold
        row["margin"] = row["target"] - threshold
        row["recovered"] = row["margin"] >= 0
        del row["controls"]
    return {"trial_count": len(rows), "aligned_control_offsets": control_count,
            "familywise_control_maxima": len(family_maxima),
            "threshold_p99_fwer": threshold, "rows": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vectors", nargs="+", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    clips = []
    for path in args.vectors:
        document, methods = load_vector(path)
        clips.append({"source": str(path), "clip": document.get("source", document.get("recording")),
                      "event_id": document.get("event_id"), "methods": stack(methods) if methods else None})
    args.output.write_text(json.dumps({"schema": "vectorwarp-wwat-stack-v1",
                                       "clips": clips}, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
