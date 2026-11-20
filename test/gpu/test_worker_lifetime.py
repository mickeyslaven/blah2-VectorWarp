#!/usr/bin/env python3
"""No GPU required: real orphan races and an optional Linux PID-1 namespace."""
import os
import select
import shutil
import subprocess
import sys


def namespace(executable):
    unshare = shutil.which("unshare")
    if not sys.platform.startswith("linux") or not unshare:
        print("SKIP PID namespace requires Linux unshare")
        return 77
    # Probe privilege separately: a failing regression must never become a skip.
    for options in (("--pid", "--fork"),
                    ("--user", "--map-root-user", "--pid", "--fork")):
        prefix = [unshare, *options]
        probe = subprocess.run([*prefix, "true"], capture_output=True, timeout=5)
        if probe.returncode == 0:
            return subprocess.run([*prefix, executable, "--require-pid-one"],
                                  timeout=15).returncode
    print("SKIP host/container denies creation of a nested PID namespace")
    return 77


def orphan(executable, phase):
    parent = subprocess.Popen([executable, phase], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    worker_pid = None
    completed = False
    try:
        assert select.select([parent.stdout], [], [], 5)[0], phase + ": worker not ready"
        line = parent.stdout.readline().strip()
        assert line.isdigit(), phase + ": missing worker PID"
        worker_pid = int(line)
        parent.kill()
        parent.wait(timeout=3)
        if phase == "--orphan-before-guard":
            parent.stdin.write("release\n")
            parent.stdin.flush()
        parent.stdin.close()
        parent.stdin = None
        output, errors = parent.communicate(timeout=3)
        if phase == "--orphan-before-guard":
            assert output.strip() == "rejected-orphan", (phase, output, errors)
        else:
            assert not output, (phase, output, errors)
        completed = True
        print("PASS worker-lifetime=" + phase)
    finally:
        if parent.poll() is None:
            parent.kill()
        parent.wait(timeout=3)
        if worker_pid and not completed:
            try:
                os.kill(worker_pid, 9)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    executable = sys.argv[1]
    if len(sys.argv) == 3 and sys.argv[2] == "--pid-namespace":
        sys.exit(namespace(executable))
    subprocess.run([executable], check=True, timeout=15)
    for mode in ("--orphan-before-guard", "--orphan-after-guard"):
        orphan(executable, mode)
