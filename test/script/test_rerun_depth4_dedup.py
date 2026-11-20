import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).parents[2]/'script'/'rerun-depth4-dedup.py'
spec = importlib.util.spec_from_file_location('dedup',path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
ADJ = {'name':'adjacent_cell_max','boundary':'inclusive',
       'delay_half_width_native_bins':1,'doppler_half_width_native_bins':1}
CENTROID = {'name':'configured_centroid_max','boundary':'strict',
            'delay_half_width_native_bins':19,'doppler_half_width_native_bins':6}


def point(delay, doppler, snr):
    return {'delay_bins':delay,'doppler_hz':doppler,'snr_db':snr,'support_sequences':['8','7','6','5']}


class FrozenSuppression(unittest.TestCase):
    def test_adjacent_boundary_uses_base_cpi(self):
        a,b,c = point(10,20,9),point(11,22,8),point(12,22,7)
        self.assertEqual(module.suppress([a,b,c],.5,ADJ),[a])

    def test_comparison_uses_all_original_points_not_greedy_winners(self):
        a,b,c = point(10,20,9),point(11,20,8),point(12,20,7)
        self.assertEqual(module.suppress([a,b,c],.5,ADJ),[a])

    def test_centroid_boundary_is_strict(self):
        a,b,c = point(10,20,9),point(29,20,8),point(10,32,7)
        self.assertEqual(module.suppress([a,b,c],.5,CENTROID),[a,b,c])

    def test_ties_stable_and_values_lineage_unchanged(self):
        a,b = point(10,20,9),point(11,20,9)
        result = module.suppress([b,a],.5,ADJ)
        self.assertEqual(result,[a])
        self.assertIs(result[0],a)
        self.assertEqual(module.suppress([a,a.copy()],.5,ADJ),[a])


if __name__ == '__main__':
    unittest.main()
