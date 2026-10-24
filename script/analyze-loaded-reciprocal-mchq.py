#!/usr/bin/env python3
"""Offline loaded reciprocal (mismatched) CAF experiment for MCHQ recordings.

This reads a retained 2.4 MHz slice only.  It does not reconstruct ATSC or
touch a receiver.  Channel 0 is the reference; channels 1..N-1 are summed
incoherently after a blockwise reciprocal/mismatched filter:

    H_lambda(f) = conj(X(f)) / (|X(f)|^2 + lambda * median(|X(f)|^2)).

Lambda controls the inverse-filter loading: smaller values flatten spectral
features but amplify low-reference bins (noise gain); larger values approach a
conventional matched filter with more waveform sidelobes.
"""
import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np

MAGIC = 0x4D434851
SAMPLE_RATE = 2_400_000
C_KM_S = 299792.458


def db(value):
    return 10 * math.log10(max(float(value), 1e-300))


def read_blocks(path):
    with Path(path).open('rb') as stream:
        while True:
            header = stream.read(32)
            if not header:
                return
            if len(header) != 32:
                raise ValueError('truncated MCHQ header')
            magic, channels, samples, *_ = struct.unpack('>8I', header)
            if magic not in (MAGIC, 0x4D434854) or not 2 <= channels <= 16 or not samples:
                raise ValueError('malformed MCHQ header')
            metadata = stream.read(channels * 8)
            if len(metadata) != channels * 8:
                raise ValueError('truncated MCHQ metadata')
            if magic == 0x4D434854:
                timing = stream.read(96)
                if len(timing) != 96 or struct.unpack('>II', timing[:8]) != (1, 96):
                    raise ValueError('Unsupported Heimdall timing extension')
            raw = np.frombuffer(stream.read(channels * samples * 2), dtype=np.uint8)
            if raw.size != channels * samples * 2:
                raise ValueError('truncated MCHQ samples')
            raw = raw.reshape(channels, samples, 2).astype(np.float32)
            yield (raw[..., 0] - 127.5 + 1j * (raw[..., 1] - 127.5)) / 127.5


def frames(path, cpi_samples):
    buffer = None
    used = 0
    for block in read_blocks(path):
        if buffer is None:
            buffer = np.empty((block.shape[0], cpi_samples), dtype=np.complex64)
        if block.shape[0] != buffer.shape[0]:
            raise ValueError('channel count changed in MCHQ')
        start = 0
        while start < block.shape[1]:
            take = min(cpi_samples - used, block.shape[1] - start)
            buffer[:, used:used + take] = block[:, start:start + take]
            used += take
            start += take
            if used == cpi_samples:
                yield buffer
                buffer = np.empty_like(buffer)
                used = 0


def caf_frame(iq, fft_size, max_delay_samples, max_doppler_hz, loadings):
    """Return lambda -> noncoherent CAF power map and filter trade-off data."""
    blocks = iq.shape[1] // fft_size
    iq = iq[:, :blocks * fft_size].reshape(iq.shape[0], blocks, fft_size)
    reference = iq[0] - iq[0].mean(axis=1, keepdims=True)
    surveillance = iq[1:] - iq[1:].mean(axis=1, keepdims=True)
    x = np.fft.fft(reference, axis=1)
    y = np.fft.fft(surveillance, axis=2)
    baseline = np.median(np.abs(x) ** 2, axis=1, keepdims=True)
    baseline = np.maximum(baseline, 1e-18)
    matched = np.conj(x)
    matched_response = float(np.mean(np.abs(x * matched)))
    matched_unit_gain = (float(np.mean(np.abs(matched / matched_response) ** 2))
                         if matched_response > 1e-30 else 0.0)
    prf = SAMPLE_RATE / fft_size
    doppler = np.fft.fftshift(np.fft.fftfreq(blocks, d=1 / prf))
    rows = np.flatnonzero(np.abs(doppler) <= max_doppler_hz)
    output, diagnostics = {}, {}
    for loading in loadings:
        h = np.conj(x) / (np.abs(x) ** 2 + loading * baseline)
        correlations = np.fft.ifft(y * h[None, :, :], axis=2)[:, :, :max_delay_samples]
        ambiguity = np.fft.fftshift(np.fft.fft(correlations, axis=1), axes=1)
        power = np.sum(np.abs(ambiguity[:, rows, :]) ** 2, axis=0).astype(np.float64)
        output[loading] = power
        response = float(np.mean(np.abs(x * h)))
        relative_noise_gain = (db(np.mean(np.abs(h / response) ** 2) / matched_unit_gain)
                               if response > 1e-30 and matched_unit_gain > 1e-30 else 0.0)
        diagnostics[str(loading)] = {
            'relative_noise_gain_db_at_equal_mean_response': relative_noise_gain,
            'filter_energy': float(np.mean(np.abs(h) ** 2)),
            'mean_response': response,
            'median_reciprocal_denominator': float(np.median(np.abs(x) ** 2 + loading * baseline)),
        }
    return doppler[rows], output, diagnostics


def local_ratio(surface, row, column, guard=2, training=8):
    left = surface[row, max(0, column - guard - training):max(0, column - guard)]
    right = surface[row, min(surface.shape[1], column + guard + 1):min(surface.shape[1], column + guard + training + 1)]
    noise = np.mean(np.concatenate((left, right))) if left.size + right.size else 1.0
    return surface[row, column] / max(float(noise), 1e-300)


def nearest(axis, value):
    return int(np.argmin(np.abs(axis - value)))


