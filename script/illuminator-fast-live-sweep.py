#!/usr/bin/env python3
"""Bounded Kraken fast illuminator proxy sweep with exact receiver restoration."""

import argparse
import json
import math
import os
import socket
import statistics
import struct
import subprocess
import time
import urllib.request

import numpy as np

MAGIC = 0x4D434851
GAIN_MIN, GAIN_MAX = 1, 28


def api_json(url, method="GET", body=None):
    payload = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(url, data=payload, method=method,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.loads(response.read())


def messages(sock, deadline):
    pending = b""
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(1 << 20)
        except socket.timeout:
            continue
        if not chunk:
            raise RuntimeError("Suite control connection closed")
        pending += chunk
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            try:
                yield json.loads(line)
            except (ValueError, UnicodeDecodeError):
                pass


def settled(message, frequency, gain, channel_gains=None):
    settings = message.get("settings", {})
    return (settings.get("center_freq") == frequency and settings.get("gain") == gain and
            (channel_gains is None or message.get("channel_gains") == channel_gains) and
            message.get("calibration_state") == "CONVERGED" and
            not message.get("recovering") and not message.get("reconfiguring") and
            not message.get("cooldown_active"))


def transact(host, port, command, accept, timeout=120):
    sock = socket.create_connection((host, port), 5)
    sock.settimeout(1)
    sock.sendall((json.dumps(command, separators=(",", ":")) + "\n").encode())
    last = None
    try:
        for message in messages(sock, time.monotonic() + timeout):
            last = message
            if message.get("status") in ("error", "failed"):
                raise RuntimeError(f"Suite rejected {command}: {message}")
            if accept(message):
                return message
    finally:
        sock.close()
    raise RuntimeError(f"Suite transaction timed out: command={command} last={last}")


def set_gain(host, port, frequency, gain):
    return transact(host, port, {"command": "set_gain", "gain": gain},
                    lambda message: settled(message, frequency, gain))


def set_frequency(host, port, frequency, gain):
    return transact(host, port, {"command": "set_frequency", "frequency": frequency},
                    lambda message: settled(message, frequency, gain))


def set_channel_gains(host, port, frequency, gain, values):
    return transact(host, port, {"command": "set_channel_gains", "gains": values},
                    lambda message: settled(message, frequency, gain, values))


def read_status(host, port, frequency, gain, minimum=65536, timeout=20):
    sock = socket.create_connection((host, port), 5)
    sock.settimeout(1)
    try:
        for message in messages(sock, time.monotonic() + timeout):
            channels = message.get("adc_headroom", {}).get("channels", [])
            if (settled(message, frequency, gain) and channels and
                    min(channel.get("sampled_components", 0) for channel in channels) >= minimum):
                return message
    finally:
        sock.close()
    raise RuntimeError("Fresh ADC headroom status was not received")


def adc_metrics(status):
    result = []
    for channel in status["adc_headroom"]["channels"]:
        histogram = channel["histogram"]
        total = sum(histogram)
        values = sorted((abs(index - 127.5) / 127.5, count)
                        for index, count in enumerate(histogram))
        cumulative, q999 = 0, 0.0
        for fraction, count in values:
            cumulative += count
            q999 = fraction
            if cumulative >= total * .999:
                break
        result.append({"channel": channel["channel"],
                       "actual_gain_db": channel.get("actual_gain_db"),
                       "rms_fraction": channel["rms_fraction"],
                       "q999_fraction": q999,
                       "peak_fraction": max((abs(index - 127.5) / 127.5
                                             for index, count in enumerate(histogram) if count), default=0),
                       "rail_fraction": channel["rail_components"] / channel["components"],
                       "near_rail_fraction": channel["near_rail_components"] / channel["components"]})
    return result


def choose_safe_gain(host, port, frequency):
    status = read_status(host, port, frequency, GAIN_MIN)
    first = adc_metrics(status)
    if max(item["near_rail_fraction"] for item in first) > 1e-3:
        return GAIN_MIN, status, "unsafe-at-minimum-gain"
    q999 = max(item["q999_fraction"] for item in first)
    estimate = GAIN_MIN if not q999 else int(math.floor(GAIN_MIN + 20 * math.log10(.62 / q999)))
    gain = min(GAIN_MAX, max(GAIN_MIN, estimate))
    if gain != GAIN_MIN:
        status = set_gain(host, port, frequency, gain)
        status = read_status(host, port, frequency, gain)
    while gain > GAIN_MIN:
        measured = adc_metrics(status)
        if (max(item["q999_fraction"] for item in measured) <= .75 and
                max(item["near_rail_fraction"] for item in measured) <= 1e-3):
            break
        gain = max(GAIN_MIN, gain - 2)
        status = set_gain(host, port, frequency, gain)
        status = read_status(host, port, frequency, gain)
    return gain, status, "measured-highest-safe-common"


def receive_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise RuntimeError("IQ stream closed")
        result.extend(chunk)
    return bytes(result)


def capture_iq(host, port, frequency, channels, maximum_samples=262144):
    sock = socket.create_connection((host, port), 5)
    sock.settimeout(10)
    captured = []
    captured_samples = 0
    try:
        for _ in range(2048):
            header = receive_exact(sock, 32)
            magic, count, samples, phase, noise, _, _, retune = struct.unpack(">8I", header)
            if magic != MAGIC or count < 1 or count > 16 or samples < 1 or samples > 10000000:
                raise RuntimeError("Malformed MCHQ header")
            metadata = receive_exact(sock, count * 8)
            payload = receive_exact(sock, count * samples * 2)
            metadata_values = struct.unpack("<" + "f" * (count * 2), metadata)
            frequencies = metadata_values[::2]
            valid = (count == channels and (phase & 255) == 4 and not (phase & 0xE00) and
                     noise == 0 and retune == 0 and all(abs(value - frequency) <= max(1, frequency * 1e-6)
                                                      for value in frequencies))
            if not valid:
                continue
            used = min(samples, maximum_samples - captured_samples)
            raw = np.frombuffer(payload, dtype=np.uint8).reshape(count, samples, 2)[:, :used]
            values = ((raw[:, :, 0].astype(np.float64) - 127.5) +
                      1j * (raw[:, :, 1].astype(np.float64) - 127.5)) / 127.5
            captured.append(values)
            captured_samples += used
            if captured_samples >= maximum_samples:
                result = np.concatenate(captured, axis=1)[:, :maximum_samples]
                result -= np.mean(result, axis=1, keepdims=True)
                return result
    finally:
        sock.close()
    raise RuntimeError("No valid coherent IQ packet was captured")


def ambiguity_metrics(reference, sample_rate):
    count = min(reference.size, 131072)
    value = reference[:count]
    energy = np.vdot(value, value).real
    fft_size = 1 << (count * 2 - 1).bit_length()
    source = np.fft.fft(value, fft_size)
    cells = []
    for doppler in (-400, -200, -100, -50, 0, 50, 100, 200, 400):
        phase = np.exp(-2j * np.pi * doppler * np.arange(count) / sample_rate)
        correlation = np.fft.ifft(np.fft.fft(value * phase, fft_size) * np.conj(source))
        magnitudes = np.abs(correlation[:401]) / max(energy, 1e-30)
        for delay, magnitude in enumerate(magnitudes):
            if not (doppler == 0 and delay <= 2):
                cells.append(float(magnitude))
    peak = max(cells, default=1.0)
    pslr = 20 * math.log10(max(peak, 1e-12))
    islr = 10 * math.log10(max(statistics.mean(value * value for value in cells), 1e-24))
    return pslr, islr


def cancellation_residual(channels, reference):
    frame = 4096
    blocks = min(channels.shape[1], reference.size) // frame
    if blocks < 8:
        raise RuntimeError("Insufficient IQ for held-out cancellation")
    used = blocks * frame
    x = reference[:used].reshape(blocks, frame)
    y = channels[:, :used].reshape(channels.shape[0], blocks, frame)
    window = np.hanning(frame)
    xf = np.fft.fft(x * window, axis=1)
    yf = np.fft.fft(y * window, axis=2)
    train = blocks // 2
    denominator = np.sum(np.abs(xf[:train]) ** 2, axis=0) + 1e-9
    transfer = np.sum(yf[:, :train] * np.conj(xf[:train])[None, :, :], axis=1) / denominator
    held_x = xf[train:]
    held_y = yf[:, train:]
    residual_f = held_y - transfer[:, None, :] * held_x[None, :, :]
    residual = np.fft.ifft(residual_f, axis=2).reshape(channels.shape[0], -1)
    input_power = np.mean(np.abs(np.fft.ifft(held_y, axis=2)) ** 2, axis=(1, 2))
    residual_power = np.mean(np.abs(residual) ** 2, axis=1)
    cancellation = float(np.median(10 * np.log10(np.maximum(input_power, 1e-30) /
                                                   np.maximum(residual_power, 1e-30))))
    return residual, np.fft.ifft(held_x, axis=1).reshape(-1), cancellation


def synthetic_trials(residual, reference, sample_rate, frequency):
    fused = np.mean(residual, axis=0)
    count = min(fused.size, reference.size, 65536)
    fused, reference = fused[:count], reference[:count]
    grid = [(delay, doppler) for delay in (20, 80, 200) for doppler in (-200, -50, 50, 200)]
    ratios = (-110, -105, -100, -95, -90, -85, -80, -75, -70, -65, -60)
    controls = []
    for delay, doppler in grid:
        for offset_delay, offset_doppler in ((-7, -23), (7, 23), (-13, 31), (13, -31)):
            controls.append((delay + offset_delay, doppler + offset_doppler))

    def score(signal, delay, doppler):
        if delay < 0 or delay >= count - 2:
            return 0.0
        indices = np.arange(delay, count)
        template = reference[:count - delay] * np.exp(2j * np.pi * doppler * indices / sample_rate)
        return float(abs(np.vdot(template, signal[delay:])) ** 2 /
                     max(np.vdot(template, template).real, 1e-30))

    baseline = [score(fused, delay, doppler) for delay, doppler in controls]
    floor = max(statistics.median(baseline), 1e-30)
    trials = []
    for ratio in ratios:
        amplitude = 10 ** (ratio / 20)
        for delay, doppler in grid:
            indices = np.arange(delay, count)
            echo = np.zeros(count, dtype=np.complex128)
            echo[delay:] = amplitude * reference[:count - delay] * np.exp(
                2j * np.pi * doppler * indices / sample_rate)
            target = score(fused + echo, delay, doppler)
            local = [score(fused + echo, delay + delta_delay, doppler + delta_doppler)
                     for delta_delay, delta_doppler in ((-7, -23), (7, 23), (-13, 31), (13, -31))]
            detected = target > max(10 * floor, 10 * statistics.median(local))
            trials.append({"trial_id": f"{frequency}-{ratio}-{delay}-{doppler}",
                           "echo_ratio_db": ratio, "delay_bin": delay,
                           "doppler_bin": doppler, "detected": bool(detected)})
    off_target = sum(value > 10 * floor for value in baseline) / len(baseline)
    return trials, off_target, 10 * math.log10(max(np.quantile(baseline, .9) / floor, 1))


def analyse(raw, sample_rate, frequency):
    covariance = raw @ np.conj(raw.T) / raw.shape[1]
    eigenvalues, eigenvectors = np.linalg.eigh(covariance)
    principal = eigenvectors[:, -1]
    reference = np.conj(principal) @ raw
    cnr = 10 * math.log10(max(float(eigenvalues[-1] /
                                   max(np.mean(eigenvalues[:-1]), 1e-30)), 1e-30))
    spectrum = np.abs(np.fft.fft(reference[:min(reference.size, 131072)] *
                                np.hanning(min(reference.size, 131072)))) ** 2
    floor = np.quantile(spectrum, .1)
    occupied = float(np.mean(spectrum > floor * 3))
    pslr, islr = ambiguity_metrics(reference, sample_rate)
    residual, held_reference, cancellation = cancellation_residual(raw, reference)
    trials, off_target, residual_excess = synthetic_trials(residual, held_reference,
                                                            sample_rate, frequency)
    powers = np.mean(np.abs(raw) ** 2, axis=1)
    span = 10 * math.log10(max(powers) / max(min(powers), 1e-30))
    return {"cnr_db": cnr, "occupied_bandwidth_fraction": occupied,
            "ambiguity_pslr_db": pslr, "ambiguity_islr_db": islr,
            "cancellation_db": cancellation, "clutter_cancellation_db": cancellation,
            "channel_power_span_db": span, "residual_floor_excess_db": residual_excess,
            "synthetic_echo_trials_data": trials, "off_target_detection_density": off_target}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", default="http://100.126.186.65:3000")
    parser.add_argument("--output", required=True)
    parser.add_argument("--candidate", action="append", required=True,
                        help="frequency-hz:name")
    args = parser.parse_args()
    candidates = [(int(item.split(":", 1)[0]), item.split(":", 1)[1])
                  for item in args.candidate]
    config = api_json(args.api + "/api/config")
    device = config["capture"]["device"]
    heimdall = device["heimdall"]
    host, data_port, control_port = heimdall["host"], heimdall["port"], heimdall["control_port"]
    original_frequency = config["capture"]["fc"]
    original_gain = heimdall["gain"]
    original_channel_gains = heimdall.get("channel_gains")
    processor_active = subprocess.run(["systemctl", "is-active", "--quiet",
                                       "vectorwarp-processor.service"]).returncode == 0
    current_frequency, current_gain = original_frequency, original_gain
    rows = []
    os.makedirs(args.output, mode=0o750, exist_ok=False)
    with open(os.path.join(args.output, "original-config.json"), "x", encoding="utf-8") as stream:
        json.dump(config, stream, indent=2); stream.write("\n")
    try:
        if processor_active:
            subprocess.run(["systemctl", "stop", "vectorwarp-processor.service"], check=True)
        for frequency, name in candidates:
            print(f"SCREEN {name} {frequency / 1e6:.0f} MHz", flush=True)
            if current_gain != GAIN_MIN:
                set_gain(host, control_port, current_frequency, GAIN_MIN)
                current_gain = GAIN_MIN
            if current_frequency != frequency:
                set_frequency(host, control_port, frequency, current_gain)
                current_frequency = frequency
            gain, status, strategy = choose_safe_gain(host, control_port, frequency)
            current_gain = gain
            adc = adc_metrics(status)
            row = {"frequency_hz": frequency, "name": name, "gain": gain,
                   "gain_strategy": strategy, "adc": adc}
            if strategy == "measured-highest-safe-common":
                row["proxy"] = analyse(capture_iq(host, data_port, frequency,
                                                   device["channel_count"]),
                                       config["capture"]["fs"], frequency)
            rows.append(row)
            with open(os.path.join(args.output, "progress.json"), "w", encoding="utf-8") as stream:
                json.dump(rows, stream, indent=2); stream.write("\n")
            print(json.dumps({key: value for key, value in row.items() if key != "proxy"},
                             separators=(",", ":")), flush=True)
    finally:
        print("RESTORING exact Suite tuple", flush=True)
        if current_frequency != original_frequency:
            set_frequency(host, control_port, original_frequency, current_gain)
            current_frequency = original_frequency
        if current_gain != original_gain:
            set_gain(host, control_port, original_frequency, original_gain)
            current_gain = original_gain
        if original_channel_gains:
            set_channel_gains(host, control_port, original_frequency, original_gain,
                              original_channel_gains)
        restored = read_status(host, control_port, original_frequency, original_gain)
        if original_channel_gains and restored.get("channel_gains") != original_channel_gains:
            raise RuntimeError("Exact channel-gain restoration failed")
        if processor_active:
            subprocess.run(["systemctl", "start", "vectorwarp-processor.service"], check=True)
    report = {"schema": "vectorwarp-fast-live-proxy-capture-v1",
              "completed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "results": rows}
    with open(os.path.join(args.output, "report.json"), "x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2); stream.write("\n")
    print(os.path.join(args.output, "report.json"), flush=True)


if __name__ == "__main__":
    main()
