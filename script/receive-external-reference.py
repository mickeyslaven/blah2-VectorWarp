#!/usr/bin/env python3
"""Receive VWREF001 frames into bounded, continuity-audited IQ segments.

The sender's sample counter is authoritative only inside the exporting radio.
Host arrival time is recorded for operations, never presented as RF time or as
cross-radio synchronization.
"""

import argparse
import hashlib
import json
import pathlib
import socket
import struct
import time
import os
import queue
import signal
import threading

HEADER = struct.Struct("!8sIIQQQIIIIII")
ACK = struct.Struct("!8sQ")


def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        block = stream.recv(count - len(data))
        if not block:
            raise EOFError("external-reference sender disconnected")
        data.extend(block)
    return bytes(data)


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, sort_keys=True)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(path)


class SegmentWriter:
    def __init__(self, root, duration_s, max_bytes):
        self.root = root
        self.duration_s = duration_s
        self.max_bytes = max_bytes
        self.handle = None
        self.path = None
        self.first = None
        self.last = None
        self.bytes = 0
        self.frames = 0
        self.digest = None
        self.seals = queue.Queue(maxsize=16)
        self.seal_error = None
        self.closed = False
        self.sealer = threading.Thread(target=self._seal_segments,
                                       name="external-reference-sealer",
                                       daemon=True)
        self.sealer.start()

    def append(self, payload, receipt):
        self._check_sealer()
        if self.handle is None:
            self.open(receipt)
        limit = max(receipt["payload_bytes"],
                    int(receipt["sample_rate_hz"] * 4 * self.duration_s))
        if self.frames and self.bytes + len(payload) > limit:
            self._queue_current()
            self.open(receipt)
        offset = self.bytes
        self.handle.write(payload)
        self.digest.update(payload)
        self.bytes += len(payload)
        self.frames += 1
        self.last = receipt
        return self.path.with_suffix("").name, offset

    def open(self, receipt):
        stem = (f'{receipt["epoch"]:016x}-'
                f'{receipt["first_sample"]:020d}')
        self.path = self.root / f"{stem}.s16le.partial"
        self.handle = self.path.open("xb")
        self.first = receipt
        self.last = receipt
        self.bytes = 0
        self.frames = 0
        self.digest = hashlib.sha256()

    def _queue_current(self):
        if self.handle is None:
            return
        self.seals.put((self.handle, self.path, self.bytes, self.frames,
                        self.digest.hexdigest(), self.first, self.last))
        self.handle = None
        self.path = None
        self.digest = None

    def _seal_segments(self):
        while True:
            item = self.seals.get()
            try:
                if item is None:
                    return
                handle, partial, size, frames, digest, first, last = item
                handle.flush()
                os.fsync(handle.fileno())
                handle.close()
                complete = partial.with_suffix("")
                partial.replace(complete)
                atomic_json(complete.with_suffix(complete.suffix + ".json"), {
                    "schema": 1,
                    "file": complete.name,
                    "bytes": size,
                    "frames": frames,
                    "sha256": digest,
                    "first": first,
                    "last": last,
                })
                self.enforce_retention()
            except BaseException as error:
                self.seal_error = error
                try:
                    if item is not None:
                        item[0].close()
                except OSError:
                    pass
            finally:
                self.seals.task_done()

    def _check_sealer(self):
        if self.seal_error is not None:
            raise RuntimeError("external-reference segment seal failed") from self.seal_error

    def close(self):
        if self.closed:
            return
        self.closed = True
        self._queue_current()
        self.seals.put(None)
        self.seals.join()
        self.sealer.join()
        self._check_sealer()

    def enforce_retention(self):
        if not self.max_bytes:
            return
        segments = sorted(self.root.glob("*.s16le"),
                          key=lambda path: path.stat().st_mtime_ns)
        total = sum(path.stat().st_size for path in segments)
        for path in segments:
            if total <= self.max_bytes:
                break
            size = path.stat().st_size
            path.unlink()
            path.with_suffix(path.suffix + ".json").unlink(missing_ok=True)
            total -= size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--expected-frequency", type=int)
    parser.add_argument("--expected-rate", type=int)
    parser.add_argument("--expected-peer")
    parser.add_argument("--expected-channel", type=int, default=0)
    parser.add_argument("--segment-seconds", type=float, default=60.0)
    parser.add_argument("--max-storage-gib", type=float, default=0.0)
    args = parser.parse_args()
    if args.segment_seconds <= 0 or args.max_storage_gib < 0:
        parser.error("segment and retention limits must be non-negative")
    args.output.mkdir(parents=True, exist_ok=True)
    ledger = args.output / "frames.jsonl"
    status_path = args.output / "status.json"
    writer = SegmentWriter(args.output, args.segment_seconds,
                           int(args.max_storage_gib * 1024 ** 3))
    counters = {"connections": 0, "frames": 0, "bytes": 0,
                "sequence_gaps": 0, "sample_gaps": 0,
                "invalid_frames": 0}
    previous = None
    if status_path.exists():
        try:
            previous = json.loads(status_path.read_text())["last_frame"]
        except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
            previous = None

    def stop(_signum, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    try:
        with ledger.open("a", encoding="utf-8", buffering=1) as journal, \
             socket.create_server((args.listen, args.port), reuse_port=False) as server:
            server.listen(1)
            while True:
                connection, peer = server.accept()
                if args.expected_peer and peer[0] != args.expected_peer:
                    connection.close()
                    continue
                counters["connections"] += 1
                with connection:
                    while True:
                        try:
                            raw = read_exact(connection, HEADER.size)
                        except EOFError:
                            break
                        try:
                            (magic, schema, header_bytes, sequence, epoch,
                             first_sample, rate, frequency, samples, channel,
                             payload_bytes, sample_format) = HEADER.unpack(raw)
                            if (magic != b"VWREF001" or schema not in (1, 2) or
                                    header_bytes != HEADER.size):
                                raise ValueError("invalid frame header")
                            if (not samples or payload_bytes != samples * 4 or
                                    sample_format != 1 or channel != args.expected_channel):
                                raise ValueError("invalid payload contract")
                            if args.expected_frequency and frequency != args.expected_frequency:
                                raise ValueError("external-reference frequency changed")
                            if args.expected_rate and rate != args.expected_rate:
                                raise ValueError("external-reference sample rate changed")
                        except ValueError:
                            counters["invalid_frames"] += 1
                            raise
                        payload = read_exact(connection, payload_bytes)
                        receipt = {
                            "schema": schema, "sequence": sequence, "epoch": epoch,
                            "first_sample": first_sample,
                            "end_sample": first_sample + samples,
                            "sample_rate_hz": rate, "frequency_hz": frequency,
                            "samples": samples, "payload_bytes": payload_bytes,
                            "channel": channel, "format": "complex-s16le",
                            "received_unix_ns": time.time_ns(), "peer": peer[0],
                            "payload_sha256": hashlib.sha256(payload).hexdigest(),
                        }
                        if schema == 2 and previous and sequence == previous["sequence"]:
                            duplicate_identity = (
                                epoch == previous["epoch"] and
                                first_sample == previous["first_sample"] and
                                receipt["payload_sha256"] == previous["payload_sha256"])
                            if not duplicate_identity:
                                counters["invalid_frames"] += 1
                                raise ValueError("duplicate sequence changed identity")
                            connection.sendall(ACK.pack(b"VWACK001", sequence))
                            continue
                        if previous and epoch == previous["epoch"]:
                            if sequence != previous["sequence"] + 1:
                                counters["sequence_gaps"] += 1
                            if first_sample != previous["end_sample"]:
                                counters["sample_gaps"] += 1
                        file_name, offset = writer.append(payload, receipt)
                        receipt.update({"segment": file_name,
                                        "segment_offset_bytes": offset})
                        journal.write(json.dumps(receipt, sort_keys=True) + "\n")
                        counters["frames"] += 1
                        counters["bytes"] += payload_bytes
                        previous = receipt
                        atomic_json(status_path, {
                            "schema": 1, "state": "receiving",
                            "updated_unix_ns": time.time_ns(),
                            "counters": counters, "last_frame": receipt,
                            "timing_contract": "source-sample-counter-only; host arrival is not RF time",
                        })
                        if schema == 2:
                            connection.sendall(ACK.pack(b"VWACK001", sequence))
    finally:
        writer.close()


if __name__ == "__main__":
    main()
