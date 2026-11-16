#!/usr/bin/env python3
"""Collect bounded, evaluation-only pairs from the live temporal observer.

ADS-B is recorded only as held-out truth beside already-produced radar output.
It is never sent to the processor or used to form a detection or track.
"""

import argparse
import hashlib
import json
import math
import os
import time
import urllib.parse
import urllib.request


LIGHT_SPEED_M_S = 299792458.0


def get_json(base, route, query=None, timeout=2.0):
    suffix = '' if not query else '?' + urllib.parse.urlencode(query)
    request = urllib.request.Request(base.rstrip('/') + route + suffix,
                                     headers={'Accept': 'application/json'})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def exact_radar(base, route, illuminator, timestamp):
    try:
        return get_json(base, route, {
            'illuminator': illuminator, 'frame_timestamp': timestamp})
    except Exception as error:
        return {'available': False, 'error': type(error).__name__}


def file_identity(path):
    if not path:
        return None
    stat = os.stat(path)
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            digest.update(block)
    return {'path': os.path.realpath(path), 'sha256': digest.hexdigest(),
            'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns}


def search_extent(config, lane_rate):
    process = config.get('process') or {}
    reference = process.get('delay_reference_sample_rate_hz') or lane_rate
    ambiguity = process.get('ambiguity') or {}
    detection = process.get('detection') or {}
    minimum = detection.get('minDelay')
    maximum = ambiguity.get('delayMax')
    if not all(isinstance(value, (int, float)) and value > 0
               for value in (reference, lane_rate, minimum, maximum)):
        raise ValueError('runtime delay reference/search extent is incomplete')
    minimum_bins = math.ceil(minimum * lane_rate / reference)
    maximum_bins = math.ceil(maximum * lane_rate / reference)
    if maximum_bins <= minimum_bins:
        raise ValueError('runtime searchable delay extent is empty')
    bin_km = LIGHT_SPEED_M_S / (lane_rate * 1000.0)
    return {'delay_min_bins': minimum_bins, 'delay_max_bins': maximum_bins,
            'delay_min_km': minimum_bins * bin_km,
            'delay_max_km': maximum_bins * bin_km,
            'reference_sample_rate_hz': reference,
            'delay_bin_sample_rate_hz': lane_rate}


def last_record_keys(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return set()
    with open(path, 'rb') as source:
        size = source.seek(0, os.SEEK_END)
        source.seek(max(0, size - (1 << 20)))
        lines = source.read().splitlines()
    if not lines:
        return set()
    try:
        record = json.loads(lines[-1])
    except (UnicodeDecodeError, json.JSONDecodeError):
        return set()
    generation = str(record.get('generation', ''))
    reset_serial = int(record.get('reset_serial', 0))
    return {(generation, reset_serial, int(window.get('depth', 0)),
             str(window.get('latest_evaluated_sequence', '0')))
            for window in record.get('windows', [])}


def collect(args):
    if not os.path.isabs(args.output):
        raise ValueError('output must be an absolute path')
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    # The processor is intentionally restartable during a live patch. Preserve
    # the bounded ledger across a dependency restart instead of failing merely
    # because its output already exists.
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    seen = last_record_keys(args.output)
    started = time.monotonic()
    bytes_written = os.path.getsize(args.output)
    failures = 0
    records = 0
    binary_identity = file_identity(args.processor_binary)
    config_identity = file_identity(args.config_file)
    with os.fdopen(descriptor, 'w', buffering=1) as output:
        while not args.duration or time.monotonic() - started < args.duration:
            try:
                live = get_json(args.base_url, '/api/illuminators/live')
                lane = next(item for item in live.get('illuminators', [])
                            if item.get('id') == args.illuminator)
                shadow = lane.get('live_temporal_shadow') or {}
                if shadow.get('disabled_after_failure'):
                    raise RuntimeError('live temporal observer disabled after failure')
                if shadow.get('result_available') and not shadow.get('null_calibrated', True):
                    generation = str(shadow.get('generation', ''))
                    reset_serial = int(shadow.get('reset_serial', 0))
                    timestamp = int(shadow.get('timestamp_ms', 0))
                    fresh = []
                    for window in shadow.get('shadow_tracking', {}).get('windows', []):
                        sequence = str(window.get('latest_evaluated_sequence', '0'))
                        key = (generation, reset_serial,
                               int(window.get('depth', 0)), sequence)
                        if window.get('evaluated') and sequence != '0' and key not in seen:
                            fresh.append((key, window))
                    if fresh:
                        status = get_json(args.base_url, '/api/system/status')
                        runtime = get_json(args.base_url, '/api/runtime/config')
                        if status.get('configRevision') != status.get('loadedRevision'):
                            raise RuntimeError('saved and loaded configuration revisions differ')
                        truth = get_json(args.base_url, '/api/adsb/delay-doppler', {
                            'illuminator': args.illuminator,
                            'presentation': 'aligned',
                            'frame_timestamp': timestamp,
                            'overlay': 1})
                        lane_rate = lane.get('output_sample_rate_hz')
                        extent = search_extent(runtime, lane_rate)
                        native_detections = exact_radar(args.base_url,
                            '/api/detection', args.illuminator, timestamp)
                        native_tracks = exact_radar(args.base_url,
                            '/api/tracker', args.illuminator, timestamp)
                        if (native_detections.get('available') is False or
                                native_tracks.get('available') is False):
                            raise RuntimeError('exact native frame is unavailable')
                        record = {
                            'schema': 'vectorwarp-live-temporal-pair-v1',
                            'collected_at_ms': int(time.time() * 1000),
                            'illuminator': args.illuminator,
                            'held_out_truth_only': True,
                            'adsb_used_by_radar': False,
                            'scoring_identity': shadow.get('scoring_identity'),
                            'null_calibrated': shadow.get('null_calibrated'),
                            'generation': generation,
                            'reset_serial': reset_serial,
                            'last_reset_reason': shadow.get('last_reset_reason'),
                            'capture_epoch': shadow.get('capture_epoch'),
                            'sequence': shadow.get('sequence'),
                            'timestamp_ms': timestamp,
                            'output_sample_rate_hz': lane_rate,
                            'delay_bin_sample_rate_hz': lane_rate,
                            'search_extent': extent,
                            'provenance': {
                                'server_id': status.get('serverId'),
                                'config_revision': status.get('configRevision'),
                                'loaded_revision': status.get('loadedRevision'),
                                'build': status.get('build'),
                                'processor_binary': binary_identity,
                                'config_file': config_identity,
                            },
                            'first_sample': shadow.get('first_sample'),
                            'end_sample': shadow.get('end_sample'),
                            'work_ms': shadow.get('work_ms'),
                            'windows': [window for _, window in fresh],
                            'native_detections': native_detections,
                            'native_tracks': native_tracks,
                            'truth_delay_doppler': truth,
                        }
                        encoded = json.dumps(record, separators=(',', ':'),
                                             allow_nan=False) + '\n'
                        size = len(encoded.encode())
                        if bytes_written + size > args.max_bytes:
                            return {'records': records, 'bytes': bytes_written,
                                    'output': args.output, 'complete': 'byte_cap'}
                        output.write(encoded); output.flush()
                        seen.update(key for key, _ in fresh)
                        bytes_written += size; records += 1
                        failures = 0
            except (StopIteration, KeyError, ValueError, OSError,
                    RuntimeError, json.JSONDecodeError):
                failures += 1
                if failures >= args.max_failures:
                    raise
            time.sleep(args.interval)
    return {'records': records, 'bytes': bytes_written, 'output': args.output}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base-url', default='http://127.0.0.1:3000')
    parser.add_argument('--illuminator', default='WPXI-RF23')
    parser.add_argument('--output', required=True)
    parser.add_argument('--interval', type=float, default=0.25)
    parser.add_argument('--duration', type=float, default=0)
    parser.add_argument('--max-bytes', type=int, default=64 << 20)
    parser.add_argument('--max-failures', type=int, default=20)
    parser.add_argument('--processor-binary', default='',
                        help='exact live processor binary to hash into provenance')
    parser.add_argument('--config-file', default='',
                        help='exact live configuration file to hash into provenance')
    parser.add_argument('--require-provenance', action='store_true')
    args = parser.parse_args()
    if args.interval < 0.1 or args.duration < 0 or args.max_bytes < 1024 or args.max_failures < 1:
        parser.error('invalid collection bounds')
    if args.require_provenance and (not args.processor_binary or not args.config_file):
        parser.error('--require-provenance needs --processor-binary and --config-file')
    print(json.dumps(collect(args), separators=(',', ':')))


if __name__ == '__main__':
    main()
