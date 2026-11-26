"""Real native fixture → pinned receipt → SVG, with semantic tamper refusals."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

RENDERER = Path(__file__).with_name('RenderPassiveIsarPsf.py')
SPEC = importlib.util.spec_from_file_location('aperture_renderer', RENDERER)
renderer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(renderer)
NS = {'s': 'http://www.w3.org/2000/svg'}


def check(value, message):
    if not value:
        raise AssertionError(message)


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = {**os.environ, 'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1',
                   'VECTORWARP_BENCH_RANGE_WORKERS': '0', 'VECTORWARP_FFTW_PLAN': 'estimate'}
    native = subprocess.run([str(executable), '--fixture'], env=environment,
                            capture_output=True, check=True, timeout=30)
    records = [json.loads(line) for line in native.stdout.splitlines()]
    check(records[0]['binary_sha256'] == hashlib.sha256(executable.read_bytes()).hexdigest(),
          'Fixture binary receipt differs')
    grids = [row for row in records if row['kind'] == 'aperture_grid']
    check(len(grids) == 9 and records[-1]['kind'] == 'fixture_pass', 'Native fixture omitted integrated grids')
    check(any(not valid for row in grids for valid in row['valid_mask']), 'Native fixture omitted masking control')
    check(any(valid for row in grids for valid in row['valid_mask']), 'Native fixture omitted positive grid cells')
    with tempfile.TemporaryDirectory(prefix='coherent-aperture-render-') as temporary:
        directory = Path(temporary)
        receipt, output = directory / 'receipt.jsonl', directory / 'grid.svg'
        receipt.write_bytes(native.stdout)
        pin = hashlib.sha256(native.stdout).hexdigest()
        def command(allow=True, expected=pin, destination=output):
            return [sys.executable, '-B', str(RENDERER), '--receipt', str(receipt), '--sha256', expected,
                    '--output', str(destination)] + (['--allow-uncalibrated-fixture'] if allow else [])
        refusal = subprocess.run(command(False), capture_output=True, timeout=10)
        check(refusal.returncode and not output.exists(), 'Uncalibrated fixture rendered without explicit opt-in')
        good = subprocess.run(command(), capture_output=True, check=True, timeout=10)
        tree = ET.parse(output)
        text = ' '.join(node.text or '' for node in tree.findall('.//s:text', NS))
        check('UNCALIBRATED FIXTURE ONLY' in text and 'Delay at terminal midpoint (samples)' in text
              and 'Residual Doppler (Hz)' in text and 'Fixed power scale' in text, 'Axes/qualification/scale labels missing')
        metadata = json.loads(tree.find('s:metadata', NS).text)
        check(metadata['receipt_sha256'] == pin and metadata['grids'] == grids, 'SVG lost raw mask/provenance')
        cells = tree.findall('.//s:rect[@data-cell]', NS)
        check(len(cells) == 9 * 63, 'Renderer omitted cells')
        check(sum(cell.attrib['data-valid'] == 'false' for cell in cells)
              == sum(not valid for grid in grids for valid in grid['valid_mask']), 'Renderer altered invalid mask')
        for cell, pixel, valid in zip(cells, (p for g in grids for p in g['pixels_delay_major']),
                                     (v for g in grids for v in g['valid_mask'])):
            check(cell.attrib['fill'] == (renderer.power_color(pixel) if valid else 'url(#invalid)'),
                  'Rendered power does not use fixed scale')
        original_svg = output.read_bytes()
        refused = subprocess.run(command(), capture_output=True, timeout=10)
        check(refused.returncode and output.read_bytes() == original_svg, 'Renderer overwrote existing artifact')
        # A changed byte must fail the external pin before decoding/rendering.
        receipt.write_bytes(native.stdout + b' ')
        refused = subprocess.run(command(destination=directory / 'tampered.svg'), capture_output=True, timeout=10)
        check(refused.returncode and not (directory / 'tampered.svg').exists(), 'Receipt transport tamper was accepted')
        receipt.write_bytes(native.stdout)
        mutations = [
            ('missing completion', lambda rows: rows.pop()),
            ('fixture relabelled calibrated', lambda rows: rows[0].update(fixture_only=False)),
            ('duplicate grid', lambda rows: rows.insert(-1, copy.deepcopy(next(r for r in rows if r['kind'] == 'aperture_grid')))),
            ('missing grid', lambda rows: rows.pop(next(i for i, r in enumerate(rows) if r['kind'] == 'aperture_grid'))),
            ('metric claim', lambda rows: grid(rows).update(metric_image=True)),
            ('scope', lambda rows: grid(rows).update(scope='live-qualified')),
            ('normalization', lambda rows: grid(rows).update(normalization='per_grid_peak')),
            ('power scale', lambda rows: grid(rows).update(render_power_reference=2)),
            ('scale limits', lambda rows: grid(rows).update(render_db_max=120)),
            ('axis', lambda rows: grid(rows)['residual_doppler_hz'].__setitem__(0, -4)),
            ('iq pin', lambda rows: grid(rows).update(iq_sha256='a' * 64)),
            ('calibration', lambda rows: grid(rows).update(calibration_sha256='a' * 64)),
            ('producer seal', lambda rows: grid(rows)['producer_seals'].__setitem__(0, 'b' * 64)),
            ('producer config', lambda rows: grid(rows)['producer_configuration_sha256'][0].__setitem__(0, 'b' * 64)),
            ('counter precision', lambda rows: grid(rows).update(first_sample=float(grid(rows)['first_sample']))),
            ('interval', lambda rows: grid(rows)['source_intervals'][0].update(end=grid(rows)['source_intervals'][0]['first'])),
            ('pilot', lambda rows: grid(rows)['pilot_coherence'].__setitem__(0, 0)),
            ('gauge', lambda rows: grid(rows)['gauge_correction'].__setitem__(0, [2, 0])),
            ('support', lambda rows: grid(rows).update(unique_blocks='1')),
            ('nonfinite', lambda rows: grid(rows)['pixels_delay_major'].__setitem__(valid_index(grid(rows)), [float('nan'), 0])),
            ('boolean complex', lambda rows: grid(rows)['pixels_delay_major'].__setitem__(valid_index(grid(rows)), [True, 0])),
            ('masked data', lambda rows: grid(rows)['valid_mask'].__setitem__(valid_index(grid(rows)), False)),
            ('mask padding', lambda rows: grid(rows)['pixels_delay_major'].__setitem__(grid(rows)['valid_mask'].index(False), [0, 0])),
        ]
        for label, mutate in mutations:
            changed = copy.deepcopy(records)
            mutate(changed)
            encoded = b''.join(json.dumps(row).encode() + b'\n' for row in changed)
            receipt.write_bytes(encoded)
            try:
                renderer.read_receipt(receipt, hashlib.sha256(encoded).hexdigest(), True)
            except (ValueError, KeyError, TypeError):
                pass
            else:
                raise AssertionError('Re-pinned semantic tamper accepted: ' + label)
        # Duplicate JSON keys are rejected even with a matching outer digest.
        duplicate = native.stdout.replace(b'"metric_image":false', b'"metric_image":true,"metric_image":false', 1)
        check(duplicate != native.stdout, 'Duplicate-key control did not alter native bytes')
        receipt.write_bytes(duplicate)
        try:
            renderer.read_receipt(receipt, hashlib.sha256(duplicate).hexdigest(), True)
        except ValueError:
            pass
        else:
            raise AssertionError('Duplicate JSON key accepted')
        # Fixed scaling remains independent of neighboring peaks and phase.
        check(renderer.power_color([1, 0]) == renderer.power_color([0, -1]) == '#5d5323', '0 dB reference/phase color changed')
        check(renderer.power_color([10000, 0]) == '#facc15', '+80 dB reference color changed')
        check(renderer.power_color([0, 0]) == '#0f172a', 'Zero power is not finite floor color')
        print(f'Native pinned-grid renderer: PASS; {len(mutations)} semantic tampers, hash/duplicate-key/overwrite/fixture refusals; {good.stdout.decode().strip()}')


def grid(rows):
    return next(row for row in rows if row['kind'] == 'aperture_grid' and row['rate_index'] == 0)


def valid_index(row):
    return row['valid_mask'].index(True)


if __name__ == '__main__':
    main()
