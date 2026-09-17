#!/usr/bin/env python3
"""Exercise installed units, not a directly launched API. Disposable containers only.

No SDR or vendor SDK is supplied. The restart test must report missing SDRplay
software, never pretend that a successful web response means RF capture worked.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.request

BASE = 'http://127.0.0.1:3000'
CONFIG = Path('/etc/vectorwarp/config.yml')
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


def main():
    assert os.geteuid() == 0 and Path('/run/.containerenv').exists(), \
        'Run only in the disposable package-test container; never on a user host'
    assert Path('/proc/1/comm').read_text().strip() == 'systemd'
    assert not Path('/usr/local/lib/libsdrplay_api.so').exists(), 'Fresh test must not contain a vendor SDK'
    assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
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
    intent = {'If-Match': revision, 'X-VectorWarp-Receiver-Sync': 'synchronize-v1',
              'X-VectorWarp-Intent': 'config-write-v1'}
    invalid = json.loads(json.dumps(config))
    invalid['capture']['fs'] = 0
    code, result, _ = request('/api/config?restart=true', 'PUT', invalid, intent)
    assert code == 422 and result['errors'], (code, result)
    code, result, _ = request('/api/config?restart=true', 'PUT', config, intent)
    assert code == 422 and result['code'] == 'SDRPLAY_LOCAL_BUILD_REQUIRED', (code, result)
    code, result, _ = request('/api/sdrplay-build')
    assert code == 200 and result['ok'] is False and 'SDRplay' in result['reason'], (code, result)
    code, result, _ = request('/api/sdrplay-build', 'POST', {},
                              {'X-VectorWarp-Intent': 'sdrplay-local-build-v1'})
    assert code == 409 and result['errors'], (code, result)
    assert hashlib.sha256(CONFIG.read_bytes()).hexdigest() == before
    checks.append('invalid settings and missing SDK fail visibly without altering config or downloading software')

    for origin in ('http://127.0.0.1:3001', 'https://127.0.0.1:3000', 'https://invalid.example'):
        code, _, _ = request('/api/config?restart=true', 'PUT', config, intent | {'Origin': origin})
        assert code == 403, (origin, code)
    for forbidden in ({'Origin': ''}, {'Host': 'untrusted.invalid:3000'}, {'X-VectorWarp-Intent': ''}):
        code, _, _ = request('/api/config?restart=true', 'PUT', config, intent | forbidden)
        assert code == 403, (forbidden, code)
    assert request('/capture/toggle')[0] in (404, 405)
    assert hashlib.sha256(CONFIG.read_bytes()).hexdigest() == before
    checks.append('foreign origins and legacy GET cannot change settings or recording')

    # Genuine API-identity -> Unix broker -> installed restart service. There is
    # no radio/SDK: the expected result is a bounded, visible startup failure.
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
    assert run('systemctl', 'show', '-p', 'ActiveState', '--value', 'vectorwarp-processor.service') == 'inactive'
    checks.append('NNP broker request reaches real restart unit; missing SDK error returns to live API')
    print(json.dumps({'passed': checks, 'hardwareTested': False, 'sdkInstalled': False}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        subprocess.run(['journalctl', '--no-pager', '-n', '100', '-u', 'vectorwarp-api.service',
                        '-u', 'vectorwarp-receiver.service', '-u', 'vectorwarp-restart.service'])
        raise
