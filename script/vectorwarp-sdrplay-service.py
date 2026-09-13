#!/usr/bin/python3
"""Start an installed, standard SDRplay API service; never obtain vendor software.

Only root installation/restart helpers invoke this fixed-name operation. Browser
discovery remains read-only; custom services retain the reviewed broker route.
"""
import importlib.machinery
import importlib.util
import json
import os
import pathlib
import re
import sys

_sibling = pathlib.Path(__file__).with_name('vectorwarp-receiver-helper')
if not _sibling.exists():
    _sibling = _sibling.with_name('vectorwarp-receiver-helper.py')
_loader = importlib.machinery.SourceFileLoader('vectorwarp_receiver_helper', str(_sibling))
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
broker = importlib.util.module_from_spec(_spec)
_loader.exec_module(broker)

DOWNLOAD = 'https://sdrplay.com/hardware-api/'
MANIFEST = '@PREFIX@/current/.vectorwarp-build'
LOCAL_BUILD_HELPER = '@PREFIX@/libexec/vectorwarp-build-sdrplay'
UNITS = broker.SERVICE_UNITS['RspDuo']
EMPTY = ('DropInPaths', 'ExecStartPre', 'ExecStartPost', 'ExecCondition',
         'ExecStop', 'ExecStopPost', 'Environment', 'EnvironmentFiles',
         'PassEnvironment', 'RootDirectory', 'RootImage', 'BindPaths',
         'BindReadOnlyPaths', 'LoadCredential', 'LoadCredentialEncrypted',
         'SetCredential', 'SetCredentialEncrypted')
PROPERTIES = ('Id', 'LoadState', 'ActiveState', 'FragmentPath', 'NeedDaemonReload',
              'ExecStart', 'WorkingDirectory', 'User', 'Group', 'Type') + EMPTY


def require(ok, message):
    broker.require(ok, 'SDRPLAY_START_BLOCKED', message)


def discover(run=broker.command, trust=broker.trusted_path, qualify_start=True):
    found = {}
    for alias in UNITS:
        result = run(['/usr/bin/systemctl', 'show', alias, '--no-pager',
                      '--property=' + ','.join(PROPERTIES)], timeout=2)
        require(not result.get('timedOut'), 'Checking the SDRplay service timed out.')
        properties = {}
        for line in result.get('output', '').splitlines():
            key, separator, value = line.partition('=')
            if separator:
                require(key not in properties, 'SDRplay returned ambiguous service metadata.')
                properties[key] = value
        if properties.get('LoadState') == 'not-found':
            continue
        require(result['exitCode'] == 0 and properties.get('LoadState') == 'loaded',
                'Could not verify the installed SDRplay service. Check it locally.')
        require(all(key in properties for key in PROPERTIES if key not in EMPTY),
                'SDRplay service metadata is incomplete; automatic startup is unavailable.')
        unit = properties['Id']
        require(unit in UNITS, 'The SDRplay service resolves to an unsupported unit.')
        require(unit not in found or found[unit] == properties,
                'SDRplay service metadata changed during discovery.')
        found[unit] = properties
    require(found, 'SDRplay API service was not found. Install the Hardware API from SDRplay yourself.')
    if len(found) > 1 and not qualify_start:
        active = {unit: p for unit, p in found.items() if p['ActiveState'] == 'active'}
        if len(active) == 1 and all(p['ActiveState'] in ('active', 'inactive', 'failed') for p in found.values()):
            found = active  # Reuse a locally selected active service; never start another.
    require(len(found) == 1, 'Multiple SDRplay services were found. Have an administrator select and start one locally.')
    unit, p = next(iter(found.items()))
    require(p['ActiveState'] in ('active', 'inactive', 'failed'),
            'SDRplay is changing state. Wait and try again.')
    if not qualify_start and p['ActiveState'] == 'active':
        # Observation is not permission to launch this definition. Administrators
        # may have started a custom service through the reviewed/manual route.
        return unit, p
    require(p['NeedDaemonReload'] == 'no' and not any(p.get(key, '') for key in EMPTY),
            'SDRplay has overrides, extra commands or environment settings; review/start it locally.')
    require(p['WorkingDirectory'] in ('', '/') and p['User'] in ('', 'root') and
            p['Group'] in ('', 'root') and p['Type'] in ('simple', 'exec', 'forking'),
            'This SDRplay service layout requires local administrator review.')
    # systemctl renders one structured ExecStart record, including its argv.
    paths = re.findall(r'path=([^ ;]+) ;', p['ExecStart'])
    require(len(paths) == 1 and re.fullmatch(r'/(?:opt|usr)/(?:[-A-Za-z0-9_./]+/)?sdrplay_apiService', paths[0]),
            'SDRplay must start its installed daemon directly, without a wrapper.')
    executable = paths[0]
    require(re.search(r'argv\[\]=' + re.escape(executable) + r' ; ignore_errors=no ;', p['ExecStart']) and
            p['ExecStart'].count('{') == 1 and p['ExecStart'].count('}') == 1,
            'SDRplay has unsupported startup arguments or commands; start it locally.')
    trust(p['FragmentPath'])
    trust(executable)
    with open(executable, 'rb') as handle:
        require(handle.read(4) == b'\x7fELF', 'SDRplay daemon is not a native executable; review it locally.')
    require(os.access(executable, os.X_OK), 'The installed SDRplay daemon is not executable.')
    return unit, p


