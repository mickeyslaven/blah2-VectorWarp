#!/usr/bin/env python3
"""Replay one Kraken IQ file through coherent and noncoherent fusion offline."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import re

try:
    import yaml
except ModuleNotFoundError:
    yaml = None


def load_harness(path):
    spec = importlib.util.spec_from_file_location("processor_replay_harness", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_config(path):
    if path.suffix.lower() == ".json":
        return json.loads(path.read_text())
    if yaml is not None:
        return yaml.safe_load(path.read_text())
    ruby = subprocess.run(
        ["ruby", "-ryaml", "-rjson", "-e",
         "puts JSON.generate(YAML.safe_load(File.read(ARGV[0]), aliases: true))", str(path)],
        check=True, capture_output=True, text=True)
    return json.loads(ruby.stdout)


def decode_many(payload):
    text = bytes(payload).decode("utf-8")
    decoder = json.JSONDecoder()
    values = []
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        values.append(value)
    return values


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(10)


def set_stationary_clutter(config, state):
    """Override only tracker admission/retirement filtering for replay A/B."""
    if state is None:
        return
    initiate = config["process"]["tracker"]["initiate"]
    if state == "on" and "stationary_clutter" not in initiate:
        # In resident mode an absent block is the production default-on path.
        # Preserve it verbatim so the ON arm reproduces the deployed config.
        return
    settings = initiate.setdefault("stationary_clutter", {})
    settings["enable"] = state == "on"
    if state == "on":
        # The native config reader requires both parameters when the block is
        # explicitly enabled. Production defaults apply only when the entire
        # block is absent.
        settings.setdefault("persistence_cpis", 60)
        settings.setdefault("max_doppler_hz", 15.0)


def parse_tracker_diagnostics(output):
    """Aggregate resident-tracker admission diagnostics over the entire replay."""
    totals = {}
    frames = 0
    for line in output.splitlines():
        if "GPU_TRACKER_PREPARED" not in line:
            continue
        frames += 1
        for key, raw in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", line):
            if key in {"frame", "generation", "transaction"}:
                continue
            totals[key] = totals.get(key, 0) + int(raw)
    return {"frames": frames, "totals": totals}


def run_arm(harness, binary, source, recording, mode, timeout, reference_channel=None,
            pfa=None, label=None, direct_path_null_db=None, capture_maps=False,
            enable_tbd=False, tracker_m=None, tracker_n=None, tracker_max_acc=None,
            acceleration="cpu", complex_cutout=None, tbd_delay_motion_bins=None,
            tbd_doppler_motion_bins=None, tracker_beam_tolerance=None,
            disable_tracker=False, stationary_clutter=None):
    label = label or (mode if reference_channel is None else f"dedicated_ch{reference_channel}")
    class CapturingSinks(harness.SinkGroup):
        def __init__(self):
            super().__init__()
            self.payloads["detection"] = bytearray()
            self.payloads["track"] = bytearray()
            if capture_maps:
                self.payloads["map"] = bytearray()

        def _serve(self, name, listener):
            while not self.stop.is_set():
                try:
                    connection, _ = listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    return
                with connection:
                    connection.settimeout(.2)
                    while not self.stop.is_set():
                        try:
                            block = connection.recv(65536)
                        except TimeoutError:
                            continue
                        except OSError:
                            break
                        if not block:
                            break
                        self.bytes[name] += len(block)
                        if name in self.payloads:
                            self.payloads[name].extend(block)

    with tempfile.TemporaryDirectory(prefix=f"vectorwarp-{label}-") as temporary:
        root = Path(temporary)
        status = harness.StatusServer()
        sinks = CapturingSinks()
        process = None
        log_file = None
        try:
            config = json.loads(json.dumps(source))
            config["capture"]["replay"] = {
                "state": True, "loop": False, "file": str(recording), "format": "auto"}
            if source.get("_replay_echo") is not None:
                config["capture"]["replay"]["inject_echo"] = source["_replay_echo"]
            config.pop("_replay_echo", None)
            config["process"]["performance"].update(
                {"acceleration": acceleration, "surveillance_workers": 2, "fft_threads": 1})
            if enable_tbd:
                config["process"]["performance"]["acceleration"] = "gpu-resident"
            config["process"]["surveillance_fusion"]["mode"] = mode
            if direct_path_null_db is not None:
                config["process"]["surveillance_fusion"]["direct_path_null_db"] = direct_path_null_db
            if reference_channel is not None:
                channels = list(range(config["capture"]["device"]["channel_count"]))
                config["capture"]["device"]["reference_channel"] = reference_channel
                config["capture"]["device"]["surveillance_channels"] = [
                    channel for channel in channels if channel != reference_channel]
                config["process"]["reference_synthesis"] = {
                    "mode": "dedicated", "channels": [reference_channel]}
            config["process"]["detection"]["track_before_detect"]["enable"] = enable_tbd
            if tbd_delay_motion_bins is not None:
                config["process"]["detection"]["track_before_detect"][
                    "delay_motion_bins"] = tbd_delay_motion_bins
            if tbd_doppler_motion_bins is not None:
                config["process"]["detection"]["track_before_detect"][
                    "doppler_motion_bins"] = tbd_doppler_motion_bins
            if not enable_tbd:
                guided = config["process"]["tracker"].get("track_guided_shadow")
                if isinstance(guided, dict):
                    guided["enable"] = False
                    guided["promote"] = False
            initiate = config["process"]["tracker"]["initiate"]
            if tracker_m is not None:
                initiate["M"] = tracker_m
            if tracker_n is not None:
                initiate["N"] = tracker_n
            if tracker_max_acc is not None:
                initiate["maxAcc"] = tracker_max_acc
            if tracker_beam_tolerance is not None:
                config["process"]["tracker"]["beam_tolerance"] = tracker_beam_tolerance
            set_stationary_clutter(config, stationary_clutter)
            if disable_tracker:
                config["process"]["tracker"]["enable"] = False
            if pfa is not None:
                config["process"]["detection"]["pfa"] = pfa
            config["process"]["bearing"]["enable"] = False
            recorder = config["process"]["bearing"].get("manifold_recorder")
            if isinstance(recorder, dict):
                recorder["enabled"] = False
            if mode == "noncoherent":
                fusion = config["process"]["surveillance_fusion"]
                fusion["effective_beam_trials"] = fusion.get("beams", 12)
                fusion["direct_path_null_db"] = 0
            status.start()
            sinks.start()
            config["network"] = {"ip": harness.HOST, "ports": {
                "api": status.port, **sinks.ports(),
                "bearing": sinks.ports()["track"],
                "manifold": sinks.ports()["track"],
                "config": sinks.ports()["timestamp"]}}
            config["truth"] = {"adsb": {"enabled": False}, "ais": {"enabled": False}}
            config["save"] = {"iq": False, "map": False, "detection": False,
                              "timing": False, "path": str(root)}
            cutout_path = root / "complex-cutout.jsonl"
            if complex_cutout is not None:
                config["save"]["complex_cutout"] = {
                    "state": True,
                    "path": str(cutout_path),
                    **complex_cutout,
                }
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            log_path = root / "processor.log"
            log_file = log_path.open("w+")
            process = subprocess.Popen(
                [str(binary), "-c", str(config_path)], cwd=root,
                stdout=log_file, stderr=subprocess.STDOUT, text=True,
                start_new_session=True, preexec_fn=lambda: os.nice(15))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                states = status.snapshot()
                if any(item.get("state") in ("complete", "error") for item in states):
                    break
                if process.poll() is not None:
                    break
                time.sleep(.25)
            states = status.snapshot()
            complete = any(item.get("state") == "complete" for item in states)
            stop(process)
            log_file.flush()
            log_file.seek(0)
            output = log_file.read()
            if not complete:
                raise RuntimeError(f"{label} replay did not complete: states={states[-3:]} log={output[-12000:]}")
            result = {
                "mode": label,
                "detections": decode_many(sinks.payloads["detection"]),
                "tracks": decode_many(sinks.payloads["track"]),
                "statuses": states,
                "bytes": sinks.bytes,
                "tracker_diagnostics": parse_tracker_diagnostics(output),
                "log_tail": output[-12000:],
            }
            if capture_maps:
                result["maps"] = decode_many(sinks.payloads["map"])
            if complex_cutout is not None:
                result["complex_cutouts"] = [json.loads(line) for line in
                    cutout_path.read_text().splitlines() if line.strip()]
            return result
        finally:
            if process is not None:
                stop(process)
            if log_file is not None:
                log_file.close()
            sinks.close()
            status.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--recording", required=True, type=Path)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--reference-sweep", action="store_true")
    parser.add_argument("--pfa", type=float)
    parser.add_argument("--disable-clutter", action="store_true")
    parser.add_argument("--clutter-delay-max", type=int,
                        help="Override the exclusive clutter-canceller delay limit")
    parser.add_argument("--cpi", type=float, help="Override CPI seconds")
    parser.add_argument("--reference-analysis-interval", type=int)
    parser.add_argument("--reference-covariance-smoothing", type=float)
    parser.add_argument("--null-sweep", type=str,
                        help="Comma-separated coherent direct-path null depths in dB")
    parser.add_argument("--null-sweep-mode", choices=("coherent_beam_bank",
                        "noise_whitened_beam_bank"), default="coherent_beam_bank",
                        help="Fusion arm used by --null-sweep")
    parser.add_argument("--capture-maps", action="store_true",
                        help="Retain full map frames for offline truth-cell diagnostics")
    parser.add_argument("--enable-tbd", action="store_true",
                        help="Keep the configured track-before-detect banks enabled")
    parser.add_argument("--tbd-delay-motion-bins", type=int,
                        help="Override replay TBD delay motion half-width")
    parser.add_argument("--tbd-doppler-motion-bins", type=int,
                        help="Override replay TBD Doppler motion half-width")
    parser.add_argument("--acceleration", choices=("cpu", "gpu-resident"), default="cpu")
    parser.add_argument("--tracker-m", type=int,
                        help="Override tracker initiation M for bounded replay experiments")
    parser.add_argument("--tracker-n", type=int,
                        help="Override tracker initiation N for bounded replay experiments")
    parser.add_argument("--tracker-max-acc", type=float,
                        help="Override tracker initiation acceleration span in Hz/s")
    parser.add_argument("--tracker-beam-tolerance", type=int,
                        help="Require TBD history to remain within this many coarse beams")
    parser.add_argument("--disable-tracker", action="store_true",
                        help="Disable tracking for detector-only replay diagnostics")
    parser.add_argument("--stationary-clutter", choices=("on", "off"),
                        help="Override only resident tracker stationary-clutter admission/retirement")
    parser.add_argument("--modes", type=str,
                        help="Comma-separated arm labels to run from the selected sweep")
    parser.add_argument("--complex-cutout-delay-km", type=float,
                        help="Replay-only pre-fusion complex CAF cutout centre")
    parser.add_argument("--complex-cutout-doppler-hz", type=float,
                        help="Replay-only pre-fusion complex CAF cutout centre")
    parser.add_argument("--complex-cutout-delay-radius-bins", type=int, default=3)
    parser.add_argument("--complex-cutout-doppler-radius-bins", type=int, default=3)
    parser.add_argument("--complex-cutout-max-frames", type=int, default=32)
    parser.add_argument("--inject-echo-amplitude-db", type=float)
    parser.add_argument("--inject-echo-delay-km", type=float)
    parser.add_argument("--inject-echo-doppler-hz", type=float)
    parser.add_argument("--inject-echo-azimuth-deg-array", type=float, default=0)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.config = args.config.resolve()
    args.recording = args.recording.resolve()
    args.harness = args.harness.resolve()
    args.output = args.output.resolve()
    harness = load_harness(args.harness)
    source = load_config(args.config)
    if ((args.complex_cutout_delay_km is None) !=
            (args.complex_cutout_doppler_hz is None)):
        parser.error("complex cutout delay and Doppler must be supplied together")
    complex_cutout = None if args.complex_cutout_delay_km is None else {
        "delay_km": args.complex_cutout_delay_km,
        "doppler_hz": args.complex_cutout_doppler_hz,
        "delay_radius_bins": args.complex_cutout_delay_radius_bins,
        "doppler_radius_bins": args.complex_cutout_doppler_radius_bins,
        "max_frames": args.complex_cutout_max_frames,
    }
    result = {"schema": "vectorwarp-fusion-replay-ab-v1",
              "recording": str(args.recording), "arms": {}}
    if args.reference_sweep:
        arms = [(f"dedicated_ch{channel}", "noncoherent", channel, None)
                for channel in range(5)]
    elif args.null_sweep:
        depths = [float(value) for value in args.null_sweep.split(",")]
        prefix = "coherent" if args.null_sweep_mode == "coherent_beam_bank" else "whitened"
        arms = [(f"{prefix}_null_{depth:g}db", args.null_sweep_mode, None, depth)
                for depth in depths]
    else:
        arms = [("coherent_beam_bank", "coherent_beam_bank", None, None),
                ("noise_whitened_beam_bank", "noise_whitened_beam_bank", None, None),
                ("noncoherent", "noncoherent", None, None)]
    if args.modes:
        requested = {value.strip() for value in args.modes.split(",") if value.strip()}
        arms = [arm for arm in arms if arm[0] in requested]
        if not arms:
            parser.error("--modes did not match any selected replay arm")
    for label, mode, reference_channel, direct_path_null_db in arms:
        arm_source = json.loads(json.dumps(source))
        if args.inject_echo_amplitude_db is not None:
            if args.inject_echo_delay_km is None or args.inject_echo_doppler_hz is None:
                parser.error("echo injection requires amplitude, delay and Doppler")
            arm_source["_replay_echo"] = {
                "state": True, "amplitude_db": args.inject_echo_amplitude_db,
                "delay_km": args.inject_echo_delay_km,
                "doppler_hz": args.inject_echo_doppler_hz,
                "target_azimuth_deg_array": args.inject_echo_azimuth_deg_array,
            }
        if args.disable_clutter:
            arm_source["process"]["clutter"]["enable"] = False
        if args.clutter_delay_max is not None:
            arm_source["process"]["clutter"]["delayMax"] = args.clutter_delay_max
        if args.cpi is not None:
            arm_source["process"]["data"]["cpi"] = args.cpi
        if args.reference_analysis_interval is not None:
            arm_source["process"]["reference_synthesis"]["analysis_interval"] = (
                args.reference_analysis_interval)
        if args.reference_covariance_smoothing is not None:
            arm_source["process"]["reference_synthesis"]["covariance_smoothing"] = (
                args.reference_covariance_smoothing)
        result["arms"][label] = run_arm(
            harness, args.binary, arm_source, args.recording, mode, args.timeout,
            reference_channel, args.pfa, label, direct_path_null_db,
            args.capture_maps, args.enable_tbd, args.tracker_m, args.tracker_n,
            args.tracker_max_acc, args.acceleration, complex_cutout,
            args.tbd_delay_motion_bins, args.tbd_doppler_motion_bins,
            args.tracker_beam_tolerance, args.disable_tracker,
            args.stationary_clutter)
    args.output.write_text(json.dumps(result, separators=(",", ":")))
    print(json.dumps({mode: {"detection_frames": len(arm["detections"]),
                             "track_frames": len(arm["tracks"]),
                             "detection_bytes": arm["bytes"]["detection"]}
                      for mode, arm in result["arms"].items()}, indent=2))


if __name__ == "__main__":
    main()
