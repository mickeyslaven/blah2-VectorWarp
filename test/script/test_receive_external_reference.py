import hashlib
import importlib.util
import json
import socket
import subprocess
import sys
import time
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "script" / "receive-external-reference.py"
SPEC = importlib.util.spec_from_file_location("external_reference_receiver", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_segment_rotation_does_not_wait_for_fsync(tmp_path, monkeypatch):
    real_fsync = MODULE.os.fsync

    def slow_fsync(descriptor):
        time.sleep(.2)
        real_fsync(descriptor)

    monkeypatch.setattr(MODULE.os, "fsync", slow_fsync)
    writer = MODULE.SegmentWriter(tmp_path, .001, 0)
    receipt = {"epoch": 1, "first_sample": 0, "payload_bytes": 40,
               "sample_rate_hz": 10}
    writer.append(b"a" * 40, receipt)
    receipt = dict(receipt, first_sample=10)
    started = time.monotonic()
    writer.append(b"b" * 40, receipt)
    elapsed = time.monotonic() - started
    writer.close()

    assert elapsed < .1
    segments = sorted(tmp_path.glob("*.s16le"))
    assert len(segments) == 2
    for segment in segments:
        metadata = json.loads(
            segment.with_suffix(segment.suffix + ".json").read_text())
        assert metadata["sha256"] == hashlib.sha256(segment.read_bytes()).hexdigest()


def test_v2_acknowledges_and_deduplicates_retried_frame(tmp_path):
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    process = subprocess.Popen([
        sys.executable, str(SCRIPT), "--listen", "127.0.0.1",
        "--port", str(port), "--output", str(tmp_path),
        "--expected-frequency", "551000000", "--expected-rate", "10",
        "--segment-seconds", "1",
    ])

    def connect():
        for _ in range(50):
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=.2)
            except OSError:
                time.sleep(.02)
        raise AssertionError("receiver did not start")

    def send(stream, sequence, first_sample, byte):
        payload = bytes([byte]) * 40
        header = MODULE.HEADER.pack(
            b"VWREF001", 2, MODULE.HEADER.size, sequence, 7, first_sample,
            10, 551000000, 10, 0, len(payload), 1)
        stream.sendall(header + payload)
        assert MODULE.ACK.unpack(MODULE.read_exact(stream, MODULE.ACK.size)) == (
            b"VWACK001", sequence)

    try:
        with connect() as stream:
            send(stream, 0, 0, 1)
        with connect() as stream:
            send(stream, 0, 0, 1)
            send(stream, 1, 10, 2)
    finally:
        process.terminate()
        process.wait(timeout=5)

    rows = [json.loads(line) for line in (tmp_path / "frames.jsonl").read_text().splitlines()]
    assert [row["sequence"] for row in rows] == [0, 1]
