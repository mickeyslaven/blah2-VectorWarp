#!/usr/bin/env python3
"""Finite, same-Pi CPU comparison; never changes receiver configuration."""
import argparse
import csv
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import statistics
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
args = parser.parse_args()
root = args.root
out = root / 'cpu-threeway-results'
out.mkdir()
recording = Path('/var/tmp/vectorwarp-pi-20260910/input/recording.mchq')
expected = '4188cd03768cf356b31e26759c1a75ec83ffc3c83ecfdc16bbb3e9bfc242d744'
with recording.open('rb') as source:
    assert hashlib.file_digest(source, 'sha256').hexdigest() == expected
base = json.loads((root / 'source/bench/profile-example.json').read_text())
base.update(sample_rate=2400000, frequency=527000000, channels=5,
            reference_channel=0, surveillance_channel=1, delay_min=-10,
            delay_max=245, cpi=.2, benchmark_workers=1, benchmark_fft_threads=4)
contract = dict(source_commit='8ba6e1330fad9fcdbe8a5a54134a712d0959775e',
    upstream_commit='c821bee3f0d27cf20c8447f3d908ef722905a4de',
    offworld_commit='1d37e29c9f788bed2bc95b1b320ccf6478430111',
    offworld_repository='https://github.com/offworldlabs/blah2-arm',
    archive_sha256='ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6',
    input_sha256=expected, fs=2400000, rf_hz=527000000,
    delay_min=-10, delay_max=245, delay_bins=256,
    max_excess_path_km=245*299792458/2400000/1000, cpi_ms=200,
    repeats=2, frames_per_repeat=20, excluded_startup_frames=8,
    cpu_affinity='0-3', cpu_quota_percent=400, fft_threads=4,
    worker_count=1, blas_threads=1, omp_threads=1,
    scope='Same-Pi sample-clock-paced real-IQ CPU processing; not live acquisition',
    tuning='Control clutter -10..200; alternate -10..189 uses 199 taps and a 480200-point convolution FFT (2^3 * 5^2 * 7^4). Both engines receive exactly the same profile. This is a configuration tradeoff, not a new algorithm or shortened output delay range.')
contract['host'] = dict(platform=platform.platform(),
    packages=subprocess.check_output(['rpm','-q','gcc-c++','fftw-libs-double','armadillo','mesa-vulkan-drivers'],text=True).strip().splitlines(),
    upstream_head=subprocess.check_output(['git','-C','/var/tmp/vectorwarp-pi-20260910/upstream','rev-parse','HEAD'],text=True).strip(),
    upstream_dirty=subprocess.check_output(['git','-C','/var/tmp/vectorwarp-pi-20260910/upstream','status','--porcelain','--untracked-files=no'],text=True).strip())
assert contract['host']['upstream_head']==contract['upstream_commit']
assert not contract['host']['upstream_dirty'], 'Baseline tracked source changed'
contract['host']['fftw_neon_symbols'] = [line for line in subprocess.check_output(
    ['nm','-D','/usr/lib64/libfftw3.so.3'],text=True).splitlines() if '_neon' in line][:3]
assert contract['host']['fftw_neon_symbols'], 'Do not omit the OffWorld NEON library optimization'
(out/'offworld-build-note.txt').write_text(
    'Unmodified offworldlabs/blah2-arm DSP in the shared recorded-IQ adapter; '
    'explicit -O3 -march=armv8-a+simd, matching its build target. All three link '
    'the same installed FFTW 3.3.10 NEON library. No Docker, SDK, acquisition or '
    'network timing. The adapter engine field is upstream for both external '
    'source builds; filenames and the variant field identify OffWorld explicitly.\n')
(out/'contract.json').write_text(json.dumps(contract, indent=2))
env = dict(os.environ, OPENBLAS_NUM_THREADS='1', OMP_NUM_THREADS='1',
           BLAH2_BENCH_PACE='1')
cases = [('standard-800hz', 800, 200), ('tuned-100hz', 100, 189),
         ('tuned-400hz', 400, 189), ('tuned-800hz', 800, 189)]
runs = []
def executable(engine):
    if engine=='offworld':
        return root/'build-offworld/bin/bench-upstream'
    return root/'build/bin'/('bench-'+engine)
for name, span, clutter_max in cases:
    config = dict(base, doppler_min=-span, doppler_max=span,
                  clutter_min=-10, clutter_max=clutter_max)
    profile = out/(name+'.profile.json')
    profile.write_text(json.dumps(config, indent=2))
    golden = out/(name+'.maps')
    for engine in ('upstream','offworld','fast'):
        binary = executable(engine)
        inspected = subprocess.run([str(binary),'--inspect-geometry',str(profile),'pair'],
                                   check=True, capture_output=True, text=True, env=env, timeout=10)
        geometry = json.loads(inspected.stdout)
        assert geometry['supported'] and geometry['delayBins']==256
        (out/f'{name}-{engine}.geometry.json').write_text(json.dumps(geometry, indent=2))
    for repeat in (1,2):
        for engine in (('upstream','offworld','fast') if repeat==1 else ('fast','offworld','upstream')):
            label = f'{name}-{engine}-r{repeat}'
            binary = executable(engine)
            with binary.open('rb') as source:
                binary_hash = hashlib.file_digest(source,'sha256').hexdigest()
            command = [str(binary), str(recording), str(profile), str(out/label),
                       'pair','cpu','20','compare' if golden.exists() else 'write',str(golden)]
            (out/(label+'.command.json')).write_text(json.dumps(dict(argv=command,
                binary_sha256=binary_hash, input_sha256=expected,
                environment={k:env[k] for k in ('OPENBLAS_NUM_THREADS','OMP_NUM_THREADS','BLAH2_BENCH_PACE')}),indent=2))
            with (out/(label+'.log')).open('x') as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                           env=env, timeout=90)
            if completed.returncode:
                raise RuntimeError(f'{label} returned {completed.returncode}; preserve failed evidence')
            summary = json.loads((out/(label+'.summary.json')).read_text())
            assert summary['frames']==20 and summary['steady_frames']==12
            with (out/(label+'.frames.csv')).open() as source:
                frames = list(csv.DictReader(source))
            assert len(frames)==20
            for frame in frames:
                for key in ('map_rms_relative','map_peak_relative','fusion_rms_relative','fusion_peak_relative'):
                    assert math.isfinite(float(frame[key])) and float(frame[key])<=1e-4
                assert frame['backend']=='cpu'
            record = {**summary, 'case':name, 'variant':engine, 'repeat':repeat, 'label':label}
            runs.append(record)
            (out/'summary.json').write_text(json.dumps(runs,indent=2))
            print(json.dumps({key:record[key] for key in ('label','steady_dsp_mean_ms','steady_dsp_deadline_misses')}),flush=True)
print('PASS all 24 CPU runs / 480 CPIs',flush=True)
