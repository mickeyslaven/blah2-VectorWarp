#!/usr/bin/env python3
"""Build versioned replay manifests from retained and archived ADS-B observations.

The source manifests are never modified.  For each replay frame, the newest
event-time observation at or before the frame is selected, subject to a maximum
age.  A legacy trajectory frame is retained only as an explicitly-labelled
fallback.  Every selected point and disagreement is auditable in the output.
"""

from __future__ import annotations

import argparse
import bisect
import copy
import gzip
import hashlib
import json
import math
from collections import Counter, defaultdict
from pathlib import Path

C = 299_792_458.0
FT_M = 0.3048
EARTH_M = 6_371_000.0


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def haversine_km(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dp, dl = p2 - p1, math.radians(b[1] - a[1])
    x = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_M * math.asin(math.sqrt(x)) / 1000


def ecef(lat, lon, alt_m):
    a, e2 = 6378137.0, 6.69437999014e-3
    p, l = math.radians(lat), math.radians(lon)
    n = a / math.sqrt(1 - e2 * math.sin(p) ** 2)
    return ((n + alt_m) * math.cos(p) * math.cos(l),
            (n + alt_m) * math.cos(p) * math.sin(l),
            (n * (1 - e2) + alt_m) * math.sin(p))


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def geometry(obs, rx, tx):
    alt_m = obs["altitude_geom_ft"] * FT_M
    target, rxe, txe = ecef(obs["latitude"], obs["longitude"], alt_m), ecef(*rx), ecef(*tx)
    rr, tr, base = dist(rxe, target), dist(txe, target), dist(rxe, txe)
    p1, p2 = math.radians(rx[0]), math.radians(obs["latitude"])
    dl = math.radians(obs["longitude"] - rx[1])
    bearing = (math.degrees(math.atan2(math.sin(dl) * math.cos(p2),
        math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl))) + 360) % 360
    ground = haversine_km(rx[:2], (obs["latitude"], obs["longitude"])) * 1000
    elev = math.degrees(math.atan2(alt_m - rx[2], ground))
    return {"delay_km": (rr + tr - base) / 1000, "bearing_deg_true": bearing,
            "elevation_deg": elev, "ground_range_km": ground / 1000,
            "slant_range_km": rr / 1000, "transmitter_range_km": tr / 1000}


def load_local(path, rx, tx):
    data = json.loads(path.read_text())
    out = defaultdict(dict)
    for event in data.get("eligibility_events", []):
        hx = str(event.get("hex", "")).lower()
        for sample in event.get("truth_samples", []):
            if sample.get("latitude") is None or sample.get("longitude") is None:
                continue
            alt = sample.get("altitude_geom_ft", sample.get("altitude_baro_ft"))
            if alt is None:
                continue
            t = round(float(sample.get("position_timestamp_s", sample["timestamp_ms"] / 1000)) * 1000)
            o = {"event_time_ms": t, "latitude": sample["latitude"], "longitude": sample["longitude"],
                 "altitude_geom_ft": alt, "source": "vectorwarp_local_readsb_retained",
                 "source_event_id": event.get("id"), "source_poll_timestamp_ms": sample.get("timestamp_ms"),
                 "source_details": {k: sample.get(k) for k in ("seen_s", "seen_pos_s", "rssi_dbfs", "category", "squawk")}}
            o.update(geometry(o, rx, tx))
            # Preserve measured-history Doppler; it is frequency/epoch specific.
            o["local_doppler_hz"] = sample.get("doppler_hz")
            out[hx][t] = o
    return {h: sorted(v.values(), key=lambda x: x["event_time_ms"]) for h, v in out.items()}, data


