#!/usr/bin/env python3
"""Record a finite, calibrated MCHQ stream without changing receiver settings."""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import time


def finalize(partial, output):
    # renameat2(RENAME_NOREPLACE) also works on external filesystems without
    # hard-link support. It never replaces an existing destination.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(partial), -100, os.fsencode(output), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(output))


def receive(sock, size):
    result = bytearray(size)
    view = memoryview(result)
    offset = 0
    while offset < size:
        count = sock.recv_into(view[offset:])
        if not count:
            raise RuntimeError("IQ connection ended before the recording completed")
        offset += count
    return result


def packet(sock):
    header = receive(sock, 32)
    magic, channels, samples, phase, noise, counter, group, retuning = struct.unpack(">8I", header)
    if magic != 0x4D434851 or not 2 <= channels <= 8 or not 1 <= samples <= 262144:
        raise RuntimeError("Invalid MCHQ header")
    metadata = receive(sock, channels * 8)
    values = struct.unpack("<" + "ff" * channels, metadata)
    payload = receive(sock, channels * samples * 2)
    info = dict(channels=channels, samples=samples, phase=phase, noise=noise,
                frequency_change_counter=counter, group=group, retuning=retuning,
                frequencies=list(values[::2]), gains=list(values[1::2]))
    if not all(math.isfinite(v) for v in values):
        raise RuntimeError("Non-finite receiver metadata")
    return (header, metadata, payload), info


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--sample-rate", type=int, default=2400000)
    parser.add_argument("--frequency", type=int, required=True)
    parser.add_argument("--channels", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.seconds <= 300 or not 2 <= args.channels <= 8 or args.sample_rate != 2400000:
        parser.error("Unsupported recording duration, channel count or Suite sample rate")
    target = math.ceil(args.seconds * args.sample_rate)
    budget = target * args.channels * 2 + (64 << 20)
    if shutil.disk_usage(args.output.parent).free < budget + (8 << 30):
        raise RuntimeError("Recording destination needs capture space plus 8 GiB free")
    partial = Path(str(args.output) + ".partial")
    manifest_path = Path(str(args.output) + ".json")
    if args.output.exists() or manifest_path.exists():
        raise RuntimeError("Recording destination already exists")
    manifest = dict(schema=1, complete=False, sample_rate=args.sample_rate,
                    requested_seconds=args.seconds, channels=args.channels,
                    format="MCHQ calibrated channel-major unsigned 8-bit I/Q",
                    acquisition_stage="Heimdall calibrated output before blah2 processing",
                    source_sequence_counter_available=False,
                    continuity="TCP order verified; no hardware sample counter in MCHQ headers",
                    packets=0, samples_per_channel=0, bytes=0)
    digest = hashlib.sha256()
    last_signature = None
    warm_deadline = time.monotonic() + 120
    try:
        with socket.create_connection((args.host, args.port), timeout=10) as sock, partial.open("xb") as stream:
            sock.settimeout(10)
            while manifest["samples_per_channel"] < target:
                blocks, info = packet(sock)
                valid = (info["phase"] & 255) == 4 and not info["noise"] and not info["retuning"]
                valid = valid and info["channels"] == args.channels
                valid = valid and all(abs(f - args.frequency) <= 256 for f in info["frequencies"])
                if not valid:
                    if manifest["packets"]:
                        raise RuntimeError("Calibration, tuning or channel count changed during capture")
                    if time.monotonic() > warm_deadline:
                        raise RuntimeError("Receiver did not provide calibrated data within 120 seconds")
                    continue
                signature = tuple(info["frequencies"] + info["gains"] + [info["frequency_change_counter"], info["group"]])
                if last_signature is not None and signature != last_signature:
                    raise RuntimeError("Receiver metadata changed during capture")
                if not manifest["packets"]:
                    manifest["start_utc_ns"] = time.time_ns()
                    manifest["first_header"] = info
                    manifest["start_monotonic"] = time.monotonic()
                last_signature = signature
                for block in blocks:
                    stream.write(block)
                    digest.update(block)
                    manifest["bytes"] += len(block)
                manifest["packets"] += 1
                manifest["samples_per_channel"] += info["samples"]
                if manifest["packets"] % 750 == 0:
                    print(json.dumps({"seconds": manifest["samples_per_channel"] / args.sample_rate,
                                      "bytes": manifest["bytes"]}), flush=True)
            stream.flush()
            os.fsync(stream.fileno())
        manifest["end_utc_ns"] = time.time_ns()
        manifest["wall_seconds"] = time.monotonic() - manifest.pop("start_monotonic")
        manifest["sample_seconds"] = manifest["samples_per_channel"] / args.sample_rate
        manifest["sha256"] = digest.hexdigest()
        finalize(partial, args.output)
        manifest["complete"] = True
    except Exception as error:
        manifest["complete"] = False
        manifest["error"] = str(error)
        manifest.pop("start_monotonic", None)
        manifest["sha256_prefix"] = digest.hexdigest()
        raise
    finally:
        with manifest_path.open("x") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
    print(json.dumps(manifest), flush=True)


if __name__ == "__main__":
    main()
