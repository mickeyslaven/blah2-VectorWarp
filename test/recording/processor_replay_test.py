#!/usr/bin/env python3
"""Offline full-processor replay acceptance harness.

This starts only a supplied processor binary, completed synthetic BLAH2IQ v1
files, and loopback HTTP/TCP peers.  It never discovers, opens, or configures an
SDR.  Run it explicitly; it is deliberately not wired into CMake yet.
All cases request CPU processing. Receiver-tag parity here does not qualify
live ingress or resident GPU execution of the selected operational workload.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
import math
import hashlib


# Source-tree qualification and explicitly staged detached fixtures share the
# production peer owner; production never imports this test module.
_SESSION_PATH = Path(os.environ.get("VECTORWARP_REPLAY_SESSION",
    str(Path(__file__).resolve().parent.parent.parent / "script/replay_session.py")))
_SESSION_SPEC = importlib.util.spec_from_file_location("processor_replay_session", _SESSION_PATH)
SESSION = importlib.util.module_from_spec(_SESSION_SPEC)
_SESSION_SPEC.loader.exec_module(SESSION)
HOST, PORT_NAMES = SESSION.HOST, SESSION.PORT_NAMES
MAX_RETAINED_OUTPUT = SESSION.MAX_RETAINED_OUTPUT
free_listener, SinkGroup, StatusServer = SESSION.free_listener, SESSION.SinkGroup, SESSION.StatusServer
json_values, assert_finite = SESSION.json_values, SESSION.assert_finite
RSP_TYPES = ("RspDuo", "Usrp", "HackRF")


def write_blah2iq(path, channels, sample_rate, frequency, frames=3, frame_samples=None):
    """Write the little-endian BLAH2IQ v1 layout used by Recording.cpp."""
    frame_samples = int(sample_rate * .02) if frame_samples is None else frame_samples
    samples = frames * frame_samples
    header = bytearray(64)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 1, 64, channels, sample_rate, frequency, 1)
    struct.pack_into("<Q", header, 40, samples)
    struct.pack_into("<Q", header, 48, 1)  # completed cleanly
    patterns = []
    for channel in range(channels):
        # Deterministic broadband samples, continuous across recording blocks.
        state = 0x9E3779B9 ^ channel
        pattern = bytearray()
        for _ in range(1024):
            state = (1664525 * state + 1013904223) & 0xffffffff
            real = ((state >> 8) / 8388608.0) - 1.0
            state = (1664525 * state + 1013904223) & 0xffffffff
            imag = ((state >> 8) / 8388608.0) - 1.0
            pattern.extend(struct.pack("<ff", real, imag))
        patterns.append(pattern)
    with path.open("wb") as stream:
        stream.write(header)
        # A file may span many CPIs, but each BLAH2IQ block is capped at
        # 262,144 samples/channel by RecordingReader/RecordingWriter.
        for position in range(0, samples, 262144):
            count = min(262144, samples - position)
            stream.write(struct.pack("<IIQ", count, 0, position))
            for pattern in patterns:
                whole, tail = divmod(count, 1024)
                stream.write(pattern * whole)
                stream.write(pattern[:tail * 8])


def write_target_blah2iq(path, sample_rate, frequency, frames=6,
                         frame_samples=None, delay_samples=8, doppler_hz=100,
                         stationary_clutter=0.0):
    """Deterministic reference, optional direct clutter, moving echo, absent tail."""
    frame_samples = int(sample_rate * .02) if frame_samples is None else frame_samples
    samples = frames * frame_samples
    header = bytearray(64)
    header[:8] = b"BLAH2IQ\0"
    struct.pack_into("<IIIIII", header, 8, 1, 64, 2, sample_rate, frequency, 1)
    struct.pack_into("<Q", header, 40, samples)
    struct.pack_into("<Q", header, 48, 1)
    state = 0x6D2B79F5
    reference = []
    for _ in range(samples):
        state = (1664525 * state + 1013904223) & 0xffffffff
        real = ((state >> 8) / 8388608.0) - 1.0
        state = (1664525 * state + 1013904223) & 0xffffffff
        imag = ((state >> 8) / 8388608.0) - 1.0
        reference.append(complex(real, imag))
    surveillance = []
    target_samples = 4 * frame_samples
    angular_step = 2 * math.pi * doppler_hz / sample_rate
    for index in range(samples):
        state = (1664525 * state + 1013904223) & 0xffffffff
        noise_real = .01 * (((state >> 8) / 8388608.0) - 1.0)
        state = (1664525 * state + 1013904223) & 0xffffffff
        noise_imag = .01 * (((state >> 8) / 8388608.0) - 1.0)
        value = complex(noise_real, noise_imag) + stationary_clutter * reference[index]
        if delay_samples <= index < target_samples:
            phase = angular_step * index
            value += .8 * reference[index - delay_samples] * complex(
                math.cos(phase), math.sin(phase))
        surveillance.append(value)
    with path.open("wb") as stream:
        stream.write(header)
        for position in range(0, samples, 262144):
            count = min(262144, samples - position)
            stream.write(struct.pack("<IIQ", count, 0, position))
            for channel in (reference, surveillance):
                block = bytearray()
                for value in channel[position:position + count]:
                    block.extend(struct.pack("<ff", value.real, value.imag))
                stream.write(block)


def write_portable_lineage(path, channels, sample_rate, frequency, samples,
                           epoch=7, first_sample=1000):
    data = path.read_bytes()
    metadata = {
        "schema": "vectorwarp-portable-iq-clip-v1", "format": "blah2iq",
        "encoding": "float32-complex-channel-major-blocks", "channels": channels,
        "sampleRate": sample_rate, "frequency": frequency,
        "samplesPerChannel": samples, "epoch": epoch,
        "firstEpochSample": first_sample,
        "endEpochSample": first_sample + samples, "dataBytes": len(data),
        "dataSha256": hashlib.sha256(data).hexdigest(),
    }
    Path(str(path) + ".json").write_text(json.dumps(metadata, separators=(",", ":")))


def config(receiver, channels, recording, api_port, sink_ports, loop=False, mismatch=False,
           temporal_lane=False):
    sample_rate = 2400000 if receiver == "Kraken" else 2000000
    frequency = 204640000
    file_channels = channels - 1 if mismatch else channels
    if mismatch:
        # The fixture writer uses this count; the processor configuration remains correct.
        write_blah2iq(recording, file_channels, sample_rate, frequency)
    device = {"type": receiver}
    if receiver == "Kraken":
        device.update(channel_count=channels, reference_channel=0,
                      surveillance_channels=[1])
    elif receiver == "RspDuo":
        device.update(agcSetPoint=-20, bandwidthNumber=5, gainReduction=[50, 45],
                      lnaState=1, dabNotch=False, rfNotch=False)
    elif receiver == "Usrp":
        device.update(address="type=b200", subdev="A:A A:B", antenna=["RX2", "RX2"], gain=[20.0, 20.0])
    elif receiver == "HackRF":
        device.update(serial=["offline-reference", "offline-surveillance"], gain_lna=[0, 0],
                      gain_vga=[0, 0], amp_enable=[False, False])
    process = {
        "performance": {"surveillance_workers": 1, "fft_threads": 1, "acceleration": "cpu"},
        "data": {"cpi": .02, "buffer": 2},
        # ±1000 Hz produces a legal FFT geometry for the legacy CPU path.
        "ambiguity": {"delayMin": -4, "delayMax": 59, "dopplerMin": -1000, "dopplerMax": 1000},
        # Isolate replay ingress from clutter rank behaviour in this harness.
        "clutter": {"enable": False, "delayMin": -4, "delayMax": 59},
        "detection": {"enable": True, "pfa": .01, "nGuard": 2, "nTrain": 6,
                      "minDelay": 1, "minDoppler": 1, "nCentroid": 1,
                      "integration_cpis": 1, "integration_motion_bins": 0},
        "tracker": {"enable": True, "initiate": {"M": 1, "N": 1, "maxAcc": 1}, "delete": 1},
    }
    if receiver == "Kraken":
        process["reference_synthesis"] = {"mode": "dedicated", "channels": [0]}
    if temporal_lane:
        # The target is at lane delay 8 (80 capture samples / decimation 10).
        # Cancel only the stationary direct-path neighborhood, leaving the
        # injected target outside the explicit half-open cancellation span.
        process["clutter"].update(enable=True, delayMin=-1, delayMax=3)
        process["multi_illuminator"] = {
            "enabled": True,
            "illuminators": [{"enabled": True, "id": "parity-lane",
                "standard": "CUSTOM", "frequency_hz": frequency,
                "occupied_bandwidth_hz": 100000,
                "output_sample_rate_hz": 200000, "filter_taps_per_phase": 16,
                "input_role_mode": "manual", "reference_channel": 0,
                "surveillance_channels": [1]}],
        }
        process["detection"]["integration_cpis"] = 2
        process["detection"]["integration_motion_bins"] = 1
    return {
        "capture": {"fs": sample_rate, "fc": frequency, "device": device,
                    "replay": {"state": True, "loop": loop, "file": str(recording),
                               "format": "auto", "provenance": "legacy-unqualified"}},
        "process": process,
        "network": {"ip": HOST, "ports": dict(api=api_port, **sink_ports)},
        "save": {"map": False, "detection": False, "path": str(recording.parent)},
    }


def stop_process(process, timeout=4):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout)
            return False
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout)
            return True
    return False


def wait_for(predicate, deadline):
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(.05)
    return False


def loop_progress_observed(statuses):
    """Periodic snapshots can miss playing; completed loop count is durable."""
    return any(isinstance(item, dict)
               and item.get("input") == "replay"
               and item.get("state") in ("playing", "draining")
               and item.get("error") == ""
               and type(item.get("loops")) is int
               and item["loops"] >= 2
               for item in statuses)


def first_json(payload, name):
    try:
        value, _ = json.JSONDecoder().raw_decode(bytes(payload).decode().lstrip())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AssertionError(f"{name} output is not JSON: {error}") from error
    return value


def assert_finite_numbers(value, name="map.data"):
    if isinstance(value, list):
        for index, item in enumerate(value):
            assert_finite_numbers(item, f"{name}[{index}]")
    elif not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
        raise AssertionError(f"{name} is not a finite numeric map value")


def canonical_product(value, *, drop_track_ids=False):
    """Remove provenance/runtime timing; retain every radar math/lineage value."""
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key == "processing_ms":
                if not isinstance(item, (int, float)) or isinstance(item, bool) or \
                        not math.isfinite(item) or item < 0:
                    raise AssertionError("invalid temporal processing timing")
                continue
            if key in ("timestamp", "eventTime") or (drop_track_ids and key == "id"):
                continue
            result[key] = canonical_product(item, drop_track_ids=drop_track_ids)
        return result
    if isinstance(value, list):
        return [canonical_product(item, drop_track_ids=drop_track_ids) for item in value]
    return value


def product_fingerprint(payload, name, *, drop_track_ids=False):
    values = json_values(payload, name)
    if not values:
        raise AssertionError(f"processor did not produce {name} output")
    if name == "detection" and not any(
            isinstance(value, dict) and value.get("delay") for value in values):
        raise AssertionError("receiver parity fixture produced no detections")
    if name == "track" and not any(
            isinstance(value, dict) and value.get("n", 0) > 0 for value in values):
        raise AssertionError("receiver parity fixture exercised no tracker state")
    canonical = [canonical_product(value, drop_track_ids=drop_track_ids)
                 for value in values]
    encoded = json.dumps(canonical, sort_keys=True, separators=(",", ":"),
                         allow_nan=False).encode()
    return hashlib.sha256(encoded).hexdigest(), len(values)


def assert_target_trace(sinks):
    detections = json_values(sinks.payloads["detection"], "detection")
    expected = False
    for frame in detections[:4]:
        if not isinstance(frame, dict):
            continue
        for delay, doppler in zip(frame.get("delay", []), frame.get("doppler", [])):
            if 1.0 <= delay <= 1.5 and 50 <= abs(doppler) <= 150:
                expected = True
                break
    if not expected:
        raise AssertionError("known delayed/Doppler echo was not detected")
    tracks = json_values(sinks.payloads["track"], "track")
    if not any(isinstance(track, dict) and track.get("n", 0) >= 2
               for frame in tracks if isinstance(frame, dict)
               for track in frame.get("data", [])):
        raise AssertionError("known echo did not produce a multi-update track")


def canonical_temporal_trace(payload):
    values = json_values(payload, "illuminator")
    if not values:
        raise AssertionError("temporal parity fixture produced no lane frames")
    result = []
    for frame in values:
        integration = frame.get("detector_integration") if isinstance(frame, dict) else None
        if not isinstance(integration, dict):
            raise AssertionError("lane frame lacks detector integration lineage")
        elapsed = integration.get("processing_ms")
        if not isinstance(elapsed, (int, float)) or isinstance(elapsed, bool) or \
                not math.isfinite(elapsed) or elapsed < 0:
            raise AssertionError("lane frame has invalid temporal processing timing")
        # Runtime diagnostics vary with scheduling; every numerical product,
        # configured policy, readiness flag and source lineage remains exact.
        integration = {key: value for key, value in integration.items()
                       if key != "processing_ms"}
        result.append({
            "first_input_sample": frame.get("first_input_sample"),
            "end_input_sample": frame.get("end_input_sample"),
            "capture_sample_position_available":
                frame.get("capture_sample_position_available"),
            "capture_epoch": frame.get("capture_epoch"),
            "capture_first_input_sample": frame.get("capture_first_input_sample"),
            "capture_end_input_sample": frame.get("capture_end_input_sample"),
            "integration": integration,
            "map": canonical_product(frame.get("map")),
            "detections": canonical_product(frame.get("detections")),
            "tracks": canonical_product(frame.get("tracks")),
        })
    if result[0]["capture_sample_position_available"] is not True:
        raise AssertionError("portable lineage was not admitted")
    if result[0]["integration"].get("available_cpis") != 1 or \
            result[0]["integration"].get("ready") is not False:
        raise AssertionError("two-CPI temporal lane skipped warmup")
    ready = [frame for frame in result if frame["integration"].get("ready") is True]
    if not ready or ready[0]["integration"].get("support_sequences") != [2, 1]:
        raise AssertionError("two-CPI temporal lane lost exact support lineage")
    if not any(frame["detections"].get("delay") for frame in ready):
        raise AssertionError("temporal lane produced no integrated target detection")
    if not any(track.get("n", 0) >= 2 for frame in ready
               for track in frame["tracks"].get("data", [])):
        raise AssertionError("temporal lane produced no multi-update track")
    return result


def assert_temporal_loop_reset(trace):
    initial_epoch = trace[0]["integration"].get("history_epoch")
    reset_index = next((index for index, frame in enumerate(trace[1:], 1)
                        if frame["integration"].get("sequence") == 1 and
                        frame["integration"].get("history_epoch") != initial_epoch), None)
    if reset_index is None:
        raise AssertionError("replay loop did not reset temporal history epoch/sequence")
    reset = trace[reset_index]
    if reset["integration"].get("available_cpis") != 1 or \
            reset["integration"].get("ready") is not False:
        raise AssertionError("replay loop admitted stale temporal support during warmup")
    tracks = reset["tracks"] if isinstance(reset["tracks"], dict) else {}
    if tracks.get("data") or tracks.get("n", 0) or tracks.get("nActive", 0):
        raise AssertionError("replay loop retained a track from the prior history epoch")
    fresh = next((frame for frame in trace[reset_index + 1:]
                  if frame["integration"].get("history_epoch") ==
                  reset["integration"].get("history_epoch") and
                  frame["integration"].get("ready") is True), None)
    if fresh is None or fresh["integration"].get("support_sequences") != [2, 1]:
        raise AssertionError("replay loop did not rebuild exact fresh temporal support")


def temporal_loop_prefix(trace, frames_per_loop):
    """Compare two complete recorded epochs, regardless of shutdown scheduling."""
    if len(trace) < 2 * frames_per_loop:
        raise AssertionError("replay did not publish two complete temporal loops")
    prefix = trace[:2 * frames_per_loop]
    assert_temporal_loop_reset(prefix)
    epochs = [prefix[index]["integration"].get("history_epoch")
              for index in (0, frames_per_loop)]
    for index, frame in enumerate(prefix):
        integration = frame["integration"]
        sequence = index % frames_per_loop + 1
        if type(integration.get("sequence")) is not int or \
                integration["sequence"] != sequence or \
                integration.get("history_epoch") != epochs[index // frames_per_loop]:
            raise AssertionError("replay loop dropped or duplicated a temporal CPI")
        # This fixture uses two-CPI integration. Check every window within its
        # own loop; matching receiver hashes cannot expose shared stale support.
        available = min(sequence, 2)
        support = integration.get("support_sequences")
        if type(integration.get("available_cpis")) is not int or \
                integration["available_cpis"] != available or \
                integration.get("ready") is not (sequence > 1) or \
                not isinstance(support, list) or \
                any(type(value) is not int for value in support) or \
                support != list(range(sequence, sequence - available, -1)):
            raise AssertionError("replay loop retained stale or invalid temporal support")
    return prefix


def assert_temporal_processor_outputs(sinks, minimum_cpis):
    if not all(sinks.bytes[name] for name in ("illuminator", "timestamp", "timing")):
        raise AssertionError("temporal processor did not produce lane, timestamp and timing output")
    timing_frames = json_values(sinks.payloads["timing"], "timing")
    completed_cpis = max((item.get("nCpi", 0) for item in timing_frames
                          if isinstance(item, dict)), default=0)
    if completed_cpis < minimum_cpis:
        raise AssertionError(
            f"temporal replay completed after {completed_cpis} CPIs; expected {minimum_cpis}")


def assert_processor_outputs(sinks, receiver, minimum_cpis=1):
    if not all(sinks.bytes[name] for name in ("map", "timestamp", "timing")):
        raise AssertionError("processor did not produce map, timestamp and timing output")
    map_json = first_json(sinks.payloads["map"], "map")
    if not isinstance(map_json, dict) or not isinstance(map_json.get("data"), list):
        raise AssertionError("map output has no data array")
    if map_json.get("presentation_only") or map_json.get("data_omitted"):
        raise AssertionError("offline replay must publish complete canonical maps")
    assert_finite(map_json)
    assert_finite_numbers(map_json["data"])
    timing_json = first_json(sinks.payloads["timing"], "timing")
    if not isinstance(timing_json, dict) or timing_json.get("nCpi", 0) < 1:
        raise AssertionError("timing output has no completed CPI")
    timing_frames = json_values(sinks.payloads["timing"], "timing")
    completed_cpis = max((item.get("nCpi", 0) for item in timing_frames
                          if isinstance(item, dict)), default=0)
    if completed_cpis < minimum_cpis:
        raise AssertionError(
            f"replay completed after {completed_cpis} processed CPIs; expected at least {minimum_cpis}")
    for key in ("acceleration", "clutterAcceleration"):
        backend = timing_json.get(key)
        if not isinstance(backend, dict) or backend.get("active") not in ("cpu", "vulkan"):
            raise AssertionError(f"timing output has no valid {key} backend")
        if not all(isinstance(backend.get(field), str) for field in ("requested", "state", "reason", "device")):
            raise AssertionError(f"timing output has incomplete {key} state")
    if not all(isinstance(timing_json["clutterAcceleration"].get(field), bool)
               for field in ("gpuExecuted", "cpuExecuted")):
        raise AssertionError("timing output does not distinguish clutter GPU work from CPU validation")
    sources = {
        "Kraken": "kraken_heimdall_host_arrival_estimate",
        "RspDuo": "sdrplay_callback_sample_counter",
        "Usrp": "usrp_uhd_device_time",
        "HackRF": "hackrf_shared_trigger_relative_sample_epoch",
    }
    expected = {
        "schema": "vectorwarp-event-time-v1", "source": sources[receiver],
        "exact": False, "uncertaintyBounded": False, "uncertaintyNs": None,
        "estimateAvailable": False, "estimateNs": None, "epochEstimateNs": None,
        "epoch": "0", "firstSample": "0", "endSample": "0", "sampleRateHz": 0,
    }
    for output_name, payload in (("map", map_json), ("timing", timing_json)):
        provenance = payload.get("eventTime")
        if not isinstance(provenance, dict):
            raise AssertionError(f"{receiver} legacy replay {output_name} has no provisional event-time provenance")
        for key, value in expected.items():
            if provenance.get(key) != value:
                raise AssertionError(
                    f"{receiver} legacy replay {output_name} eventTime.{key} was "
                    f"{provenance.get(key)!r}, expected {value!r}")
        if not isinstance(provenance.get("reason"), str) or not provenance["reason"]:
            raise AssertionError(f"{receiver} legacy replay {output_name} lacks an uncertainty reason")
        if receiver == "Usrp" and provenance.get("deviceTimeAvailable") is not False:
            raise AssertionError("Legacy USRP replay must not invent UHD device time")
    timestamp_frame = first_json(sinks.payloads["timestamp"], "timestamp")
    timestamp = timestamp_frame.get("timestamp") if isinstance(timestamp_frame, dict) else None
    if not isinstance(timestamp, int) or isinstance(timestamp, bool) or timestamp < 0:
        raise AssertionError("timestamp output has no non-negative integer timestamp")


def run_case(binary, receiver, channels, kind="complete", *, sample_rate=None, cpi=.02,
             clutter=False, recording_frames=3, recording_frame_samples=None,
             minimum_cpis=1, target_fixture=False, temporal_lane=False):
    loop = kind == "loop"
    with tempfile.TemporaryDirectory(prefix="vectorwarp-replay-") as temporary:
        root = Path(temporary)
        recording = root / "input.blah2iq"
        sample_rate = sample_rate or (2400000 if receiver == "Kraken" else 2000000)
        if kind != "missing":
            if target_fixture:
                if channels != 2:
                    raise AssertionError("target replay fixture requires exactly two channels")
                write_target_blah2iq(recording, sample_rate, 204640000,
                                     frames=recording_frames,
                                     frame_samples=(recording_frame_samples or
                                                    int(sample_rate * cpi)),
                                     delay_samples=80 if temporal_lane else 8,
                                     stationary_clutter=.25 if clutter else 0.0)
            else:
                write_blah2iq(recording, channels, sample_rate, 204640000,
                              frames=recording_frames,
                              frame_samples=(recording_frame_samples or
                                             int(sample_rate * cpi)))
            if temporal_lane:
                write_portable_lineage(recording, channels, sample_rate, 204640000,
                    recording_frames * (recording_frame_samples or int(sample_rate * cpi)))
        status = StatusServer()
        sinks = SinkGroup()
        process = None
        try:
            status.start()
            sinks.start()
            payload = config(receiver, channels, recording, status.port, sinks.ports(), loop,
                             mismatch=kind == "mismatch", temporal_lane=temporal_lane)
            payload["capture"]["fs"] = sample_rate
            payload["process"]["data"]["cpi"] = cpi
            payload["process"]["clutter"]["enable"] = clutter
            config_path = root / "config.json"
            config_path.write_text(json.dumps(payload))
            process = subprocess.Popen([str(binary), "-c", str(config_path)], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            if kind in ("missing", "mismatch"):
                # Recording identity is checked before HTTP status/networking.
                # A fail-fast rejection must not be mistaken for a hung replay.
                output, _ = process.communicate(timeout=14)
                expected = ("Cannot open replay file:" if kind == "missing" else
                            "Recording channel count differs from Settings")
                if process.returncode != 1 or expected not in output:
                    raise AssertionError(f"replay identity was not refused: {output[-4000:]}")
                if "Setting up device" in output or status.snapshot() or any(sinks.bytes.values()):
                    raise AssertionError("invalid replay admitted hardware or published products")
                return {"case": f"{receiver}-{channels}-{kind}", "ok": True,
                        "startup_rejection": expected, "output_bytes": 0}
            # Large-rate software fixtures get a bounded allowance on slow CI
            # hosts. This is a correctness test, never a real-time speed claim.
            deadline = time.monotonic() + (60 if sample_rate == 6000000 else 14)
            if kind == "complete":
                ok = wait_for(lambda: any(item.get("state") == "complete" for item in status.snapshot()), deadline)
                expectation = "complete"
            elif kind == "loop":
                ok = wait_for(lambda: loop_progress_observed(status.snapshot()), deadline)
                expectation = "loop >= 2"
            else:
                ok = wait_for(lambda: any(item.get("state") == "error" and item.get("error")
                                          for item in status.snapshot()), deadline)
                expectation = "error"
            output = ""
            if not ok:
                raise AssertionError(f"{receiver}/{channels} did not report {expectation}: {status.snapshot()[-3:]}")
            if process.poll() is not None:
                raise AssertionError(f"{receiver}/{channels} exited before controlled shutdown")
            forced = stop_process(process)
            output = process.stdout.read() if process.stdout else ""
            if forced or process.returncode != 0:
                raise AssertionError(f"{receiver}/{channels} did not stop cleanly (returncode={process.returncode}): {output[-16000:]}")
            states = status.snapshot()
            if sinks.truncated:
                raise AssertionError(
                    f"bounded output retention overflowed for {sorted(sinks.truncated)}")
            if not states or states[-1].get("captureStopped") is not True:
                raise AssertionError(f"{receiver}/{channels} did not publish final stopped-capture status")
            final = states[-1]
            for field in ("captureBacklogSamples", "captureDroppedSamples"):
                values = final.get(field)
                if (not isinstance(values, list) or len(values) != channels
                        or any(type(value) is not int or value < 0 for value in values)):
                    raise AssertionError(f"{receiver}/{channels} has invalid final {field}: {values}")
            if any(final["captureDroppedSamples"]):
                raise AssertionError(f"{receiver}/{channels} lost samples during lossless replay")
            expected_state = "complete" if kind == "complete" else "stopped" if loop else "error"
            state_evidence = next((item for item in reversed(states)
                                   if item.get("state") == expected_state), None)
            if state_evidence is None or (expected_state == "error" and
                                          not state_evidence.get("error")):
                raise AssertionError(f"{receiver}/{channels} terminal state/error was not preserved: {states[-3:]}")
            if "Setting up device" in output:
                raise AssertionError(f"{receiver}/{channels} attempted hardware setup: {output}")
            if kind in ("complete", "loop"):
                required_state = "loop >= 2" if kind == "loop" else "complete"
                progress_observed = (loop_progress_observed(states) if kind == "loop" else
                                     any(item.get("state") == "complete" for item in states))
                if not progress_observed:
                    raise AssertionError(f"{receiver}/{channels} never reported {required_state}")
                if temporal_lane:
                    assert_temporal_processor_outputs(sinks, minimum_cpis)
                else:
                    assert_processor_outputs(sinks, receiver, minimum_cpis)
            product_fingerprints = None
            product_frames = None
            if kind == "complete":
                if target_fixture and not temporal_lane:
                    assert_target_trace(sinks)
                if not temporal_lane:
                    products = {
                        "map": product_fingerprint(sinks.payloads["map"], "map"),
                        "detection": product_fingerprint(
                            sinks.payloads["detection"], "detection"),
                        "track": product_fingerprint(
                            sinks.payloads["track"], "track"),
                    }
                    product_fingerprints = {name: value[0] for name, value in products.items()}
                    product_frames = {name: value[1] for name, value in products.items()}
            return {"case": f"{receiver}-{channels}-{kind}", "ok": True,
                    "sample_rate": sample_rate, "cpi_seconds": cpi, "clutter": clutter,
                    "states": sorted({item.get("state") for item in states}),
                    "output_bytes": sum(sinks.bytes.values()),
                    "product_fingerprints": product_fingerprints,
                    "product_frames": product_frames,
                    "temporal_trace": canonical_temporal_trace(
                        sinks.payloads["illuminator"]) if temporal_lane else None}
        except BaseException as error:
            diagnostics = ""
            if process is not None:
                stop_process(process)
                if process.stdout:
                    diagnostics = process.stdout.read()
            raise AssertionError(f"{receiver}/{channels}/{kind}: {error}; processor output: {diagnostics[-16000:]}") from error
        finally:
            if process is not None:
                stop_process(process)
                if process.stdout:
                    process.stdout.close()
            sinks.close()
            status.close()


def invalid_startup_case(binary, field):
    """Reject invalid required numbers before networking or receiver setup."""
    with tempfile.TemporaryDirectory(prefix="vectorwarp-invalid-config-") as temporary:
        root = Path(temporary)
        payload = config("Kraken", 2, root / "unused.blah2iq", 3000,
                         {name: 3100 + index for index, name in enumerate(PORT_NAMES)})
        if field == "api-port":
            payload["network"]["ports"]["api"] = 0
        else:
            payload["capture"][field] = 0
        config_path = root / "config.json"
        config_path.write_text(json.dumps(payload))
        result = subprocess.run([str(binary), "-c", str(config_path)], cwd=root,
                                capture_output=True, text=True, timeout=5, check=False)
        output = result.stdout + result.stderr
        if result.returncode != 1 or "sample rate, centre frequency and API port must be positive" not in output:
            raise AssertionError(f"Invalid {field} must fail clearly before setup: {output[-4000:]}")
        if "Setting up device" in output or "Failed to initialize socket" in output:
            raise AssertionError(f"Invalid {field} attempted setup")
        return {"case": f"invalid-{field}", "ok": True, "output_bytes": 0}


def invalid_tracker_dependency_case(binary, receiver, channelized):
    """Native YAML rejects tracking without detection instead of rewriting it."""
    with tempfile.TemporaryDirectory(prefix="vectorwarp-invalid-multi-") as temporary:
        root = Path(temporary)
        recording = root / "target.blah2iq"
        write_target_blah2iq(recording, 2000000, 204640000, frames=1,
                            delay_samples=80, stationary_clutter=.25)
        status, sinks = StatusServer(), SinkGroup()
        try:
            status.start()
            sinks.start()
            payload = config(receiver, 2, recording, status.port, sinks.ports(), temporal_lane=channelized)
            payload["capture"]["fs"] = 2000000
            payload["process"]["detection"]["enable"] = False
            payload["process"]["tracker"]["enable"] = True
            config_path = root / "config.json"
            config_path.write_text(json.dumps(payload))
            result = subprocess.run([str(binary), "-c", str(config_path)], cwd=root,
                                    capture_output=True, text=True, timeout=15, check=False)
            output = result.stdout + result.stderr
            if result.returncode != 1 or "Tracking requires detection" not in output:
                raise AssertionError(f"{receiver}: tracking without detection was not refused: {output[-4000:]}")
            if "Setting up device" in output or any(sinks.bytes.values()):
                raise AssertionError(f"{receiver}: invalid tracker dependency admitted IQ/products")
            return {"case": "invalid-tracker-dependency", "receiver": receiver,
                    "channelized": channelized, "ok": True}
        finally:
            sinks.close()
            status.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("--binary must be an executable processor path")
    results = [invalid_startup_case(binary, field) for field in ("fs", "fc", "api-port")]
    results.extend(invalid_tracker_dependency_case(binary, receiver, channelized)
                   for receiver in (*RSP_TYPES, "Kraken") for channelized in (False, True))
    parity = [run_case(binary, receiver, 2, sample_rate=2000000,
                       recording_frames=6, minimum_cpis=6, target_fixture=True)
              for receiver in (*RSP_TYPES, "Kraken")]
    for product in ("map", "detection", "track"):
        fingerprints = {item["product_fingerprints"][product] for item in parity}
        frame_counts = {item["product_frames"][product] for item in parity}
        if len(fingerprints) != 1 or len(frame_counts) != 1:
            raise AssertionError(
                f"receiver identities changed common {product} evolution: {parity}")
        if frame_counts != {6}:
            raise AssertionError(
                f"receiver parity emitted {product} frames {frame_counts}; expected exactly six")
    results.extend(parity)
    temporal = [run_case(binary, receiver, 2, sample_rate=2000000,
                         recording_frames=6, minimum_cpis=6,
                         target_fixture=True, temporal_lane=True, clutter=True)
                for receiver in (*RSP_TYPES, "Kraken")]
    temporal_fingerprints = {hashlib.sha256(json.dumps(
        item["temporal_trace"], sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode()).hexdigest() for item in temporal}
    if len(temporal_fingerprints) != 1:
        raise AssertionError(f"receiver identities changed temporal lineage/math: {temporal}")
    results.extend(temporal)
    temporal_loops = [run_case(binary, receiver, 2, "loop", sample_rate=2000000,
                               recording_frames=6, minimum_cpis=6,
                               target_fixture=True, temporal_lane=True, clutter=True)
                      for receiver in (*RSP_TYPES, "Kraken")]
    for item in temporal_loops:
        item["temporal_trace"] = temporal_loop_prefix(item["temporal_trace"], 6)
    loop_fingerprints = {hashlib.sha256(json.dumps(
        item["temporal_trace"], sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode()).hexdigest() for item in temporal_loops}
    if len(loop_fingerprints) != 1:
        raise AssertionError(
            f"receiver identities changed temporal reset/track identity: {temporal_loops}")
    results.extend(temporal_loops)
    for channels in range(3, 9):
        results.append(run_case(binary, "Kraken", channels))
    results.append(run_case(binary, "RspDuo", 2, "loop"))
    results.append(run_case(binary, "RspDuo", 2, "missing"))
    results.append(run_case(binary, "Kraken", 3, "mismatch"))
    # Exercise the real processor (including clutter and map generation) with
    # valid 6-MS/s headers and matching samples, without opening any SDR.
    results.append(run_case(binary, "Usrp", 2, sample_rate=6000000, cpi=.2, clutter=True))
    results.append(run_case(binary, "HackRF", 2, sample_rate=6000000, clutter=True))
    # A finite replay ending halfway through its third CPI must process both
    # complete CPIs before ReplayPlayer reports completion and discards only
    # the final sub-CPI tail.
    results.append(run_case(binary, "Usrp", 2, recording_frames=5,
                            recording_frame_samples=20000, minimum_cpis=2))
    print(json.dumps({"acceptance": "offline processor replay", "cases": results,
                      "count": len(results)}, separators=(",", ":")))


if __name__ == "__main__":
    main()