def p99(values):
    values = sorted(values)
    return values[min(len(values) - 1, int(.99 * len(values)))]


def score(maps, doppler_axis, trajectory, loadings, guard, training, controls_stride):
    # Controls are complete trajectory translations on a fixed grid.  Per-control
    # max over lambda accounts for every predeclared parameter trial.
    target_rows, target_columns = [], []
    for truth in trajectory[:len(maps[loadings[0]])]:
        target_rows.append(nearest(doppler_axis, truth['doppler_hz']))
        target_columns.append(nearest(np.arange(maps[loadings[0]][0].shape[1]) * C_KM_S / SAMPLE_RATE,
                                      truth['delay_km']))
    row_offsets = range(-min(target_rows), len(doppler_axis) - max(target_rows), controls_stride)
    column_offsets = range(-min(target_columns), maps[loadings[0]][0].shape[1] - max(target_columns), controls_stride)
    offsets = [(row, column) for row in row_offsets for column in column_offsets
               if abs(row) > guard or abs(column) > guard]
    per_loading = {}
    all_trial_controls = []
    for loading in loadings:
        target_values = [local_ratio(surface, row, column, guard, training)
                         for surface, row, column in zip(maps[loading], target_rows, target_columns)]
        controls = []
        for row_offset, column_offset in offsets:
            values = [local_ratio(surface, row + row_offset, column + column_offset, guard, training)
                      for surface, row, column in zip(maps[loading], target_rows, target_columns)]
            controls.append(float(np.mean(values)))
        per_loading[str(loading)] = {'target': float(np.mean(target_values)), 'control_p99': p99(controls),
            'margin_db': db(np.mean(target_values) / p99(controls)), 'per_frame_ratio_db': [db(value) for value in target_values],
            # Retain the matched off-trajectory evidence distribution so a
            # later cross-family stack can calibrate its own look-elsewhere tail.
            'control_log_ratio_db': [db(value) for value in controls], 'control_count': len(controls)}
    for index, (row_offset, column_offset) in enumerate(offsets):
        all_trial_controls.append(max(
            np.mean([local_ratio(surface, row + row_offset, column + column_offset, guard, training)
                     for surface, row, column in zip(maps[loading], target_rows, target_columns)])
            for loading in loadings))
    return per_loading, {'target_rows': target_rows, 'target_columns': target_columns,
                          'control_stride_bins': controls_stride, 'control_count': len(offsets),
                          'parameter_trial_control_p99': p99(all_trial_controls)}


def analyze(recording, trajectory_file, loadings, cpi, fft_size, max_delay_km,
            max_doppler_hz, guard, training, controls_stride):
    trajectory = json.loads(Path(trajectory_file).read_text())['trajectory']
    cpi_samples = int(round(cpi * SAMPLE_RATE))
    max_delay_samples = int(math.ceil(max_delay_km * SAMPLE_RATE / C_KM_S)) + 1
    maps = {loading: [] for loading in loadings}
    filter_metrics = {str(loading): [] for loading in loadings}
    doppler_axis = None
    for index, iq in enumerate(frames(recording, cpi_samples)):
        if index >= len(trajectory):
            break
        axis, surfaces, diagnostics = caf_frame(iq, fft_size, max_delay_samples, max_doppler_hz, loadings)
        doppler_axis = axis
        for loading in loadings:
            maps[loading].append(surfaces[loading])
            filter_metrics[str(loading)].append(diagnostics[str(loading)])
    if not maps[loadings[0]]:
        raise ValueError('recording has no complete CPI')
    scores, control = score(maps, doppler_axis, trajectory, loadings, guard, training, controls_stride)
    for loading in loadings:
        entry = scores[str(loading)]
        entry['margin_over_parameter_trial_control_p99_db'] = db(entry['target'] / control['parameter_trial_control_p99'])
    return {'schema': 'vectorwarp-loaded-reciprocal-mchq-v1', 'candidate_only': True,
            'recording': str(recording), 'trajectory': str(trajectory_file), 'frames': len(maps[loadings[0]]),
            'sample_rate_hz': SAMPLE_RATE, 'cpi_s': cpi,
            'filter': {'form': 'conj(X) / (abs(X)^2 + lambda*median(abs(X)^2))', 'loadings': loadings,
                       'fft_size': fft_size, 'max_delay_km': max_delay_km, 'max_doppler_hz': max_doppler_hz,
                       'tradeoffs': filter_metrics}, 'controls': control, 'scores': scores,
            'deployment_note': 'Offline replay experiment only; no ATSC reconstruction or live configuration.'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recording', required=True, type=Path); parser.add_argument('--trajectory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path); parser.add_argument('--loadings', default='0.01,0.1,1.0')
    parser.add_argument('--cpi', type=float, default=.5); parser.add_argument('--fft-size', type=int, default=2048)
    parser.add_argument('--max-delay-km', type=float, default=64); parser.add_argument('--max-doppler-hz', type=float, default=400)
    parser.add_argument('--guard', type=int, default=2); parser.add_argument('--training', type=int, default=8)
    parser.add_argument('--controls-stride', type=int, default=2)
    args = parser.parse_args()
    loadings = [float(value) for value in args.loadings.split(',')]
    if not loadings or any(value <= 0 for value in loadings): parser.error('loadings must be positive')
    result = analyze(args.recording, args.trajectory, loadings, args.cpi, args.fft_size, args.max_delay_km,
                     args.max_doppler_hz, args.guard, args.training, args.controls_stride)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
