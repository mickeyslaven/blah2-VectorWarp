#!/usr/bin/env python3
"""Regression checks for the recorded-output acceptance oracle."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('outputs', Path(__file__).with_name('compare-outputs.py'))
outputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(outputs)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.frames = [{
            'frame': 0,
            'iq': {'timestamp': 1700000000000, 'frequency': [551000.0], 'spectrum': [-42.25]},
            'detections': {'timestamp': 1700000000000, 'delay': [12.5], 'doppler': [30.0], 'snr': [14.5]},
            'tracks': {'timestamp': 1700000000000, 'n': 1, 'data': [
                {'id': '0001', 'state': 'ACTIVE', 'n': 1000001, 'delay': 12.5, 'doppler': 30.0}]} }]

    def test_identity_counts_and_rounding(self):
        candidate = copy.deepcopy(self.frames)
        candidate[0]['iq']['spectrum'][0] += .01
        result = outputs.compare(self.frames, candidate)
        self.assertFalse(result.errors)
        self.assertEqual((result.detections, result.tracks), (1, 1))

    def test_regressions_are_rejected(self):
        changes = [
            lambda f: f[0].update(frame=1),
            lambda f: f[0]['iq'].update(timestamp=1700000000001),
            lambda f: f[0]['iq'].update(frequency=[551000.01]),
            lambda f: f[0]['iq'].update(spectrum=[float('nan')]),
            lambda f: f[0]['detections'].update(delay=[]),
            lambda f: f[0]['detections'].update(delay=[12.6]),
            lambda f: f[0]['tracks']['data'][0].update(id='0002'),
            lambda f: f[0]['tracks']['data'][0].update(state='COASTING'),
            lambda f: f[0]['tracks']['data'][0].update(n=1000002),
        ]
        for change in changes:
            with self.subTest(change=change):
                candidate = copy.deepcopy(self.frames)
                change(candidate)
                self.assertTrue(outputs.compare(self.frames, candidate).errors)
        self.assertTrue(outputs.compare([], []).errors)
        self.assertTrue(outputs.compare(self.frames, []).errors)


if __name__ == '__main__':
    unittest.main()
