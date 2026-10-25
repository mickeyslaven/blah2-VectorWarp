#!/usr/bin/env python3
"""Convert a Kraken MCHQ clip to frame-aligned BLAH2IQ replay input.

Event clips can begin between live CPI boundaries.  Replaying them from sample
zero changes the coherent integration windows and can make a real live
detection disappear.  The recorder's firstEpochSample identifies the phase;
this converter drops only the prefix needed to reach the next CPI boundary.
"""

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np


MAGIC = b"BLAH2IQ\0"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--frame-samples", type=int,
                        help="Legacy alignment to an epoch-zero CPI grid")
    parser.add_argument("--admission", type=Path,
                        help="Admission sidecar containing exact live processor CPI ranges")
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text())
    channels = int(metadata["channels"])
    sample_rate = int(metadata["sampleRate"])
    frequency = int(metadata["frequency"])
    expected_gains = metadata.get("channelGains")
    if (not isinstance(expected_gains, list) or len(expected_gains) != channels or
            not all(isinstance(value, (int, float)) and math.isfinite(value)
                    for value in expected_gains)):
        raise ValueError("recording metadata has no valid per-channel gain vector")
    first_epoch_sample = int(metadata["firstEpochSample"])
    if args.admission is not None:
        admission = json.loads(args.admission.read_text())
        captures = [value for value in admission.get("captures", {}).values()
                    if value.get("valid") is True and value.get("processor_cpis")]
        if not captures:
            raise ValueError("admission has no valid exact processor CPI ranges")
        ranges = captures[0]["processor_cpis"]
        epochs = {int(value["epoch"]) for value in ranges}
        if epochs != {int(metadata["epoch"])}:
            raise ValueError("processor CPI and recording epochs do not match")
        target_first = int(ranges[0]["first_epoch_sample"])
        target_end = int(ranges[-1]["end_epoch_sample"])
        skip = target_first - first_epoch_sample
        sample_limit = target_end - target_first
        if skip < 0 or sample_limit <= 0:
            raise ValueError("processor CPI range is outside recording")
        if any(int(right["first_epoch_sample"]) != int(left["end_epoch_sample"])
               for left, right in zip(ranges, ranges[1:])):
            raise ValueError("processor CPI ranges are not contiguous")
    else:
        if args.frame_samples is None or args.frame_samples <= 0:
            parser.error("either --admission or a positive --frame-samples is required")
        target_first = first_epoch_sample + ((-first_epoch_sample) % args.frame_samples)
        sample_limit = None
        skip = target_first - first_epoch_sample
    written = 0

    with args.input.open("rb") as source, args.output.open("xb+") as destination:
        header = bytearray(64)
        header[:8] = MAGIC
        struct.pack_into("<IIIIII", header, 8, 1, 64, channels, sample_rate,
                         frequency, 1)
        destination.write(header)
        remaining_skip = skip
        while True:
            fixed = source.read(32)
            if not fixed:
                break
            if len(fixed) != 32:
                raise ValueError("truncated MCHQ header")
            magic, frame_channels, samples, *_ = struct.unpack(">8I", fixed)
            if magic not in (0x4D434851, 0x4D434854) or frame_channels != channels:
                raise ValueError("invalid or changing MCHQ stream")
            channel_metadata = source.read(channels * 8)
            if len(channel_metadata) != channels * 8:
                raise ValueError("truncated MCHQ metadata")
            if magic == 0x4D434854:
                timing = source.read(96)
                if len(timing) != 96 or struct.unpack('>II', timing[:8]) != (1, 96):
                    raise ValueError('Unsupported Heimdall timing extension')
            metadata_values = struct.unpack(f"<{channels * 2}f", channel_metadata)
            frequencies = np.asarray(metadata_values[0::2], dtype=np.float64)
            gains = np.asarray(metadata_values[1::2], dtype=np.float64)
            if (not np.all(np.isfinite(frequencies)) or not np.all(np.isfinite(gains)) or
                    not np.allclose(frequencies, frequency, rtol=0, atol=1) or
                    not np.allclose(gains, expected_gains, rtol=0, atol=1e-4)):
                raise ValueError("MCHQ packet RF metadata differs from recording sidecar")
            raw = source.read(samples * channels * 2)
            if len(raw) != samples * channels * 2:
                raise ValueError("truncated MCHQ payload")
            values = np.frombuffer(raw, dtype=np.uint8).reshape(channels, samples, 2)
            # Match HeimdallFrame::decode_payload exactly: remove each packet's
            # component mean, normalize unsigned 8-bit samples by 127.5, and
            # refer every channel to the packet's minimum tuner gain.  Offline
            # replay must not silently reintroduce the live gain-vector bias.
            decoded = ((values.astype(np.float32) - values.mean(axis=1, keepdims=True,
                                                                dtype=np.float64)) / 127.5)
            gain_scales = np.power(10.0, (np.min(gains) - gains) / 20.0)
            decoded *= gain_scales[:, None, None]
            if remaining_skip:
                drop = min(remaining_skip, samples)
                decoded = decoded[:, drop:, :]
                remaining_skip -= drop
            if sample_limit is not None:
                remaining = sample_limit - written
                if remaining <= 0:
                    break
                decoded = decoded[:, :remaining, :]
            count = decoded.shape[1]
            if not count:
                continue
            block_header = bytearray(16)
            struct.pack_into("<IIQ", block_header, 0, count, 0, written)
            destination.write(block_header)
            complex_values = np.empty((channels, count, 2), dtype="<f4")
            complex_values[:] = decoded
            destination.write(complex_values.tobytes(order="C"))
            written += count
        if remaining_skip:
            raise ValueError("clip is shorter than its alignment prefix")
        if sample_limit is not None and written != sample_limit:
            raise ValueError("clip does not contain the complete admitted CPI range")
        destination.seek(40)
        destination.write(struct.pack("<QQ", written, 1))
        destination.flush()
    print(json.dumps({"schema": "vectorwarp-aligned-replay-v1",
                      "input": str(args.input), "output": str(args.output),
                      "first_epoch_sample": first_epoch_sample,
                      "target_first_epoch_sample": target_first,
                      "frame_samples": args.frame_samples,
                      "skipped_samples": skip, "written_samples": written}))


if __name__ == "__main__":
    main()
