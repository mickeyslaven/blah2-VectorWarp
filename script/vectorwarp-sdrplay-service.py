#!/usr/bin/python3
"""Start an installed, standard SDRplay API service; never obtain vendor software.

Only root installation/restart helpers invoke this fixed-name operation. Browser
discovery remains read-only; custom services retain the reviewed broker route.
"""
import importlib.machinery
import importlib.util
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


def main(argv):
    require(os.geteuid() == 0, 'Starting SDRplay requires the installed privileged restart helper.')
    require(argv in (['install'], ['start']), 'This helper accepts only install or start, without paths or service names.')
    if 'RspDuo' not in broker.parse_manifest(MANIFEST):
        if argv == ['install']:
            return 'This VectorWarp build has no RSPduo adapter; no SDRplay service was changed.'
        require(False, 'Install a VectorWarp build containing the RSPduo adapter before starting live RSPduo capture.')
    return start_service()


if __name__ == '__main__':
    try:
        print(main(sys.argv[1:]))
    except (broker.Refused, OSError, ValueError) as error:
        print(f'SDRplay: {error} Hardware API: {DOWNLOAD} VectorWarp never downloads or licenses it for you.', file=sys.stderr)
        sys.exit(1)
