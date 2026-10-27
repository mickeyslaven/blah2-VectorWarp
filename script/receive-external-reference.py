#!/usr/bin/env python3
"""Receive bounded VWREF001 frames without inventing cross-radio timing."""

import argparse
import hashlib
import json
import pathlib
import socket
import struct
import time

HEADER = struct.Struct("!8sIIQQQIIIIII")


def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        block = stream.recv(count - len(data))
        if not block:
            raise EOFError("external-reference sender disconnected")
        data.extend(block)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--expected-frequency", type=int)
    parser.add_argument("--expected-rate", type=int)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    ledger = args.output / "frames.jsonl"

    with socket.create_server((args.listen, args.port), reuse_port=False) as server:
        server.listen(1)
        while True:
            connection, peer = server.accept()
            with connection:
                while True:
                    try:
                        raw = read_exact(connection, HEADER.size)
                    except EOFError:
                        break
                    (magic, schema, header_bytes, sequence, epoch, first_sample,
                     rate, frequency, samples, channel, payload_bytes,
                     sample_format) = HEADER.unpack(raw)
                    if magic != b"VWREF001" or schema != 1 or header_bytes != HEADER.size:
                        raise RuntimeError("invalid external-reference frame header")
                    if payload_bytes != samples * 4 or sample_format != 1 or channel > 1:
                        raise RuntimeError("invalid external-reference payload contract")
                    if args.expected_frequency and frequency != args.expected_frequency:
                        raise RuntimeError("external-reference frequency changed")
                    if args.expected_rate and rate != args.expected_rate:
                        raise RuntimeError("external-reference sample rate changed")
                    payload = read_exact(connection, payload_bytes)
                    received_ns = time.time_ns()
                    name = f"{epoch:016x}-{first_sample:020d}-{sequence:020d}.s16le"
                    destination = args.output / name
                    destination.write_bytes(payload)
                    receipt = {
                        "schema": 1,
                        "sequence": sequence,
                        "epoch": epoch,
                        "first_sample": first_sample,
                        "end_sample": first_sample + samples,
                        "sample_rate_hz": rate,
                        "frequency_hz": frequency,
                        "samples": samples,
                        "channel": channel,
                        "format": "complex-s16le",
                        "received_unix_ns": received_ns,
                        "peer": peer[0],
                        "file": name,
                        "sha256": hashlib.sha256(payload).hexdigest(),
                    }
                    with ledger.open("a", encoding="utf-8") as output:
                        output.write(json.dumps(receipt, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
