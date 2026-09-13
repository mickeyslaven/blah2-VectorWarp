#!/usr/bin/env python3
"""Hermetic execution checks for the rendered privileged restart script."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SdrplayRestartTest(unittest.TestCase):
    def render(self, prefix, sysconf, commands):
        rendered = (ROOT / 'script/vectorwarp-restart').read_text()
        rendered = rendered.replace('@PREFIX@', str(prefix)).replace('@SYSCONFDIR@', str(sysconf))
        for original, replacement in commands.items():
            rendered = rendered.replace(original, str(replacement))
        # Every absolute privileged executable in the template is a fixture
        # command before it can be executed by this test.
        self.assertNotIn('/usr/bin/', rendered)
        self.assertNotIn('@PREFIX@', rendered)
        self.assertNotIn('@SYSCONFDIR@', rendered)
        return rendered

    def fixture(self, mode='success'):
        temporary = tempfile.TemporaryDirectory(prefix='vectorwarp-sdrplay-restart-')
        root = Path(temporary.name)
        log = root / 'command.log'
        marker = root / 'mode'
        marker.write_text(mode)
        fake = root / 'fake'
        fake.mkdir()
        prefix = root / 'prefix'
        (prefix / 'libexec').mkdir(parents=True)
        (prefix / 'current/api/node_modules').mkdir(parents=True)
        sysconf = root / 'etc'
        sysconf.mkdir()
        (sysconf / 'config.yml').write_text('receiver: RspDuo\n')
        service = prefix / 'libexec/vectorwarp-sdrplay-service'
        if mode != 'prepare-missing':
            service.write_text('#!/bin/sh\nexit 0\n')
            service.chmod(0o755)

        def executable(name, body):
            path = fake / name
            path.write_text('#!/bin/sh\nset -eu\n' + body)
            path.chmod(0o755)
            return path

        # The fixture logger is an absolute, test-owned path: env -i drops
        # arbitrary inherited environment, so no logger configuration is read
        # from the test process.
        env = executable('env', f'''
printf '%s\\n' "env:$*" >> {log!s}
[ "$1" = -i ] || exit 97
shift
while [ "$#" -gt 0 ]; do
  case "$1" in *=*) shift ;; *) break ;; esac
done
exec "$@"
''')
        systemctl = executable('systemctl', f'''
printf '%s\\n' "systemctl:$*" >> {log!s}
[ "$(cat {marker!s})" = processor-start-fail ] && [ "$1" = start ] && exit 17
exit 0
''')
        node = executable('node', f'''
mode=$(cat {marker!s})
case "$2" in
  --finish) printf '%s\\n' "node:prepare-finish:$3" >> {log!s}; exit 0 ;;
  --begin) printf '%s\\n' 'node:prepare-begin' >> {log!s}; exit 0 ;;
esac
case "$1" in
  *vectorwarp-wait-api.js)
    printf '%s\\n' 'node:wait-api' >> {log!s}
    [ "$mode" = wait-fail ] && exit 13
    exit 0 ;;
  *vectorwarp-prepare-sdrplay.js)
    printf '%s\\n' 'node:prepare-sdr' >> {log!s}
    [ -x "$4" ] || exit 14
    [ "$mode" = prepare-fail ] && exit 15
    exit 0 ;;
esac
exit 98
''')
        script = root / 'vectorwarp-restart'
        script.write_text(self.render(prefix, sysconf, {
            '/usr/bin/env': env,
            '/usr/bin/node': node,
            '/usr/bin/systemctl': systemctl,
        }))
        script.chmod(0o755)
        return temporary, script, log

    @staticmethod
    def run_script(script):
        return subprocess.run(['/bin/sh', str(script)], text=True, capture_output=True,
                              env={'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}, check=False)

    @staticmethod
    def entries(log):
        return log.read_text().splitlines() if log.exists() else []

    def test_success_orders_sdrplay_preparation_before_processor_start(self):
        temporary, script, log = self.fixture()
        with temporary:
            result = self.run_script(script)
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self.entries(log)
            self.assertGreater(len(entries), 0)
            self.assertEqual(entries, [
                'env:-i PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C LC_ALL=C ' +
                str(script.parent / 'fake/node') + ' ' + str(script.parent / 'prefix/libexec/vectorwarp-prepare-sdrplay.js') +
                ' --begin ' + str(script.parent / 'etc/config.yml'),
                'node:prepare-begin',
                'systemctl:stop vectorwarp-processor.service',
                'systemctl:restart vectorwarp-api.service',
                'node:wait-api',
                'env:-i PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C LC_ALL=C ' +
                str(script.parent / 'fake/node') + ' ' + str(script.parent / 'prefix/libexec/vectorwarp-prepare-sdrplay.js') +
                ' ' + str(script.parent / 'etc/config.yml') + ' ' + str(script.parent / 'prefix/current/api') +
                ' ' + str(script.parent / 'prefix/libexec/vectorwarp-sdrplay-service'),
                'node:prepare-sdr',
                'systemctl:start vectorwarp-processor.service',
                'env:-i PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C LC_ALL=C ' +
                str(script.parent / 'fake/node') + ' ' + str(script.parent / 'prefix/libexec/vectorwarp-prepare-sdrplay.js') +
                ' --finish 0',
                'node:prepare-finish:0',
            ])

    def test_missing_or_failed_sdrplay_preparation_keeps_processor_stopped(self):
        for mode in ('prepare-missing', 'prepare-fail'):
            with self.subTest(mode=mode):
                temporary, script, log = self.fixture(mode)
                with temporary:
                    result = self.run_script(script)
                    self.assertNotEqual(result.returncode, 0)
                    entries = self.entries(log)
                    self.assertGreater(len(entries), 0)
                    self.assertIn('systemctl:restart vectorwarp-api.service', entries)
                    self.assertIn('node:prepare-sdr', entries)
                    self.assertNotIn('systemctl:start vectorwarp-processor.service', entries)
                    self.assertIn('node:prepare-finish:' + ('14' if mode == 'prepare-missing' else '15'), entries)

    def test_api_wait_failure_skips_sdrplay_and_processor_start(self):
        temporary, script, log = self.fixture('wait-fail')
        with temporary:
            result = self.run_script(script)
            self.assertNotEqual(result.returncode, 0)
            entries = self.entries(log)
            self.assertGreater(len(entries), 0)
            self.assertIn('systemctl:restart vectorwarp-api.service', entries)
            self.assertIn('node:wait-api', entries)
            self.assertNotIn('node:prepare-sdr', entries)
            self.assertNotIn('systemctl:start vectorwarp-processor.service', entries)
            self.assertIn('node:prepare-finish:13', entries)

    def test_processor_start_failure_is_finalized_without_false_completion(self):
        temporary, script, log = self.fixture('processor-start-fail')
        with temporary:
            result = self.run_script(script)
            self.assertEqual(result.returncode, 17, result.stderr)
            entries = self.entries(log)
            self.assertGreater(len(entries), 0)
            self.assertIn('node:prepare-sdr', entries)
            self.assertIn('systemctl:start vectorwarp-processor.service', entries)
            self.assertIn('node:prepare-finish:17', entries)
            self.assertNotIn('node:prepare-finish:0', entries)


if __name__ == '__main__':
    unittest.main()
