#!/usr/bin/python3 -I
"""Recover a persistently down-trained Linux PCIe GPU without touching SDR capture.

This helper is deliberately opt-in.  It only acts when the configured GPU link is
slower than its advertised maximum *and* VectorWarp has exceeded its CPI budget on
two consecutive checks.  Recovery re-enumerates the GPU and restarts only the
display manager (when active) and VectorWarp processor.  Receiver/Heimdall services
are never stopped.
"""

import argparse
import fcntl
import json
import os
import pathlib
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request


class Refused(RuntimeError):
    pass


def require(value, message):
    if not value:
        raise Refused(message)


def read(path):
    return pathlib.Path(path).read_text(encoding='utf-8').strip()


def speed(value):
    match = re.fullmatch(r'([0-9]+(?:\.[0-9]+)?) GT/s PCIe', value)
    require(match, 'Unexpected PCIe speed value: ' + value)
    return float(match.group(1))


def run(command, check=True):
    return subprocess.run(command, check=check, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT,
                          env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})


def active(unit):
    return run(['/usr/bin/systemctl', 'is-active', '--quiet', unit], check=False).returncode == 0


def timing(url):
    with urllib.request.urlopen(url, timeout=3) as response:
        require(response.status == 200, 'Timing endpoint is not healthy.')
        value = json.loads(response.read(65537))
    require(isinstance(value, dict) and isinstance(value.get('cpi'), (int, float)),
            'Timing endpoint returned no CPI value.')
    acceleration = value.get('acceleration') or {}
    require(acceleration.get('state') == 'ready', 'GPU acceleration is not ready.')
    return float(value['cpi'])


def link_paths(sysfs, bdf):
    require(re.fullmatch(r'0000:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]', bdf), 'Invalid GPU PCI BDF.')
    device = pathlib.Path(sysfs) / 'bus/pci/devices' / bdf
    require(device.is_dir(), 'Configured GPU PCI device is missing.')
    return device / 'current_link_speed', device / 'max_link_speed'


def recover(sysfs, bdf):
    require(os.geteuid() == 0, 'GPU link recovery requires root.')
    processor_was_active = active('vectorwarp-processor.service')
    display_was_active = active('display-manager.service')
    require(processor_was_active, 'VectorWarp processor is not active; refusing unrelated GPU recovery.')
    run(['/usr/bin/systemctl', 'stop', 'vectorwarp-processor.service'])
    try:
        if display_was_active:
            run(['/usr/bin/systemctl', 'stop', 'display-manager.service'])
        slot = bdf.rsplit('.', 1)[0]
        devices = pathlib.Path(sysfs) / 'bus/pci/devices'
        functions = sorted(devices.glob(slot + '.*'), reverse=True)
        require(functions, 'No PCI functions found for configured GPU.')
        for function in functions:
            remove = function / 'remove'
            if remove.is_file():
                remove.write_text('1\n', encoding='ascii')
        time.sleep(2)
        (pathlib.Path(sysfs) / 'bus/pci/rescan').write_text('1\n', encoding='ascii')
        for _ in range(15):
            if (devices / bdf).is_dir():
                break
            time.sleep(1)
        require((devices / bdf).is_dir(), 'GPU did not return after PCI rescan.')
        run(['/usr/bin/udevadm', 'settle'])
    finally:
        if display_was_active:
            run(['/usr/bin/systemctl', 'start', 'display-manager.service'], check=False)
        if processor_was_active:
            run(['/usr/bin/systemctl', 'start', 'vectorwarp-processor.service'], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bdf', required=True, help='GPU PCI BDF, for example 0000:01:00.0')
    parser.add_argument('--timing-url', default='http://127.0.0.1:3000/api/timing')
    parser.add_argument('--cpi-limit-ms', type=float, default=500.0)
    parser.add_argument('--state-dir', default='/run/vectorwarp-gpu-link-watchdog')
    parser.add_argument('--sysfs', default='/sys', help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        require(args.cpi_limit_ms > 0, 'CPI limit must be positive.')
        current_path, maximum_path = link_paths(args.sysfs, args.bdf)
        current, maximum = speed(read(current_path)), speed(read(maximum_path))
        state = pathlib.Path(args.state_dir)
        state.mkdir(mode=0o700, parents=True, exist_ok=True)
        with (state / 'lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            strike_file = state / 'strikes'
            if current >= maximum:
                strike_file.unlink(missing_ok=True)
                print(f'GPU PCIe link healthy: {current:g} GT/s (maximum {maximum:g} GT/s).')
                return 0
            cpi = timing(args.timing_url)
            if cpi <= args.cpi_limit_ms:
                strike_file.unlink(missing_ok=True)
                print(f'GPU link is {current:g}/{maximum:g} GT/s, but CPI {cpi:.2f} ms is within budget; no action.')
                return 0
            strikes = int(read(strike_file)) + 1 if strike_file.exists() else 1
            strike_file.write_text(str(strikes) + '\n', encoding='ascii')
            if strikes < 2:
                print(f'GPU link degradation candidate {strikes}/2: {current:g}/{maximum:g} GT/s, CPI {cpi:.2f} ms.')
                return 0
            attempt_file = state / 'last-attempt'
            now = int(time.time())
            last_attempt = int(read(attempt_file)) if attempt_file.exists() else 0
            if now - last_attempt < 900:
                print(f'GPU link remains degraded, but recovery is in its 15-minute cooldown; no action.')
                return 0
            attempt_file.write_text(str(now) + '\n', encoding='ascii')
            print(f'Recovering down-trained GPU link: {current:g}/{maximum:g} GT/s, CPI {cpi:.2f} ms.', flush=True)
            recover(args.sysfs, args.bdf)
            time.sleep(3)
            new_current = speed(read(link_paths(args.sysfs, args.bdf)[0]))
            require(new_current >= maximum, f'GPU returned at only {new_current:g} GT/s; manual hardware review required.')
            strike_file.unlink(missing_ok=True)
            print(f'GPU PCIe link recovered to {new_current:g} GT/s. SDR capture was left running.')
            return 0
    except (OSError, ValueError, Refused, urllib.error.URLError, BlockingIOError,
            subprocess.CalledProcessError) as error:
        print('GPU link watchdog: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