def start_service(run=broker.command, trust=broker.trusted_path, exists=os.path.exists):
    require(exists('/run/systemd/system'), 'No running systemd manager; start SDRplay on the host after installation.')
    chroot = run(['/usr/bin/systemd-detect-virt', '--chroot'], timeout=2)
    require(chroot['exitCode'] == 1 and not chroot.get('timedOut'),
            'SDRplay startup is disabled in a chroot or unverified host environment.')
    unit, before = discover(run, trust, qualify_start=False)
    if before['ActiveState'] == 'active':
        return 'SDRplay API service is already running; reused without restarting it.'
    # Recheck just before the mutation. No arbitrary unit crosses this boundary.
    require(discover(run, trust) == (unit, before), 'SDRplay changed during review; check it again.')
    if exists('/usr/bin/deb-systemd-invoke'):
        trust('/usr/bin/deb-systemd-invoke')
        argv = ['/usr/bin/deb-systemd-invoke', 'start', unit]
    else:
        require(not exists('/usr/sbin/policy-rc.d'),
                'Local service-start policy needs Debian service helpers; start SDRplay locally.')
        argv = ['/usr/bin/systemctl', 'start', '--', unit]
    result = run(argv, timeout=20)
    require(not result.get('timedOut') and result['exitCode'] == 0,
            'SDRplay could not start. Check its service log and local service-start policy.')
    after_unit, after = discover(run, trust)
    # deb-systemd-invoke may return success when policy-rc.d denied the start.
    require(after_unit == unit and after['ActiveState'] == 'active' and
            all(after.get(key) == before.get(key) for key in PROPERTIES if key not in ('ActiveState', 'ExecStart')) and
            after['ExecStart'].split(' ; start_time=')[0] == before['ExecStart'].split(' ; start_time=')[0],
            'SDRplay did not become active, or changed during startup. Check its service log and local service-start policy.')
    return 'SDRplay API service started. Radio capture is checked separately by the processor.'


def local_kit_available(trust=broker.trusted_path):
    """Recognize only the installed all-profile local-kit declaration.

    This permits the first-install service preparation.  It does not claim that
    an adapter is loadable; `start` below separately asks the fixed local builder
    for its current, hash-verified result.
    """
    trust(MANIFEST)
    values = {}
    for line in broker.bounded_read(MANIFEST).decode().splitlines():
        key, separator, value = line.partition('=')
        require(separator and key not in values,
                'Installed build manifest is malformed.')
        values[key] = value
    require(values.get('backend') == 'all' and
            values.get('local_build_receivers') == 'RspDuo',
            'Installed build has no local RSPduo source kit.')
    trust(LOCAL_BUILD_HELPER)
    return True


def local_build_is_current(run=broker.command, trust=broker.trusted_path):
    """Use the fixed read-only builder status; never load a module as root."""
    trust(LOCAL_BUILD_HELPER)
    result = run(['/usr/bin/python3', '-I', LOCAL_BUILD_HELPER, 'status'], timeout=5)
    require(not result.get('timedOut') and result.get('exitCode') == 0,
            'Could not verify the locally built RSPduo adapter. Build SDRplay support in Settings first.')
    try:
        status = json.loads(result.get('output', ''))
    except (TypeError, ValueError) as error:
        raise broker.Refused('SDRPLAY_START_BLOCKED',
                             'Local RSPduo build status is invalid; rebuild SDRplay support in Settings.') from error
    require(isinstance(status, dict) and status.get('ok') is True and status.get('state') == 'current' and
            isinstance(status.get('kit_id'), str) and re.fullmatch(r'[a-f0-9]{64}', status['kit_id']) and
            isinstance(status.get('cohort'), str) and re.fullmatch(r'[a-f0-9]{64}', status['cohort']),
            'Local RSPduo build is not current; Build SDRplay support in Settings before starting capture.')
    return True


def main(argv, run=broker.command, trust=broker.trusted_path):
    require(os.geteuid() == 0, 'Starting SDRplay requires the installed privileged restart helper.')
    require(argv in (['install'], ['start']), 'This helper accepts only install or start, without paths or service names.')
    compiled = 'RspDuo' in broker.parse_manifest(MANIFEST)
    local_kit = False
    if not compiled:
        try:
            local_kit = local_kit_available(trust)
        except broker.Refused:
            if argv == ['install']:
                return 'This VectorWarp build has no RSPduo adapter or local source kit; no SDRplay service was changed.'
            raise
    if not compiled and argv == ['start']:
        require(local_kit and local_build_is_current(run, trust),
                'Build SDRplay support in Settings before starting live RSPduo capture.')
    return start_service(run, trust)


if __name__ == '__main__':
    try:
        print(main(sys.argv[1:]))
    except (broker.Refused, OSError, ValueError) as error:
        print(f'SDRplay: {error} Hardware API: {DOWNLOAD} VectorWarp never downloads or licenses it for you.', file=sys.stderr)
        sys.exit(1)
