import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).parents[2] / 'script' / 'rerun-temporal-endpoints.py'
spec = importlib.util.spec_from_file_location('rerun_temporal_endpoints', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PairedScoringContract(unittest.TestCase):
    def row(self):
        return {'generation': '1', 'sequence': '8', 'capture_epoch': '0',
                'timestamp_ms': 1000, 'delay_bin_sample_rate_hz': 6144000,
                'provenance': {'processor_binary': {'sha256': 'binary'},
                               'config_file': {'sha256': 'config'},
                               'loaded_revision': 'config', 'config_revision': 'config'},
                'native_detections': {'timestamp': 1000, 'delay': [], 'doppler': [], 'snr': []},
                'windows': [{'depth': 2, 'latest_evaluated_sequence': '8',
                             'base_cpi_seconds': .5, 'confirmation_m': 4,
                             'confirmation_n': 6, 'delete_after': 3,
                             'latest_candidates': [], 'latest_candidate_count': 0,
                             'latest_candidates_export_limit': 256}]}

    def test_missing_policy_and_endpoint_mismatch_quarantine(self):
        row = self.row()
        del row['windows'][0]['base_cpi_seconds']
        row['native_detections']['timestamp'] = 999
        clean, bad = module.clean_rows([row])
        self.assertEqual(clean, [])
        self.assertEqual(set(bad[0]['reasons']), {'pre_v9_policy_missing', 'native_endpoint_mismatch'})

    def test_truncated_and_duplicate_exports_never_count(self):
        row = self.row()
        clean, bad = module.clean_rows([row, row])
        self.assertEqual(len(clean), 1)
        self.assertIn('duplicate_endpoint', bad[0]['reasons'])
        row['windows'][0]['latest_candidate_count'] = 1
        self.assertEqual(module.clean_rows([row])[0], [])

    def test_invalid_support_lineage_quarantined(self):
        row = self.row()
        row['windows'][0].update(latest_candidate_count=1,
            latest_candidates=[{'epoch': '0', 'support_sequences': ['8', '6']}])
        self.assertIn('candidate_lineage_invalid', module.clean_rows([row])[1][0]['reasons'])

    def test_effect_is_paired_and_cluster_uncertainty_not_row_iid(self):
        rows = [{'native': 0, 'shadow': 1, 'generation': '1', 'aircraft': 'a'},
                {'native': 1, 'shadow': 0, 'generation': '2', 'aircraft': 'a'}]
        result = module.summarize(rows)
        self.assertEqual(result['paired_delta_pp'], 0)
        self.assertIsNone(result['aircraft_cluster_bootstrap']['ci95_pp'])
        self.assertEqual(result['generation_cluster_bootstrap']['ci95_pp'], [-100, 100])


if __name__ == '__main__':
    unittest.main()
