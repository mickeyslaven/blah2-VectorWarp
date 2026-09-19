"""Repository integrity tests; never install packages or publish a repository.

VECTORWARP_REPOSITORY_INTEGRATION=1 also builds tiny format fixtures and signs
them with a temporary test-only key. Run that mode in an isolated test runner.
"""
import argparse
from copy import deepcopy
import importlib.util
from html import unescape
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("repository", ROOT / "script/build-package-repository.py")
repository = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(repository)


class HomepageTests(unittest.TestCase):
    def release_manifest(self):
        entries = []
        for (format, distro, version), (_, architectures) in repository.TARGETS.items():
            for arch in sorted(architectures):
                label = f"{distro}{version}"
                filename = (f"vectorwarp_1.2.3-1_{label}_{arch}.deb" if format == "deb" else
                            f"vectorwarp-1.2.3-1.fc44.{arch}.rpm")
                entries.append(dict(format=format, distro=distro, distro_version=version,
                                    arch=arch, filename=filename))
        return dict(version="1.2.3", source_commit="a" * 40,
                    signing_fingerprint="A" * 40, packages=entries)

    def test_downloads_use_exact_release_assets_and_all_os_rows(self):
        manifest = self.release_manifest()
        page = repository.repository_homepage(manifest)
        links = re.findall(r'href="([^"]+)"', page)
        package_links = {link for link in links if link.endswith((".deb", ".rpm"))}
        base = "https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v1.2.3"
        self.assertEqual(package_links, {f"{base}/{entry['filename']}" for entry in manifest['packages']})
        self.assertEqual(len(package_links), 10)
        for label in ('Ubuntu 22.04', 'Ubuntu 24.04', 'Ubuntu 26.04', 'Debian 13', 'Fedora 44',
                      'DragonOS · Ubuntu 22.04 base', 'DragonOS · Ubuntu 24.04 base',
                      'DragonOS · Ubuntu 26.04 base', 'Raspberry Pi OS · 64-bit Trixie',
                      'amd64 / x86_64', 'arm64 / aarch64'):
            self.assertIn(label, page)
        pi_row = next(row for row in re.findall(r'<tr>.*?</tr>', page) if 'Raspberry Pi OS' in row)
        self.assertIn('<td>—</td>', pi_row)
        self.assertIn('debian13_arm64.deb', pi_row)
        self.assertNotIn('amd64.deb', pi_row)
        self.assertLess(page.index('id="install"'), page.index('id="results"'))

    def test_download_preview_and_partial_manifests_never_invent_assets(self):
        preview = repository.repository_homepage()
        self.assertIn('downloads are not available in this preview', preview)
        self.assertNotIn('/releases/download/', preview)
        manifest = self.release_manifest()
        manifest['packages'] = [entry for entry in manifest['packages'] if
                                (entry['distro'], entry['arch']) == ('fedora', 'aarch64')]
        page = repository.repository_homepage(manifest)
        self.assertNotIn('Download DEB', page)
        self.assertNotIn('Raspberry Pi OS · 64-bit Trixie', page)
        self.assertEqual(page.count('Download RPM'), 1)

    def test_downloads_reject_unsafe_or_duplicate_identities(self):
        for invalid in ('latest', '../main', '1.2.3?bad', '<script>'):
            manifest = self.release_manifest()
            manifest['version'] = invalid
            with self.subTest(version=invalid), self.assertRaises(ValueError):
                repository.repository_homepage(manifest)
        for invalid in ('../../package.deb', '" onclick="bad', None):
            manifest = self.release_manifest()
            manifest['packages'][0]['filename'] = invalid
            with self.subTest(filename=invalid), self.assertRaises(ValueError):
                repository.repository_homepage(manifest)
        manifest = self.release_manifest()
        manifest['packages'].append(manifest['packages'][0])
        with self.assertRaises(ValueError):
            repository.repository_homepage(manifest)

    def test_install_and_verification_commands_are_explicit_and_valid_shell(self):
        page = repository.repository_homepage(self.release_manifest())
        for text in ('sudo apt install -y curl gnupg', 'sudo dnf install -y curl gnupg2',
                     'sudo bash vectorwarp-install.sh --repo-only',
                     'sudo apt install -y vectorwarp', 'sudo dnf install -y vectorwarp',
                     'sudo dnf upgrade --refresh vectorwarp',
                     'sudo apt install ./matching.deb', 'sudo dnf install ./matching.rpm',
                     '<code>vectorwarp</code>', '<code>vectorwarp start</code>',
                     '<code>vectorwarp stop</code>', '<code>vectorwarp status</code>',
                     'Successful upgrades restart previously running VectorWarp services',
                     'intentionally stopped radar stopped',
                     'FocalX R37.1 is Ubuntu 22.04 (Jammy) amd64,\nnot Ubuntu 26.04',
                     'installs the prerequisites', 'installer source',
                     'On a fresh install, radar processing stays stopped',
                     'SHA256SUMS.asc', 'vectorwarp-archive-key.asc', 'A' * 40,
                     'checksum alone does not authenticate', 'gpgv --keyring',
                     'sha256sum --check --strict --ignore-missing'):
            self.assertIn(text, page)
        self.assertNotIn('--only-upgrade', page)
        self.assertNotIn('less vectorwarp-install.sh', page)
        install_commands = []
        for code in re.findall(r'<pre><code>(.*?)</code></pre>', page, re.DOTALL):
            if 'curl --fail' in code:
                command = unescape(code)
                self.assertNotIn('\n', command)
                self.assertTrue(command.endswith('&& vectorwarp'))
                install_commands.append(command)
            if 'sudo apt' in code:
                self.assertNotIn('sudo dnf', code, 'Do not mix OS commands in one copyable block')
            result = subprocess.run(['bash', '-n'], input=unescape(code), text=True,
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(install_commands), 2)
        for guide in ('README.md', 'docs/INSTALL.md'):
            text = (ROOT / guide).read_text()
            for command in install_commands:
                self.assertIn(command, text, f'{guide} must match the published installation command')
        for heading in ('1. Install for your OS', '2. Configure and start radar',
                        '3. Start, stop and check services', '4. Update'):
            self.assertIn(f'<h3>{heading}</h3>', page)

    def test_install_guidance_tracks_verified_release_version(self):
        manifest = self.release_manifest()
        manifest['version'] = '0.1.6'
        old_page = repository.repository_homepage(manifest)
        self.assertIn('Install VectorWarp 0.1.6', old_page)
        self.assertIn('A package update does not restart a running API or receiver helper', old_page)
        self.assertIn('sudo systemctl enable --now vectorwarp-api.service', old_page)
        self.assertIn('sudo systemctl restart vectorwarp-receiver.service', old_page)
        self.assertNotIn('<code>vectorwarp start</code>', old_page)
        self.assertNotIn('<code>vectorwarp help</code>', old_page)
        self.assertNotIn('Successful upgrades restart previously running VectorWarp services', old_page)

        manifest['version'] = '0.1.7'
        new_page = repository.repository_homepage(manifest)
        self.assertIn('Install VectorWarp 0.1.7', new_page)
        self.assertIn('<code>vectorwarp start</code>', new_page)
        self.assertIn('<code>vectorwarp restart</code>', new_page)
        self.assertIn('<code>vectorwarp help</code>', new_page)
        self.assertIn('Stop includes the web interface', new_page)
        self.assertIn('Successful upgrades restart previously running VectorWarp services', new_page)
        self.assertNotIn('A package update does not restart a running API', new_page)
        self.assertNotIn('sudo systemctl restart vectorwarp-receiver.service', new_page)
        self.assertNotIn('--only-upgrade', old_page)
        self.assertNotIn('--only-upgrade', new_page)

    def test_timing_claims_keep_their_scope(self):
        page = repository.repository_homepage()
        for required in ('Matched 200 ms processing workloads', 'RTX 4050 Laptop', 'Pavilion AMD GPU',
                         'Earlier live array proof', '95.5 ms',
                         'Same recorded IQ at its original rate', 'Same CPU budget',
                         'clutter FFT/filtering', 'small FP64 coefficient solve',
                         'cannot safely represent the requested geometry',
                         'NVIDIA, AMD and Intel GPU checks',
                         'Equal-range Doppler tests', '30.604 km excess-path',
                         'GPU_BENCHMARK_20260911.md'):
            self.assertIn(required, page)

        # The front page selects examples; the linked report must keep the
        # full comparison, including slower configurations and test boundaries.
        report = (ROOT / 'docs/GPU_BENCHMARK_20260911.md').read_text()
        plain_report = ' '.join(report.split())
        for required in ('c821bee3f0d27cf20c8447f3d908ef722905a4de',
                         '1e-4', '30.604 km', '41 paired groups', '14 prior-version groups',
                         '110 timed runs', '2,200 complete CPIs',
                         'not an endurance test', 'not bit-exact output', 'future work'):
            self.assertIn(required, plain_report)
        cohort = json.loads((ROOT / 'docs/benchmarks/20260911-efficiency/comparison.json').read_text())
        self.assertEqual(len(cohort['rows']), 55)
        current = [row for row in cohort['rows'] if not row['variant'].startswith('vectorwarp-before-')]
        prior = [row for row in cohort['rows'] if row['variant'].startswith('vectorwarp-before-')]
        self.assertEqual(len(current), 41)
        self.assertEqual(len(prior), 14)
        self.assertEqual(sum(item['measurement_runs'] for item in cohort['receipts'].values()), 110)
        self.assertEqual(sum(item['complete_cpis'] for item in cohort['receipts'].values()), 2200)
        self.assertIn('ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6', report)
        self.assertIn('8ba6e1330fad9fcdbe8a5a54134a712d0959775e', report)
        readme = (ROOT / 'README.md').read_text()
        for row in cohort['rows']:
            self.assertEqual(row['delay_bins'], 256)
            self.assertAlmostEqual(row['max_excess_path_km'], 30.603813420833334)
            self.assertEqual(row['frames'], 24)
            self.assertIn(f"{row['mean_ms']:.3f} / {row['p95_ms']:.3f} / {row['deadline_misses']}/24", report)
            if row['variant'] == 'vectorwarp-auto':
                self.assertEqual(row['cpu_oracle_frames'], 0)
                self.assertEqual(row['gpu_dd_frames'], 24)
                self.assertEqual(row['gpu_clutter_frames'], 24)
            if row in prior:
                corresponding = next(item for item in current if
                    (item['host'], item['case'], item['device'], item['variant']) ==
                    (row['host'], row['case'], row['device'], row['variant'].replace('-before', '')))
                reduction = 100 * (1 - corresponding['mean_ms'] / row['mean_ms'])
                self.assertIn(f'{reduction:.1f}%', report)
                continue
            selected = ((row['host'] in ('strix', 'nvidia') and row['case'] == 'pair-200ms-800hz') or
                        (row['host'] == 'pavilion' and row['case'] == 'pair-200ms-2400hz' and
                         row['device'] in ('auto', '4098:27039:1')))
            if selected:
                self.assertIn(f"{row['mean_ms']:.1f} ms", page)
                self.assertIn(f"{row['mean_ms']:.1f} ms", readme)
        for variant in ('vectorwarp-cpu', 'vectorwarp-auto'):
            reductions = []
            for row in current:
                if row['variant'] != variant or row['case'] != 'pair-200ms-2400hz':
                    continue
                upstream = next(item for item in current if item['host'] == row['host'] and
                                item['case'] == row['case'] and item['variant'] == 'regular-blah2')
                reductions.append(100 * (1 - row['mean_ms'] / upstream['mean_ms']))
            self.assertIn(f'{min(reductions):.1f}–{max(reductions):.1f}%', report)

    def test_current_sink_profile_and_historical_boundaries(self):
        report = (ROOT / 'docs/GPU_BENCHMARK_20260911.md').read_text()
        cohort = json.loads((ROOT / 'docs/benchmarks/20260911-efficiency/comparison.json').read_text())
        profiles = {row['variant']: row for row in cohort['rows']
                    if row['host'] == 'strix' and row['case'] == 'pair-200ms-2400hz'}
        for key in ('json_ms', 'clutter_ms', 'ambiguity_ms', 'fusion_ms',
                    'spectrum_ms', 'extract_ms', 'detection_ms', 'tracker_ms'):
            prior = profiles['vectorwarp-before-auto']['stages'][key]
            current = profiles['vectorwarp-auto']['stages'][key]
            self.assertIn(f'{prior:.3f} ms | {current:.3f} ms', report)
        for scope in ('not evidence of improved detection', 'not a guarantee',
                      'not isolate each change', 'not a kernel-only or zero-copy claim',
                      'Earlier native live processor measurements',
                      'not rerun for the startup-only or combined efficiency changes',
                      'benchmarks/20260911-equal-range/README.md',
                      'benchmarks/20260911-efficiency/comparison.json'):
            self.assertIn(scope, ' '.join(report.split()))
        self.assertIn('2a9bfdf', report)

    def test_pi_three_way_results_keep_baselines_and_limits(self):
        report = (ROOT / 'docs/PI4_PERFORMANCE_20260911.md').read_text()
        cohort = json.loads((ROOT / 'docs/benchmarks/20260911-pi-efficiency/comparison.json').read_text())
        self.assertEqual((cohort['runs'], cohort['complete_cpis'], len(cohort['rows'])), (24, 480, 12))
        self.assertEqual({row['variant'] for row in cohort['rows']},
                         {'regular-blah2', 'offworld-blah2-arm', 'vectorwarp-cpu'})
        self.assertEqual(cohort['contract']['offworld_commit'], '1d37e29c9f788bed2bc95b1b320ccf6478430111')
        self.assertTrue(cohort['contract']['host']['fftw_neon_symbols'])
        for row in cohort['rows']:
            self.assertEqual((row['frames'], row['deadline_misses'], row['cpi_ms']), (24, 24, 200))
            self.assertAlmostEqual(row['max_excess_path_km'], 30.603813420833333)
            self.assertIn(f"{row['mean_ms']:.3f} / {row['p95_ms']:.3f}", report)
            if row['case'] == 'standard-800hz':
                self.assertIn(f"{row['mean_ms']:.1f} ms", repository.repository_homepage())
                self.assertIn(f"{row['mean_ms']:.1f} ms", (ROOT / 'README.md').read_text())
        for text in ('NEON-enabled FFTW', '24/24', 'not live RF', '7.5–12.6%',
                     '6.7–11.9%', '199 taps instead of 210', 'no GPU radar-processing path'):
            self.assertIn(text, ' '.join(report.split()))

    def test_page_has_accessible_layout_and_current_repository(self):
        page = repository.repository_homepage()
        for required in ('lang="en"', 'name="viewport"', '<caption>',
                         'scope="col"', 'scope="row"', 'overflow-x:auto',
                         'tabindex="0"', 'href="#install"',
                         'blah2-VectorWarp/blob/main/docs/INSTALL.md#build-from-source',
                         'https://github.com/30hours/blah2'):
            self.assertIn(required, page)

    def test_quickstart_links_and_release_status(self):
        # Wrapping and headings are editorial choices. Keep the measured scope,
        # actual upstream comparison, and release boundaries under test.
        readme = ' '.join((ROOT / 'README.md').read_text().split())
        self.assertIn('repository installer chooses the matching signed APT or DNF repository', readme)
        self.assertIn('sudo bash vectorwarp-install.sh --repo-only', readme)
        self.assertIn('https://mickeyslaven.github.io/blah2-VectorWarp/#install', readme)
        self.assertNotRegex(readme, r'/releases/download/v[0-9]')
        self.assertIn('replaying the same recorded signal at its original rate', readme)
        self.assertIn('CPU budget', readme)
        self.assertIn('2–8-channel network input', readme)
        self.assertIn('one package', readme.lower())
        for claim in ('Regular blah2 CPU', 'VectorWarp CPU', 'VectorWarp GPU',
                      'clutter FFT/filtering', 'startup',
                      'docs/GPU_BENCHMARK_20260911.md',
                      'Wider Doppler coverage'):
            self.assertIn(claim, readme)
        # Numeric findings belong to the linked report rather than an old
        # README headline; editorial changes must not resurrect obsolete runs.
        report = (ROOT / 'docs/GPU_BENCHMARK_20260911.md').read_text()
        self.assertIn('c821bee3f0d27cf20c8447f3d908ef722905a4de', report)
        self.assertIn('1e-4', report)
        self.assertTrue((ROOT / 'html/favicon/vectorwarp-vw.svg').is_file())
        for name in ('README.md', 'docs/INSTALL.md', 'docs/SETUP.md', 'packaging/README.md'):
            document = ROOT / name
            for target in re.findall(r'\]\(([^)]+)\)', document.read_text()):
                if '://' in target or target.startswith('mailto:'):
                    continue
                relative, _, anchor = target.partition('#')
                destination = document.parent / relative if relative else document
                with self.subTest(document=name, target=target):
                    self.assertTrue(destination.is_file(), f'missing link: {target}')
                    if anchor:
                        headings = re.findall(r'^#+\s+(.+)$', destination.read_text(), re.MULTILINE)
                        anchors = {re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')
                                   for heading in headings}
                        self.assertIn(anchor, anchors)

    def test_readme_install_routes_cover_mac_and_live_linux_packages(self):
        readme = (ROOT / 'README.md').read_text()
        self.assertIn('[Install](docs/INSTALL.md)', readme)
        self.assertIn('[macOS / Homebrew](docs/MACOS_HOMEBREW.md)', readme)
        guide = (ROOT / 'docs/INSTALL.md').read_text()
        self.assertIn('(https://mickeyslaven.github.io/blah2-VectorWarp/#install)', guide)
        self.assertIn('(MACOS_HOMEBREW.md)', guide)
        self.assertIn('## Build from source', guide)
        page = repository.repository_homepage(self.release_manifest())
        for command in ('sudo bash vectorwarp-install.sh --repo-only',
                        'vectorwarp start', 'vectorwarp stop', 'vectorwarp status'):
            self.assertIn(command, guide)
            self.assertIn(command, page)

    def test_install_docs_use_canonical_page_not_hardcoded_release_assets(self):
        for document in ('README.md', 'docs/INSTALL.md'):
            text = (ROOT / document).read_text()
            self.assertIn('https://mickeyslaven.github.io/blah2-VectorWarp/#install', text)
            self.assertNotRegex(text, r'https://github\.com/mickeyslaven/blah2-VectorWarp/releases/download/v')

    def test_future_release_homepage_has_only_its_own_asset_urls(self):
        manifest = self.release_manifest()
        manifest['version'] = '2.4.6'
        for entry in manifest['packages']:
            entry['filename'] = entry['filename'].replace('1.2.3', '2.4.6')
        page = repository.repository_homepage(manifest)
        base = 'https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v2.4.6'
        self.assertEqual({link for link in re.findall(r'href="([^"]+)"', page)
                          if link.endswith(('.deb', '.rpm'))},
                         {f"{base}/{entry['filename']}" for entry in manifest['packages']})
        self.assertNotIn('/v1.2.3/', page)
        self.assertNotIn('/v0.1.0/', page)
        for command in ('sudo bash vectorwarp-install.sh --repo-only',
                        'vectorwarp start', 'vectorwarp stop', 'vectorwarp status'):
            self.assertIn(command, page)

    def _release_selection_script(self):
        workflow = (ROOT / '.github/workflows/publish-package-repository.yml').read_text()
        match = re.search(r'(?ms)^      - id: release\n.*?^        run: \|\n'
                          r'((?:^          [^\n]*\n)+)', workflow)
        self.assertIsNotNone(match, 'workflow must retain the bounded release-selection step')
        return textwrap.dedent(match.group(1))

    def _select_release_tag(self, event, releases, event_tag='', input_tag='', ref='refs/heads/main'):
        with tempfile.TemporaryDirectory(prefix='vectorwarp-release-selection-') as temporary:
            temporary = Path(temporary)
            fixture = temporary / 'releases.json'
            fixture.write_text(json.dumps(releases))
            fake_gh = temporary / 'gh'
            fake_gh.write_text('#!/bin/sh\n[ "$1" = release ] && [ "$2" = list ] || exit 64\n'
                               'cat -- "$FAKE_RELEASES"\n')
            fake_gh.chmod(0o755)
            output = temporary / 'github-output'
            environment = {**os.environ, 'PATH': f'{temporary}:{os.environ["PATH"]}',
                           'GITHUB_EVENT_NAME': event, 'EVENT_TAG': event_tag,
                           'INPUT_TAG': input_tag, 'GITHUB_OUTPUT': str(output),
                           'FAKE_RELEASES': str(fixture), 'GITHUB_REF': ref}
            result = subprocess.run(['bash', '-c', self._release_selection_script()], env=environment,
                                    text=True, capture_output=True, timeout=5)
            return result, output.read_text() if output.exists() else ''

    def test_repository_refresh_workflow_selects_only_latest_stable_release(self):
        workflow = (ROOT / '.github/workflows/publish-package-repository.yml').read_text()
        self.assertRegex(workflow, r'(?m)^  push:\n    branches: \[main\]$')
        self.assertRegex(workflow, r'(?m)^  release:\n    types: \[published\]$')
        self.assertRegex(workflow, r'(?m)^  schedule:\n')
        self.assertNotIn('pull_request:', workflow)
        self.assertNotIn('pull_request_target:', workflow)
        self.assertIn('environment: release-signing', workflow)
        self.assertRegex(workflow, r'uses: actions/checkout@[^\n]+\n        with:\n          ref: main')
        self.assertIn('case "$GITHUB_EVENT_NAME" in', workflow)
        self.assertIn('push)', workflow)
        self.assertIn('[[ $GITHUB_REF == refs/heads/main ]]', workflow)

        releases = [
            {'tagName': 'v2.4.5', 'isDraft': False, 'isPrerelease': False},
            {'tagName': 'v2.4.6', 'isDraft': False, 'isPrerelease': False},
            {'tagName': 'v2.5.0-rc.1', 'isDraft': False, 'isPrerelease': True},
            {'tagName': 'v9.9.9', 'isDraft': False, 'isPrerelease': True},
            {'tagName': 'v2.4.7', 'isDraft': True, 'isPrerelease': False},
        ]
        for event, event_tag, input_tag in (('release', 'v2.4.6', ''), ('push', '', ''),
                                            ('schedule', '', ''), ('workflow_dispatch', '', 'v2.4.6')):
            with self.subTest(event=event):
                result, output = self._select_release_tag(event, releases, event_tag, input_tag)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output, 'tag=v2.4.6\n')
        for event, event_tag, input_tag, fixture in (
                ('release', 'v2.4.5', '', releases),
                ('release', 'v2.4.7', '', releases),
                ('workflow_dispatch', '', 'v2.4.5', releases),
                ('pull_request', '', '', releases),
                ('schedule', '', '', [])):
            with self.subTest(event=event, tag=event_tag or input_tag):
                result, _ = self._select_release_tag(event, fixture, event_tag, input_tag)
                self.assertNotEqual(result.returncode, 0)
        result, _ = self._select_release_tag('push', releases, ref='refs/heads/untrusted')
        self.assertNotEqual(result.returncode, 0)


