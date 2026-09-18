#!/usr/bin/env python3
"""Build a pinned, isolated Heimdall companion using Homebrew dependencies.

This explicit source build never installs a global RTL driver, runs Heimdall,
opens USB, or downloads SDRplay. Upstream components retain their own terms.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    subprocess.run([str(item) for item in command], check=True, **kwargs)


def source(name, spec, build, supplied):
    if supplied:
        directory = supplied / name
        if not directory.is_dir():
            raise ValueError(f'Missing staged Homebrew resource: {name}')
        return directory
    archive = build / (name + '.tar.gz')
    if not archive.exists():
        temporary = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen(spec['url'], timeout=60) as response, temporary.open('wb') as stream:
                shutil.copyfileobj(response, stream)
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
        raise ValueError(f'Pinned {name} archive checksum mismatch: {archive}')
    destination = build / 'sources' / name
    destination.mkdir(parents=True)
    with tarfile.open(archive) as bundle:
        bundle.extractall(destination, filter='data')
    entries = list(destination.iterdir())
    if len(entries) != 1 or not entries[0].is_dir():
        raise ValueError(f'Unexpected {name} source archive layout')
    return entries[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/kraken-native')
    parser.add_argument('--sources-dir', type=Path, help='Homebrew checksum-verified resource directories')
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--test', action='store_true', help='Verify patched driver and calibration semantics without USB')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This companion build targets macOS')
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be from 1 through 32')
    brew = os.environ.get('VECTORWARP_MACOS_BREW_PREFIX')
    if not brew:
        brew = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    brew = Path(brew)
    for dependency in ('eigen', 'fftw', 'libusb'):
        if not (brew / 'opt' / dependency).is_dir():
            parser.error(f'Install the build dependency: brew install {dependency}')
    base = args.build_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    # Fresh per-build source and output trees prevent reapplying a patch over
    # stale experimental sources. Retain the exact tree and logs for diagnosis.
    work = Path(tempfile.mkdtemp(prefix='build-', dir=base))
    print(f'Kraken companion build: {work}', flush=True)
    sources = json.loads((ROOT / 'config/kraken-macos-sources.json').read_text())
    supplied = args.sources_dir.resolve() if args.sources_dir else None
    trees = {name: source(name, spec, work, supplied) for name, spec in sources.items()}
    # Never patch a Homebrew resource or the caller's source in place.
    suite = work / 'suite'
    shutil.copytree(trees['suite'], suite)
    native = suite / 'heimdall_v2'
    shutil.copytree(trees['uwebsockets'], native / 'uWebSockets', dirs_exist_ok=True)
    shutil.copytree(trees['usockets'], native / 'uWebSockets/uSockets', dirs_exist_ok=True)
    patch = work / 'heimdall-v2-macos.patch'
    shutil.copy2(ROOT / 'script/patches/heimdall-v2-macos.patch', patch)
    run(['patch', '--batch', '-p1', '-i', patch], cwd=suite)
    driver_source = work / 'rtlsdr'
    shutil.copytree(trees['rtlsdr'], driver_source)
    driver_patch = work / 'kraken-librtlsdr-macos.patch'
    shutil.copy2(ROOT / 'script/patches/kraken-librtlsdr-macos.patch', driver_patch)
    run(['patch', '--batch', '-p1', '-i', driver_patch], cwd=driver_source)
    if args.test:
        for test in ('test/macos/kraken_driver/test_biastee_error_propagation.py',
                     'test/macos/kraken/gain_oracle_test.py'):
            run([sys.executable, ROOT / test, '--source', driver_source])
        run([sys.executable, ROOT / 'test/macos/kraken/convergence_oracle_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/tcp_data_send_test.py', '--source', native])
    prefix = work / 'driver'
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={brew}']
    run(['cmake', '-S', driver_source, '-B', work / 'driver-build', *common,
         f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DINSTALL_UDEV_RULES=OFF', '-DDETACH_KERNEL_DRIVER=OFF', '-DWITH_RPC=OFF'])
    run(['cmake', '--build', work / 'driver-build', '--parallel', args.jobs])
    run(['cmake', '--install', work / 'driver-build'])
    run(['make', 'WITH_SSL=0', f'-j{args.jobs}'], cwd=native / 'uWebSockets/uSockets')
    env = dict(os.environ, PKG_CONFIG_PATH=f'{prefix}/lib/pkgconfig:{brew}/lib/pkgconfig')
    run(['cmake', '-S', native, '-B', work / 'heimdall-build', *common], env=env)
    run(['cmake', '--build', work / 'heimdall-build', '--parallel', args.jobs])
    destination = args.output_dir.resolve()
    for directory in ('bin', 'lib/kraken', 'share/heimdall', 'share/heimdall/licenses'):
        (destination / directory).mkdir(parents=True, exist_ok=True)
    executable = destination / 'bin/heimdall'
    library = destination / 'lib/kraken/librtlsdr.0.dylib'
    shutil.copy2(work / 'heimdall-build/heimdall', executable)
    shutil.copy2((prefix / 'lib/librtlsdr.0.dylib').resolve(), library)
    run(['install_name_tool', '-change', '@rpath/librtlsdr.0.dylib',
         '@loader_path/../lib/kraken/librtlsdr.0.dylib', executable])
    for binary in (library, executable):
        run(['codesign', '--force', '--sign', '-', binary])
    shutil.copy2(native / 'index.html', destination / 'share/heimdall/index.html')
    shutil.copytree(prefix / 'include', destination / 'share/heimdall/include', dirs_exist_ok=True)
    for name, paths in [('rtlsdr', ['COPYING']), ('uwebsockets', ['LICENSE']), ('usockets', ['LICENSE'])]:
        for filename in paths:
            path = trees[name] / filename
            if path.is_file():
                shutil.copy2(path, destination / 'share/heimdall/licenses' / (name + '-' + filename))
    receipt = dict(schema=1, sources=sources, patchSha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                   driverPatchSha256=hashlib.sha256(driver_patch.read_bytes()).hexdigest(),
                   hardwareTested=False, suiteLicense='No repository-wide license file supplied by this pinned upstream revision.')
    (destination / 'share/heimdall/build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'Built native Kraken companion at {destination}. No receiver was opened.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'build-kraken-macos: {error}')
