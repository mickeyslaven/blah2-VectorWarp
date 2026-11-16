import importlib.util
from pathlib import Path

import pytest


PATH = Path(__file__).parents[2] / 'script' / 'analyze-live-temporal-pairs.py'
SPEC = importlib.util.spec_from_file_location('temporal_pairs', PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class Args:
    delay_gate = .01
    doppler_gate = 1
    control_shift = 1
    sample_rate_hz = 0
    minimum_delay = 0
    maximum_delay = 0
    minimum_doppler = 0
    maximum_doppler = 600


def record(truth_delay=MODULE.LIGHT_SPEED_M_S / 6_144_000 / 1000 * 10):
    return {
        'output_sample_rate_hz': 6_144_000,
        'search_extent': {'delay_min_km': 0,
                          'delay_max_bins': 400,
                          'delay_max_km': MODULE.LIGHT_SPEED_M_S / 2_000_000 / 1000 * 400},
        'truth_delay_doppler': {'aircraft': {'delay': truth_delay, 'doppler': 20}},
        'native_detections': {'delay': [], 'doppler': []},
        'windows': [{'depth': 2, 'latest_candidate_count': 1,
                     'latest_candidates': [
            {'delay_bins': 10, 'doppler_hz': 20}]}],
    }


def test_delay_bins_use_lane_output_rate():
    analysis = MODULE.analyze([record(), record()], Args())
    result = analysis['depths']['2']
    assert result['truth_opportunities'] == 2
    assert result['shadow_hits'] == 2
    assert result['native_hits'] == 0
    assert analysis['qualification']['valid_for_promotion'] is False
    assert 'missing_immutable_runtime_provenance' in analysis[
        'qualification']['invalid_reasons']


def test_truth_outside_search_extent_is_not_an_opportunity():
    outside = MODULE.LIGHT_SPEED_M_S / 2_000_000 / 1000 * 401
    result = MODULE.analyze([record(outside), record(outside)], Args())['depths']['2']
    assert result['truth_opportunities'] == 0


def test_legacy_ledger_requires_explicit_bin_scale_and_extent():
    rows = [record(), record()]
    for row in rows:
        row.pop('output_sample_rate_hz')
        row.pop('search_extent')
    with pytest.raises(ValueError, match='searchable delay limits'):
        MODULE.analyze(rows, Args())
    Args.minimum_delay = 0
    Args.maximum_delay = 60
    try:
        with pytest.raises(ValueError, match='delay-bin sample rate'):
            MODULE.analyze(rows, Args())
    finally:
        Args.minimum_delay = 0
        Args.maximum_delay = 0
