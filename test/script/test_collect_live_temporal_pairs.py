import importlib.util
from pathlib import Path


PATH = Path(__file__).parents[2] / 'script' / 'collect-live-temporal-pairs.py'
SPEC = importlib.util.spec_from_file_location('collect_temporal_pairs', PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_search_extent_uses_reference_rate_but_lane_bin_scale():
    config = {'process': {'delay_reference_sample_rate_hz': 2_000_000,
                          'ambiguity': {'delayMax': 400},
                          'detection': {'minDelay': 5}}}
    extent = MODULE.search_extent(config, 6_144_000)
    assert extent['delay_min_bins'] == 16
    assert extent['delay_max_bins'] == 1229
    assert extent['reference_sample_rate_hz'] == 2_000_000
    assert extent['delay_bin_sample_rate_hz'] == 6_144_000
    assert abs(extent['delay_min_km'] - .7807095260416667) < 1e-12
    assert abs(extent['delay_max_km'] - 59.96825046907552) < 1e-12


def test_seen_keys_advance_only_after_durable_record_write():
    source = PATH.read_text()
    assert source.index('output.write(encoded); output.flush()') < source.index(
        'seen.update(key for key, _ in fresh)')


def test_resume_key_includes_semantic_reset_serial(tmp_path):
    ledger = tmp_path/'ledger.jsonl'
    ledger.write_text('{"generation":"g","reset_serial":9,"windows":'
                      '[{"depth":4,"latest_evaluated_sequence":"80"}]}\n')
    assert MODULE.last_record_keys(ledger) == {('g', 9, 4, '80')}
