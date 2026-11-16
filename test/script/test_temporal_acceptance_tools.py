import importlib.util
from pathlib import Path
import pytest

ROOT = Path(__file__).parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT/'script'/filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


calibration = load('calibration', 'calibrate-temporal-score-families.py')
passes = load('passes', 'validate-temporal-whole-passes.py')


def test_calibration_is_whole_pass_disjoint_and_per_depth():
    rows = []
    for split, pass_id, offset in [('calibration', 'a', 0), ('holdout', 'b', 1)]:
        for depth in (2, 4, 8):
            rows += [{'pass_id': pass_id, 'split': split, 'adjudication': 'nuisance',
                      'score_family': 'bootstrap', 'depth': depth,
                      'score_db': offset+i/10} for i in range(32)]
    result = calibration.calibrate(rows, .1, 32)
    assert set(result['strata']) == {'bootstrap:depth2', 'bootstrap:depth4', 'bootstrap:depth8'}
    rows[-1]['pass_id'] = 'a'
    with pytest.raises(ValueError, match='split leakage'):
        calibration.calibrate(rows, .1, 32)


def test_whole_pass_validator_rejects_gaps_and_accepts_complete_depth4():
    records = []
    for sequence in range(4, 84, 4):
        records.append({'generation': 'g', 'reset_serial': 7,
          'provenance': {'loaded_revision': 'r', 'processor_binary': {'sha256': 'h'}},
          'windows': [{'depth': 4, 'latest_evaluated_sequence': str(sequence),
                       'truncated_candidates': 0, 'latest_candidate_count': 0,
                       'latest_candidates': []}]})
    manifest = {'passes': [{'pass_id': 'p', 'split': 'evaluation', 'generation': 'g',
                            'reset_serial': 7, 'depth': 4,
                            'first_sequence': 4, 'last_sequence': 80}]}
    assert len(passes.validate(records, manifest)['certified']) == 1
    records.pop(3)
    assert passes.validate(records, manifest)['rejected'][0]['reasons'] == ['endpoint_gap']


def test_whole_pass_validator_rejects_missing_identity_and_aliases():
    records = [{'generation': 'g', 'reset_serial': 1, 'provenance': {},
      'windows': [{'depth': 4, 'latest_evaluated_sequence': str(sequence),
                   'truncated_candidates': 0, 'latest_candidate_count': 0,
                   'latest_candidates': []}]}
      for sequence in range(4, 84, 4)]
    one = {'pass_id': 'p1', 'split': 'evaluation', 'generation': 'g',
           'reset_serial': 1, 'depth': 4, 'first_sequence': 4, 'last_sequence': 80}
    result = passes.validate(records, {'passes': [one]})
    assert result['rejected'][0]['reasons'] == ['runtime_identity_changed_or_missing']
    alias = dict(one, pass_id='p2', split='calibration')
    with pytest.raises(ValueError, match='overlapping'):
        passes.validate(records, {'passes': [one, alias]})
    cross_depth_alias = dict(alias, depth=8, first_sequence=8)
    with pytest.raises(ValueError, match='overlapping'):
        passes.validate(records, {'passes': [one, cross_depth_alias]})


def test_effect_readiness_counts_unique_pass_ids_not_declarations():
    records = [{'generation': 'g', 'reset_serial': 1,
      'provenance': {'loaded_revision': 'r', 'processor_binary': {'sha256': 'h'}},
      'windows': [{'depth': depth, 'latest_evaluated_sequence': str(sequence),
                   'truncated_candidates': 0, 'latest_candidate_count': 0,
                   'latest_candidates': []}]}
      for depth in (2, 4, 8) for sequence in range(depth, 81, depth)]
    declarations = [{'pass_id': 'same', 'split': 'evaluation', 'generation': 'g',
                     'reset_serial': 1, 'depth': depth,
                     'first_sequence': depth, 'last_sequence': 80}
                    for depth in (2, 4, 8)]
    result = passes.validate(records, {'passes': declarations})
    assert result['certified_counts']['evaluation'] == 1
    assert not result['effect_study_ready']
