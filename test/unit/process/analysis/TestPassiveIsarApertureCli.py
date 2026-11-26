"""Real CLI round-trip over independently synthesized, sealed IQ fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


def main():
    executable, fixture = map(Path, sys.argv[1:])
    environment = {**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1',
                   'VECTORWARP_BENCH_RANGE_WORKERS': '0'}
    with tempfile.TemporaryDirectory(prefix='passive-isar-cli-') as temporary:
        evidence = Path(temporary) / 'evidence'
        subprocess.run([str(fixture), '--evidence-dir', str(evidence)],
                       env=environment, check=True, capture_output=True, timeout=20)
        arguments = [str(executable), '--channels', '0,1', '--anchor', '0',
                     '--projection', '1', '--rates', '0,-.0005,.0005', '--delay', '0',
                     '--residual-hz', ','.join(str(i / 4) for i in range(-16, 17))]
        frames = [str(evidence / f'producer-0-{i}.vwrps') for i in range(1, 10)]
        result = subprocess.run(arguments + frames, env=environment,
                                capture_output=True, check=True, timeout=20)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(records) == 9 and records[-1]['record'] == 'complete'
        complete = records[-1]
        assert complete['offline_only'] and not complete['metric_image'] and not complete['null_calibrated']
        assert complete['unique_blocks'] == '73' and complete['evaluated_diagnostic_cells'] == '99'
        assert sum(block['owned'] for frame in records[:-1] for block in frame['blocks']) == 73
        assert records[0]['first_sample'] == '9007199254741513'
        assert complete['bootstrap_first_sample'] == '9007199254741001'
        assert complete['handoff'] == 'canonical_pre_winner_coherent_producer'
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        for frame in records[:-1]:
            assert frame['executable_sha256'] == digest
            assert len(frame['source_block_sha256_in_BlockKind_order']) == 20
            assert len(frame['channels']) == 2
            assert len(frame['producer_seal']) == 64 and len(frame['producer_configuration_sha256']) == 3
            assert frame['producer_hop_samples'] == '512'
            assert frame['effective_occupied_bandwidth_hz'] is None
            for channel in frame['channels']:
                assert len(channel['caf_doppler_major']) == 17 * 49
                assert len(channel['range_slow_time_uncropped']) == 17 * 49
                assert [rate['delay_rate_bins_per_second'] for rate in channel['rates']] == [0, -.0005, .0005]
                assert all(len(rate['kernels_by_block']) == 17 for rate in channel['rates'])
        oracle = next(json.loads(line) for line in (evidence / 'producer-seam-psf.jsonl').read_text().splitlines()
                      if json.loads(line)['scatterers'] == 1 and json.loads(line)['frames'] == 8)
        for actual, reference in zip(complete['spectra'][0]['amplitude_rectangular_sum_over_sqrt_unique_blocks'], oracle['bins']):
            expected = complex(reference[3], reference[4])
            assert abs(complex(*actual) - expected) <= 3e-6 * max(1, abs(expected))
        for changed, paths in ((arguments, frames[:1] * 2),
                               (arguments[:6] + ['99'] + arguments[7:], frames),
                               (arguments[:8] + ['.0005'] + arguments[9:], frames),
                               (arguments[:12] + ['-30,0,30'], frames)):
            refused = subprocess.run(changed + paths, env=environment,
                                     capture_output=True, timeout=20)
            assert refused.returncode and not refused.stdout, 'Invalid request emitted a complete/partial aperture'
        authenticated_ingress(executable, evidence, records[0], environment)
    print('Passive ISAR real CLI export/oracle/refusal contract passed')


def authenticated_ingress(executable, evidence, native, environment):
    """Synthetic real-format extraction; never a smaller fabricated snapshot."""
    folder = evidence / 'segments'
    folder.mkdir()
    snapshot = evidence / 'producer-0-2.vwrps'
    data = snapshot.read_bytes()
    sidecar = Path(str(snapshot) + '.vwrl').read_bytes()
    blocks = {str(kind): list(struct.unpack_from('<QQ', data, 512 + 24 * (kind - 1) + 8)) for kind in range(1, 21)}
    digest = lambda value: hashlib.sha256(value).hexdigest()
    source = dict(name=snapshot.name, split='calibration', blocks=blocks,
                  bytes=len(data), first_sample=native['first_sample'], end_sample=native['end_sample'],
                  config_sha256=native['config_sha256'], raw_sha256=native['raw_iq_sha256'],
                  source_sha256=native['source_sha256'], rf_sha256=native['rf_sha256'],
                  snapshot_sha256=digest(data), sidecar_hex=sidecar.hex(), sidecar_sha256=digest(sidecar))
    plan = dict(schema='vectorwarp-coherent-development-ab-plan-v1', channel=0,
                sample_count=1088, sample_rate=1088, delay_min=-24, delay_max=24,
                doppler_min=-8, doppler_max=8, sources=[source])
    plan_path, receipt_path = folder / 'plan.json', folder / 'receipt.json'
    plan_bytes = json.dumps(plan).encode()
    plan_path.write_bytes(plan_bytes)
    ranges = [[blocks[str(kind)][0], blocks[str(kind)][0] + 1088 * 16] for kind in (14, 15, 16)]
    payload = b''.join(data[begin:end] for begin, end in ranges)
    payload_path = folder / 'calibration-001.iqseg'
    payload_path.write_bytes(payload)
    segment = dict(file=payload_path.name, split='calibration', parent_snapshot_sha256=digest(data),
                   first_sample=native['first_sample'], end_sample=native['end_sample'],
                   epoch=native['capture_epoch'], gain_epoch=native['gain_calibration_epoch'],
                   tuning_hz=native['tuning_hz'], raw_reference_filtered_order=True,
                   byte_ranges=[[str(a), str(b)] for a, b in ranges], sha256=digest(payload))
    receipt = dict(schema='vectorwarp-authenticated-iq-segments-v1', plan_sha256=digest(plan_bytes),
                   native_snapshot=False, promotion_eligible=False, segments=[segment],
                   sources=[dict(snapshot_sha256=digest(data), raw_sha256=native['raw_iq_sha256'], envelope_valid=True)])
    receipt_bytes = json.dumps(receipt).encode()
    receipt_path.write_bytes(receipt_bytes)
    def command(pin=None):
        return [str(executable), '--iq', str(plan_path), str(receipt_path), digest(plan_bytes), pin or digest(receipt_bytes),
                '--channels', '0', '--anchor', '0', '--projection', '0', '--rates', '0', '--delay', '0',
                '--residual-hz', '-2,0,2', str(payload_path)]
    def run(args):
        return subprocess.run(args, env=environment, capture_output=True, timeout=20)
    good = run(command())
    assert good.returncode == 0, good.stderr
    exported, complete = map(json.loads, good.stdout.splitlines())
    assert exported['origin'] == 'authenticated_derived_iq_not_native_snapshot'
    assert exported['snapshot_sha256'] == digest(data) and exported['derived_iq_sha256'] == digest(payload)
    assert exported['derived_plan_sha256'] == digest(plan_bytes) and exported['extraction_receipt_sha256'] == digest(receipt_bytes)
    assert exported['parent_byte_ranges_raw_reference_filtered'] == segment['byte_ranges']
    assert exported['carrier_hz'] is None and exported['source_config_wire_hex'] is None
    assert exported['unprocessed_tail_samples'] == '0' and complete['missing_support_samples'] == '0'
    def refuses(args):
        result = run(args)
        assert result.returncode and not result.stdout, ('Malformed authenticated input produced output', result.stderr)
    plan_path.write_bytes(plan_bytes + b' ')
    refuses(command()); plan_path.write_bytes(plan_bytes)
    receipt_path.write_bytes(receipt_bytes + b' ')
    refuses(command()); receipt_path.write_bytes(receipt_bytes)
    payload_path.write_bytes(bytes([payload[0] ^ 1]) + payload[1:])
    refuses(command()); payload_path.write_bytes(payload[:-1])
    refuses(command()); payload_path.write_bytes(payload)
    mutations = [lambda r: r['segments'][0].update(end_sample=str(int(segment['end_sample']) - 1)),
                 lambda r: r['segments'][0]['byte_ranges'][0].__setitem__(1, str(ranges[0][1] - 16)),
                 lambda r: r['segments'][0].update(split='holdout'),
                 lambda r: r['segments'][0].update(epoch='999'),
                 lambda r: r['sources'][0].update(envelope_valid=False),
                 lambda r: r['segments'].append(r['segments'][0])]
    for mutate in mutations:
        changed = json.loads(receipt_bytes)
        mutate(changed)
        encoded = json.dumps(changed).encode()
        receipt_path.write_bytes(encoded)
        refuses(command(digest(encoded)))
    receipt_path.write_bytes(receipt_bytes)
    # Even re-pinned manifests cannot move parent blocks beyond the declared
    # original file, overlap them, or change channel-major block lengths.
    for mutate in (lambda p: p['sources'][0].update(bytes=blocks['16'][0]),
                   lambda p: p['sources'][0]['blocks']['15'].__setitem__(0, blocks['14'][0]),
                   lambda p: p['sources'][0]['blocks']['14'].__setitem__(1, blocks['14'][1] + 16)):
        changed_plan = json.loads(plan_bytes)
        mutate(changed_plan)
        changed_bytes = json.dumps(changed_plan).encode()
        plan_path.write_bytes(changed_bytes)
        changed_receipt = json.loads(receipt_bytes)
        changed_receipt['plan_sha256'] = digest(changed_bytes)
        encoded = json.dumps(changed_receipt).encode()
        receipt_path.write_bytes(encoded)
        args = command(digest(encoded)); args[4] = digest(changed_bytes)
        refuses(args)
    plan_path.write_bytes(plan_bytes); receipt_path.write_bytes(receipt_bytes)
    refuses(command() + [str(payload_path)])
    moving = command(); moving[moving.index('--rates') + 1] = '0,.0005'
    refuses(moving)  # Unknown physical carrier may not manufacture translation.


if __name__ == '__main__':
    main()
