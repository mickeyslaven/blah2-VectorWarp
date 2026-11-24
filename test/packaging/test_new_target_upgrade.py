#!/usr/bin/env python3
"""Offline introduction-baseline refusal and prior-package resolution checks."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('new_target_upgrade', Path(__file__).with_name('new_target_upgrade.py'))
baseline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(baseline)


def manifest(version='0.1.38'):
    entries = []
    for (kind, distro, release), (_, arches) in baseline.PLAN['release_targets'](version).items():
        for arch in arches:
            filename = (f'vectorwarp_{version}-1_{distro}{release}_{arch}.deb' if kind == 'deb' else
                        f'vectorwarp-{version}-1.fc{release}.{arch}.rpm')
            entries.append(dict(format=kind, distro=distro, distro_version=release,
                                arch=arch, filename=filename))
    return dict(version=version, source_commit=baseline.BASELINE, packages=entries)


class IntroductionBaselineTests(unittest.TestCase):
    def test_only_the_reviewed_introduction_transition_can_bootstrap(self):
        good = dict(distro='fedora43', arch='x86_64', candidate='0.1.39',
                    tag='v0.1.38', manifest=manifest())
        self.assertEqual(baseline.request(**good)['sourceCommit'], baseline.BASELINE)
        self.assertEqual(baseline.request(**{**good, 'candidate': '9999.0.0'})['candidate'], '9999.0.0')
        for changes in ({'distro': 'fedora44'}, {'arch': 'aarch64'}, {'candidate': '0.1.40'},
                        {'tag': 'v0.1.37'}, {'manifest': manifest('0.1.39')},
                        {'manifest': {**manifest(), 'source_commit': 'a' * 40}},
                        {'manifest': {**manifest(), 'packages': []}}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                baseline.request(**{**good, **changes})

    def test_adapter_changes_only_external_target_lookup_and_rpm_suffix(self):
        source = ('SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)\n'
                  'case "$DISTRO" in\nfedora44) DISTRO_VERSION=44 ;;\nesac\n'
                  'python3 "$SOURCE_DIR/script/artifact-plan.py" validate "$ARTIFACT"\n'
                  'EXPECTED_ASSET="vectorwarp-${VERSION}-${PACKAGE_RELEASE}.fc44.${RPM_ARCH}.rpm"\n'
                  'RPM_RELEASE="${PACKAGE_RELEASE}.fc44"\n'
                  'bash "$SOURCE_DIR/script/artifact-source-commit.sh" "$ARTIFACT" "$SOURCE_DIR"\n')
        with patch.object(baseline, 'PACKAGER_SHA256', hashlib.sha256(source.encode()).hexdigest()):
            actual = baseline.adapted_packager(source, Path('/tmp/prior source'))
            self.assertIn("SOURCE_DIR='/tmp/prior source'", actual)
            self.assertIn(str(ROOT / 'script/artifact-plan.py') + ' target', actual)
            self.assertIn('"$SOURCE_DIR/script/artifact-plan.py" validate', actual)
            self.assertIn('script/artifact-source-commit.sh', actual)
            self.assertEqual(actual.count('.fc${DISTRO_VERSION}'), 2)
            with self.assertRaisesRegex(ValueError, 'reviewed bytes'):
                baseline.adapted_packager(source + '\n', Path('/tmp/source'))
        changed = source.replace('.fc44', '.fc42')
        with patch.object(baseline, 'PACKAGER_SHA256', hashlib.sha256(changed.encode()).hexdigest()), \
                self.assertRaisesRegex(ValueError, 'no longer matches'):
            baseline.adapted_packager(changed, Path('/tmp/source'))

    def resolve(self, previous, distro='fedora43', candidate='0.1.39'):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        output = root / 'output'
        records = root / 'manifest.json'
        records.write_text(json.dumps(previous))
        release = dict(tag_name='v' + previous['version'], draft=False, prerelease=False,
                       published_at='2026-11-24T00:00:00Z', assets=[
                           dict(name='package-manifest.json', url='https://fixture/manifest'),
                           *[dict(name=entry['filename'], url='https://fixture/package')
                             for entry in previous['packages']]])
        (root / 'releases.json').write_text(json.dumps([release]))
        fake = root / 'gh'
        fake.write_text('#!/bin/sh\ncase "$*" in\n'
                        '*releases?per_page=100*) cat "$FIXTURE_ROOT/releases.json" ;;\n'
                        '*https://fixture/manifest*) cat "$FIXTURE_ROOT/manifest.json" ;;\n'
                        '*https://fixture/package*) printf "published package" ;;\n'
                        '*) exit 93 ;;\nesac\n')
        fake.chmod(0o755)
        result = subprocess.run(['bash', str(ROOT / 'script/download-previous-package.sh'),
                                 '--distro', distro, '--arch', 'x86_64', '--output', str(output),
                                 '--candidate-version', candidate], text=True, capture_output=True,
                                env={**os.environ, 'PATH': f'{root}:{os.environ["PATH"]}',
                                     'FIXTURE_ROOT': str(root)}, timeout=10)
        return result, output

    def test_missing_first_target_creates_request_without_fake_package(self):
        result, output = self.resolve(manifest())
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads((output / 'bootstrap-request.json').read_text())
        self.assertEqual(value['sourceCommit'], baseline.BASELINE)
        self.assertFalse(list(output.glob('*.rpm')))

    def test_later_release_uses_published_target_and_never_bootstraps(self):
        result, output = self.resolve(manifest('0.1.39'), candidate='0.1.40')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((output / 'vectorwarp-0.1.39-1.fc43.x86_64.rpm').read_text(), 'published package')
        self.assertFalse((output / 'bootstrap-request.json').exists())

    def test_missing_old_target_or_later_missing_target_remains_a_failure(self):
        old = copy.deepcopy(manifest())
        old['packages'] = [entry for entry in old['packages'] if entry['distro_version'] != '44']
        for previous, distro, candidate in ((old, 'fedora44', '0.1.39'),
                                             (manifest(), 'fedora43', '0.1.40')):
            result, output = self.resolve(previous, distro, candidate)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((output / 'bootstrap-request.json').exists())


if __name__ == '__main__':
    unittest.main()
