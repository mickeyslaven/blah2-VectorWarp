#!/usr/bin/env python3
"""Inspect observation-only live CAF exports; never emits inference inputs.

This is a complete post-clutter channel surface, not a resident detector replay.
Keep source/config fingerprints and paired detector output in the collection
receipt. No CFAR threshold, global PFA, false-track rate, or beam calibration is
inferred from this artifact. A partial frame invalidates the artifact.
"""

import argparse
import array
import hashlib
import json
import math
import os
import struct
import sys


class ArtifactError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ArtifactError(message)


class Reader:
    def __init__(self, stream):
        self.stream = stream
        self.digest = hashlib.sha256()

    def read(self, size):
        data = self.stream.read(size)
        require(len(data) == size, 'truncated weak-evidence artifact')
        self.digest.update(data)
        return data

    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.read(struct.calcsize('<' + fmt)))


def inspect(path, cells=()):
    file_bytes = os.path.getsize(path)
    require(100 <= file_bytes <= 1 << 30, 'artifact exceeds its absolute byte bound')
    with open(path, 'rb') as stream:
        reader = Reader(stream)
        require(reader.read(8) == b'VWWEAK01', 'wrong weak-evidence magic')
        version, rows, columns, channels = reader.unpack('IIII')
        require(version == 1, 'unsupported weak-evidence version')
        require(0 < rows <= 4096 and 0 < columns <= 4096 and
                rows * columns <= 524288 and 0 < channels <= 8,
                'invalid or unbounded CAF geometry')
        byte_cap, frame_cap = reader.unpack('QI')
        require(1 << 20 <= byte_cap <= 1 << 30 and file_bytes <= byte_cap and
                0 < frame_cap <= 128, 'invalid artifact caps')
        covariance, guard, training = reader.unpack('III')
        loading, shrinkage, clip, floor, ceiling = reader.unpack('ddddd')
        depth, delay_motion, doppler_motion = reader.unpack('III')
        require(covariance <= 2 and guard <= 128 and 0 < training <= 256 and
                all(math.isfinite(v) for v in (loading, shrinkage, clip, floor, ceiling)) and
                loading > 0 and 0 <= shrinkage <= 1 and clip >= 1 and
                0 <= floor <= ceiling <= 1 and depth in (4, 8) and
                delay_motion <= 2 and doppler_motion <= 16,
                'invalid shadow configuration')
        channel_ids = list(reader.unpack('I' * channels))
        require(len(set(channel_ids)) == channels, 'duplicate channel identity')
        delays = reader.unpack('i' * columns)
        dopplers = reader.unpack('d' * rows)
        require(all(math.isfinite(value) for value in dopplers), 'nonfinite Doppler axis')
        require(all(0 <= row < rows and 0 <= column < columns for row, column in cells),
                'requested cell lies outside CAF geometry')
        result = {
            'format': 'VWWEAK01', 'sha256': None, 'bytes': file_bytes,
            'observation_only': True, 'detector_replay': False, 'null_calibrated': False,
            'rows': rows, 'columns': columns, 'channel_ids': channel_ids,
            'bootstrap': {'covariance': ('diagonal', 'shrinkage', 'robust_shrinkage')[covariance],
                          'guard_cells': guard, 'training_cells': training,
                          'diagonal_loading': loading, 'shrinkage': shrinkage,
                          'robust_clip': clip, 'robust_shrinkage_floor': floor,
                          'robust_shrinkage_ceiling': ceiling},
            'temporal': {'max_cpis': depth, 'delay_motion_bins': delay_motion,
                         'doppler_motion_bins': doppler_motion},
            'frames': [],
        }
        previous = None
        while stream.tell() < file_bytes:
            require(len(result['frames']) < frame_cap, 'artifact exceeds frame cap')
            start = stream.tell()
            require(reader.read(8) == b'FRMWEAK1', 'missing frame marker')
            (record_bytes,) = reader.unpack('Q')
            epoch, sequence, first, end, tuning, configuration, generation, timestamp = reader.unpack('Q' * 8)
            rate, map_rate, encoding, reserved, sample_count = reader.unpack('IIIIQ')
            require(encoding in (1, 2) and reserved == 0 and sample_count == rows * columns * channels,
                    'CAF shape or encoding differs from header')
            expected = 120 + sample_count * (16 if encoding == 1 else 8)
            require(record_bytes == expected and record_bytes <= file_bytes - start,
                    'invalid frame length or truncated CAF')
            require(end > first and tuning and configuration and timestamp and rate and map_rate,
                    'incomplete frame lineage')
            payload = array.array('d' if encoding == 1 else 'f')
            payload.frombytes(reader.read(sample_count * (16 if encoding == 1 else 8)))
            if sys.byteorder != 'little':
                payload.byteswap()
            require(all(math.isfinite(value) for value in payload), 'nonfinite channel CAF')
            require(reader.read(8) == b'ENDWEAK1' and reader.unpack('Q')[0] == record_bytes,
                    'uncommitted or malformed frame footer')
            continuous = previous is not None and (
                epoch == previous[0] and sequence == previous[1] + 1 and
                first == previous[3] and end - first == previous[3] - previous[2] and
                (tuning, configuration, generation, rate, map_rate) == previous[4:])
            frame = {
                'capture_epoch': str(epoch), 'sequence': str(sequence),
                'first_sample': str(first), 'end_sample': str(end),
                'tuning_hz': str(tuning), 'configuration_epoch': str(configuration),
                'generation': str(generation), 'timestamp_ms': str(timestamp),
                'capture_sample_rate_hz': rate, 'map_sample_rate_hz': map_rate,
                'continuous_with_previous': continuous,
                'encoding': 'cpu_row_major_f64' if encoding == 1 else 'resident_delay_major_f32',
                'cells': [],
            }
            for row, column in cells:
                index = row * columns + column if encoding == 1 else (
                    column * rows + (row + rows // 2 + 1) % rows)
                values = []
                for channel in range(channels):
                    offset = 2 * (channel * rows * columns + index)
                    values.append([payload[offset], payload[offset + 1]])
                frame['cells'].append({'row': row, 'column': column,
                    'delay_samples': delays[column], 'doppler_hz': dopplers[row],
                    'complex_by_channel': values})
            result['frames'].append(frame)
            previous = (epoch, sequence, first, end, tuning, configuration, generation, rate, map_rate)
        require(result['frames'], 'artifact has no committed observations')
        result['sha256'] = reader.digest.hexdigest()
        return result


def cell(text):
    try:
        row, column = text.split(',')
        return int(row), int(column)
    except ValueError as error:
        raise argparse.ArgumentTypeError('cell must be ROW,COLUMN') from error


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact')
    parser.add_argument('--cell', action='append', type=cell, default=[],
                        help='return exact complex channel CAFs at ROW,COLUMN')
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.artifact, args.cell), allow_nan=False, indent=2))
    except (ArtifactError, OSError) as error:
        parser.exit(2, f'Invalid weak-evidence artifact: {error}\n')
