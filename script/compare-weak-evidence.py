#!/usr/bin/env python3
"""Frozen, bounded forensic comparison; cannot authorize live promotion.

Cutouts are derived artifacts, not an IQ replay receipt. Exact production
weights, complete warmup, independently qualified negatives and held-out passes
are mandatory before a decision-grade comparison. Truth/target coordinates are
used only in the final diagnostic summary, never in map construction or paths.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess

SPEC = importlib.util.spec_from_file_location("calibrated", Path(__file__).with_name("analyze-calibrated-complex-replay.py"))
CAF = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(CAF)
POLICY = {"schema": "vectorwarp-weak-evidence-policy-v1", "warmup_cpis": 8,
          "baseline_cpis": 2, "banks": [2, 4, 8], "delay_motion_bins": 2,
          "doppler_motion_bins": 4, "fractional_delay": True, "bootstrap_guard": 2,
          "bootstrap_train": 12, "bootstrap_loading": .001, "shrinkage": .75,
          "promote": False, "threshold_policy": "unchanged-production;bootstrap-uncalibrated"}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""): h.update(block)
    return h.hexdigest()


def canonical(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def validate_cohort(entries):
    """Reject overlap even within development; qualification is never inferred."""
    seen, intervals, roles = set(), [], set()
    for row in entries:
        if row.get("role") not in ("development", "calibration", "holdout", "negative_control"):
            raise ValueError("invalid cohort role")
        if not isinstance(row.get("pass_id"), str) or not row["pass_id"] or row["pass_id"] in seen:
            raise ValueError("whole-pass overlap or missing pass identity")
        seen.add(row["pass_id"]); roles.add(row["role"])
        first, end = row.get("first_sample"), row.get("end_sample")
        if type(first) is not int or type(end) is not int or first < 0 or end <= first or not row.get("hardware_epoch"):
            raise ValueError("invalid sample interval")
        for epoch, a, b in intervals:
            if epoch == row["hardware_epoch"] and max(a, first) < min(b, end):
                raise ValueError("sample overlap including warmup")
        intervals.append((row["hardware_epoch"], first, end))
        if row["role"] != "development" and (row.get("qualified") is not True or row.get("inspected_before_freeze") is not False):
            raise ValueError("non-development split lacks independent qualification")
        if row["role"] == "negative_control" and row.get("target_free_review") is not True:
            raise ValueError("aircraft zero-hit is not a target-free negative")
    return ["missing qualified " + role for role in ("calibration", "holdout", "negative_control") if role not in roles]


def disjoint_credits(endpoints):
    """One immutable epoch/sequence lineage; a gap or reset clears support."""
    credits, previous, epoch, run = [], None, None, 0
    for item in endpoints:
        e, t, depth, hit = item["epoch"], item["sequence"], item["depth"], item["hit"]
        if type(depth) is not int or depth not in (1, 2, 4, 8): raise ValueError("invalid depth")
        if type(t) is not int or t < 0 or not isinstance(e, str) or not e or type(hit) is not bool:
            raise ValueError("invalid support identity")
        if previous is not None and e == epoch and t == previous:
            raise ValueError("duplicate endpoint")
        if e != epoch or previous is None or t != previous + 1: credits, run = [], 0
        run += 1
        epoch, previous = e, t
        credits = [x for x in credits if x >= t - 7]
        credit = hit and run >= depth and not any(t - depth < x < t for x in credits)
        if credit: credits.append(t)
        yield credit


def motion_bank(powers, doppler, scale, depth, moving=True):
    """Same finite Q16 trajectory grid; output also retains support endpoints."""
    rows, columns = len(powers[0]), len(powers[0][0])
    paths = [(dr, dc) for dr in range(-4, 5) for dc in range(-2, 3)] if moving else [(0, 0)]
    result = []
    for end in range(depth - 1, len(powers)):
        surface = [[0.0] * columns for _ in range(rows)]
        for r in range(rows):
            for c in range(columns):
                best = 0.0
                for dr, dc in paths:
                    total = 0.0
                    for age in range(depth):
                        rr = r - dr * age
                        if not 0 <= rr < rows: break
                        cc = CAF.delay_path_column(c, dc, age, scale if moving else 0, doppler[r], doppler[rr])
                        if not 0 <= cc < columns: break
                        total += powers[end - age][rr][cc]
                    else: best = max(best, total / depth)
                surface[r][c] = best
        result.append({"first_frame": end-depth+1, "ending_frame": end, "depth": depth, "power": surface})
    return result


def summarize(bank, row, col, guard=2, train=6):
    # Evaluation-only target location: all surfaces above were computed blind.
    out = []
    last_disjoint_end = -1
    for item in bank:
        p = item["power"]; cols = len(p[0])
        training = [p[row][c] for c in range(max(0, col-guard-train), min(cols, col+guard+train+1)) if abs(c-col)>guard]
        if not training: continue
        mean = sum(training)/len(training)
        contrast = 10*math.log10(max(p[row][col], 1e-300)/max(mean, 1e-300))
        disjoint = item["first_frame"] > last_disjoint_end
        if disjoint: last_disjoint_end = item["ending_frame"]
        out.append({**{k: item[k] for k in ("first_frame", "ending_frame", "depth")},
                    "disjoint_analysis_window": disjoint,
                    "target_training_contrast_db": contrast, "training_cells": len(training)})
    return out


def compare(cutouts, config, bootstrap, evaluation):
    if not 1 <= len(cutouts) <= 32 or len(cutouts) != len(bootstrap): raise ValueError("frame bound")
    rows, cols = len(cutouts[0]["doppler_hz"]), len(cutouts[0]["delay_km"])
    if not 1 <= rows <= 129 or not 1 <= cols <= 129: raise ValueError("cutout bound")
    if len(cutouts)*rows*cols*45*14*3 > 50000000:
        raise ValueError("bounded comparison operation budget exceeded")
    for i, frame in enumerate(cutouts):
        if frame["frame"] != cutouts[0]["frame"]+i or bootstrap[i]["frame"] != frame["frame"] or \
           frame["delay_km"] != cutouts[0]["delay_km"] or frame["doppler_hz"] != cutouts[0]["doppler_hz"]:
            raise ValueError("cutout gap/reset/axis change; split the sequence")
    provenance = CAF.fusion_provenance(cutouts, config)
    raw, channel_health = [], []
    for frame in cutouts:
        channels = frame["channels"]
        vectors = [[complex(*channels[c][r][col]) for r in range(rows) for col in range(cols)]
                   for c in range(len(channels))]
        covariance = [[sum(x*y.conjugate() for x,y in zip(a,b))/len(a) for b in vectors] for a in vectors]
        powers = [covariance[i][i].real for i in range(len(channels))]
        coherence = [abs(covariance[i][j])**2/max(powers[i]*powers[j],1e-300)
                     for i in range(len(channels)) for j in range(i)]
        channel_health.append({"frame":frame["frame"],"channel_power":powers,
            "channel_power_spread_db":10*math.log10(max(max(powers),1e-300)/max(min(powers),1e-300)),
            "mean_pair_squared_coherence":sum(coherence)/len(coherence) if coherence else None,
            "scope":"entire selected cutout; contains echoes/clutter, not a noise-only covariance"})
        weights = CAF.applied_fusion_weights(frame, config)
        if weights is None: weights = CAF.calibrated_weights(config, [complex(*z) for z in frame["reference_weights"]])
        raw.append([[max(abs(sum(w*complex(*frame["channels"][c][r][col]) for c,w in enumerate(beam)))**2
                         for beam in weights) for col in range(cols)] for r in range(rows)])
    scale = CAF.fractional_delay_scale(config)
    row = CAF.nearest(cutouts[0]["doppler_hz"], evaluation["doppler_hz"])
    col = CAF.nearest(cutouts[0]["delay_km"], evaluation["delay_km"])
    arms = {"baseline_fixed2": summarize(motion_bank(raw, cutouts[0]["doppler_hz"], scale, 2, False), row, col)}
    for label, powers in [("existing_fractional", raw)] + [(key, [x[key]["power"] for x in bootstrap]) for key in ("diagonal", "shrinkage075")]:
        for depth in POLICY["banks"]:
            arms[label+"_"+str(depth)] = summarize(motion_bank(powers, cutouts[0]["doppler_hz"], scale, depth), row, col)
    blockers = ["no independently calibrated bootstrap null-tail/PFA", "no native tracker replay/false-track exposure",
                "diagnostic path envelope lacks native beam-compatibility and detector-selection replay",
                "v1 cutouts lack committed sample-lineage receipt"]
    if not provenance["production_equivalent"]: blockers.append("missing applied production fusion weights")
    if len(cutouts) <= POLICY["warmup_cpis"]: blockers.append("no evaluation CPI after eight warmup CPIs")
    return {"production_fusion": provenance, "arms": arms, "channel_health":channel_health,"blockers": blockers,
            "false_track_delta": None, "null_tail_pfa": None, "promotion": "NO-GO",
            "interpretation": "development target/training contrast only; not CFAR margin or target-free PFA"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest",type=Path);parser.add_argument("--bootstrap-executable",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True);args=parser.parse_args()
    manifest=json.loads(args.manifest.read_text());require_schema=manifest.get("schema")=="vectorwarp-weak-evidence-comparison-v1"
    if not require_schema or manifest.get("policy_sha256")!=canonical(POLICY): raise ValueError("unfrozen policy")
    blockers=validate_cohort(manifest["entries"]);results=[]
    for entry in manifest["entries"]:
        if digest(entry["iq_path"])!=entry["iq_sha256"]: raise ValueError("IQ bytes changed")
        if entry.get("cutout_path") is None:
            results.append({"pass_id":entry["pass_id"],"role":entry["role"],"iq_sha256":entry["iq_sha256"],
                "arms":{},"blockers":["missing bounded native post-clutter cutout replay and applied weights",
                    "quarantined truth timing: radar-only maintenance, not scored positive"],
                "false_track_delta":None,"promotion":"NO-GO"})
            continue
        if digest(entry["cutout_path"])!=entry["cutout_sha256"] or digest(entry["config_path"])!=entry["config_sha256"]:
            raise ValueError("derived input bytes changed")
        path=Path(entry["cutout_path"])
        if path.stat().st_size>16*1024*1024: raise ValueError("cutout byte bound")
        with path.open("rb") as stream:data=stream.read(16*1024*1024+1)
        if len(data)>16*1024*1024: raise ValueError("cutout byte bound")
        result=subprocess.run([str(args.bootstrap_executable.resolve())],input=data,capture_output=True,timeout=60,check=True)
        cutouts=[json.loads(x) for x in data.splitlines()]
        bootstrap=[json.loads(x) for x in result.stdout.splitlines()]
        comparison=compare(cutouts,json.loads(Path(entry["config_path"]).read_text()),bootstrap,entry["evaluation"])
        results.append({"pass_id":entry["pass_id"],"role":entry["role"],"iq_sha256":entry["iq_sha256"],**comparison})
    receipt={"schema":"vectorwarp-weak-evidence-comparison-receipt-v1","policy":POLICY,
             "policy_sha256":canonical(POLICY),"manifest_sha256":digest(args.manifest),
             "runner_sha256":digest(__file__),"bootstrap_executable_sha256":digest(args.bootstrap_executable),
             "blockers":blockers,"results":results,"go":False,"false_track_delta":None}
    # Never replace a prior experimental receipt.
    with args.output.open("x") as stream: json.dump(receipt,stream,indent=2);stream.write("\n")


if __name__=="__main__": main()
