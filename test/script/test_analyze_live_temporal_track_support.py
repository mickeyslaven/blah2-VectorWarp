import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / 'script' / 'analyze-live-temporal-track-support.py'
SPEC = importlib.util.spec_from_file_location('track_support', PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class Args:
    delay_gate = 1
    doppler_gate = 2
    control_shift = [1]
    sample_rate_hz = 299792.458


def record(candidate, track, native=None, state='COASTING'):
    return {
        'output_sample_rate_hz': 299792.458,
        'windows': [{'depth': 2, 'latest_candidates': [
            {'delay_bins': candidate[0], 'doppler_hz': candidate[1]}]}],
        'native_detections': {'delay': [] if native is None else [native[0]],
                              'doppler': [] if native is None else [native[1]]},
        'native_tracks': {'data': [{'delay': track[0], 'doppler': track[1],
                                    'state': state}]},
    }


def test_temporal_only_support_and_shifted_control_are_separate():
    rows = [record((10, 5), (10, 5)), record((30, 9), (30, 9))]
    result = MODULE.analyze(rows, Args())['depths']['2']
    assert result['opportunities'] == 2
    assert result['temporal_only_support'] == 2
    assert result['temporal_only_support_rate'] == 1
    assert result['controls']['1']['support'] == 0
    assert result['states']['COASTING']['temporal_support'] == 2


def test_native_support_is_not_counted_as_temporal_only():
    rows = [record((10, 5), (10, 5), (10, 5), 'ACTIVE'),
            record((30, 9), (30, 9), (30, 9), 'ACTIVE')]
    result = MODULE.analyze(rows, Args())['depths']['2']
    assert result['native_support'] == 2
    assert result['temporal_support'] == 2
    assert result['temporal_only_support'] == 0