def load_archive(root, rx, tx):
    out, files = defaultdict(list), {}
    for path in sorted(root.glob("traces/*/trace_full_*.json")):
        files[path.name] = sha256(path)
        with gzip.open(path, "rt") as f:
            d = json.load(f)
        hx, base = d["icao"].lower(), float(d["timestamp"])
        for row in d["trace"]:
            if len(row) < 3 or row[1] is None or row[2] is None:
                continue
            alt = row[10] if len(row) > 10 and row[10] is not None else row[3]
            if not isinstance(alt, (int, float)):
                continue
            o = {"event_time_ms": round((base + row[0]) * 1000), "latitude": row[1], "longitude": row[2],
                 "altitude_geom_ft": alt, "source": "adsb_lol_daily_archive",
                 "source_trace": str(path.relative_to(root)), "position_source": row[9] if len(row) > 9 else None,
                 "source_details": {"altitude_baro_ft": row[3], "ground_speed_kt": row[4],
                                    "track_deg_true": row[5], "baro_rate_fpm": row[7]}}
            o.update(geometry(o, rx, tx)); out[hx].append(o)
    for values in out.values():
        values.sort(key=lambda x: x["event_time_ms"])
        for i, o in enumerate(values):
            if i:
                prior, dt = values[i - 1], (o["event_time_ms"] - values[i - 1]["event_time_ms"]) / 1000
                if 0 < dt <= 30:
                    o["path_rate_m_s"] = (o["delay_km"] - prior["delay_km"]) * 1000 / dt
    return dict(out), files


def latest(values, timestamp_ms, max_age_ms):
    if not values:
        return None
    i = bisect.bisect_right([v["event_time_ms"] for v in values], timestamp_ms) - 1
    if i < 0 or timestamp_ms - values[i]["event_time_ms"] > max_age_ms:
        return None
    return values[i]


def percentile(values, p):
    if not values: return None
    s = sorted(values); return s[round((len(s) - 1) * p)]


