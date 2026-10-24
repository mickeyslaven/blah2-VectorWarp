#!/usr/bin/env python3
"""Bounded, read-mostly Kraken control/MCHQ attestation for the f47 live gate."""

import argparse
import json
import math
import socket
import struct
import time


EXPECTED_FREQ = 527_000_000
EXPECTED_RATE = 2_400_000
EXPECTED_CHANNELS = 5
EXPECTED_GAINS = [2.7, 2.7, 1.4, 1.4, 3.7]


def exact(sock, count):
    blocks = bytearray()
    while len(blocks) < count:
        block = sock.recv(min(1 << 20, count - len(blocks)))
        if not block:
            raise RuntimeError("stream closed")
        blocks.extend(block)
    return bytes(blocks)


def control_status(apply_gains, timeout):
    deadline = time.monotonic() + timeout
    statuses, response = [], None
    sent = False
    with socket.create_connection(("127.0.0.1", 8092), timeout=5) as sock:
        sock.settimeout(1)
        pending = bytearray()
        while time.monotonic() < deadline:
            try:
                pending.extend(sock.recv(65536))
            except TimeoutError:
                continue
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                if not line:
                    continue
                message = json.loads(line)
                if "settings" not in message:
                    response = message
                    continue
                statuses.append(message)
                settings = message.get("settings", {})
                if settings.get("center_freq") != EXPECTED_FREQ:
                    raise RuntimeError(f"frequency drift before command: {settings}")
                if settings.get("sample_rate") != EXPECTED_RATE:
                    raise RuntimeError(f"sample-rate drift: {settings}")
                if message.get("num_channels") != EXPECTED_CHANNELS:
                    raise RuntimeError(f"channel-count drift: {message.get('num_channels')}")
                if message.get("operating_mode") != "coherent" or message.get("wideband_enabled") is True:
                    raise RuntimeError("receiver mode drift")
                if apply_gains and not sent:
                    command = {"command": "set_channel_gains", "gains": EXPECTED_GAINS}
                    sock.sendall((json.dumps(command, separators=(",", ":")) + "\n").encode())
                    sent = True
                channels = message.get("adc_headroom", {}).get("channels", [])
                settled = (message.get("calibration_state") == "CONVERGED" and
                           not message.get("recovering") and not message.get("reconfiguring") and
                           not message.get("cooldown_active") and not message.get("noise_source") and
                           message.get("channel_gains") == EXPECTED_GAINS and len(channels) == EXPECTED_CHANNELS)
                if settled and all(not c.get("unavailable") and c.get("components", 0) >= 65536 and
                                   c.get("sampled_components", 0) >= 1024 and
                                   0 <= c.get("rail_components", -1) <= c.get("near_rail_components", -1) <= c.get("components", -1) and
                                   len(c.get("histogram", [])) == 256 and sum(c.get("histogram", [])) == c.get("sampled_components") and
                                   math.isfinite(c.get("rms_fraction", math.nan)) and 0 <= c["rms_fraction"] <= 1
                                   for c in channels):
                    return message, response, len(statuses)
    raise RuntimeError(f"no settled exact control status; last={statuses[-1] if statuses else None}")


def stream_frames(seconds, require_timing):
    deadline = time.monotonic() + seconds
    frames = []
    prior = None
    with socket.create_connection(("127.0.0.1", 8091), timeout=5) as sock:
        sock.settimeout(5)
        while time.monotonic() < deadline or len(frames) < 3:
            fields = struct.unpack(">8I", exact(sock, 32))
            magic, channels, samples, phase, noise, counter, group, retuning = fields
            if magic not in (0x4D434851, 0x4D434854) or channels != EXPECTED_CHANNELS or not 0 < samples <= 262144:
                raise RuntimeError(f"bad MCHQ header {fields}")
            if require_timing and magic != 0x4D434854:
                raise RuntimeError("candidate did not emit required MCHT-v1 frames")
            if phase & ~0xFFF or (phase & 0xFF) != 4 or phase & 0xE00 or noise != 0 or retuning != 0:
                raise RuntimeError(f"invalid coherent frame state {fields}")
            metadata = struct.unpack("<" + "ff" * channels, exact(sock, channels * 8))
            frequencies, gains = list(metadata[0::2]), list(metadata[1::2])
            if frequencies != [float(EXPECTED_FREQ)] * channels:
                raise RuntimeError(f"wire frequency drift {frequencies}")
            if any(abs(a - b) > 1e-5 for a, b in zip(gains, EXPECTED_GAINS)):
                raise RuntimeError(f"wire gain drift {gains}")
            timing = None
            if magic == 0x4D434854:
                timing = struct.unpack(">4I10Q", exact(sock, 96))
                if timing[:2] != (1, 96) or timing[2] & ~7 or timing[3] >= channels:
                    raise RuntimeError(f"invalid MCHT extension {timing}")
                if timing[-1] < timing[5] + samples:
                    raise RuntimeError(f"invalid MCHT sample span {timing}")
                if prior is not None:
                    if timing[4] != prior[4] or timing[5] <= prior[5] or timing[6] <= prior[6] or timing[12] <= prior[12]:
                        raise RuntimeError(f"non-advancing timing prior={prior} current={timing}")
                prior = timing
            exact(sock, channels * samples * 2)
            frames.append({"magic": "MCHT" if magic == 0x4D434854 else "MCHQ",
                           "samples": samples, "phase": phase & 0xFF, "counter": counter,
                           "group": group, "frequencies": frequencies, "gains": gains,
                           "timing": list(timing) if timing else None})
    return {"count": len(frames), "first": frames[0], "last": frames[-1],
            "totalSamplesPerChannel": sum(frame["samples"] for frame in frames)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply-gains", action="store_true")
    parser.add_argument("--require-timing", action="store_true")
    parser.add_argument("--duration", type=float, default=5)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    before, response, observed = control_status(args.apply_gains, args.timeout)
    stream = stream_frames(args.duration, args.require_timing)
    after, _, observed_after = control_status(False, 20)
    if after.get("coherence_events") != before.get("coherence_events"):
        raise RuntimeError("coherence watchdog event during bounded capture")
    result = {"schema": 1, "passed": True, "appliedExactConfiguredGains": args.apply_gains,
              "requiredTiming": args.require_timing, "controlStatusesBeforeSettle": observed,
              "commandResponse": response, "settledStatus": before, "postCaptureStatus": after,
              "postCaptureStatusesBeforeSettle": observed_after, "stream": stream,
              "completedAtUnixNs": time.time_ns()}
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
