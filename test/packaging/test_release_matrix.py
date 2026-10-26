#!/usr/bin/env python3
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ReleaseMatrixTest(unittest.TestCase):
    maxDiff = None

    def test_linux_ci_covers_every_published_target_with_both_paths(self):
        workflow = (ROOT / '.github/workflows/release-packages.yml').read_text()
        expected = {
            ('ubuntu22.04', 'amd64'), ('ubuntu22.04', 'arm64'),
            ('ubuntu24.04', 'amd64'), ('ubuntu24.04', 'arm64'),
            ('ubuntu26.04', 'amd64'), ('ubuntu26.04', 'arm64'),
            ('debian12', 'arm64'),
            ('debian13', 'amd64'), ('debian13', 'arm64'),
            ('fedora44', 'x86_64'), ('fedora44', 'aarch64'),
        }
        actual = set(re.findall(r'distro: ([\w.]+), format: \w+, arch: (\w+)', workflow))
        self.assertEqual(expected, actual)
        self.assertEqual(workflow.count('script/build-native.sh --backend all --gpu auto --build-dir build/package --jobs 2 --test'), 6)
        self.assertIn('script/test-package-upgrade.sh --previous', workflow)
        self.assertIn('script/test-installed-package.sh --package', workflow)
        self.assertIn('--dragonos-uhd-fixture', workflow)
        self.assertIn('mkdir -p previous-package', workflow)
        upgrade = (ROOT / 'script/test-package-upgrade.sh').read_text()
        self.assertIn('config-preservation.txt', upgrade)
        self.assertIn('receiver-journal-missing', upgrade)
        self.assertIn('vectorwarp-receiver.socket', upgrade)
        self.assertIn('stopped processor was incorrectly activated', upgrade)
        self.assertIn('--cpu-period=100000 --cpu-quota=200000', upgrade)
        self.assertNotIn('podman build --jobs=1 --memory=2g --memory-swap=2g --cpus=', upgrade)
        self.assertIn('command failed with status', upgrade)
        self.assertIn("run_with_log 'base test image build'", upgrade)

    def test_local_matrix_matches_ci_and_dragonos_generations(self):
        rows = []
        for line in (ROOT / 'vm-lab/release-matrix.tsv').read_text().splitlines():
            if line and not line.startswith('#'):
                rows.append(line.split('\t'))
        targets = {row[6] for row in rows}
        self.assertTrue({
            'ubuntu22.04/amd64', 'ubuntu22.04/arm64',
            'ubuntu24.04/amd64', 'ubuntu24.04/arm64',
            'ubuntu26.04/amd64', 'ubuntu26.04/arm64',
            'debian12/arm64', 'debian13/amd64', 'debian13/arm64',
            'fedora44/x86_64', 'fedora44/aarch64',
        }.issubset(targets))
        names = {row[0] for row in rows}
        self.assertTrue({'dragonos-focalx-r37.1', 'dragonos-noble-r9',
                         'dragonos-resolute-r1'}.issubset(names))
        m2 = [row for row in rows if row[1] == 'm2' and row[2] != 'host']
        self.assertTrue(m2)
        self.assertTrue(all(row[7:9] == ['2048', 'lima-serial'] for row in m2))

    def test_macos_has_clean_installs_on_both_runners_and_upgrade(self):
        macos = (ROOT / '.github/workflows/macos.yml').read_text()
        homebrew = (ROOT / '.github/workflows/homebrew.yml').read_text()
        installed_step = macos.split('- name: Verify the installed Homebrew package', 1)[1]
        self.assertNotIn("if: matrix.runner == 'macos-15'", installed_step.split('- name:', 1)[0])
        self.assertIn('brew upgrade --build-from-source mickeyslaven/vectorwarp/vectorwarp', homebrew)
        self.assertEqual(homebrew.count("jq -er '.formulae[0].linked_keg'"), 2)
        self.assertIn("github.event_name == 'pull_request'", homebrew)
        self.assertIn('script/repair-macos-homebrew-python-links.sh', macos)

    def test_release_build_isolated_from_test_outputs(self):
        build = (ROOT / 'script/build-native.sh').read_text()
        package = (ROOT / 'script/package-native.sh').read_text()
        self.assertIn('TEST_CMAKE_DIR="$BUILD_DIR/cmake-tests"', build)
        self.assertIn('-DBUILD_TESTING=ON -DBLAH2_OUTPUT_DIR="$TEST_OUTPUT_DIR"', build)
        self.assertIn('-DBUILD_TESTING=OFF -DBLAH2_OUTPUT_DIR="$ARTIFACT_TMP/bin"', build)
        self.assertIn("-name 'test*'", build)
        self.assertIn('release artifact must not contain test executables', package)


if __name__ == '__main__':
    unittest.main()
