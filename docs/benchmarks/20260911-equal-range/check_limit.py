#!/usr/bin/env python3
"""Read-only confirmation of the deliberately excluded full-window profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--bundle', type=Path, required=True)
parser.add_argument('--profile', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
config = json.loads(args.profile.read_text())
assert (config['delay_min'], config['delay_max'], config['sample_rate']) == (-10, 245, 2400000)
assert (config['doppler_min'], config['doppler_max'], config['cpi']) == (-40000, 40000, .2)
checks = []
for engine in ('upstream', 'fast'):
    binary = args.bundle / ('bin/bench-' + engine)
    command = [str(binary), '--inspect-geometry', str(args.profile), 'pair']
    run = subprocess.run(command, env=dict(os.environ, LD_LIBRARY_PATH=str(args.bundle / 'bin')),
                         capture_output=True, text=True, timeout=10)
    expected = 'BENCHMARK FAILED: Delay limits exceed the correlation block; reduce the delay range or narrow the Doppler span'
    assert run.returncode == 1 and run.stderr.strip() == expected and not run.stdout.strip()
    with binary.open('rb') as source:
        binary_sha = hashlib.file_digest(source, 'sha256').hexdigest()
    checks.append(dict(engine=engine, supported=False, rejection_verified=True,
                       command=command, binary_sha256=binary_sha,
                       returncode=run.returncode, stderr=run.stderr.strip()))
with args.output.open('x') as output:
    json.dump(dict(acceptance='expected full-range rejection; no timed processing attempted',
                   checks=checks), output, indent=2)
print('Both engines correctly reject ±40 kHz at the fixed 30.604-km window')
