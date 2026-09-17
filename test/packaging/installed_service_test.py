#!/usr/bin/env python3
"""Exercise installed units in a disposable container, never receiver hardware.

The positive path uses generated IQ replay. It proves installed service wiring,
not SDRplay SDK acceptance, live RF capture, or detection accuracy.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

BASE = 'http://127.0.0.1:3000'
CONFIG = Path('/etc/vectorwarp/config.yml')
REPLAY = Path('/var/lib/vectorwarp/package-test-replay.blah2iq')
MISSING_REPLAY = '/var/lib/vectorwarp/package-test-missing.blah2iq'
checks = []


def run(*args, timeout=30):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    assert result.returncode == 0, (args, result.returncode, result.stdout)
    return result.stdout.strip()


def request(route, method='GET', body=None, headers=None):
    supplied = {'Referer': BASE + '/display/configuration/'}
    if body is not None:
        supplied.update({'Content-Type': 'application/json', 'Origin': BASE})
    supplied.update(headers or {})
    req = urllib.request.Request(BASE + route,
        data=None if body is None else json.dumps(body).encode(),
        headers=supplied, method=method)
    try:
        response = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        raw = response.read()
        return response.status, json.loads(raw) if raw.startswith((b'{', b'[')) else raw.decode(), response.headers


def wait_ready():
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        try:
            if request('/api/system/status')[0] == 200:
                return
        except (OSError, TimeoutError):
            pass
        time.sleep(.25)
    raise AssertionError('Installed API did not become ready')


def wait_status(predicate, description, timeout=45):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            code, last, _ = request('/api/system/status')
            if code == 200 and predicate(last):
                return last
        except (OSError, TimeoutError):
            pass
        time.sleep(.25)
    raise AssertionError(f'{description}: {last}')


def intent(revision, pending=False):
    return {'If-Match': revision, 'X-VectorWarp-Intent': 'config-write-v1',
            'X-VectorWarp-Receiver-Sync': 'save-pending-v1' if pending else 'synchronize-v1'}


def verify_usb_permissions():
    """Exercise installed service accounts against vendor-style DAC permissions.

    This synthetic file test cannot verify USB hotplug, device cgroups, or RF.
    """
    rule = Path('/usr/lib/udev/rules.d/72-vectorwarp-hackrf.rules')
    distro = platform.freedesktop_os_release()['ID']
    if distro in ('debian', 'ubuntu'):
        assert not rule.exists(), 'Do not replace the vendor plugdev group on Debian/Ubuntu'
        assert 'plugdev' in run('id', '-nG', 'vectorwarp').split()
        assert 'plugdev' not in run('id', '-nG', 'vectorwarp-api').split()
        device_group = 'plugdev'
    elif distro == 'fedora':
        assert rule.is_file(), 'Fedora needs the packaged HackRF service-access rule'
        lines = [line.strip() for line in rule.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        assert len(lines) == 1, lines
        for selector in ('ACTION=="add"', 'SUBSYSTEM=="usb"', 'ENV{DEVTYPE}=="usb_device"',
                         'ATTR{idVendor}=="1d50"', 'ATTR{idProduct}=="6089"', 'GROUP="vectorwarp"'):
            assert selector in lines[0], (selector, lines)
        assert not any(token in lines[0] for token in ('MODE=', 'TAG=', 'RUN', 'setfacl')), lines
        device_group = 'vectorwarp'
    else:
        raise AssertionError(f'Add installed USB permission coverage for {distro}')
    group_id = int(run('getent', 'group', device_group).split(':')[2])

    def can_open(account, path, expected):
        # systemd initializes supplementary groups, just as for installed units.
        # Test actual open(O_RDWR), not root's access or a mocked SDK response.
        probe = ('import os,sys\n'
                 'try:\n fd=os.open(sys.argv[1],os.O_RDWR|os.O_CLOEXEC); os.close(fd)\n'
                 'except PermissionError:\n sys.exit(77)\n')
        result = subprocess.run([
            'systemd-run', '--quiet', '--wait', '--collect', '--pipe',
            '-p', f'User={account}', '-p', f'Group={run("id", "-gn", account)}',
            '-p', 'NoNewPrivileges=yes', '-p', 'CapabilityBoundingSet=',
            'python3', '-c', probe, str(path)], text=True, capture_output=True, timeout=20)
        assert result.returncode == expected, (account, expected, result.returncode,
                                               result.stdout, result.stderr)

    with tempfile.TemporaryDirectory(prefix='vectorwarp-usb-permissions-', dir='/run') as folder:
        os.chmod(folder, 0o755)
        fixture = Path(folder) / 'synthetic-device'
        fixture.touch()
        os.chown(fixture, 0, 0)
        os.chmod(fixture, 0o660)
        can_open('vectorwarp', fixture, 77)
        can_open('vectorwarp-api', fixture, 77)
        os.chown(fixture, 0, group_id)
        can_open('vectorwarp', fixture, 0)
        can_open('vectorwarp-api', fixture, 77)
        can_open('nobody', fixture, 77)
        assert stat.S_IMODE(fixture.stat().st_mode) == 0o660
    checks.append(f'installed processor can open mode0660 {device_group} fixture; API and unrelated users cannot (synthetic DAC test, no USB hardware)')


def restart_replay(replay_file):
    previous = wait_status(lambda item: item['restart']['state'] not in ('running', 'scheduled'),
                           'Previous restart did not settle')
    code, current, headers = request('/api/config')
    assert code == 200 and current['capture']['replay']['state'] is True
    changed = copy.deepcopy(current)
    changed['capture']['replay']['file'] = replay_file
    started = int(time.time() * 1000)
    code, result, _ = request('/api/config?restart=true', 'PUT', changed,
                              intent(headers['ETag']))
    assert code == 200 and result['restarting'] and result['receiverSync']['acceptance']['mode'] == 'replay', (code, result)
    assert result['receiverSync']['acceptance']['physicalReceiverVerified'] is False
    status = wait_status(lambda item: item['serverId'] != previous['serverId'] and
                         item['loadedRevision'] == result['revision'] and
                         item['restart']['state'] in ('command-complete', 'failed'),
                         'Installed API did not rotate and load the requested replay configuration', 90)
    assert status['restart']['state'] == 'command-complete', status
    assert run('systemctl', 'show', '-p', 'Result', '--value', 'vectorwarp-restart.service') == 'success'
    return result, status, started


def verify_replay():
    assert REPLAY.is_file() and os.geteuid() == 0 and Path('/run/.containerenv').exists()
    assert Path('/proc/1/comm').read_text().strip() == 'systemd'
    assert not Path('/usr/local/lib/libsdrplay_api.so').exists()
    status = wait_status(lambda item: item['processorFresh'] and item['radar'] == 'receiving' and
                         item['processor']['input'] == 'replay' and
                         item['processor']['state'] == 'playing' and item['lastFrameAt'],
                         'Browser-applied replay did not produce fresh installed-service frames')
    api_pid = int(run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-api.service'))
    assert api_pid > 0 and 'NoNewPrivs:\t1' in Path(f'/proc/{api_pid}/status').read_text()
    assert run('systemctl', 'show', '-p', 'User', '--value', 'vectorwarp-api.service') == 'vectorwarp-api'
    assert run('systemctl', 'show', '-p', 'Result', '--value', 'vectorwarp-restart.service') == 'success'
    pid = int(run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-processor.service'))
    assert pid > 0 and run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'active'
    assert run('systemctl', 'show', '-p', 'User', '--value', 'vectorwarp-processor.service') == 'vectorwarp'
    identity = Path(f'/proc/{pid}/status').read_text()
    assert 'NoNewPrivs:\t1' in identity
    assert int(next(line.split()[1] for line in identity.splitlines() if line.startswith('Uid:'))) == int(
        run('id', '-u', 'vectorwarp'))
    checks.append('browser Apply started the installed non-root NNP processor and produced synthetic replay frames')

    assert not Path(MISSING_REPLAY).exists(), 'Missing-file negative control must truly be absent'
    _, failed_start, _ = restart_replay(MISSING_REPLAY)
    failed = wait_status(lambda item: item['processorFresh'] and
                         item['processor']['state'] == 'error' and
                         'Cannot open replay file' in item['processor']['error'],
                         'Missing replay file error was not visible through installed API', 30)
    assert failed['serverId'] == failed_start['serverId']
    assert failed['radar'] == 'no-data' and failed['lastFrameAt'] is None, failed
    time.sleep(2)
    stale = request('/api/system/status')[1]
    assert stale['serverId'] == failed_start['serverId'] and stale['lastFrameAt'] is None and stale['radar'] == 'no-data', stale
    checks.append('missing replay file reports a processor error without reusing pre-restart frames')

    _, restored, started = restart_replay(str(REPLAY))
    recovered = wait_status(lambda item: item['serverId'] == restored['serverId'] and
                            item['processorFresh'] and item['processor']['state'] == 'playing' and
                            item['radar'] == 'receiving' and item['lastFrameAt'] and
                            item['lastFrameAt'] > started,
                            'Restored replay did not produce new frames', 45)
    assert recovered['processor']['file'] == str(REPLAY)
    checks.append('corrected replay file recovers through a second installed restart with new frames')
    print(json.dumps({'passed': checks, 'hardwareTested': False, 'sdkInstalled': False,
                      'input': 'synthetic BLAH2IQ replay', 'recoveredRevision': recovered['loadedRevision']}, indent=2))


def main():
    assert os.geteuid() == 0 and Path('/run/.containerenv').exists(), \
        'Run only in the disposable package-test container; never on a user host'
    assert Path('/proc/1/comm').read_text().strip() == 'systemd'
    assert not Path('/usr/local/lib/libsdrplay_api.so').exists(), 'Fresh test must not contain a vendor SDK'
    assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
    verify_usb_permissions()
    run('systemctl', 'start', 'vectorwarp-api.service', 'vectorwarp-receiver.socket')
    wait_ready()
    pid = run('systemctl', 'show', '-p', 'MainPID', '--value', 'vectorwarp-api.service')
    identity = Path(f'/proc/{pid}/status').read_text()
    assert 'NoNewPrivs:\t1' in identity, 'API must explicitly retain NoNewPrivileges under systemd'
    assert run('systemctl', 'show', '-p', 'User', '--value', 'vectorwarp-api.service') == 'vectorwarp-api'
    for key, expected in [('PrivateDevices', 'yes'), ('ProtectSystem', 'strict')]:
        assert run('systemctl', 'show', '-p', key, '--value', 'vectorwarp-api.service') == expected
    checks.append('installed API starts with its real service identity and sandbox')

    _, capabilities, _ = request('/api/config/capabilities')
    assert capabilities['editable'] and capabilities['restartAvailable'] and not capabilities['setupRequired']
    assert capabilities['configPath'] == str(CONFIG)
    assert request('/display/configuration/')[0] == 200
    assert request('/js/config_ui.js')[0] == 200
    checks.append('installed config is editable and web assets are served')

    adapters = json.loads(run('/opt/vectorwarp/current/bin/blah2', '--receiver-status'))
    assert adapters['hardwareProbed'] is False
    for item in adapters['receivers']:
        if item['receiver'] in ('Kraken', 'Usrp', 'HackRF'):
            assert item['compiled'] and item['moduleLoadable'], item
    checks.append('packaged open receiver adapters load without the SDRplay SDK')

    before = hashlib.sha256(CONFIG.read_bytes()).hexdigest()
    _, config, headers = request('/api/config')
    revision = headers['ETag']
    assert config['capture']['device']['type'] == 'RspDuo', 'Test expects neutral packaged defaults'
    write_intent = intent(revision)
    invalid = json.loads(json.dumps(config))
    invalid['capture']['fs'] = 0
    code, result, _ = request('/api/config?restart=true', 'PUT', invalid, write_intent)
    assert code == 422 and result['errors'], (code, result)
    code, result, _ = request('/api/config?restart=true', 'PUT', config, write_intent)
    assert code == 422 and result['code'] == 'SDRPLAY_LOCAL_BUILD_REQUIRED', (code, result)
    code, result, _ = request('/api/sdrplay-build')
    assert code == 200 and result['ok'] is False and 'SDRplay' in result['reason'], (code, result)
    code, result, _ = request('/api/sdrplay-build', 'POST', {},
                              {'X-VectorWarp-Intent': 'sdrplay-local-build-v1'})
    assert code == 409 and result['errors'], (code, result)
    assert hashlib.sha256(CONFIG.read_bytes()).hexdigest() == before
    checks.append('invalid settings and missing SDK fail visibly without altering config or downloading software')

    for origin in ('http://127.0.0.1:3001', 'https://127.0.0.1:3000', 'https://invalid.example'):
        code, _, _ = request('/api/config?restart=true', 'PUT', config, write_intent | {'Origin': origin})
        assert code == 403, (origin, code)
    for forbidden in ({'Origin': ''}, {'Host': 'untrusted.invalid:3000'}, {'X-VectorWarp-Intent': ''}):
        code, _, _ = request('/api/config?restart=true', 'PUT', config, write_intent | forbidden)
        assert code == 403, (forbidden, code)
    assert request('/capture/toggle')[0] in (404, 405)
    assert hashlib.sha256(CONFIG.read_bytes()).hexdigest() == before
    checks.append('foreign origins and legacy GET cannot change settings or recording')

    # Genuine API-identity -> Unix broker -> installed restart service. There is
    # no radio/SDK and the neutral config is not applied for live processing:
    # the expected result is a bounded, visible startup failure.
    run('systemd-run', '--quiet', '--wait', '--collect', '--pipe',
        '--unit=vectorwarp-package-test-request', '-p', 'User=vectorwarp-api',
        '-p', 'Group=vectorwarp-api', '-p', 'NoNewPrivileges=yes',
        '/opt/vectorwarp/libexec/vectorwarp-receiver-helper', 'request-restart')
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        state = run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-restart.service')
        if state == 'failed':
            break
        time.sleep(.25)
    assert state == 'failed', 'Missing SDK restart must finish with a failure, not hang'
    wait_ready()
    _, status, _ = request('/api/system/status')
    assert status['restart']['state'] == 'failed', status
    assert any(word in status['restart']['message'].lower() for word in ('pending', 'sdrplay', 'sdk')), status
    assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
    checks.append('NNP broker reaches real restart unit; actionable startup failure returns to live API')

    # The package must retain every receiver as a valid pending/replay choice,
    # even though no SDK or hardware is installed. Pending is disk-only: it must
    # not start the processor or claim any receiver was configured.
    profiles = {item['type']: item for item in capabilities['deviceProfiles']}
    assert set(profiles) == {'Kraken', 'RspDuo', 'Usrp', 'HackRF'}
    for receiver in ('Kraken', 'RspDuo', 'Usrp', 'HackRF'):
        code, current, headers = request('/api/config')
        assert code == 200
        profile = profiles[receiver]
        changed = copy.deepcopy(current)
        changed['capture']['fs'] = profile['sampleRate']
        changed['capture']['device'] = copy.deepcopy(profile['device'])
        if receiver == 'HackRF':
            # Packaged defaults are instructional placeholders, not legal
            # serial numbers. Use explicit valid-but-absent replay identities.
            changed['capture']['device']['serial'] = ['0001', '0002']
        changed['capture']['replay']['state'] = True
        for key in ('performance', 'reference_synthesis'):
            if key in profile.get('process', {}):
                changed['process'][key] = copy.deepcopy(profile['process'][key])
            else:
                changed['process'].pop(key, None)
        changed['process']['performance']['acceleration'] = current['process']['performance']['acceleration']
        code, result, _ = request('/api/config?mode=pending&restart=false', 'PUT', changed,
                                  intent(headers['ETag'], pending=True))
        assert code == 200 and result['receiverSync']['status'] == 'saved-pending', (receiver, code, result)
        assert result['receiverSync']['hardwareVerified'] is False and result['restarting'] is False
        assert request('/api/config')[1]['capture']['device']['type'] == receiver
        assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
    code, _, headers = request('/api/config')
    assert code == 200
    code, result, _ = request('/api/config?mode=pending&restart=false', 'PUT', config,
                              intent(headers['ETag'], pending=True))
    assert code == 200 and result['config']['capture']['device']['type'] == 'RspDuo'
    checks.append('all four receiver profiles persist as pending/replay without opening hardware')

    # Reuse the same deterministic BLAH2IQ writer as the direct replay suite.
    # This is synthetic input, not a claim about live RF or vendor SDK behavior.
    sys.path.insert(0, '/tmp')
    from processor_replay_test import write_blah2iq
    write_blah2iq(REPLAY, 2, config['capture']['fs'], config['capture']['fc'], frames=4)
    REPLAY.chmod(0o644)
    run('runuser', '-u', 'vectorwarp', '--', 'test', '-r', str(REPLAY))
    checks.append('completed, deterministic two-channel synthetic IQ is readable by the installed processor')
    print(json.dumps({'passed': checks, 'hardwareTested': False, 'sdkInstalled': False}, indent=2))


if __name__ == '__main__':
    try:
        if len(sys.argv) == 2 and sys.argv[1] == '--verify-replay':
            verify_replay()
        elif len(sys.argv) == 1:
            main()
        else:
            raise SystemExit('usage: installed_service_test.py [--verify-replay]')
    except Exception:
        subprocess.run(['journalctl', '--no-pager', '-n', '100', '-u', 'vectorwarp-api.service',
                        '-u', 'vectorwarp-receiver.service', '-u', 'vectorwarp-restart.service'])
        raise
