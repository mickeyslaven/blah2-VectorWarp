#!/usr/bin/env python3
"""Resumable Mac-to-CASS serial runner for the current 563 MHz waveform cohort.

Processes positives before misses.  Each retained MCHQ is streamed once from the
read-only recorder to CASS, SHA-verified, scored by both raw waveform methods
at low priority, verified as JSON, then evicted.  State is written after every
successful clip so rerunning the command resumes safely.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

CASS = 'mickeyslaven@100.104.10.46'
RAW = 'root@100.126.186.65'
REMOTE_ROOT = '/home/mickeyslaven/wwat563-results'
REMOTE_CACHE = '/home/mickeyslaven/wwat563-cache'
REMOTE_SPATIAL_READY = '/home/mickeyslaven/wwat563-results/spatial-ready'
REMOTE_SPATIAL_ACK = '/home/mickeyslaven/wwat563-results/spatial-ack'
REMOTE_WAVEFORM_VECTORS = '/home/mickeyslaven/wwat563-results/waveform-vectors'
VENV = '/home/mickeyslaven/radar-training/venv/bin/python'


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def ssh(host, command, **kwargs):
    return run(['ssh', '-o', 'BatchMode=yes', host, command], **kwargs)


def quote(value):
    return shlex.quote(str(value))


def unique_rows(manifest):
    result = {}
    for row in manifest['rows']:
        path = row['capture']['file']
        if path not in result:
            result[path] = row
    return [result[path] for path in sorted(result)]


def clip_id(row):
    digest = hashlib.sha256(row['capture']['file'].encode()).hexdigest()[:12]
    return f"r{row['capture']['revision']}-{digest}"


def trajectory(row):
    return {'schema': 'vectorwarp-clip-truth-trajectory-v1',
            'clip_revision': row['capture']['revision'], 'clip': row['capture']['file'],
            'window_start_ms': row['capture']['window_start'],
            'window_end_ms': row['capture']['window_end'], 'cpi_s': .5,
            'event_id': row['event_id'], 'hex': row.get('hex'), 'flight': row.get('flight'),
            'outcome': row.get('outcome'), 'trajectory': row['replay_trajectory']}


def remote_json(path):
    completed = ssh(CASS, f'cat {quote(path)}', capture_output=True)
    return json.loads(completed.stdout)


def verify_result(path):
    value = remote_json(path)
    if value.get('frames', 0) < 1 or not value.get('scores'):
        raise RuntimeError(f'incomplete result {path}')
    return value


def publish_vector(key, raw, digest, loaded, spectral):
    def one(method_id, document):
        scores = document['scores']
        selected = max(scores, key=lambda parameter:
                       scores[parameter]['margin_over_parameter_trial_control_p99_db'])
        score = scores[selected]
        return {'method_id': method_id, 'selected_parameter': selected,
                'parameter_trial_count': len(scores),
                'matched_control_count': score['control_count'],
                # The stacker calibrates cross-family combinations from this
                # unthresholded matched-control population, rather than from a
                # single-method p99 decision.
                'evidence_log10': db(score['target']),
                'control_evidence_log10': score['control_log_ratio_db'],
                'per_frame_log_ratio_db': score['per_frame_ratio_db'],
                'trajectory_mean_log_ratio_db': sum(score['per_frame_ratio_db']) /
                                                len(score['per_frame_ratio_db']),
                'matched_control_p99': score['control_p99'],
                'parameter_trial_control_p99': document['controls']['parameter_trial_control_p99'],
                'single_method_margin_db': score['margin_over_parameter_trial_control_p99_db'],
                'single_method_accepted': score['margin_over_parameter_trial_control_p99_db'] >= 3.0}
    basename = Path(raw).name
    path = f'{REMOTE_WAVEFORM_VECTORS}/{basename}.json'
    document = {'schema': 'vectorwarp-wwat563-waveform-vector-v1', 'clip_id': key,
                'raw': raw, 'raw_sha256': digest,
                'methods': {'loaded_reciprocal': one('loaded_reciprocal', loaded),
                            'spectral_interference': one('spectral_interference', spectral)}}
    ssh(CASS, f'cat > {quote(path + ".part")} && mv {quote(path + ".part")} {quote(path)}',
        input=json.dumps(document) + '\n')
    return path


def publish_spatial_ready(key, raw, digest, state):
    """Make verified raw available before waveform work, so spatial can overlap it."""
    basename = Path(raw).name
    ready = f'{REMOTE_SPATIAL_READY}/{basename}.json'
    ready_document = {'schema': 'vectorwarp-wwat563-spatial-ready-v1', 'clip_id': key,
        'source': state['clips'][key]['recording'], 'raw': raw, 'sha256': digest,
        'bytes': int(ssh(CASS, f'stat -c %s {quote(raw)}', capture_output=True).stdout)}
    ssh(CASS, f'cat > {quote(ready + ".part")} && mv {quote(ready + ".part")} {quote(ready)}',
        input=json.dumps(ready_document) + '\n')
    return ready


def wait_for_cass_headroom(local_log):
    """Avoid starting another low-priority waveform replay on a saturated CASS."""
    while True:
        fields = ssh(CASS, "nproc; cut -d' ' -f1 /proc/loadavg", capture_output=True).stdout.split()
        cpus, load = int(fields[0]), float(fields[1])
        if load <= max(8.0, cpus * 0.9):
            return
        with local_log.open('a') as log:
            log.write(f'waiting for CASS headroom: load={load:.2f}, cpus={cpus}\n')
        time.sleep(30)


def wait_for_spatial_ack(raw, state, state_path, poll_seconds):
    """Retain raw until the already-published spatial job atomically acknowledges it."""
    basename = Path(raw).name
    ack = f'{REMOTE_SPATIAL_ACK}/{basename}.json'
    while True:
        probe = subprocess.run(['ssh', '-o', 'BatchMode=yes', CASS,
                                f'test -f {quote(ack)} && cat {quote(ack)}'],
                               text=True, capture_output=True)
        if probe.returncode == 0:
            value = json.loads(probe.stdout)
            if value.get('status') not in ('complete', 'not_in_frozen_cohort'):
                raise RuntimeError(f'invalid spatial acknowledgement {ack}')
            return ack, value
        time.sleep(poll_seconds)


def copy_and_verify(row, local_log):
    source = row['capture']['file']
    name = Path(source).name
    target = f'{REMOTE_CACHE}/{name}'
    source_hash = ssh(RAW, f'sha256sum {quote(source)}', capture_output=True).stdout.split()[0]
    pipeline = (f'ssh -o BatchMode=yes {quote(RAW)} "cat {quote(source)}" | '
                f'ssh -o BatchMode=yes {quote(CASS)} "cat > {quote(target)}"')
    with local_log.open('a') as log:
        run(['bash', '-lc', pipeline], stdout=log, stderr=subprocess.STDOUT)
    remote_hash = ssh(CASS, f'sha256sum {quote(target)}', capture_output=True).stdout.split()[0]
    if source_hash != remote_hash:
        raise RuntimeError(f'SHA mismatch for {name}')
    return target, source_hash


def process(row, state, state_path, local_log):
    key = clip_id(row)
    if state['clips'].get(key, {}).get('status') == 'complete':
        return
    state['clips'][key] = {'status': 'running', 'role': row['role'],
                           'recording': row['capture']['file'], 'event_id': row['event_id']}
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    raw, digest = copy_and_verify(row, local_log)
    ready_path = publish_spatial_ready(key, raw, digest, state)
    state['clips'][key] = {**state['clips'][key], 'status': 'waveform_scoring',
        'raw': raw, 'sha256': digest, 'spatial_ready': ready_path}
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    trajectory_path = f'{REMOTE_ROOT}/{key}-trajectory.json'
    ssh(CASS, f'cat > {quote(trajectory_path)}', input=json.dumps(trajectory(row)) + '\n')
    loaded_path = f'{REMOTE_ROOT}/{key}-loaded-reciprocal.json'
    spectral_path = f'{REMOTE_ROOT}/{key}-spectral-interference.json'
    wait_for_cass_headroom(local_log)
    commands = [
        f'nice -n 19 {quote(VENV)} {quote(REMOTE_ROOT + "/analyze-loaded-reciprocal-mchq.py")} '
        f'--recording {quote(raw)} --trajectory {quote(trajectory_path)} --output {quote(loaded_path)} '
        '--fft-size 1024 --max-doppler-hz 800 --max-delay-km 64',
        f'nice -n 19 {quote(VENV)} {quote(REMOTE_ROOT + "/analyze-spectral-interference-rejection.py")} '
        f'--recording {quote(raw)} --trajectory {quote(trajectory_path)} --output {quote(spectral_path)} '
        '--fft-size 1024 --max-doppler-hz 800 --max-delay-km 64',
    ]
    with local_log.open('a') as log:
        for command in commands:
            ssh(CASS, command, stdout=log, stderr=subprocess.STDOUT)
    loaded, spectral = verify_result(loaded_path), verify_result(spectral_path)
    vector_path = publish_vector(key, raw, digest, loaded, spectral)
    state['clips'][key] = {**state['clips'][key], 'status': 'spatial_wait',
        'loaded_result': loaded_path, 'spectral_result': spectral_path,
        'waveform_vector': vector_path}
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    ack_path, acknowledgement = wait_for_spatial_ack(raw, state, state_path, 30)
    ssh(CASS, f'rm -f {quote(raw)}')
    state['clips'][key] = {'status': 'complete', 'role': row['role'],
        'recording': row['capture']['file'], 'event_id': row['event_id'], 'sha256': digest,
        'loaded_result': loaded_path, 'spectral_result': spectral_path,
        'waveform_vector': vector_path,
        'spatial_ack': ack_path, 'spatial_ack_status': acknowledgement['status'],
        'loaded_best_margin_db': max(item['margin_over_parameter_trial_control_p99_db']
                                     for item in loaded['scores'].values()),
        'spectral_selected_margin_db': spectral['selected_margin_db'],
        'completed_at': int(time.time())}
    state_path.write_text(json.dumps(state, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--positive-manifest', required=True, type=Path)
    parser.add_argument('--miss-manifest', required=True, type=Path)
    parser.add_argument('--state', required=True, type=Path)
    parser.add_argument('--log', required=True, type=Path)
    args = parser.parse_args()
    positives = unique_rows(json.loads(args.positive_manifest.read_text()))
    misses = unique_rows(json.loads(args.miss_manifest.read_text()))
    state = (json.loads(args.state.read_text()) if args.state.exists() else
             {'schema': 'vectorwarp-wwat563-waveform-runner-v1', 'clips': {}})
    ssh(CASS, f'mkdir -p {quote(REMOTE_ROOT)} {quote(REMOTE_CACHE)} {quote(REMOTE_SPATIAL_READY)} {quote(REMOTE_SPATIAL_ACK)} {quote(REMOTE_WAVEFORM_VECTORS)}')
    for source in ('analyze-loaded-reciprocal-mchq.py', 'analyze-spectral-interference-rejection.py'):
        run(['scp', '-q', str(Path(__file__).with_name(source)), f'{CASS}:{REMOTE_ROOT}/{source}'])
    for row in positives + misses:
        process(row, state, args.state, args.log)


if __name__ == '__main__':
    main()