def frequency(path):
    return 563_000_000.0 if "563" in path.name else 527_000_000.0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifests", type=Path, required=True)
    ap.add_argument("--statistics", type=Path, required=True)
    ap.add_argument("--traces", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--rx", default="40.4412816,-79.9706092,300")
    ap.add_argument("--tx", default="40.46325,-80.0043056,615")
    ap.add_argument("--max-age-s", type=float, default=30)
    args = ap.parse_args(argv)
    rx, tx = tuple(map(float, args.rx.split(","))), tuple(map(float, args.tx.split(",")))
    args.output.mkdir(parents=True, exist_ok=False)
    local, stats = load_local(args.statistics, rx, tx)
    archive, trace_hashes = load_archive(args.traces, rx, tx)
    report = {"schema": "vectorwarp-adsb-reconciliation-report-v2", "policy": {
        "selection": "newest event_time observation at or before CPI; local wins exact timestamp ties",
        "max_age_s": args.max_age_s, "fresh_threshold_s": 3, "disagreement_threshold_km": 3},
        "inputs": {"statistics": {"path": str(args.statistics), "sha256": sha256(args.statistics)},
                   "archive_trace_count": len(trace_hashes), "archive_trace_sha256": trace_hashes,
                   "rx": rx, "tx": tx}, "manifests": {}, "totals": Counter()}
    all_ages, all_dis = [], []
    for src in sorted(args.manifests.glob("*manifest.json")):
        original = json.loads(src.read_text()); doc = copy.deepcopy(original); freq = frequency(src)
        counts, ages, disagreements = Counter(), [], []
        for row in doc.get("rows", []):
            hx = str(row.get("hex", "")).lower()
            for frame in row.get("replay_trajectory", []):
                t = int(frame["timestamp_ms"]); lo = latest(local.get(hx), t, args.max_age_s * 1000)
                ar = latest(archive.get(hx), t, args.max_age_s * 1000)
                candidates = [x for x in (lo, ar) if x]
                selected = max(candidates, key=lambda x: (x["event_time_ms"], x["source"].startswith("vectorwarp"))) if candidates else None
                prov = {"selection_policy": report["policy"]["selection"], "candidates": []}
                for candidate in candidates:
                    prov["candidates"].append({"source": candidate["source"], "event_time_ms": candidate["event_time_ms"],
                                               "age_ms": t - candidate["event_time_ms"]})
                if lo and ar:
                    sep = haversine_km((lo["latitude"], lo["longitude"]), (ar["latitude"], ar["longitude"]))
                    disagreement = {"horizontal_km": sep, "altitude_ft": abs(lo["altitude_geom_ft"] - ar["altitude_geom_ft"]),
                                    "event_time_delta_ms": abs(lo["event_time_ms"] - ar["event_time_ms"]), "outlier": sep > 3}
                    prov["disagreement"] = disagreement; disagreements.append(sep)
                    counts["disagreement_pairs"] += 1; counts["outlier_pairs"] += int(sep > 3)
                if selected:
                    age = t - selected["event_time_ms"]; ages.append(age); counts[selected["source"]] += 1
                    counts["raw_observation_frames"] += 1; counts["fresh_le_3s"] += int(age <= 3000)
                    doppler = selected.get("local_doppler_hz")
                    if selected["source"] == "adsb_lol_daily_archive" and selected.get("path_rate_m_s") is not None:
                        doppler = -selected["path_rate_m_s"] / (C / freq)
                    frame.update({k: selected[k] for k in ("latitude", "longitude", "altitude_geom_ft", "delay_km", "bearing_deg_true", "elevation_deg")})
                    if doppler is not None: frame["doppler_hz"] = doppler
                    prov.update({"selected_source": selected["source"], "observation_event_time_ms": selected["event_time_ms"],
                                 "timing_error_ms": age, "source_details": selected.get("source_details", {})})
                    if selected.get("source_trace"): prov["source_trace"] = selected["source_trace"]
                    if selected.get("source_event_id"): prov["source_event_id"] = selected["source_event_id"]
                else:
                    counts["legacy_manifest_v1_fallback"] += 1
                    prov.update({"selected_source": "legacy_manifest_v1_fallback", "observation_event_time_ms": None,
                                 "timing_error_ms": None, "reason": "no raw observation at or before CPI within max age"})
                frame["adsb_reconciliation"] = prov
        doc["schema"] = "vectorwarp-replay-manifest-adsb-reconciled-candidate-v2"
        doc["adsb_reconciliation"] = {"source_manifest": src.name, "source_manifest_sha256": sha256(src),
            "statistics_sha256": sha256(args.statistics), "frequency_hz": freq, "policy": report["policy"]}
        dst = args.output / src.name.replace(".json", ".adsb-reconciled-v2.json")
        dst.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
        frames = sum(len(r.get("replay_trajectory", [])) for r in doc.get("rows", []))
        m = {"source_sha256": sha256(src), "candidate": dst.name, "candidate_sha256": sha256(dst),
             "rows": len(doc.get("rows", [])), "frames": frames, "counts": dict(counts),
             "timing_error_ms": {"median": percentile(ages, .5), "p95": percentile(ages, .95), "max": max(ages) if ages else None},
             "disagreement_horizontal_km": {"median": percentile(disagreements, .5), "p95": percentile(disagreements, .95), "max": max(disagreements) if disagreements else None}}
        report["manifests"][src.name] = m; report["totals"].update(counts); report["totals"]["frames"] += frames
        all_ages += ages; all_dis += disagreements
    report["totals"] = dict(report["totals"])
    report["timing_error_ms"] = {"median": percentile(all_ages, .5), "p95": percentile(all_ages, .95), "max": max(all_ages) if all_ages else None}
    report["disagreement_horizontal_km"] = {"median": percentile(all_dis, .5), "p95": percentile(all_dis, .95), "max": max(all_dis) if all_dis else None}
    (args.output / "validation-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "totals": report["totals"], "timing_error_ms": report["timing_error_ms"]}, sort_keys=True))


if __name__ == "__main__":
    main()
