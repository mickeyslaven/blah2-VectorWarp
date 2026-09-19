#!/usr/bin/env python3
"""Bounded native startup regression for calibration samples in FIR history."""
import argparse
import json
from pathlib import Path

import native_simulation_test as simulation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--heimdall', type=Path, required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-rejection', action='store_true')
    parser.add_argument('--port-base', type=int, default=29391)
    args = parser.parse_args()
    args.heimdall = args.heimdall.resolve(strict=True)
    args.include = args.include.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, mode=0o700, exist_ok=False)
    args.data_port, args.control_port, args.web_port, args.rtl_port = range(args.port_base, args.port_base + 4)
    args.calibration_timeout = 45
    binary = simulation.prepare(args, args.output)
    simulation.reserve_ports([args.data_port, args.control_port, args.web_port, args.rtl_port])
    try:
        report = simulation.run_case(args, binary, args.output, 'restart_retry')
    except AssertionError as error:
        if not args.expect_rejection or 'Calibration marker leaked from FIR history' not in str(error):
            raise
        report = dict(simulatedOnly=True, rejectedOldCalibrationTail=True, error=str(error))
    else:
        assert not args.expect_rejection, 'Pre-fix binary unexpectedly passed the FIR-tail regression'
        report['freshFirHistoryVerified'] = True
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
