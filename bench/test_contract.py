#!/usr/bin/env python3
"""Tiny geometry/guard checks; no IQ, GPU, or benchmark workload is run."""

import json
import csv
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import os
import time


def run(*command):
    return subprocess.run(command, text=True, capture_output=True, timeout=10,
                          check=False)


def write_recording(path, packets=150, samples=4096):
    wire = bytearray()
    for packet in range(packets):
        wire.extend(struct.pack(">8I", 0x4D434851, 2, samples, 4, 0, 1, 0, 0))
        wire.extend(struct.pack("<4f", 527000000.0, 49.6, 527000000.0, 49.6))
        for channel in range(2):
            for sample in range(samples):
                wire.extend(((96 + sample + packet * 3 + channel * 7) % 256,
                             (160 - sample + packet * 5 + channel * 11) % 256))
    path.write_bytes(wire)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_contract.py UPSTREAM FAST PROFILE")
    upstream, fast, profile_path = sys.argv[1:]
    profile = json.loads(Path(profile_path).read_text())
    safe = run(fast, "--inspect-geometry", profile_path, "pair")
    assert safe.returncode == 0, safe.stderr
    inspected = json.loads(safe.stdout)
    assert inspected["supported"] and inspected["upstreamSafe"]
    assert inspected["workers"] == profile["benchmark_workers"]
    assert inspected["fftThreads"] == profile["benchmark_fft_threads"]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        full = dict(profile, sample_rate=100, cpi=.08, doppler_min=-1,
                    doppler_max=1, delay_min=-7, delay_max=7, round_hamming=False)
        full_path = root/'full-delay.json'
        full_path.write_text(json.dumps(full))
        for engine, supported in [(fast, True), (upstream, False)]:
            checked = run(engine, '--inspect-geometry', str(full_path), 'pair')
            assert checked.returncode == 0, checked.stderr
            assert json.loads(checked.stdout)['supported'] == supported
        for delay_min, delay_max, accepted in [(-198, 198, True), (-199, 198, False),
                                               (-198, 199, False), (-10, 245, False)]:
            geometry = dict(profile, sample_rate=2400000, cpi=.2,
                            doppler_min=-6000, doppler_max=6000,
                            delay_min=delay_min, delay_max=delay_max)
            boundary_path = root / "boundary.json"
            boundary_path.write_text(json.dumps(geometry))
            checked = run(fast, "--inspect-geometry", str(boundary_path), "pair")
            assert (checked.returncode == 0) == accepted, checked.stderr
            if not accepted:
                assert "Delay limits exceed the correlation block" in checked.stderr
        unsafe_profile = root / "unsafe.json"
        unsafe = dict(profile, cpi=0.5, doppler_min=-1600, doppler_max=1600)
        unsafe_profile.write_text(json.dumps(unsafe))
        inspected_run = run(upstream, "--inspect-geometry", str(unsafe_profile), "pair")
        assert inspected_run.returncode == 0, inspected_run.stderr
        rejected = json.loads(inspected_run.stdout)
        assert not rejected["supported"] and not rejected["upstreamSafe"]
        output = root / "must-not-exist"
        execution = run(upstream, str(root / "absent.mchq"), str(unsafe_profile),
                        str(output), "pair", "cpu", "1", "none",
                        str(root / "absent.maps"))
        assert execution.returncode != 0
        assert "UNSAFE_UPSTREAM_GEOMETRY" in execution.stderr
        assert not Path(str(output) + ".frames.csv").exists()
        missing = dict(profile)
        missing.pop("benchmark_fft_threads")
        missing_path = root / "missing.json"
        missing_path.write_text(json.dumps(missing))
        invalid = run(fast, "--inspect-geometry", str(missing_path), "pair")
        assert invalid.returncode != 0
        assert "benchmark_fft_threads" in invalid.stderr
        smoke_profile = root / "smoke.json"
        smoke = dict(profile, sample_rate=2400000, frequency=527000000,
                     channels=2, reference_channel=0, surveillance_channel=1,
                     cpi=0.05, delay_min=-10, delay_max=245,
                     doppler_min=-400, doppler_max=400,
                     clutter_min=-10, clutter_max=200,
                     benchmark_workers=1, benchmark_fft_threads=2)
        smoke_profile.write_text(json.dumps(smoke))
        # Explicit mixed replay is diagnostic-only and must reject unsupported
        # geometry instead of silently measuring CPU fallback.
        rejected_mixed = run(fast, str(root / "absent.mchq"), str(smoke_profile),
                             str(root / "mixed-rejected"), "pair", "mixed", "1",
                             "none", str(root / "absent.maps"))
        assert rejected_mixed.returncode != 0
        assert "Mixed benchmark requires" in rejected_mixed.stderr
        assert not (root / "mixed-rejected.frames.csv").exists()
        recording = root / "smoke.mchq"
        write_recording(recording)
        golden = root / "smoke.maps"
        upstream_prefix = root / "upstream"
        fork_prefix = root / "fast"
        baseline = run(upstream, str(recording), str(smoke_profile),
                       str(upstream_prefix), "pair", "cpu", "5", "write", str(golden))
        assert baseline.returncode == 0, (baseline.returncode, baseline.stdout, baseline.stderr)
        comparison = run(fast, str(recording), str(smoke_profile),
                         str(fork_prefix), "pair", "cpu", "5", "compare", str(golden))
        assert comparison.returncode == 0, (comparison.returncode, comparison.stdout, comparison.stderr)
        summary = json.loads(Path(str(fork_prefix) + ".summary.json").read_text())
        assert summary["frames"] == 5 and summary["cpu_frames"] == 5
        assert summary["gpu_frames"] == 0 and summary["forced_gpu_verified"] is None
        assert summary["map_rms_relative"] == 0 and summary["fusion_rms_relative"] == 0
        assert summary["workers"] == 1 and summary["fft_threads"] == 2
        with Path(str(fork_prefix) + ".frames.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == 5 and all(None not in row for row in rows)
        assert all(row["pipeline_ms"] == row["dsp_ms"] for row in rows)
        assert all(float(row["fusion_ms"]) >= 0 for row in rows)
        paced_prefix = root / "paced"
        started = time.monotonic()
        paced = subprocess.run([fast, str(recording), str(smoke_profile), str(paced_prefix),
                                "pair", "cpu", "5", "none", str(golden)],
                               env=dict(os.environ, BLAH2_BENCH_PACE="1"),
                               text=True, capture_output=True, timeout=10)
        assert paced.returncode == 0, paced.stderr
        assert time.monotonic() - started >= .25, 'Paced input ran faster than its sample clock'
        receipt = json.loads(Path(str(paced_prefix)+'.summary.json').read_text())
        assert receipt['sample_clock_paced'] and receipt['final_schedule_lag_ms'] >= 0
    print("benchmark geometry contract passed")


if __name__ == "__main__":
    main()
