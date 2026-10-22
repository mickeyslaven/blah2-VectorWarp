#!/usr/bin/env python3
"""Run bounded 509/527/557 MHz live-aircraft collection blocks.

The runner preserves the complete starting configuration, changes only the
frequency, transmitter metadata, and the frequency-specific fixed common gain,
then restores the exact starting configuration.  It writes an atomic state
file suitable for the hourly monitor and never scores aircraft itself.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import urllib.error
import urllib.request


API = "http://100.126.186.65:3000"
PROCESSING = ("vectorwarp-processor", "vectorwarp-tracker-primary")
QUIET_DURING_RETUNE = (
    "ford-street-radar-comparison",
    "ford-street-bearing-calibration",
    "vectorwarp-frequency-resolved-shadow.timer",
)
CANDIDATES = (
    {
        "frequency_hz": 509_000_000,
        "gain_db": 6,
        "name": "WPGH-TV RF20 (509 MHz center)",
        "latitude": 40.495278,
        "longitude": -80.004444,
        "altitude": 691.8,
    },
    {
        "frequency_hz": 527_000_000,
        "gain_db": 2,
        "name": "WPXI RF23 (527 MHz center)",
        "latitude": 40.46325,
        "longitude": -80.004306,
        "altitude": 666.0,
    },
    {
        "frequency_hz": 557_000_000,
        "gain_db": 7,
        "name": "WPCB-TV RF28 (557 MHz center)",
        "latitude": 40.392778,
        "longitude": -79.781389,
        "altitude": 630.0,
    },
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
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(raw).hexdigest()


def request(path, body=None, revision=None, method=None, timeout=180):
    headers = {"Origin": API}
    data = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    if data is not None:
        headers["Content-Type"] = "application/json"
    if revision is not None:
        headers.update(
            {
                "If-Match": '"' + revision + '"',
                "X-VectorWarp-Intent": "config-write-v1",
                "X-VectorWarp-Receiver-Sync": "synchronize-v1",
            }
        )
    req = urllib.request.Request(
        API + path,
        data=data,
        headers=headers,
        method=method or ("PUT" if data is not None else "GET"),
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read(16 * 1024 * 1024 + 1)
            require(len(raw) <= 16 * 1024 * 1024, "oversized API response")
            value = json.loads(raw)
            return value, response.headers.get("ETag", "").strip('"')
    except urllib.error.HTTPError as error:
        detail = error.read(131072).decode(errors="replace")
        raise RuntimeError(f"HTTP {error.code} {path}: {detail}") from error


def service_active(name):
    return subprocess.run(
        ["systemctl", "is-active", "--quiet", name], check=False
    ).returncode == 0


def service(action, names):
    existing = [
        name
        for name in names
        if subprocess.run(
            ["systemctl", "show", name, "--property=LoadState", "--value"],
            check=False,
            capture_output=True,
            text=True,
        ).stdout.strip()
        == "loaded"
    ]
    if existing:
        subprocess.run(["systemctl", action, *existing], check=True, timeout=180)


def capture_summary():
    status, _ = request("/capture/status", timeout=30)
    clip = status.get("clip", {})
    recent = clip.get("recentEvents", [])
    admitted = quarantined = failed = 0
    for event in recent:
        if event.get("state") == "failed":
            failed += 1
        for window in event.get("captureWindows", []):
            state = window.get("admissionState")
            admitted += state == "admitted"
            quarantined += state == "quarantined"
    return {
        "enabled": clip.get("enabled"),
        "recording": status.get("recording"),
        "queued_clips": clip.get("queuedClips"),
        "dropped_events": clip.get("droppedEvents"),
        "recent_admitted_windows": admitted,
        "recent_quarantined_windows": quarantined,
        "recent_failed_events": failed,
        "clip_revision": clip.get("clipRevision"),
    }


def wait_healthy(frequency, gain, timeout=180):
    deadline = time.monotonic() + timeout
    previous_frame = None
    last = ""
    while time.monotonic() < deadline:
        try:
            status, _ = request("/api/system/status", timeout=20)
            upstream, _ = request("/api/upstream/status", timeout=20)
            actual = upstream.get("actual", {})
            frame = status.get("lastFrameAt")
            healthy = (
                status.get("processorFresh") is True
                and status.get("radar") == "receiving"
                and not status.get("errors")
                and upstream.get("matched") is True
                and actual.get("centerFrequency") == frequency
                and actual.get("sampleRate") == 2_400_000
                and actual.get("channels") == 5
                and actual.get("mode") == "coherent"
                and actual.get("gain") == gain
                and not any(
                    actual.get(key)
                    for key in ("reconfiguring", "recovering", "cooldownActive")
                )
            )
            if healthy and previous_frame is not None and frame > previous_frame:
                return {"system": status, "upstream": upstream}
            if healthy:
                previous_frame = frame
            last = (
                f"frame={frame} radar={status.get('radar')} "
                f"fresh={status.get('processorFresh')} actual={actual}"
            )
        except Exception as error:  # health loop records the latest bounded error
            last = str(error)
        time.sleep(3)
    raise RuntimeError("live chain did not become healthy: " + last)


def apply_config(config, revision):
    result, _ = request(
        "/api/config?restart=true", config, revision=revision, timeout=600
    )
    require(result.get("ok") is True, "managed config apply failed")
    return result["config"], result["revision"], result


def wait_restart_reconfigurable(timeout=180):
    """Wait until Apply is no longer holding the config-write restart lock.

    A retune deliberately invalidates physical attestations.  The processor
    cannot produce a fresh frame until those attestations are restored, so
    waiting for fresh radar data here would deadlock the re-attestation Apply.
    `command-complete` is the first safe state for that second write.
    """
    deadline = time.monotonic() + timeout
    last = "unknown"
    while time.monotonic() < deadline:
        try:
            status, _ = request("/api/system/status", timeout=20)
            last = (status.get("restart") or {}).get("state", "none")
            if last not in ("running", "scheduled"):
                return last
        except Exception as error:
            last = str(error)
        time.sleep(2)
    raise RuntimeError("managed restart did not release config lock: " + last)


def candidate_config(original, candidate):
    config = copy.deepcopy(original)
    config["capture"]["fc"] = candidate["frequency_hz"]
    heimdall = config["capture"]["device"]["heimdall"]
    heimdall["gain"] = candidate["gain_db"]
    heimdall["gain_control"] = "fixed"
    heimdall.pop("channel_gains", None)
    config["location"]["tx"] = {
        "latitude": candidate["latitude"],
        "longitude": candidate["longitude"],
        "altitude": candidate["altitude"],
        "name": candidate["name"] + " bounded aircraft trial",
    }
    # Bearing is already disabled in the source epoch.  Keep it disabled so a
    # frequency trial tests detection rather than an unqualified manifold.
    config["process"]["bearing"]["enable"] = False
    return config


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--minutes", type=float, default=30.0)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    require(args.root.is_absolute(), "--root must be absolute")
    require(args.minutes >= 1, "--minutes must be at least one")
    args.root.mkdir(parents=True, exist_ok=False)

    original, original_revision = request("/api/config")
    require(original["capture"]["fc"] == 563_000_000, "expected 563 MHz origin")
    require(original["capture"]["fs"] == 2_400_000, "sample rate changed")
    require(original["capture"]["device"]["channel_count"] == 5, "channel count changed")
    original_sha = canonical_sha(original)
    service_states = {
        name: service_active(name) for name in PROCESSING + QUIET_DURING_RETUNE
    }
    atomic(args.root / "original-config.json", original)
    atomic(
        args.root / "manifest.json",
        {
            "schema": "vectorwarp-three-frequency-aircraft-trial-v1",
            "created_at": time.time(),
            "minutes_per_block": args.minutes,
            "original_revision": original_revision,
            "original_canonical_sha256": original_sha,
            "candidates": CANDIDATES,
            "service_states": service_states,
        },
    )

    # The public preview validator intentionally has no baseline document and
    # therefore rejects unchanged advanced ADS-B fields as "read-only".  Live
    # Apply validates against the current baseline and is the authoritative
    # check.  Perform the bounded invariants locally before any mutation.
    for candidate in CANDIDATES:
        proposed = candidate_config(original, candidate)
        require(proposed["capture"]["fs"] == 2_400_000, "candidate changed sample rate")
        require(proposed["capture"]["device"]["channel_count"] == 5,
                "candidate changed channel count")
        require(proposed["process"]["bearing"]["enable"] is False,
                "candidate unexpectedly enables bearing")
    if args.validate_only:
        atomic(args.root / "validation.json", {"valid": True, "at": time.time()})
        print(json.dumps({"ok": True, "validate_only": True}))
        return

    interrupted = False

    def stop_requested(signum, _frame):
        nonlocal interrupted
        interrupted = True

    signal.signal(signal.SIGTERM, stop_requested)
    signal.signal(signal.SIGINT, stop_requested)
    blocks = []
    state = {"status": "starting", "blocks": blocks, "original_revision": original_revision}
    atomic(args.root / "state.json", state)
    current_revision = original_revision

    try:
        service("stop", QUIET_DURING_RETUNE)
        for index, candidate in enumerate(CANDIDATES):
            require(not interrupted, "trial interrupted")
            state.update(
                {
                    "status": "retuning",
                    "candidate_index": index,
                    "frequency_hz": candidate["frequency_hz"],
                }
            )
            atomic(args.root / "state.json", state)
            service("stop", PROCESSING)
            current, current_revision = request("/api/config")
            desired = candidate_config(original, candidate)
            selected, current_revision, receipt = apply_config(desired, current_revision)
            atomic(args.root / f"{candidate['frequency_hz']}-apply.json", receipt)
            wait_restart_reconfigurable()

            # Managed frequency changes may clear physical attestations.  The
            # hardware did not move, so restore exactly the original attestations.
            geometry = selected["capture"]["device"]["array_geometry"]
            original_geometry = original["capture"]["device"]["array_geometry"]
            changed = False
            for key in ("mapping_confirmed", "geometry_confirmed", "orientation_confirmed"):
                if geometry.get(key) != original_geometry.get(key):
                    geometry[key] = original_geometry.get(key)
                    changed = True
            if changed:
                selected, current_revision, receipt = apply_config(selected, current_revision)
                atomic(args.root / f"{candidate['frequency_hz']}-reattest.json", receipt)
                wait_restart_reconfigurable()

            service("start", PROCESSING)
            health = wait_healthy(candidate["frequency_hz"], candidate["gain_db"])
            started = time.time()
            block = {
                "frequency_hz": candidate["frequency_hz"],
                "name": candidate["name"],
                "gain_db": candidate["gain_db"],
                "revision": current_revision,
                "started_at": started,
                "start_capture": capture_summary(),
                "start_last_frame_at": health["system"].get("lastFrameAt"),
                "status": "collecting",
            }
            blocks.append(block)
            state.update({"status": "collecting", "blocks": blocks})
            atomic(args.root / "state.json", state)

            deadline = time.monotonic() + args.minutes * 60
            while time.monotonic() < deadline:
                require(not interrupted, "trial interrupted")
                remaining = max(0, deadline - time.monotonic())
                state["remaining_seconds"] = round(remaining, 1)
                atomic(args.root / "state.json", state)
                time.sleep(min(30, remaining))

            end_health = wait_healthy(candidate["frequency_hz"], candidate["gain_db"])
            block.update(
                {
                    "ended_at": time.time(),
                    "duration_seconds": time.time() - started,
                    "end_capture": capture_summary(),
                    "end_last_frame_at": end_health["system"].get("lastFrameAt"),
                    "status": "complete",
                }
            )
            atomic(args.root / "state.json", state)

        state["status"] = "restoring"
        atomic(args.root / "state.json", state)
    finally:
        try:
            service("stop", PROCESSING)
            _current, current_revision = request("/api/config")
            restored, restored_revision, receipt = apply_config(original, current_revision)
            atomic(args.root / "restore-receipt.json", receipt)
            wait_restart_reconfigurable()
            # Receiver operations deliberately clear physical attestations in
            # the saved candidate.  Once readback matches and no further RF
            # operation is needed, restore the unchanged operator attestations
            # in a second managed Apply.
            if restored != original:
                restored_geometry = restored["capture"]["device"]["array_geometry"]
                original_geometry = original["capture"]["device"]["array_geometry"]
                for key in ("mapping_confirmed", "geometry_confirmed", "orientation_confirmed"):
                    restored_geometry[key] = original_geometry.get(key)
                restored, restored_revision, receipt2 = apply_config(restored, restored_revision)
                atomic(args.root / "restore-reattest-receipt.json", receipt2)
                wait_restart_reconfigurable()
            require(restored == original, "restored config differs from original")
            require(restored_revision == original_revision, "restored revision differs from original")
            service("start", [name for name in PROCESSING if service_states.get(name)])
            final_health = wait_healthy(
                original["capture"]["fc"],
                original["capture"]["device"]["heimdall"]["gain"],
            )
            for name in QUIET_DURING_RETUNE:
                if service_states.get(name):
                    service("start", (name,))
            state.update(
                {
                    "status": "complete" if not interrupted else "interrupted-restored",
                    "completed_at": time.time(),
                    "restored_revision": restored_revision,
                    "restored_canonical_sha256": canonical_sha(restored),
                    "restored_last_frame_at": final_health["system"].get("lastFrameAt"),
                }
            )
            atomic(args.root / "state.json", state)
        except BaseException as restore_error:
            state.update(
                {
                    "status": "restore-failed",
                    "restore_error": str(restore_error),
                    "failed_at": time.time(),
                }
            )
            atomic(args.root / "state.json", state)
            raise

    print(json.dumps({"ok": True, "root": str(args.root), "blocks": blocks}))


if __name__ == "__main__":
    main()
