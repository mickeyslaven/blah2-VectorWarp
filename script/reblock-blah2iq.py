#!/usr/bin/env python3
"""Repair oversized BLAH2IQ blocks in place without duplicating IQ storage."""

import argparse
import os
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    parser.add_argument("--maximum", type=int, default=65536)
    args = parser.parse_args()
    if args.maximum <= 0:
        parser.error("maximum must be positive")
    descriptor = os.open(args.file, os.O_RDWR)
    try:
        header = os.pread(descriptor, 64, 0)
        if len(header) != 64 or header[:8] != b"BLAH2IQ\0":
            raise ValueError("invalid BLAH2IQ header")
        channels = struct.unpack_from("<I", header, 16)[0]
        size = os.fstat(descriptor).st_size
        blocks, offset = [], 64
        while offset < size:
            raw = os.pread(descriptor, 16, offset)
            if len(raw) != 16:
                raise ValueError("truncated block header")
            samples, flags, _ = struct.unpack("<IIQ", raw)
            payload_offset = offset + 16
            payload_bytes = channels * samples * 8
            if samples <= 0 or payload_offset + payload_bytes > size:
                raise ValueError("invalid block payload")
            blocks.append((payload_offset, samples, flags))
            offset = payload_offset + payload_bytes
        if offset != size:
            raise ValueError("trailing bytes")

        destinations, output_size, sequence = [], 64, 0
        for _, samples, _ in blocks:
            destinations.append(output_size)
            parts = (samples + args.maximum - 1) // args.maximum
            output_size += samples * channels * 8 + parts * 16
            sequence += samples
        os.ftruncate(descriptor, output_size)
        first_samples, total = [], 0
        for _, samples, _ in blocks:
            first_samples.append(total)
            total += samples
        for index in range(len(blocks) - 1, -1, -1):
            payload_offset, samples, flags = blocks[index]
            payload = os.pread(descriptor, samples * channels * 8,
                               payload_offset)
            if len(payload) != samples * channels * 8:
                raise ValueError("truncated source payload")
            cursor, sample = destinations[index], first_samples[index]
            for begin in range(0, samples, args.maximum):
                count = min(args.maximum, samples - begin)
                os.pwrite(descriptor, struct.pack("<IIQ", count, flags, sample),
                          cursor)
                cursor += 16
                part = bytearray(channels * count * 8)
                destination = 0
                for channel in range(channels):
                    source = (channel * samples + begin) * 8
                    span = count * 8
                    part[destination:destination + span] = payload[source:source + span]
                    destination += span
                os.pwrite(descriptor, part, cursor)
                cursor += len(part)
                sample += count
        os.pwrite(descriptor, struct.pack("<QQ", total, 1), 40)
        os.fsync(descriptor)
        print({"source_blocks": len(blocks), "samples": total,
               "bytes": output_size, "maximum_block_samples": args.maximum})
    finally:
        os.close(descriptor)


if __name__ == "__main__":
    main()
