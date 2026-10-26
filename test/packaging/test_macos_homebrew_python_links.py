#!/usr/bin/env python3
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'script/repair-macos-homebrew-python-links.sh'
LINK = '/usr/local/bin/2to3-3.12'
EXPECTED = '/Library/Frameworks/Python.framework/Versions/3.12/bin/2to3-3.12'


def validate(target):
    return subprocess.run([
        'bash', '-c',
        'source "$1"; validate_link_target_text "$2" "$3" "$4"',
        'bash', str(SCRIPT), LINK, target, EXPECTED,
    ], text=True, capture_output=True, check=False)


class HomebrewPythonLinkTest(unittest.TestCase):
    def test_accepts_exact_absolute_and_relative_framework_targets(self):
        for target in (EXPECTED,
                       '../../../Library/Frameworks/Python.framework/Versions/3.12/bin/2to3-3.12'):
            with self.subTest(target=target):
                result = validate(target)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_other_targets_with_normalized_diagnostic(self):
        target = '../../../tmp/2to3-3.12'
        result = validate(target)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'actual={target}', result.stderr)
        self.assertIn('normalized=/tmp/2to3-3.12', result.stderr)
        self.assertIn(f'expected={EXPECTED}', result.stderr)


if __name__ == '__main__':
    unittest.main()
