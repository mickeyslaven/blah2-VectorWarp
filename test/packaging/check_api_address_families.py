#!/usr/bin/env python3
"""Exercise packaged Node with the API unit's Linux socket-family allowlist.

This adds a seccomp filter only to short-lived child processes. It does not
start services, bind ports, change host policy, or access receiver hardware.
It tests RestrictAddressFamilies, not every systemd sandbox setting.
"""

import argparse
import ctypes
import ctypes.util
import errno
import os
from pathlib import Path
import socket
import subprocess
import sys


def read_families(unit):
    lines = [line.strip().split('=', 1)[1].split()
             for line in Path(unit).read_text().splitlines()
             if line.strip().startswith('RestrictAddressFamilies=')]
    if len(lines) != 1 or not lines[0]:
        raise ValueError('Expected one nonempty API socket-family allowlist')
    families = set()
    for name in lines[0]:
        if not name.startswith('AF_') or not hasattr(socket, name):
            raise ValueError(f'Unsupported API socket-family entry: {name}')
        families.add(int(getattr(socket, name)))
    if not {socket.AF_UNIX, socket.AF_INET, socket.AF_INET6} <= families:
        raise ValueError('API allowlist must retain UNIX, IPv4, and IPv6')
    if socket.AF_PACKET in families:
        raise ValueError('API allowlist must not permit raw packet sockets')
    return families


def restrict_child(families):
    library_name = ctypes.util.find_library('seccomp')
    if not library_name:
        raise RuntimeError('libseccomp is required for the package sandbox check')
    library = ctypes.CDLL(library_name)
    library.seccomp_init.argtypes = [ctypes.c_uint32]
    library.seccomp_init.restype = ctypes.c_void_p
    library.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    library.seccomp_syscall_resolve_name.restype = ctypes.c_int

    class Comparison(ctypes.Structure):
        _fields_ = [('arg', ctypes.c_uint), ('op', ctypes.c_uint),
                    ('a', ctypes.c_uint64), ('b', ctypes.c_uint64)]

    library.seccomp_rule_add_array.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_int, ctypes.c_uint, ctypes.POINTER(Comparison)]
    library.seccomp_load.argtypes = [ctypes.c_void_p]
    library.seccomp_release.argtypes = [ctypes.c_void_p]
    context = library.seccomp_init(0x7fff0000)  # SCMP_ACT_ALLOW
    if not context:
        raise RuntimeError('Could not allocate a seccomp test context')
    try:
        socket_call = library.seccomp_syscall_resolve_name(b'socket')
        if socket_call < 0:
            raise RuntimeError('Native socket syscall could not be resolved')
        # Linux currently defines families below 64. This reproduces the unit's
        # allowlist over that range; it is a test, not a production policy loader.
        for family in range(64):
            if family not in families:
                comparison = Comparison(0, 4, family, 0)  # SCMP_CMP_EQ
                result = library.seccomp_rule_add_array(context,
                    0x50000 | errno.EAFNOSUPPORT, socket_call, 1,
                    ctypes.byref(comparison))
                if result != 0:
                    raise RuntimeError(f'Could not add socket test rule: {result}')
        result = library.seccomp_load(context)
        if result != 0:
            raise RuntimeError(f'Could not load socket test rules: {result}')
    finally:
        library.seccomp_release(context)
    try:
        probe = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    except OSError as error:
        if error.errno != errno.EAFNOSUPPORT:
            raise RuntimeError('Raw packet socket denial was not the test filter') from error
    else:
        probe.close()
        raise RuntimeError('Raw packet sockets unexpectedly allowed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unit', required=True)
    parser.add_argument('--node', required=True)
    parser.add_argument('--child', choices=['configured', 'without-netlink'],
                        help=argparse.SUPPRESS)
    args = parser.parse_args()
    if sys.platform != 'linux':
        raise RuntimeError('This package acceptance check requires Linux')
    families = read_families(args.unit)
    if args.child:
        if args.child == 'without-netlink':
            families.discard(socket.AF_NETLINK)
        restrict_child(families)
        os.execv(args.node, [args.node, '-e',
            'console.log(JSON.stringify({node:process.version,interfaceCount:'
            'Object.keys(require("node:os").networkInterfaces()).length}))'])
    for case in ['without-netlink', 'configured']:
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
            '--unit', str(Path(args.unit).resolve()), '--node', str(Path(args.node).resolve()),
            '--child', case], text=True, capture_output=True, timeout=15)
        if case == 'without-netlink':
            if result.returncode == 0 or 'uv_interface_addresses' not in result.stderr:
                raise RuntimeError(f'Negative control did not reproduce the interface error:\n{result.stderr}')
            print('PASS: missing AF_NETLINK reproduces the Node interface-enumeration failure')
        elif result.returncode != 0:
            raise RuntimeError(f'Packaged API socket allowlist prevents Node startup:\n{result.stderr}')
        else:
            print(f'PASS: configured API socket allowlist: {result.stdout.strip()}')


if __name__ == '__main__':
    main()
