#!/usr/bin/env python3
"""Verify every raw packet and its digest; optionally finalize a recorded prefix."""
import argparse
import hashlib
import json
import math
from pathlib import Path
from record_iq import packet, finalize


class FileSocket:
    def __init__(self, stream, digest):
        self.stream, self.digest = stream, digest
    def recv_into(self, view):
        count = self.stream.readinto(view)
        self.digest.update(view[:count])
        return count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("recording", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--finalize-to", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    digest = hashlib.sha256()
    size = args.recording.stat().st_size
    count = samples = 0
    signature = None
    with args.recording.open("rb") as stream:
        socket = FileSocket(stream, digest)
        while stream.tell() < size:
            blocks, info = packet(socket)
            if not ((info["phase"] & 255) == 4 and info["noise"] == 0 and info["retuning"] == 0):
                raise RuntimeError("Recording contains uncalibrated samples")
            current = {k: v for k, v in info.items() if k != "samples"}
            if signature is not None and current != signature:
                raise RuntimeError("Receiver metadata changes within recording")
            signature = current
            samples += info["samples"]
            count += 1
    for actual, expected, label in ((size, manifest["bytes"], "bytes"),
            (samples, manifest["samples_per_channel"], "samples"),
            (count, manifest["packets"], "packets"),
            (digest.hexdigest(), manifest["sha256"], "SHA256")):
        if actual != expected:
            raise RuntimeError(f"Recording {label} does not match manifest")
    if samples < math.ceil(manifest["sample_rate"] * manifest["requested_seconds"]):
        raise RuntimeError("Recording did not reach requested duration")
    if args.finalize_to:
        finalize(args.recording, args.finalize_to)
    report = dict(complete=True, verification="all packet metadata, duration, byte count and SHA256",
        source_manifest=str(args.manifest), recording=str(args.finalize_to or args.recording),
        sha256=digest.hexdigest(), bytes=size, samples_per_channel=samples, packets=count)
    destination = Path(str(args.finalize_to or args.recording) + ".verified.json")
    with destination.open("x") as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps(report))


if __name__ == "__main__":
    main()
