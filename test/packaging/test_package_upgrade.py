#!/usr/bin/env python3
"""Offline maintainer-hook regression; replaces runtime paths and systemctl."""
import os
import fcntl
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UpgradeHookTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='vectorwarp-upgrade-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.tools = self.base / 'tools'
        self.tools.mkdir()
        self.systemd = self.base / 'systemd'
        self.systemd.mkdir()
        self.state = self.base / 'upgrade.state'
        self.socket_unit = self.base / 'vectorwarp-receiver.socket'
        shutil.copy2(ROOT / 'contrib/systemd/vectorwarp-receiver.socket', self.socket_unit)
        self.log = self.base / 'calls.log'
        systemctl = self.tools / 'systemctl'
        systemctl.write_text('''#!/bin/sh
printf '%s\\n' "$*" >>"$UPGRADE_TEST_LOG"
case "$1" in
  show)
    case "$2:$4" in
      --property=ActiveState:vectorwarp-api.service) echo "${API_STATE:-inactive}" ;;
      --property=ActiveState:vectorwarp-processor.service) echo "${PROCESSOR_STATE:-inactive}" ;;
      --property=ActiveState:vectorwarp-receiver.socket)
        if [ -f "$SOCKET_STATE_FILE" ]; then cat "$SOCKET_STATE_FILE"; else echo "${SOCKET_STATE:-inactive}"; fi ;;
      --property=ActiveState:vectorwarp-receiver.service)
        if [ "${BROKER_RACE:-0}" = 1 ] && [ -f "$SOCKET_STATE_FILE" ]; then echo active;
        else echo "${BROKER_STATE:-inactive}"; fi ;;
      --property=ActiveState:vectorwarp-restart.service) echo "${RESTART_STATE:-inactive}" ;;
      --property=ActiveState:vectorwarp-sdrplay-build.service) echo "${BUILD_STATE:-inactive}" ;;
      --property=Job:*) echo "${JOB_STATE:-}" ;;
      --property=MainPID:*) echo "${MAIN_PID:-0}" ;;
      --property=FragmentPath:vectorwarp-receiver.socket) echo "$SOCKET_UNIT" ;;
      --property=DropInPaths:vectorwarp-receiver.socket) echo ;;
      --property=NeedDaemonReload:vectorwarp-receiver.socket) echo no ;;
      *) exit 9 ;;
    esac ;;
  start)
    [ "$2" != "${FAIL_START:-none}" ] ;;
  stop|daemon-reload) exit 0 ;;
  --job-mode=ignore-dependencies)
    [ "$*" = '--job-mode=ignore-dependencies --no-block stop vectorwarp-receiver.socket' ] || exit 9
    echo inactive > "$SOCKET_STATE_FILE" ;;
  *) exit 9 ;;
esac
''')
        systemctl.chmod(0o755)
        node = self.tools / 'node'
        node.write_text('#!/bin/sh\nprintf "node %s\\n" "$*" >>"$UPGRADE_TEST_LOG"\n'
                        '[ "${FAIL_NODE:-0}" != 1 ]\n')
        node.chmod(0o755)
        self.env = os.environ | {'PATH': f'{self.tools}:{os.environ["PATH"]}',
                                 'UPGRADE_TEST_LOG': str(self.log),
                                 'SOCKET_UNIT': str(self.socket_unit),
                                 'SOCKET_STATE_FILE': str(self.base / 'socket.state')}

    def script(self, relative, prefix='/opt/vectorwarp'):
        source = (ROOT / relative).read_text()
        source = source.replace('@PREFIX@', prefix)
        source = source.replace('/run/systemd/system', str(self.systemd))
        source = source.replace('/run/vectorwarp-manual-stop.', str(self.base / 'manual-stop.'))
        source = source.replace('/run/vectorwarp-package-upgrade.state', str(self.state))
        source = source.replace('/run/vectorwarp-package-upgrade.lock', str(self.base / 'upgrade.lock'))
        source = source.replace('/usr/lib/systemd/system/vectorwarp-receiver.socket', str(self.socket_unit))
        source = source.replace('/opt/vectorwarp/runtime/node/bin/node', str(self.tools / 'node'))
        script = self.base / Path(relative).name
        script.write_text(source)
        script.chmod(0o755)
        return script

    def call(self, script, *args, **state):
        env = self.env | state
        return subprocess.run(['sh', str(script), *args], env=env, text=True,
                              capture_output=True, check=False, timeout=5)

    def postinst_upgrade_branch(self):
        """Exercise the production postinst upgrade branch without first-install setup."""
        source = (ROOT / 'packaging/deb/postinst').read_text()
        source = source.split('if [ "$1" = configure ] && [ -d /run/systemd/system ]; then', 1)[1]
        source = source.split('# First configure only:', 1)[0]
        source = 'set -e\nif [ "$1" = configure ] && [ -d /run/systemd/system ]; then' + source
        source = source.replace('/run/systemd/system', str(self.systemd))
        source = source.replace('/run/vectorwarp-package-upgrade.state', str(self.state))
        source = source.replace('/opt/vectorwarp/libexec/vectorwarp-activate-upgrade',
                                str(self.tools / 'activate-upgrade'))
        script = self.base / 'postinst-upgrade-branch'
        script.write_text(source)
        script.chmod(0o755)
        return script

    def test_upgrade_quiesces_own_active_units_and_preserves_stopped_processor(self):
        pre = self.script('packaging/deb/preinst')
        result = self.call(pre, 'upgrade', '0.1.6', API_STATE='active', SOCKET_STATE='active')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.state.read_text(), 'api=1\nprocessor=0\nsocket=1\nbroker=0\n')
        calls = self.log.read_text().splitlines()
        self.assertIn('stop vectorwarp-api.service', calls)
        self.assertIn('stop vectorwarp-receiver.service vectorwarp-receiver.socket vectorwarp-processor.service', calls)
        self.assertFalse(any('sdrplay_apiService' in call or 'krakensdr' in call for call in calls))

    def test_preflight_rejects_active_or_queued_build_before_stopping_anything(self):
        pre = self.script('packaging/deb/preinst')
        for state in ({'BUILD_STATE': 'active'}, {'JOB_STATE': '123'}):
            with self.subTest(state=state):
                self.log.unlink(missing_ok=True)
                result = self.call(pre, '2', API_STATE='active', **state)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.state.exists())
                self.assertFalse(any(line.startswith('stop ') for line in self.log.read_text().splitlines()))

    def test_activate_only_snapshot_and_wait_for_api_before_processor(self):
        post = self.script('packaging/activate-upgrade')
        self.state.write_text('api=1\nprocessor=1\nsocket=1\nbroker=0\n')
        self.state.chmod(0o600)
        result = self.call(post)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.state.exists())
        calls = self.log.read_text().splitlines()
        self.assertLess(calls.index('start vectorwarp-api.service'),
                        next(i for i, line in enumerate(calls) if line.startswith('node ')))
        self.assertLess(next(i for i, line in enumerate(calls) if line.startswith('node ')),
                        calls.index('start vectorwarp-processor.service'))

    def test_failed_api_readiness_never_starts_processor_or_claims_activation(self):
        post = self.script('packaging/activate-upgrade')
        self.state.write_text('api=1\nprocessor=1\nsocket=1\nbroker=0\n')
        self.state.chmod(0o600)
        result = self.call(post, FAIL_NODE='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('activation failed', result.stderr)
        self.assertNotIn('start vectorwarp-processor.service', self.log.read_text())
        self.assertFalse(self.state.exists())

    def test_first_install_never_quiesces(self):
        pre = self.script('packaging/deb/preinst')
        self.assertEqual(self.call(pre, 'install').returncode, 0)
        self.assertFalse(self.state.exists())
        self.assertFalse(self.log.exists())

    def test_manual_stop_stops_every_own_unit_and_removes_only_its_snapshot(self):
        result = self.call(self.script('packaging/deb/preinst'), 'manual-stop', SOCKET_STATE='active')
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.log.read_text().splitlines()
        self.assertIn('stop vectorwarp-api.service', calls)
        self.assertIn('stop vectorwarp-receiver.service vectorwarp-receiver.socket vectorwarp-processor.service', calls)
        self.assertIn('stop vectorwarp-restart.service vectorwarp-api.service', calls)
        for unit in ('api', 'receiver', 'processor', 'restart'):
            self.assertIn(f'show --property=MainPID --value vectorwarp-{unit}.service', calls)
        self.assertIn('VectorWarp stopped: radar, web interface', result.stdout)
        self.assertFalse(self.state.exists())
        self.assertEqual(list(self.base.glob('manual-stop.*')), [])
        self.assertFalse(any('krakensdr' in call or 'sdrplay_apiService' in call for call in calls))

    def test_manual_stop_preserves_pending_package_marker_without_service_actions(self):
        self.state.write_text('pending package activation\n')
        result = self.call(self.script('packaging/deb/preinst'), 'manual-stop')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('package update has not completed', result.stderr)
        self.assertEqual(self.state.read_text(), 'pending package activation\n')
        self.assertFalse(self.log.exists())
        self.assertEqual(list(self.base.glob('manual-stop.*')), [])

    def test_manual_stop_busy_refusal_changes_no_service(self):
        script = self.script('packaging/deb/preinst')
        for state in ({'BUILD_STATE': 'active'}, {'RESTART_STATE': 'active'}, {'JOB_STATE': '123'}):
            with self.subTest(state=state):
                self.log.unlink(missing_ok=True)
                result = self.call(script, 'manual-stop', **state)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(line.startswith(('stop ', 'start '))
                                     for line in self.log.read_text().splitlines()))
                self.assertFalse(self.state.exists())
                self.assertEqual(list(self.base.glob('manual-stop.*')), [])

    def test_manual_stop_rendered_custom_prefix_refuses_before_any_service_call(self):
        result = self.call(self.script('packaging/deb/preinst', prefix='/srv/vectorwarp'), 'manual-stop')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('custom installation prefix', result.stderr)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.state.exists())

    def test_manual_stop_requires_systemd_and_zero_remaining_processes(self):
        script = self.script('packaging/deb/preinst')
        result = self.call(script, 'manual-stop', MAIN_PID='123')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('still has a running process', result.stderr)
        self.assertNotIn('VectorWarp stopped:', result.stdout)
        self.assertFalse(self.state.exists())
        self.assertEqual(list(self.base.glob('manual-stop.*')), [])
        self.systemd.rmdir()
        result = self.call(script, 'manual-stop')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires a running systemd', result.stderr)

    def test_upgrade_refuses_an_inflight_launcher_action_before_quiesce(self):
        pre = self.script('packaging/deb/preinst')
        lock = self.base / 'upgrade.lock'
        lock.touch(mode=0o600)
        with lock.open('r+') as held:
            fcntl.flock(held, fcntl.LOCK_SH | fcntl.LOCK_NB)
            result = self.call(pre, 'upgrade', '0.1.6', API_STATE='active')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('another VectorWarp service action', result.stderr)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.state.exists())

    def test_lock_fifo_is_rejected_without_blocking(self):
        pre = self.script('packaging/deb/preinst')
        os.mkfifo(self.base / 'upgrade.lock', 0o600)
        result = self.call(pre, 'upgrade', '0.1.6')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not a regular file', result.stderr)
        self.assertFalse(self.log.exists())

    def test_raced_busy_broker_is_not_killed_or_falsely_restored(self):
        pre = self.script('packaging/deb/preinst')
        # The installed-systemd test exercises the actual freezer. Here force
        # its failure after a queued broker activation to test shell unwind.
        pre.write_text(pre.read_text().replace('/usr/bin/python3 -I -', 'false'))
        result = self.call(pre, 'upgrade', '0.1.6', API_STATE='active',
                           SOCKET_STATE='active', BROKER_RACE='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('A receiver action may still be active', result.stderr)
        self.assertNotIn('start vectorwarp-receiver.socket', self.log.read_text())
        self.assertNotIn('start vectorwarp-api.service', self.log.read_text())
        self.assertNotIn('stop vectorwarp-receiver.service', self.log.read_text())

    def test_stopped_api_and_processor_remain_stopped_after_upgrade(self):
        post = self.script('packaging/activate-upgrade')
        self.state.write_text('api=0\nprocessor=0\nsocket=1\nbroker=0\n')
        self.state.chmod(0o600)
        result = self.call(post)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('start vectorwarp-receiver.socket', self.log.read_text())
        self.assertNotIn('start vectorwarp-api.service', self.log.read_text())
        self.assertNotIn('start vectorwarp-processor.service', self.log.read_text())

    def test_postinst_consumed_state_never_replays_activation(self):
        helper = self.tools / 'activate-upgrade'
        helper.write_text('#!/bin/sh\nprintf "activate-upgrade\\n" >>"$UPGRADE_TEST_LOG"\nexit 9\n')
        helper.chmod(0o755)
        result = self.call(self.postinst_upgrade_branch(), 'configure', '0.1.6')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no pending upgrade state', result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), ['daemon-reload'])

    def test_postinst_pending_state_calls_helper_once_and_propagates_failure(self):
        helper = self.tools / 'activate-upgrade'
        helper.write_text('#!/bin/sh\nprintf "activate-upgrade\\n" >>"$UPGRADE_TEST_LOG"\n'
                          'rm "$UPGRADE_TEST_STATE"\nexit 9\n')
        helper.chmod(0o755)
        self.state.write_text('api=1\nprocessor=0\nsocket=1\nbroker=0\n')
        self.env['UPGRADE_TEST_STATE'] = str(self.state)
        script = self.postinst_upgrade_branch()
        result = self.call(script, 'configure', '0.1.6')
        self.assertEqual(result.returncode, 9, result.stderr)
        self.assertEqual(self.log.read_text().splitlines(), ['daemon-reload', 'activate-upgrade'])
        self.assertFalse(self.state.exists())
        result = self.call(script, 'configure', '0.1.6')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no pending upgrade state', result.stderr)
        self.assertEqual(self.log.read_text().splitlines(),
                         ['daemon-reload', 'activate-upgrade', 'daemon-reload'])


if __name__ == '__main__':
    unittest.main()
