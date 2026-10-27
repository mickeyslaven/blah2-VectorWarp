#!/usr/bin/env python3
"""Qualify 551 MHz Kraken surveillance gain with OWL as an external reference.

The trial snapshots the complete starting configuration, changes only frequency,
transmitter identity, and per-channel fixed gain, and measures every candidate
after a full managed receiver restart/calibration.  A short native recording is
retained per stage.  The maximum stage is always observed; the final live stage
is the highest candidate whose raw pre-calibration rail fraction stays below the
configured ceiling.  Any failure restores the exact starting configuration.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request


API = "http://100.126.186.65:3000"
WTAE = {
    "latitude": 40.28028,
    "longitude": -79.80278,
    "altitude": 564.1848,
    "name": "WTAE-TV RF27 551 MHz OWL external-reference gain trial",
}
STAGES = (
    ("existing", [7.7, 7.7, 12.5, 3.7, 2.7]),
    ("moderate", [19.7, 19.7, 22.9, 15.7, 14.4]),
    ("high", [29.7, 29.7, 32.8, 25.4, 22.9]),
    ("very-high", [38.6, 38.6, 42.1, 33.8, 32.8]),
    ("maximum", [49.6, 49.6, 49.6, 49.6, 49.6]),
)


def require(value, message):
    if not value:
        raise RuntimeError(message)


def atomic(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    os.replace(temporary, path)


def canonical_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(",", ":")).encode()).hexdigest()


def request(path, body=None, revision=None, method=None, timeout=600,
            recording=False):
    headers = {"Origin": API}
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    if data is not None:
        headers["Content-Type"] = "application/json"
    if revision is not None:
        headers.update({
            "If-Match": f'"{revision}"',
            "X-VectorWarp-Intent": "config-write-v1",
            "X-VectorWarp-Receiver-Sync": "synchronize-v1",
        })
    if recording:
        headers["X-VectorWarp-Intent"] = "recording-toggle-v1"
    req = urllib.request.Request(API + path, data=data, headers=headers,
        method=method or ("PUT" if data is not None else "GET"))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read(16 * 1024 * 1024 + 1)
            require(len(raw) <= 16 * 1024 * 1024, "oversized API response")
            return json.loads(raw), response.headers.get("ETag", "").strip('"')
    except urllib.error.HTTPError as error:
        detail = error.read(131072).decode(errors="replace")
        raise RuntimeError(f"HTTP {error.code} {path}: {detail}") from error


def wait_reconfigurable(timeout=240):
    deadline = time.monotonic() + timeout
    last = "unknown"
    while time.monotonic() < deadline:
        try:
            status, _ = request("/api/system/status", timeout=20)
            last = (status.get("restart") or {}).get("state", "none")
            if last not in ("running", "scheduled"):
                return
        except (ConnectionError, OSError, urllib.error.URLError) as error:
            last = str(error)
        time.sleep(2)
    raise RuntimeError(f"restart did not release config lock: {last}")


def wait_healthy(gains, timeout=360):
    deadline = time.monotonic() + timeout
    previous = None
    last = "unknown"
    while time.monotonic() < deadline:
        try:
            status, _ = request("/api/system/status", timeout=20)
            upstream, _ = request("/api/upstream/status", timeout=20)
            actual = upstream.get("actual", {})
            adc = actual.get("adcHeadroom") or {}
            channels = adc.get("channels") or []
            frame = status.get("lastFrameAt")
            observed = [round(float(c.get("actualGainDb", -999)), 1) for c in channels]
            healthy = (status.get("radar") == "receiving" and
                status.get("processorFresh") is True and not status.get("errors") and
                upstream.get("matched") is True and
                actual.get("centerFrequency") == 551_000_000 and
                actual.get("sampleRate") == 2_400_000 and
                actual.get("channels") == 5 and actual.get("mode") == "coherent" and
                actual.get("calibrationState") == "CONVERGED" and
                not any(actual.get(key) for key in
                    ("reconfiguring", "recovering", "cooldownActive")) and
                len(channels) == 5 and observed == gains)
            if healthy and previous is not None and frame > previous:
                return status, upstream
            if healthy:
                previous = frame
            last = {"frame": frame, "system": status, "upstream": upstream}
        except (ConnectionError, OSError, urllib.error.URLError) as error:
            last = str(error)
        time.sleep(3)
    raise RuntimeError("live chain did not become healthy: " +
        json.dumps(last, separators=(",", ":"))[:8192])


def apply_config(config, revision):
    try:
        result, etag = request("/api/config?restart=true", config, revision=revision)
        require(result.get("ok") is True, "managed config apply failed")
        return result["config"], result.get("revision") or etag, result
    except (ConnectionError, OSError, urllib.error.URLError) as error:
        # A successful managed apply restarts the API that accepted the PUT. On
        # this installed release the socket can close before the JSON response
        # reaches the client. Recover only when the complete saved document is
        # byte-for-byte equivalent to the requested document; otherwise fail.
        deadline = time.monotonic() + 180
        latest = error
        while time.monotonic() < deadline:
            try:
                selected, etag = request("/api/config", timeout=20)
                require(canonical_sha(selected) == canonical_sha(config),
                    "API restarted but saved configuration differs from request")
                return selected, etag, {
                    "ok": True,
                    "recovered_after_api_restart": True,
                    "revision": etag,
                    "config": selected,
                    "connection_error": str(error),
                }
            except Exception as retry_error:
                latest = retry_error
                time.sleep(2)
        raise RuntimeError(f"managed apply outcome unresolved after API restart: {latest}") from error


def apply_stage(original, gains, revision, root, label):
    desired = copy.deepcopy(original)
    desired["capture"]["fc"] = 551_000_000
    heimdall = desired["capture"]["device"]["heimdall"]
    heimdall["gain_control"] = "fixed"
    heimdall["channel_gains"] = gains
    desired["location"]["tx"] = copy.deepcopy(WTAE)
    desired["process"]["bearing"]["enable"] = False
    selected, revision, receipt = apply_config(desired, revision)
    atomic(root / f"{label}-apply.json", receipt)
    wait_reconfigurable()

    geometry = selected["capture"]["device"]["array_geometry"]
    original_geometry = original["capture"]["device"]["array_geometry"]
    changed = False
    for key in ("mapping_confirmed", "geometry_confirmed", "orientation_confirmed"):
        if geometry.get(key) != original_geometry.get(key):
            geometry[key] = original_geometry.get(key)
            changed = True
    if changed:
        selected, revision, receipt = apply_config(selected, revision)
        atomic(root / f"{label}-reattest.json", receipt)
        wait_reconfigurable()
    return selected, revision


def adc_snapshot(upstream):
    adc = upstream["actual"]["adcHeadroom"]
    return [{key: channel.get(key) for key in (
        "channel", "generation", "components", "sampledComponents",
        "actualGainDb", "railFraction", "nearRailFraction", "rmsFraction",
        "p999Fraction")} for channel in adc["channels"]]


def toggle_recording(want, timeout=60):
    current, _ = request("/capture/status", timeout=20)
    if current.get("requested") != want:
        request("/capture/toggle", {}, method="POST", timeout=20, recording=True)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status, _ = request("/capture/status", timeout=20)
        if status.get("requested") == want and status.get("acknowledged") is True and \
                status.get("recording") == want:
            return status
        time.sleep(1)
    raise RuntimeError(f"recording did not reach requested={want}: {status}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--settle-seconds", type=float, default=15)
    parser.add_argument("--record-seconds", type=float, default=3)
    parser.add_argument("--rail-ceiling", type=float, default=.01)
    args = parser.parse_args()
    require(args.root.is_absolute(), "--root must be absolute")
    args.root.mkdir(parents=True, exist_ok=False)

    original, revision = request("/api/config")
    original_revision = revision
    atomic(args.root / "original-config.json", original)
    state = {"schema": "vectorwarp-551-owl-high-gain-trial-v1",
        "started_at": time.time(), "original_revision": original_revision,
        "original_sha256": canonical_sha(original), "stages": [],
        "status": "starting", "rail_ceiling": args.rail_ceiling}
    atomic(args.root / "state.json", state)
    selected = None
    try:
        for label, gains in STAGES:
            state.update({"status": "applying", "active_stage": label,
                "active_gains_db": gains})
            atomic(args.root / "state.json", state)
            _config, revision = apply_stage(original, gains, revision,
                args.root, label)
            _system, upstream = wait_healthy(gains)
            time.sleep(args.settle_seconds)
            system, _ = request("/api/system/status", timeout=20)
            upstream, _ = request("/api/upstream/status", timeout=20)
            before = adc_snapshot(upstream)
            started = toggle_recording(True)
            time.sleep(args.record_seconds)
            stopped = toggle_recording(False)
            system2, _ = request("/api/system/status", timeout=20)
            upstream2, _ = request("/api/upstream/status", timeout=20)
            after = adc_snapshot(upstream2)
            worst_rail = max(float(channel["railFraction"]) for channel in after)
            worst_near = max(float(channel["nearRailFraction"]) for channel in after)
            worst_p999 = max(float(channel["p999Fraction"]) for channel in after)
            stage = {"label": label, "gains_db": gains,
                "revision": revision, "started_at": time.time(),
                "adc_before_recording": before, "adc_after_recording": after,
                "worst_rail_fraction": worst_rail,
                "worst_near_rail_fraction": worst_near,
                "worst_p999_fraction": worst_p999,
                "passes_rail_ceiling": worst_rail < args.rail_ceiling,
                "recording_file": started.get("recordingFile"),
                "recorded_samples": stopped.get("recordedSamples"),
                "first_frame_at": system.get("lastFrameAt"),
                "last_frame_at": system2.get("lastFrameAt")}
            state["stages"].append(stage)
            if stage["passes_rail_ceiling"]:
                selected = (label, gains)
            atomic(args.root / f"{label}-measurement.json", stage)
            atomic(args.root / "state.json", state)

        require(selected is not None, "every gain stage exceeded the rail ceiling")
        label, gains = selected
        state.update({"status": "selecting", "selected_stage": label,
            "selected_gains_db": gains})
        atomic(args.root / "state.json", state)
        _config, revision = apply_stage(original, gains, revision, args.root,
            "selected-final")
        system, upstream = wait_healthy(gains)
        state.update({"status": "complete", "completed_at": time.time(),
            "selected_revision": revision,
            "selected_adc": adc_snapshot(upstream),
            "selected_last_frame_at": system.get("lastFrameAt")})
        atomic(args.root / "state.json", state)
    except Exception as error:
        state.update({"status": "failed-restoring", "error": str(error),
            "failed_at": time.time()})
        atomic(args.root / "state.json", state)
        try:
            deadline = time.monotonic() + 180
            while True:
                try:
                    _current, revision = request("/api/config", timeout=20)
                    break
                except (ConnectionError, OSError, urllib.error.URLError):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(2)
            restored, revision, receipt = apply_config(original, revision)
            atomic(args.root / "restore-receipt.json", receipt)
            wait_reconfigurable()
            state.update({"status": "failed-restored", "restored_revision": revision,
                "restored_sha256": canonical_sha(restored)})
            atomic(args.root / "state.json", state)
        finally:
            raise


if __name__ == "__main__":
    main()
