#!/usr/bin/env python3
"""Run the actual CI isolation shell with guarded, temporary filesystem fixtures."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


def workflow_script(arch='arm64'):
    source = (ROOT / '.github/workflows/macos-standalone.yml').read_text()
    block = re.search(r'      - name: Verify replay, configuration, and standalone isolation\n'
                      r'        run: \|\n((?:          .*\n)+)', source)
    if not block:
        raise AssertionError('workflow isolation block missing')
    return textwrap.dedent(block.group(1)).replace('${{ matrix.arch }}', arch)


class StandaloneIsolationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vectorwarp isolation ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / 'brew prefix'
        self.prefix.mkdir()
        (self.prefix / 'Cellar').mkdir()
        (self.prefix / 'Cellar/library').write_text('original library')
        (self.prefix / '.dot').write_text('dot contents')
        (self.prefix / 'ordinary').write_text('original contents')
        (self.prefix / 'broken').symlink_to('missing')
        self.runtime = self.root / 'build/standalone runtime'
        self.tools = self.root / 'tools'
        self.tools.mkdir()
        self.log = self.root / 'calls'
        self.hidden = self.prefix / '.vectorwarp-ci-hidden'
        # No real sudo or mutation outside this disposable prefix is permitted.
        helper = self.root / 'guarded_sudo.py'
        helper.write_text(textwrap.dedent('''\
            import os
            from pathlib import Path
            import shutil
            import sys
            prefix = Path(os.environ['FIXTURE_PREFIX'])
            action, *arguments = sys.argv[1:]
            assert action in ('mv', 'mkdir', 'rmdir'), action
            assert len(arguments) == (2 if action == 'mv' else 1), arguments
            paths = [Path(os.path.abspath(p)) for p in arguments]
            assert all(p == prefix or prefix in p.parents for p in paths), paths
            assert paths[0] != prefix, 'the prefix/mount itself must never move'
            if action == 'mkdir':
                paths[0].mkdir()
            elif action == 'rmdir':
                paths[0].rmdir()
            else:
                source, target = paths
                if (os.environ['FIXTURE_MODE'] == 'partial' and
                        source == prefix / 'ordinary'):
                    raise SystemExit(29)
                destination = target / source.name if target.is_dir() else target
                assert not destination.exists() and not destination.is_symlink(), destination
                shutil.move(str(source), str(destination))
            '''))
        sudo = ('#!/bin/bash\nexec /usr/bin/env -u PYTHONHOME -u PYTHONPATH '
                '"$FIXTURE_PYTHON" "$FIXTURE_HELPER" "$@"\n')
        for directory in (self.tools, self.runtime / 'bin'):
            self.executable(directory / 'sudo', sudo)
        self.executable(self.tools / 'brew', '#!/bin/bash\nprintf "%s\\n" "$FIXTURE_PREFIX"\n')
        self.executable(self.tools / 'uname', '#!/bin/bash\nif [ "$1" = -s ]; then echo Darwin; else echo "$FIXTURE_ARCH"; fi\n')
        self.executable(self.runtime / 'bin/node', '#!/bin/bash\nexit 0\n')
        runtime_stub = textwrap.dedent('''\
            #!/bin/bash
            set -eu
            test -d "$FIXTURE_PREFIX"
            if [ "${3:-}" = script/package-macos-standalone.py ]; then
              test ! -e "$FIXTURE_PREFIX/.vectorwarp-ci-hidden"
              test "$(cat "$FIXTURE_PREFIX/ordinary")" = 'original contents'
              echo audit >> "$FIXTURE_LOG"
              exit 0
            fi
            for name in Cellar .dot ordinary broken; do
              test ! -e "$FIXTURE_PREFIX/$name" && test ! -L "$FIXTURE_PREFIX/$name"
              test -e "$FIXTURE_PREFIX/.vectorwarp-ci-hidden/$name" || test -L "$FIXTURE_PREFIX/.vectorwarp-ci-hidden/$name"
            done
            echo runtime >> "$FIXTURE_LOG"
            if [ "${MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS:-}" = 0 ]; then
              case "$*" in
                *--observe-only*|'-s -P - '*) exit 0 ;;
                *) exit 91 ;;
              esac
            fi
            if [ "${3:-}" = test/macos/standalone_driver_probe.py ] && [ "$FIXTURE_MODE" = driver-failure ]; then
              exit 23
            fi
            if [ "${3:-}" = test/recording/processor_replay_test.py ]; then
              case "$FIXTURE_MODE" in
                failure) exit 23 ;;
                signal) kill -TERM "$PPID" ;;
                collision) echo replacement > "$FIXTURE_PREFIX/ordinary"; exit 31 ;;
              esac
            fi
            ''')
        self.executable(self.runtime / 'python/bin/python3', runtime_stub)
        self.executable(self.runtime / 'bin/blah2-gpu-worker', runtime_stub)
        self.env = {
            'PATH': f'{self.tools}:/usr/bin:/bin:/usr/sbin:/sbin',
            'GITHUB_ACTIONS': 'true', 'RUNNER_OS': 'macOS',
            'GITHUB_WORKSPACE': str(self.root),
            'FIXTURE_PREFIX': str(self.prefix), 'FIXTURE_MODE': 'success',
            'FIXTURE_LOG': str(self.log), 'FIXTURE_PYTHON': sys.executable,
            'FIXTURE_HELPER': str(helper),
            'FIXTURE_ARCH': 'arm64',
        }

    def executable(self, path, source):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
        path.chmod(0o755)

    def run_fixture(self, mode='success', arch='arm64', **environment):
        return subprocess.run(['/bin/bash', '-c', workflow_script(arch)], cwd=self.root,
                              env={**self.env, 'FIXTURE_MODE': mode, 'FIXTURE_ARCH': arch, **environment},
                              capture_output=True, text=True, timeout=30)

    def assert_restored(self):
        self.assertTrue(self.prefix.is_dir())
        self.assertEqual((self.prefix / 'ordinary').read_text(), 'original contents')
        self.assertEqual((self.prefix / '.dot').read_text(), 'dot contents')
        self.assertEqual((self.prefix / 'Cellar/library').read_text(), 'original library')
        self.assertEqual(os.readlink(self.prefix / 'broken'), 'missing')
        self.assertFalse(self.hidden.exists())

    def test_success_hides_contents_and_restores_before_audit(self):
        result = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_restored()
        self.assertEqual(self.log.read_text().splitlines(), ['runtime'] * 5 + ['audit'])

    def test_runtime_failure_retains_status_and_restores_contents(self):
        result = self.run_fixture('failure')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assert_restored()
        self.assertEqual(self.log.read_text().splitlines(), ['runtime'])

    def test_partial_hide_failure_restores_already_moved_entries(self):
        result = self.run_fixture('partial')
        self.assertEqual(result.returncode, 29, result.stderr)
        self.assert_restored()
        self.assertFalse(self.log.exists())

    def test_driver_failure_still_audits_and_fails_after_restoring(self):
        result = self.run_fixture('driver-failure')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assert_restored()
        self.assertEqual(self.log.read_text().splitlines(), ['runtime'] * 5 + ['audit'])

    def test_intel_compatibility_observation_cannot_clear_default_driver_failure(self):
        result = self.run_fixture('driver-failure', arch='x86_64')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assert_restored()
        self.assertEqual(self.log.read_text().splitlines(), ['runtime'] * 7 + ['audit'])

    def test_term_restores_contents_and_preserves_signal_status(self):
        result = self.run_fixture('signal')
        self.assertEqual(result.returncode, 143, result.stderr)
        self.assert_restored()

    def test_existing_hidden_directory_is_untouched(self):
        self.hidden.mkdir()
        (self.hidden / 'sentinel').write_text('preserve')
        result = self.run_fixture()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.hidden / 'sentinel').read_text(), 'preserve')
        self.assertFalse(self.log.exists())
        (self.hidden / 'sentinel').unlink()
        self.hidden.rmdir()
        self.assert_restored()

    def test_non_ci_context_is_rejected_before_mutation(self):
        result = self.run_fixture(GITHUB_ACTIONS='false')
        self.assertNotEqual(result.returncode, 0)
        self.assert_restored()
        self.assertFalse(self.log.exists())

    def test_restore_collision_preserves_both_copies(self):
        result = self.run_fixture('collision')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.prefix / 'ordinary').read_text(), 'replacement\n')
        self.assertEqual((self.hidden / 'ordinary').read_text(), 'original contents')


if __name__ == '__main__':
    unittest.main()
