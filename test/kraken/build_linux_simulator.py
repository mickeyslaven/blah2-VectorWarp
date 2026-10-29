#!/usr/bin/env python3
"""Build pinned Linux Heimdall, then relink its objects with a test-only driver.

The production static-driver binary is compile evidence only and is never run.
No upstream install scripts or hardware access are used.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SONAME = 'libvectorwarp-simulated-rtlsdr.so'
FAKE = ROOT / 'test/macos/kraken/fake_rtlsdr.cpp'
LINUX_DRIVER_FLAGS = ['-DCMAKE_BUILD_TYPE=Release', '-DINSTALL_UDEV_RULES=OFF',
                      '-DDETACH_KERNEL_DRIVER=ON', '-DWITH_RPC=OFF']
CONTRACT = runpy.run_path(str(ROOT / 'script/build-kraken-macos.py'))
PINS = CONTRACT['PINS']
PATCHES = [ROOT / 'script/patches' / name
           for group in CONTRACT['PATCHES'].values() for name in group]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def clean_environment():
    for key in ('LD_PRELOAD', 'LD_LIBRARY_PATH', 'LD_AUDIT'):
        if os.environ.get(key):
            raise ValueError(f'Unset ambient {key} before building or running simulation')
    return dict(os.environ)


def checked(command, **kwargs):
    return subprocess.check_output([str(x) for x in command], text=True,
                                   env=clean_environment(), **kwargs)


def run(command, **kwargs):
    subprocess.run([str(x) for x in command], check=True,
                   env=clean_environment(), **kwargs)


def apply_patch(source, patch):
    # Keep preparation and the installable source kit on the same deterministic
    # patch engine so the simulator cannot pass or fail for a different reason.
    CONTRACT['apply_source_patch'](source, patch)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def audit(binary, fake, link, link_map):
    """Fail closed on driver provenance, ELF symbols, and loader closure."""
    clean_environment()
    command, mapping = link.read_text(), link_map.read_text()
    for label, text in (('link command', command), ('link map', mapping)):
        require(SONAME in text, f'{label} does not contain simulated driver')
        require(not re.search(r'librtlsdr|libusb|rtl-sdr|rtlsdr\.a', text, re.I),
                f'{label} contains a real RTL/USB driver')
    dynamic = checked(['readelf', '-d', binary])
    fake_dynamic = checked(['readelf', '-d', fake])
    require(f'[{SONAME}]' in dynamic, 'ELF lacks simulated driver NEEDED entry')
    require(f'[{SONAME}]' in fake_dynamic, 'Fake driver SONAME is incorrect')
    for label, content in (('binary', dynamic), ('fake', fake_dynamic)):
        require(not re.search(r'librtlsdr|libusb', content, re.I), f'{label} depends on real driver')
        paths = re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]', content)
        require(all(p == '$ORIGIN' for p in paths), f'Unexpected {label} loader path: {paths}')
    require('$ORIGIN' in dynamic, 'Binary must resolve simulated library beside itself')
    binary_symbols = checked(['nm', '-a', binary])
    fake_symbols = checked(['nm', '-a', fake])
    require(not re.search(r'\b[TtWw] rtlsdr_', binary_symbols), 'Binary embeds static RTL implementation')
    for label, symbols in (('binary', binary_symbols), ('fake', fake_symbols)):
        require(not re.search(r'\blibusb_|\b(?:dlopen|dlmopen|dlsym)(?:@|$)', symbols, re.M),
                f'{label} contains USB or dynamic-driver fallback symbols')
    imports = set(re.findall(r'\bU (rtlsdr_\w+)', binary_symbols))
    exports = set(re.findall(r'\b[TW] (rtlsdr_\w+)', fake_symbols))
    require(bool(imports) and imports <= exports, 'Fake driver does not supply all RTL imports')
    closure = checked(['ldd', binary])
    require('not found' not in closure, 'ELF dependency is unresolved')
    require(not re.search(r'librtlsdr|libusb', closure, re.I), 'Loader closure includes RTL/USB')
    resolved_fake = re.search(re.escape(SONAME) + r'\s+=>\s+(\S+)', closure)
    require(resolved_fake and Path(resolved_fake[1]).resolve() == fake.resolve(),
            'Loader resolves simulated driver outside prepared directory')
    closure_paths = set(re.findall(r'(?:=>\s+|^\s*)(/\S+)\s+\(', closure, re.M))
    for dependency in sorted(closure_paths):
        if Path(dependency).resolve() == fake.resolve():
            continue
        symbols = checked(['nm', '-D', '--defined-only', dependency])
        require(not re.search(r'\b(?:rtlsdr_|libusb_)', symbols),
                f'Loader dependency embeds RTL/USB symbols: {dependency}')
    return {'dynamic': dynamic, 'fakeDynamic': fake_dynamic, 'loaderClosure': closure,
            'rtlImports': sorted(imports)}


def verify_prepared(binary, manifest):
    """Verify immutable prepared artifacts immediately before every execution."""
    binary, manifest = Path(binary).resolve(), Path(manifest).resolve()
    data = json.loads(manifest.read_text())
    require(data.get('schema') == 1 and data.get('simulatedOnly') is True,
            'Invalid Linux simulation manifest')
    require(data.get('platform') == 'linux' and data.get('hardwareTested') is False,
            'Manifest must describe Linux software simulation')
    require(data['sources'] == json.loads(PINS.read_text()), 'Source pins changed')
    require(data['patchSha256'] == {p.name: digest(p) for p in PATCHES}, 'Source patches changed')
    require(data['fakeSourceSha256'] == digest(FAKE), 'Simulated driver source changed')
    require(binary.name == 'heimdall-simulated' and binary.parent == manifest.parent,
            'Binary and manifest must share prepared directory')
    for filename, expected in data['artifactSha256'].items():
        require(Path(filename).name == filename, 'Manifest contains nonlocal artifact')
        require(digest(binary.parent / filename) == expected, f'Artifact changed: {filename}')
    for required in ('heimdall-simulated', SONAME, 'simulated-link.txt', 'simulated-link.map',
                     'heimdall-production-compile-only'):
        require(required in data['artifactSha256'], f'Manifest lacks {required}')
    audit(binary, binary.parent / SONAME, binary.parent / 'simulated-link.txt',
          binary.parent / 'simulated-link.map')
    return data


def build(args):
    require(sys.platform.startswith('linux'), 'This test builder requires Linux')
    require(1 <= args.jobs <= 4, '--jobs must be from 1 through 4')
    clean_environment()
    destination = args.output_dir.resolve()
    require(not destination.exists(), 'Output directory must be new')
    args.build_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='linux-simulation-', dir=args.build_dir.resolve()))
    spec = importlib.util.spec_from_file_location('kraken_source_builder', ROOT / 'script/build-kraken-macos.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    pins = json.loads(PINS.read_text())
    if args.archives_dir:
        for name in pins:
            shutil.copy2(args.archives_dir / (name + '.tar.gz'), work / (name + '.tar.gz'))
    trees = {name: helper.source(name, pin, work, None) for name, pin in pins.items()}
    suite = work / 'suite'
    shutil.copytree(trees['suite'], suite)
    native = suite / 'heimdall_v2'
    shutil.copytree(trees['uwebsockets'], native / 'uWebSockets', dirs_exist_ok=True)
    shutil.copytree(trees['usockets'], native / 'uWebSockets/uSockets', dirs_exist_ok=True)
    for name in CONTRACT['PATCHES']['suite']:
        apply_patch(suite, ROOT / 'script/patches' / name)
    driver = suite / 'librtlsdr'
    if driver.exists():
        shutil.rmtree(driver)
    shutil.copytree(trees['rtlsdr'], driver)
    for name in CONTRACT['PATCHES']['rtlsdr']:
        apply_patch(driver, ROOT / 'script/patches' / name)
    for test in ('convergence_oracle_test.py', 'tcp_data_send_test.py',
                 'tcp_data_backpressure_test.py', 'socket_lifecycle_test.py',
                 'watchdog_oracle_test.py'):
        run([sys.executable, ROOT / 'test/macos/kraken' / test, '--source', native])
    for test in ('control_reader_test.py', 'driver_events_test.py', 'shutdown_process_test.py'):
        run([sys.executable, ROOT / 'test/macos/kraken' / test, '-v'])
    # A production Linux receiver must reclaim RTL2832 devices after a clean
    # service restart. Without this option librtlsdr reports every tuner as
    # claimed by dvb_usb_rtl28xxu and Heimdall exits before opening its ports.
    flags = LINUX_DRIVER_FLAGS
    run(['cmake', '-S', driver, '-B', driver / 'build', '-G', 'Unix Makefiles', *flags])
    run(['cmake', '--build', driver / 'build', '--target', 'rtlsdr_static', '--parallel', args.jobs])
    run(['make', 'WITH_SSL=0', f'-j{args.jobs}'], cwd=native / 'uWebSockets/uSockets')
    build_dir = work / 'heimdall-build'
    run(['cmake', '-S', native, '-B', build_dir, '-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release'])
    run(['cmake', '--build', build_dir, '--target', 'heimdall', '--parallel', args.jobs])
    destination.mkdir(parents=True)
    shutil.copy2(build_dir / 'heimdall', destination / 'heimdall-production-compile-only')
    # Deliberately remove execute permission from the production receipt.
    (destination / 'heimdall-production-compile-only').chmod(0o600)
    fake = destination / SONAME
    fake_command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-shared', '-fPIC', '-pthread',
                    '-I', driver / 'include', FAKE, '-Wl,-soname,' + SONAME, '-o', fake]
    run(fake_command)
    # Test-only override in the disposable source tree. Preserve compiled native
    # objects and all production non-driver libraries; replace only driver links.
    override = f'''\n# VectorWarp TEST ONLY: production binary already built; never install this target.\nget_target_property(VECTORWARP_LINKS heimdall LINK_LIBRARIES)\nlist(REMOVE_ITEM VECTORWARP_LINKS "${{RTLSDR_STATIC_LIB}}" PkgConfig::LIBUSB ${{LIBUSB_LIBRARIES}})\nset_property(TARGET heimdall PROPERTY LINK_LIBRARIES "{fake}" ${{VECTORWARP_LINKS}})\nset_property(TARGET heimdall PROPERTY BUILD_RPATH "$ORIGIN")\nset_property(TARGET heimdall PROPERTY BUILD_WITH_INSTALL_RPATH TRUE)\nset_property(TARGET heimdall PROPERTY INSTALL_RPATH "$ORIGIN")\ntarget_link_options(heimdall PRIVATE "-Wl,-Map,{destination / 'simulated-link.map'}")\n'''
    with (native / 'CMakeLists.txt').open('a') as stream:
        stream.write(override)
    run(['cmake', '-S', native, '-B', build_dir])
    run(['cmake', '--build', build_dir, '--target', 'heimdall', '--parallel', args.jobs])
    binary = destination / 'heimdall-simulated'
    shutil.copy2(build_dir / 'heimdall', binary)
    shutil.copy2(build_dir / 'CMakeFiles/heimdall.dir/link.txt', destination / 'simulated-link.txt')
    shutil.copy2(build_dir / 'CMakeFiles/heimdall.dir/flags.make', destination / 'native-compile-flags.txt')
    shutil.copy2(driver / 'build/src/CMakeFiles/rtlsdr_static.dir/flags.make', destination / 'driver-compile-flags.txt')
    evidence = audit(binary, fake, destination / 'simulated-link.txt', destination / 'simulated-link.map')
    files = ['heimdall-simulated', SONAME, 'simulated-link.txt', 'simulated-link.map',
             'heimdall-production-compile-only', 'native-compile-flags.txt', 'driver-compile-flags.txt']
    receipt = dict(schema=1, simulatedOnly=True, platform='linux', architecture=platform.machine(),
                   hardwareTested=False, sources=pins, patchSha256={p.name: digest(p) for p in PATCHES},
                   fakeSourceSha256=digest(FAKE), artifactSha256={p: digest(destination / p) for p in files},
                   driverFlags=flags, fakeDriverCommand=[str(x) for x in fake_command],
                   nativeConfigureFlags=['-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release'],
                   uSocketsFlags=['WITH_SSL=0'], testOverride=override,
                   buildDirectory=str(work), audit=evidence)
    manifest = destination / 'binary-manifest.json'
    manifest.write_text(json.dumps(receipt, indent=2) + '\n')
    verify_prepared(binary, manifest)
    print(f'Prepared test-only Linux simulation: {binary}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--build-dir', default=ROOT / 'build/kraken-linux', type=Path)
    parser.add_argument('--archives-dir', type=Path, help='Optional pinned suite/rtlsdr/uwebsockets/usockets .tar.gz archives')
    parser.add_argument('--jobs', default=2, type=int)
    build(parser.parse_args())


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'build_linux_simulator: {error}')
