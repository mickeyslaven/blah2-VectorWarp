#!/usr/bin/env python3
"""Strict semantic comparison for Pipeline ``*.outputs.jsonl`` artifacts."""
import argparse
import json
import math
import sys
from pathlib import Path

ABS_TOLERANCE = 0.011  # Pipeline JSON currently rounds display values to .01.
REL_TOLERANCE = 1e-6
IDENTITY_KEYS = ("id", "uuid", "trackId")


class Comparison:
    def __init__(self):
        self.errors, self.numeric, self.max_abs, self.max_rel = [], 0, 0.0, 0.0
        self.detections = self.tracks = 0

    def error(self, path, message):
        if len(self.errors) < 32:
            self.errors.append({"path": path, "error": message})

    def number(self, left, right, path):
        if not (math.isfinite(left) and math.isfinite(right)):
            self.error(path, "non-finite numeric value")
            return
        absolute = abs(left - right)
        relative = absolute / max(abs(left), abs(right), 1e-30)
        self.numeric += 1
        self.max_abs, self.max_rel = max(self.max_abs, absolute), max(self.max_rel, relative)
        if absolute > ABS_TOLERANCE and relative > REL_TOLERANCE:
            self.error(path, f"numeric mismatch baseline={left} candidate={right}")

    def value(self, left, right, path):
        if isinstance(left, bool) or isinstance(right, bool):
            if left is not right: self.error(path, f"value mismatch baseline={left!r} candidate={right!r}")
        elif isinstance(left, (int, float)) and isinstance(right, (int, float)):
            exact = (path.endswith(".timestamp") or ".frequency[" in path or
                     path.rsplit(".", 1)[-1] in ("n", "nTentative", "nAssociated", "nActive", "nCoasting"))
            if exact and (not math.isfinite(left) or not math.isfinite(right) or left != right):
                self.error(path, f"exact metadata mismatch baseline={left} candidate={right}")
            else:
                self.number(float(left), float(right), path)
        elif isinstance(left, dict) and isinstance(right, dict):
            left_keys, right_keys = set(left), set(right)
            if left_keys != right_keys:
                self.error(path, f"field set mismatch baseline={sorted(left_keys)} candidate={sorted(right_keys)}")
            for key in sorted(left_keys & right_keys): self.value(left[key], right[key], f"{path}.{key}")
        elif isinstance(left, list) and isinstance(right, list):
            if len(left) != len(right):
                self.error(path, f"array length mismatch baseline={len(left)} candidate={len(right)}")
                return
            for index, (a, b) in enumerate(zip(left, right)): self.value(a, b, f"{path}[{index}]")
        elif left != right:
            self.error(path, f"value mismatch baseline={left!r} candidate={right!r}")

    def tracks_value(self, left, right, path):
        # Track JSON contains a data list. Compare tracks by durable identity so
        # presentation ordering does not hide an identity/state regression.
        for side, value in (("baseline", left), ("candidate", right)):
            if not isinstance(value, dict):
                self.error(path, f"{side} tracks is not an object"); return
        left_data, right_data = left.get("data", []), right.get("data", [])
        if not isinstance(left_data, list) or not isinstance(right_data, list):
            self.error(path + ".data", "tracks data is not an array"); return
        def index(items, side):
            result = {}
            for item in items:
                if not isinstance(item, dict): self.error(path + ".data", f"{side} track is not an object"); continue
                key = next((item[k] for k in IDENTITY_KEYS if k in item), None)
                if key is None or key in result: self.error(path + ".data", f"{side} track identity missing or duplicate"); continue
                result[key] = item
            return result
        left_index, right_index = index(left_data, "baseline"), index(right_data, "candidate")
        if set(left_index) != set(right_index): self.error(path + ".data", "track identities differ")
        self.value({k:v for k,v in left.items() if k != "data"}, {k:v for k,v in right.items() if k != "data"}, path)
        for identity in sorted(set(left_index) & set(right_index), key=str):
            self.value(left_index[identity], right_index[identity], f"{path}.data[{identity!r}]")


def read(path):
    frames = []
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip(): raise ValueError(f"{path}:{line_number}: blank JSONL record")
            try: frame = json.loads(line)
            except json.JSONDecodeError as error: raise ValueError(f"{path}:{line_number}: {error.msg}") from error
            if not isinstance(frame, dict): raise ValueError(f"{path}:{line_number}: frame is not an object")
            frames.append(frame)
    return frames


def compare(baseline, candidate):
    result = Comparison()
    if not baseline or not candidate: result.error("$", "no frames to compare")
    if len(baseline) != len(candidate): result.error("$", f"frame count mismatch baseline={len(baseline)} candidate={len(candidate)}")
    for index, (left, right) in enumerate(zip(baseline, candidate)):
        path = f"frames[{index}]"
        if left.get("frame") != index or right.get("frame") != index:
            result.error(path, "missing or reordered frame number")
        if left.get("frame") != right.get("frame"):
            result.error(path + ".frame", "frame timestamp/index differs")
        for key in ("iq", "detections"):
            if key not in left or key not in right: result.error(path, f"missing {key}"); continue
            result.value(left[key], right[key], path + "." + key)
        if "tracks" not in left or "tracks" not in right: result.error(path, "missing tracks")
        else: result.tracks_value(left["tracks"], right["tracks"], path + ".tracks")
        detections = left.get("detections", {}).get("delay", []) if isinstance(left.get("detections"), dict) else []
        tracks = left.get("tracks", {}).get("data", []) if isinstance(left.get("tracks"), dict) else []
        result.detections += len(detections) if isinstance(detections, list) else 0
        result.tracks += len(tracks) if isinstance(tracks, list) else 0
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path); parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    try:
        baseline, candidate = read(args.baseline), read(args.candidate)
        outcome = compare(baseline, candidate)
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)})); return 2
    evidence = {"ok": not outcome.errors, "frames_compared": min(len(baseline), len(candidate)),
        "absolute_tolerance": ABS_TOLERANCE, "relative_tolerance": REL_TOLERANCE,
        "numeric_values": outcome.numeric, "max_absolute_error": outcome.max_abs,
        "max_relative_error": outcome.max_rel, "detection_count": outcome.detections,
        "track_count": outcome.tracks, "errors": outcome.errors}
    print(json.dumps(evidence, sort_keys=True, allow_nan=False))
    return 0 if evidence["ok"] else 1


if __name__ == "__main__": sys.exit(main())
