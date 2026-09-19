#!/usr/bin/env python3
"""Bounded standalone driver gate with diagnostic evidence on failure; no RF."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def bounded(command, environment, stdout, stderr, timeout):
    with stdout.open('wb') as output, stderr.open('wb') as errors:
        child = subprocess.Popen(command, env=environment, stdout=output, stderr=errors,
                                 start_new_session=True)
        try:
            return {'returncode': child.wait(timeout=timeout), 'timed_out': False}
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
            return {'returncode': child.returncode, 'timed_out': True}


def valid_response(result, output):
    if result['timed_out'] or result['returncode'] != 0 or output.stat().st_size > 32768:
        return False
    try:
        value = json.loads(output.read_text())
        return (isinstance(value, dict) and value.get('version') == 1 and
                isinstance(value.get('available'), bool) and
                value.get('qualification') == 'not-run' and
                isinstance(value.get('devices'), list) and len(value['devices']) <= 32)
    except (ValueError, UnicodeError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--observe-only', action='store_true',
                        help='Record the original build control; not the standalone acceptance gate.')
    parser.add_argument('--icd-library', type=Path,
                        help='Use an explicit MoltenVK library for the original build control.')
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    worker = args.worker.resolve(strict=True)
    environment = {**os.environ, 'BLAH2_GPU_DIAGNOSTICS': '1', 'VK_LOADER_DEBUG': 'all',
                   'DYLD_PRINT_LIBRARIES': '1'}
    if args.icd_library:
        icd = args.evidence.resolve() / 'control-icd.json'
        icd.write_text(json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'library_path': str(args.icd_library.resolve(strict=True)), 'api_version': '1.2.0'}}))
        environment['VK_DRIVER_FILES'] = environment['VK_ICD_FILENAMES'] = str(icd)
    started = time.monotonic()
    result = bounded([str(worker), '--driver-status'], environment,
                     args.evidence / 'stdout.log', args.evidence / 'stderr.log', 10)
    result['accepted'] = valid_response(result, args.evidence / 'stdout.log')
    result['elapsed_seconds'] = round(time.monotonic() - started, 3)
    result['observe_only'] = args.observe_only
    if not result['accepted']:
        # Keep the original failure as the gate. The debugger only diagnoses it.
        debugger_environment = dict(environment)
        for key in ('PYTHONHOME', 'PYTHONPATH'):
            debugger_environment.pop(key, None)
        try:
            result['debugger'] = bounded(['/usr/bin/lldb', '--batch', '--no-lldbinit',
                '-o', 'run --driver-status', '-o', 'thread backtrace all',
                '-o', 'image list', '-o', 'process kill', '-o', 'quit',
                '-k', 'thread backtrace all', '-k', 'image list',
                '-k', 'process kill', '-k', 'quit', '--', str(worker)], debugger_environment,
                args.evidence / 'backtrace.log', args.evidence / 'debugger-errors.log', 30)
        except OSError as error:
            result['debugger'] = {'error': type(error).__name__}
    (args.evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, sort_keys=True))
    return 0 if result['accepted'] or args.observe_only else 1


if __name__ == '__main__':
    raise SystemExit(main())
