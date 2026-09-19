"""Processor thread defaults stay aligned with the native profiling runs."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


LAUNCHER = Path(__file__).resolve().parents[2] / 'script/vectorwarp-macos.py'
spec = importlib.util.spec_from_file_location('vectorwarp_macos', LAUNCHER)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class ProcessorThreadsTests(unittest.TestCase):
    def check_launch(self, overrides):
        env = {'VECTORWARP_MACOS_PROCESSOR': str(LAUNCHER), **overrides}
        life = launcher.Lifecycle(env)
        with patch.object(life, 'live', return_value=None), patch.object(life, 'spawn') as spawn:
            life.start_processor()
        args, _ = spawn.call_args
        self.assertEqual(args[0], 'processor')
        return args[1]

    def test_defaults_apply_to_processor(self):
        env = self.check_launch({})
        self.assertEqual(env['OPENBLAS_NUM_THREADS'], '1')
        self.assertEqual(env['OMP_NUM_THREADS'], '1')

    def test_explicit_settings_are_preserved(self):
        env = self.check_launch({'OPENBLAS_NUM_THREADS': '2', 'OMP_NUM_THREADS': '3'})
        self.assertEqual(env['OPENBLAS_NUM_THREADS'], '2')
        self.assertEqual(env['OMP_NUM_THREADS'], '3')


if __name__ == '__main__':
    unittest.main()
