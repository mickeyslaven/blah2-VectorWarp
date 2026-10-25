#!/usr/bin/env python3
"""Verify a PAM-free service identity, then execute its test command."""

import os
from pathlib import Path
import pwd
import sys


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: service_account_exec.py ACCOUNT COMMAND [ARG ...]')
    assert Path('/run/.containerenv').exists(), 'Service-account execution requires a disposable container'
    account = sys.argv[1]
    identity = pwd.getpwnam(account)
    expected_groups = set(os.getgrouplist(account, identity.pw_gid))
    assert os.getresuid() == (identity.pw_uid,) * 3, 'Service-account UIDs differ'
    assert os.getresgid() == (identity.pw_gid,) * 3, 'Service-account GIDs differ'
    assert set(os.getgroups()) == expected_groups, 'Service-account supplementary groups differ'
    status = dict(line.split(':', 1) for line in Path('/proc/self/status').read_text().splitlines()
                  if ':' in line)
    for field in ('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb'):
        assert int(status[field].strip(), 16) == 0, f'Service account retained {field}'
    assert int(status['NoNewPrivs'].strip()) == 0, 'Identity transition unexpectedly set NoNewPrivs'
    print(f'SERVICE_ACCOUNT_IDENTITY account={account} uid={identity.pw_uid} '
          f'gid={identity.pw_gid} groups={sorted(expected_groups)} capabilities=0', flush=True)
    os.execvp(sys.argv[2], sys.argv[2:])


if __name__ == '__main__':
    main()