class PublicDeploymentTests(unittest.TestCase):
    """Execute the actual post-deploy shell with offline HTTP fixtures."""

    def run_check(self, mode='current', manifest=None, page_url=None):
        workflow = (ROOT / '.github/workflows/publish-package-repository.yml').read_text()
        match = re.search(r'(?ms)^      - name: Check public installation page and downloads\n'
                          r'.*?^        run: \|\n((?:^          [^\n]*\n)+)', workflow)
        self.assertIsNotNone(match)
        manifest = manifest if manifest is not None else HomepageTests().release_manifest()
        with tempfile.TemporaryDirectory(prefix='vectorwarp-public-check-') as directory:
            directory = Path(directory)
            staged = directory / 'repository'
            (staged / 'keys').mkdir(parents=True)
            for name in ('index.html', 'install.sh', 'keys/vectorwarp.asc'):
                (staged / name).write_text(f'fixture {name}\n')
            (staged / 'repository-manifest.json').write_text(json.dumps(manifest))
            fake_curl = directory / 'curl'
            fake_curl.write_text(f'#!{sys.executable}\n' + textwrap.dedent('''\
                import os, sys
                from pathlib import Path
                from urllib.parse import urlsplit
                args = sys.argv[1:]
                url = next(value for value in args if value.startswith('https://'))
                with open('requests.log', 'a') as log:
                    log.write(url + '\\n')
                mode = os.environ['FIXTURE_MODE']
                if '--head' in args:
                    sys.exit(22 if mode == 'missing-package' else 0)
                if mode == 'unavailable':
                    sys.exit(22)
                name = urlsplit(url).path.split('/blah2-VectorWarp/', 1)[1]
                state = Path('retry-state')
                stale = mode == 'stale' or (mode == 'retry' and not state.exists())
                state.touch()
                content = b'stale' if stale else (Path('repository') / name).read_bytes()
                Path(args[args.index('--output') + 1]).write_bytes(content)
                '''))
            fake_curl.chmod(0o755)
            fake_sleep = directory / 'sleep'
            fake_sleep.write_text('#!/bin/sh\nexit 0\n')
            fake_sleep.chmod(0o755)
            env = {**os.environ, 'PATH': f'{directory}:{os.environ["PATH"]}',
                   'RUNNER_TEMP': str(directory), 'FIXTURE_MODE': mode, 'GITHUB_RUN_ID': '123',
                   'PAGE_URL': page_url or 'https://mickeyslaven.github.io/blah2-VectorWarp/'}
            result = subprocess.run(['bash', '-c', textwrap.dedent(match.group(1))],
                                    cwd=directory, env=env, text=True, capture_output=True, timeout=10)
            log = directory / 'requests.log'
            return result, log.read_text().splitlines() if log.exists() else []

    def test_current_and_eventually_current_files_check_every_package(self):
        manifest = HomepageTests().release_manifest()
        expected = {f'https://github.com/mickeyslaven/blah2-VectorWarp/releases/download/v1.2.3/{p["filename"]}'
                    for p in manifest['packages']}
        for mode, count in (('current', 14), ('retry', 15)):
            with self.subTest(mode=mode):
                result, requests = self.run_check(mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(requests), count)
                self.assertEqual({url for url in requests if '/releases/download/' in url}, expected)

    def test_stale_unavailable_or_missing_downloads_fail(self):
        for mode in ('stale', 'unavailable', 'missing-package'):
            with self.subTest(mode=mode):
                result, requests = self.run_check(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertLessEqual(len(requests), 6)
        result, requests = self.run_check(page_url='https://untrusted.example/')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(requests, [])

    def test_invalid_package_lists_and_versions_fail_before_download_checks(self):
        valid = HomepageTests().release_manifest()
        invalid_manifests = []
        for packages in ([], None, 'not an array', valid['packages'][:-1],
                         [valid['packages'][0]] * 10):
            invalid_manifests.append({**valid, 'packages': packages})
        for filename in ('../secret.deb', 'bad\\nfile.rpm', '', None, 123):
            fixture = deepcopy(valid)
            fixture['packages'][0]['filename'] = filename
            invalid_manifests.append(fixture)
        for version in ('latest', None, '../v1.2.3'):
            invalid_manifests.append({**valid, 'version': version})
        for manifest in invalid_manifests:
            with self.subTest(manifest=manifest):
                result, requests = self.run_check(manifest=manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any('/releases/download/' in url for url in requests))


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-manifest-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "vectorwarp_1.2.3-1_ubuntu24.04_amd64.deb"
        self.package.write_bytes(b"manifest validation fixture, not a DEB")
        self.entry = dict(name="vectorwarp", version="1.2.3", release="1", format="deb",
                          distro="ubuntu", distro_version="24.04", codename="noble", arch="amd64",
                          filename=self.package.name, size=self.package.stat().st_size,
                          sha256=repository.sha256(self.package), backend="all", gpu="auto",
                          compiled_receivers=["Usrp", "HackRF", "Kraken"], local_build_receivers=["RspDuo"],
                          node_version="24.21.0")
        self.manifest = self.root / "manifest.json"

    def load(self, entries=None, document=None):
        self.manifest.write_text(json.dumps(document if document is not None else
                                           {"schema": 1, "version": "1.2.3",
                                            "source_commit": "a" * 40,
                                            "packages": entries or [self.entry]}))
        return repository.load_manifest(self.manifest, self.root)[1]

    def test_valid_entry(self):
        self.assertEqual(self.load(), [self.entry])

    def test_receiver_specific_or_incomplete_packages_are_rejected(self):
        for changes in ({"backend": "kraken"},
                        {"backend": "open-test", "test_only": True,
                         "compiled_receivers": ["Kraken", "Usrp", "HackRF"]},
                        {"test_only": True}, {"test_only": "false"},
                        {"test_only": None}, {"test_only": 0},
                        {"local_build_receivers": []},
                        {"compiled_receivers": ["Kraken", "RspDuo", "Usrp", "Usrp"]}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.load([{**self.entry, **changes}])

    def test_resolute_target_is_valid(self):
        package = self.root / "vectorwarp_1.2.3-1_ubuntu26.04_amd64.deb"
        package.write_bytes(b"resolute manifest fixture, not a DEB")
        entry = {**self.entry, "distro_version": "26.04", "codename": "resolute",
                 "filename": package.name, "size": package.stat().st_size,
                 "sha256": repository.sha256(package)}
        self.assertEqual(self.load([entry]), [entry])

    def test_debian_trixie_target_is_valid(self):
        package = self.root / "vectorwarp_1.2.3-1_debian13_amd64.deb"
        package.write_bytes(b"trixie manifest fixture, not a DEB")
        entry = {**self.entry, "distro": "debian", "distro_version": "13", "codename": "trixie",
                 "filename": package.name, "size": package.stat().st_size,
                 "sha256": repository.sha256(package)}
        self.assertEqual(self.load([entry]), [entry])

    def test_bad_document_shapes(self):
        for document in ([], None, 1, "text", {"schema": 2, "packages": []},
                         {"schema": 1, "packages": []}, {"schema": 1, "packages": [None]}):
            with self.subTest(document=document):
                self.manifest.write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    repository.load_manifest(self.manifest, self.root)

    def test_manifest_requires_source_and_single_version(self):
        for document in ({"schema": 1, "version": "1.2.3", "packages": [self.entry]},
                         {"schema": 1, "version": "1.2.3", "source_commit": "short",
                          "packages": [self.entry]},
                         {"schema": 1, "version": "9.9.9", "source_commit": "a" * 40,
                          "packages": [self.entry]}):
            with self.subTest(document=document), self.assertRaises(ValueError):
                self.load(document=document)

    def test_release_matrix_requires_every_target_once(self):
        entries = []
        for format, distro, version, arch in repository.RELEASE_TARGETS:
            entries.append({"format": format, "distro": distro, "distro_version": version,
                            "arch": arch})
        repository.verify_release_matrix(entries)
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.verify_release_matrix(entries[:-1])
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.verify_release_matrix(entries + [entries[0]])

    def test_path_traversal_and_duplicate_names(self):
        for name in ("../a.deb", "/tmp/a.deb", "a/b.deb", "-a.deb", "a\nb.deb"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.load([{**self.entry, "filename": name}])
        with self.assertRaisesRegex(ValueError, "unique"):
            self.load([self.entry, self.entry])

    def test_wrong_metadata_types(self):
        for field in ("format", "distro", "distro_version", "arch", "version", "name", "release"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.load([{**self.entry, field: []}])

    def test_target_and_version_mismatch(self):
        for update in ({"arch": "riscv64"}, {"codename": "jammy"}, {"version": "1.2.3-rc1"},
                       {"name": "another-package"}, {"release": "2"}, {"distro_version": "26.04"}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_filename_and_build_profile_are_immutable(self):
        for update in ({"filename": "renamed.deb"}, {"backend": "kraken"}, {"gpu": "off"},
                       {"node_version": "25.0.0"}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_checksum_and_size_tampering(self):
        for update in ({"sha256": "0" * 64}, {"size": 1}, {"size": True}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.load([{**self.entry, **update}])

    def test_symlink_package(self):
        other = self.root / "other"
        self.package.rename(other)
        self.package.symlink_to(other)
        with self.assertRaisesRegex(ValueError, "regular package"):
            self.load()

    def test_size_budget(self):
        with patch.object(repository, "MAX_BYTES", 1), self.assertRaisesRegex(ValueError, "budget"):
            self.load()

    def test_rpm_digest_only_is_not_signature(self):
        reports = ((0, b"SHA256 digest: OK\n", False),
                   (0, b"Header V4 RSA/SHA256 Signature, key ID 1234: OK\n", True),
                   (0, b"Header V4 RSA/SHA256 Signature, key ID 1234: NOKEY\n", False),
                   (1, b"Header V4 RSA/SHA256 Signature, key ID 1234: OK\n", False))
        for code, report, expected in reports:
            result = subprocess.CompletedProcess([], code, stdout=report, stderr=b"")
            with patch.object(repository.subprocess, "run", return_value=result):
                self.assertEqual(repository.verified_rpm(Path("fixture"), Path("db")), expected)


@unittest.skipUnless(os.environ.get("VECTORWARP_REPOSITORY_INTEGRATION") == "1",
                     "set VECTORWARP_REPOSITORY_INTEGRATION=1 in an isolated runner")
class SignedRepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("gpg", "gpgv", "gpgconf", "dpkg-deb", "apt-ftparchive", "rpmbuild",
                     "rpm", "rpmsign", "createrepo_c", "apt-get"):
            if not shutil.which(tool):
                raise RuntimeError(f"Integration tests require {tool}; no silent skips")
        cls.temporary = tempfile.TemporaryDirectory(prefix="vectorwarp-signed-repository-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        # Model the production custody plan: a certification-only primary stays
        # offline while CI imports an export containing only its signing subkey.
        offline_home = cls.root / "offline-gnupg"
        offline_home.mkdir(mode=0o700)
        cls.addClassCleanup(lambda: subprocess.run(
            ["gpgconf", "--homedir", str(offline_home), "--kill", "gpg-agent"],
            capture_output=True, timeout=10))
        with patch.dict(os.environ, {"GNUPGHOME": str(offline_home)}):
            repository.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                            "--quick-generate-key", "VectorWarp ephemeral test <test@example.invalid>",
                            "rsa2048", "cert", "1d"])
            cls.public = cls.root / "public.asc"
            cls.public.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export"]))
            cls.fingerprint = repository.public_fingerprint(cls.public)
            repository.run(["gpg", "--batch", "--pinentry-mode", "loopback", "--passphrase", "",
                            "--quick-add-key", cls.fingerprint, "rsa2048", "sign", "1d"])
            cls.public.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export",
                                                   cls.fingerprint]))
            subkeys = repository.run(["gpg", "--batch", "--armor", "--export-secret-subkeys",
                                      cls.fingerprint])
        cls.keyhome = cls.root / "ci-gnupg"
        cls.keyhome.mkdir(mode=0o700)
        cls.environment = patch.dict(os.environ, {"GNUPGHOME": str(cls.keyhome)})
        cls.environment.start()
        cls.addClassCleanup(cls.environment.stop)
        cls.addClassCleanup(lambda: subprocess.run(
            ["gpgconf", "--homedir", str(cls.keyhome), "--kill", "gpg-agent"],
            capture_output=True, timeout=10))
        subprocess.run(["gpg", "--batch", "--import"], input=subkeys, check=True,
                       capture_output=True, timeout=180)
        records = repository.run(["gpg", "--batch", "--with-colons", "--list-secret-keys"]).decode().splitlines()
        primary_records = [record.split(":") for record in records if record.startswith("sec:")]
        subkey_records = [record.split(":") for record in records if record.startswith("ssb:")]
        if (len(primary_records) != 1 or primary_records[0][14] != "#" or
                len(subkey_records) != 1 or subkey_records[0][11] != "s" or
                subkey_records[0][14] != "+"):
            raise RuntimeError("CI fixture must contain only a usable signing subkey, not the primary secret")
        cls.packages = cls.root / "packages"
        cls.packages.mkdir()
        cls.entries = []
        for codename, distro, version, arch in (("jammy", "ubuntu", "22.04", "amd64"),
                                                ("noble", "ubuntu", "24.04", "arm64"),
                                                ("resolute", "ubuntu", "26.04", "amd64"),
                                                ("trixie", "debian", "13", "amd64")):
            stage = cls.root / f"deb-{codename}"
            (stage / "DEBIAN").mkdir(parents=True)
            (stage / "DEBIAN/control").write_text(
                f"Package: vectorwarp\nVersion: 1.2.3-1\nArchitecture: {arch}\n"
                "Maintainer: Test <test@example.invalid>\nDescription: Repository test only\n")
            (stage / "usr/share/vectorwarp").mkdir(parents=True)
            (stage / "usr/share/vectorwarp/test.txt").write_text("Not an application package.\n")
            distro_label = "debian13" if distro == "debian" else f"ubuntu{version}"
            package = cls.packages / f"vectorwarp_1.2.3-1_{distro_label}_{arch}.deb"
            repository.run(["dpkg-deb", "--build", "--root-owner-group", str(stage), str(package)])
            cls.entries.append(cls.entry(package, "deb", distro, version, arch, codename))
        top = cls.root / "rpmbuild"
        top.mkdir()
        rpm_arch = {"x86_64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}.get(platform.machine())
        if not rpm_arch:
            raise RuntimeError("RPM integration fixtures require a supported release architecture")
        spec = cls.root / "fixture.spec"
        spec.write_text("Name: vectorwarp\nVersion: 1.2.3\nRelease: 1.fc44\nSummary: Repository test only\n"
                        f"License: MIT\nBuildArch: {rpm_arch}\n%description\nNot an application package.\n"
                        "%install\nmkdir -p %{buildroot}/usr/share/vectorwarp\n"
                        "printf 'test only\\n' > %{buildroot}/usr/share/vectorwarp/test.txt\n"
                        "%files\n/usr/share/vectorwarp/test.txt\n")
        repository.run(["rpmbuild", "--define", f"_topdir {top}", "-bb", str(spec)])
        rpm = next((top / "RPMS").rglob("*.rpm"))
        package = cls.packages / rpm.name
        shutil.copyfile(rpm, package)
        cls.entries.append(cls.entry(package, "rpm", "fedora", "44", rpm_arch))
        cls.manifest = cls.root / "manifest.json"
        cls.manifest.write_text(json.dumps({"schema": 1, "version": "1.2.3",
                                           "source_commit": "a" * 40,
                                           "packages": cls.entries}))

    @staticmethod
    def entry(file, format, distro, version, arch, codename=None):
        result = dict(name="vectorwarp", version="1.2.3", release="1.fc44" if format == "rpm" else "1",
                      format=format, distro=distro, distro_version=version, arch=arch,
                      filename=file.name, sha256=repository.sha256(file), size=file.stat().st_size,
                      backend="all", gpu="auto", node_version="24.21.0",
                      compiled_receivers=["Usrp", "HackRF", "Kraken"], local_build_receivers=["RspDuo"])
        if codename:
            result["codename"] = codename
        return result

    def args(self, output):
        return argparse.Namespace(packages=str(self.packages), manifest=str(self.manifest),
                                  output=str(self.root / output), public_key=str(self.public),
                                  fingerprint=self.fingerprint, signing_key=self.fingerprint,
                                  installer_template=str(ROOT / "script/install-release.sh"),
                                  expected_version="1.2.3", expected_source_commit="a" * 40,
                                  require_release_matrix=False)

    def test_signed_site_and_refresh_preserve_packages(self):
        args = self.args("signed-site")
        original_hashes = {file.name: repository.sha256(file) for file in self.packages.iterdir()}
        document = repository.build(args)
        site = Path(args.output)
        self.assertEqual(len(document["packages"]), 5)
        for codename in ("jammy", "noble", "resolute", "trixie"):
            self.assertIn("Valid-Until:", (site / f"apt/dists/{codename}/Release").read_text())
            self.assertTrue((site / f"apt/dists/{codename}/InRelease").is_file())
        self.assertIn(self.fingerprint, (site / "install.sh").read_text())
        self.assertNotIn("@SIGNING_FINGERPRINT@", (site / "install.sh").read_text())
        # Let the real package manager verify/download indexes, but confine all
        # state/cache writes to this disposable fixture. Never install packages.
        apt_state = self.root / "apt-test"
        (apt_state / "lists/partial").mkdir(parents=True)
        (apt_state / "cache/archives/partial").mkdir(parents=True)
        (apt_state / "status").touch()
        sources = apt_state / "sources.list"
        sources.write_text("".join(
            f"deb [arch=amd64 signed-by={site}/keys/vectorwarp.gpg] file://{site}/apt {codename} main\n"
            for codename in ("jammy", "noble", "resolute", "trixie")))
        repository.run(["apt-get", "-o", f"Dir::Etc::sourcelist={sources}",
                        "-o", "Dir::Etc::sourceparts=-", "-o", f"Dir::State={apt_state}",
                        "-o", f"Dir::State::status={apt_state}/status", "-o", f"Dir::Cache={apt_state}/cache",
                        "-o", "APT::Sandbox::User=root", "-o", "APT::Architecture=amd64",
                        "-o", "Acquire::Languages=none", "-o", "APT::Update::Error-Mode=any", "update"])
        for file in self.packages.iterdir():
            self.assertEqual(repository.sha256(file), original_hashes[file.name], "Input package mutated")
        refresh_packages = self.root / "refresh-packages"
        refresh_packages.mkdir()
        for entry in document["packages"]:
            published = site / entry["repository_path"]
            self.assertEqual(repository.sha256(published), entry["sha256"])
            shutil.copyfile(published, refresh_packages / entry["filename"])
        refresh = self.args("refreshed-site")
        refresh.packages = str(refresh_packages)
        refresh.manifest = str(site / "repository-manifest.json")
        actual_run = repository.run

        def forbid_resigning(command, **kwargs):
            self.assertNotEqual(command[0], "rpmsign", "Refresh must not re-sign an immutable RPM")
            return actual_run(command, **kwargs)

        with patch.object(repository, "run", side_effect=forbid_resigning):
            refreshed = repository.build(refresh)
        self.assertEqual(document["packages"], refreshed["packages"])
        with self.assertRaisesRegex(ValueError, "must not exist"):
            repository.build(args)

    def test_bad_public_key_and_secret_file_fail_closed(self):
        args = self.args("wrong-key-site")
        args.fingerprint = args.signing_key = "0" * 40
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            repository.build(args)
        secret = self.root / "secret-test-key.asc"
        secret.write_bytes(repository.run(["gpg", "--batch", "--armor", "--export-secret-keys",
                                          self.fingerprint]))
        args = self.args("secret-site")
        args.public_key = str(secret)
        with self.assertRaisesRegex(ValueError, "public|secret"):
            repository.build(args)
        self.assertFalse(Path(args.output).exists())

    def test_release_selection_and_full_matrix_fail_closed(self):
        args = self.args("wrong-version-site")
        args.expected_version = "9.9.9"
        with self.assertRaisesRegex(ValueError, "version"):
            repository.build(args)
        args = self.args("wrong-source-site")
        args.expected_source_commit = "b" * 40
        with self.assertRaisesRegex(ValueError, "source commit"):
            repository.build(args)
        args = self.args("partial-matrix-site")
        args.require_release_matrix = True
        with self.assertRaisesRegex(ValueError, "matrix"):
            repository.build(args)

    def test_real_metadata_mismatch_and_tampering(self):
        entry = deepcopy(self.entries[0])
        entry["version"] = "9.9.9"
        with self.assertRaisesRegex(ValueError, "metadata"):
            repository.verify_metadata(entry, self.packages / entry["filename"])
        args = self.args("tampered-site")
        bad = self.root / "tampered-manifest.json"
        bad.write_text(json.dumps({"schema": 1, "version": "1.2.3", "source_commit": "a" * 40,
                                   "packages": [{**self.entries[0], "sha256": "0" * 64}]}))
        args.manifest = str(bad)
        with self.assertRaisesRegex(ValueError, "checksum"):
            repository.build(args)
        self.assertFalse(Path(args.output).exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
