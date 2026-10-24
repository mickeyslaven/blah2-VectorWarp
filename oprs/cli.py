"""Command-line interface for the OPRS reference implementation."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .common import ContractError, file_hash, load_json, object_hash, write_json
from .contracts import validate_manifest, validate_plan
from .iq import analyze_iq, inject_blah2iq, make_fixture
from .scoring import invalid_scorecard, score
from .synthetic import conformance_suite


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="oprs", description="Open Passive Radar Scorecard reference CLI")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate a system manifest or frozen evaluation plan")
    validate.add_argument("kind", choices=["manifest", "plan"]); validate.add_argument("path", type=Path)
    digest = commands.add_parser("hash", help="SHA-256 a file"); digest.add_argument("path", type=Path)
    suite = commands.add_parser("generate-suite", help="write deterministic conformance scenarios")
    suite.add_argument("--profile", required=True); suite.add_argument("--seed", type=int, default=20261022)
    suite.add_argument("--output", type=Path, required=True)
    fixture = commands.add_parser("make-fixture", help="create deterministic BLAH2IQ test background")
    fixture.add_argument("--output", type=Path, required=True); fixture.add_argument("--channels", type=int, default=2)
    fixture.add_argument("--sample-rate", type=int, default=20000); fixture.add_argument("--seconds", type=float, default=1)
    health = commands.add_parser("analyze-iq", help="derive portable RF health from BLAH2IQ")
    health.add_argument("--manifest", type=Path, required=True); health.add_argument("--iq", type=Path, required=True)
    health.add_argument("--output", type=Path, required=True)
    inject = commands.add_parser("inject", help="inject a replay-only time-varying echo")
    inject.add_argument("--manifest", type=Path, required=True); inject.add_argument("--scenario", type=Path, required=True)
    inject.add_argument("--input", type=Path, required=True); inject.add_argument("--output", type=Path, required=True)
    inject.add_argument("--receipt", type=Path, required=True)
    scoring = commands.add_parser("score", help="score strict trial outputs against a frozen plan")
    scoring.add_argument("--manifest", type=Path, required=True); scoring.add_argument("--plan", type=Path, required=True)
    scoring.add_argument("--trial", type=Path, action="append", required=True); scoring.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "validate":
            value = load_json(args.path)
            (validate_manifest if args.kind == "manifest" else validate_plan)(value)
            print(json.dumps({"valid": True, "canonical_hash": object_hash(value)})); return 0
        if args.command == "hash":
            print(file_hash(args.path)); return 0
        if args.command == "generate-suite":
            write_json(args.output, conformance_suite(args.profile, args.seed)); return 0
        if args.command == "make-fixture":
            print(json.dumps(make_fixture(args.output, args.channels, args.sample_rate, args.seconds), sort_keys=True)); return 0
        if args.command == "analyze-iq":
            write_json(args.output, analyze_iq(args.iq, load_json(args.manifest))); return 0
        if args.command == "inject":
            receipt = inject_blah2iq(args.input, args.output, load_json(args.manifest), load_json(args.scenario))
            write_json(args.receipt, receipt); return 0
        if args.command == "score":
            manifest, plan = load_json(args.manifest), load_json(args.plan)
            trials = [load_json(path) for path in args.trial]
            try:
                result = score(manifest, plan, trials)
            except (ContractError, ValueError) as error:
                result = invalid_scorecard(manifest, plan, [str(error)])
                write_json(args.output, result)
                print(str(error), file=sys.stderr); return 2
            write_json(args.output, result); return 0
    except (ContractError, OSError, KeyError) as error:
        print(f"oprs: {error}", file=sys.stderr); return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
