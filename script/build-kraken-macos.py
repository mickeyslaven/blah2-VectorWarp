#!/usr/bin/env python3
"""Build a pinned, isolated Heimdall companion on Linux or macOS.

This explicit source build never installs a global RTL driver, runs Heimdall,
opens USB, or downloads SDRplay. Upstream components retain their own terms.
"""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
PINS = ROOT / 'config/kraken-macos-sources.json'
PATCHES = {
    'suite': ['heimdall-v2-macos.patch', 'heimdall-v2-coherent-channel-gains.patch', 'heimdall-v2-ingest-timing.patch', 'heimdall-v2-portable-cpu.patch'],
    'rtlsdr': ['kraken-librtlsdr-macos.patch'],
}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **kwargs):
    subprocess.run([str(item) for item in command], check=True, **kwargs)


def apply_source_patch(tree, patch):
    if shutil.which('patch'):
        run(['patch', '--batch', '--fuzz=0', '-p1', '-i', patch], cwd=tree)
    else:
        # The disposable source tree normally lives below VectorWarp's own
        # Git worktree.  Bind Git to a deliberately absent repository and to
        # this tree so its patch paths cannot resolve against the parent repo.
        isolated = ['git', f'--git-dir={tree / ".git-disabled"}',
                    f'--work-tree={tree}', 'apply', '--no-index']
        run([*isolated, '--check', patch], cwd=tree)
        run([*isolated, patch], cwd=tree)


def extract_sources(bundle, destination):
    """Portable Python 3.10+ extraction: source archives need no links/devices.

    Validate the entire archive before writing anything. Reject every link,
    rather than relying on version-dependent tarfile filters and link ordering.
    """
    members = bundle.getmembers()
    seen = set()
    for member in members:
        parts = Path(member.name).parts
        if (not parts or Path(member.name).is_absolute() or '..' in parts or
                '\\' in member.name or not (member.isdir() or member.isfile()) or
                member.name in seen or member.size < 0):
            raise ValueError('Unsafe source archive member: ' + member.name)
        seen.add(member.name)
    # No extract/extractall call: neither old tar semantics nor archive owner,
    # setuid bits, symlinks, hardlinks, FIFOs or devices can affect the host.
    for member in members:
        target = destination / member.name
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.extractfile(member) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o755 if member.mode & 0o111 else 0o644)


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
        extract_sources(bundle, destination)
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
    if sys.platform != 'darwin' and not sys.platform.startswith('linux'):
        parser.error('This companion build supports macOS and Linux')
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be from 1 through 32')
    macos = sys.platform == 'darwin'
    brew = os.environ.get('VECTORWARP_MACOS_BREW_PREFIX') if macos else '/usr'
    if macos and not brew:
        brew = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    brew = Path(brew)
    for dependency in ('eigen', 'libusb'):
        if macos and not (brew / 'opt' / dependency).is_dir():
            parser.error(f'Install the build dependency: brew install {dependency}')
    base = args.build_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    # Fresh per-build source and output trees prevent reapplying a patch over
    # stale experimental sources. Retain the exact tree and logs for diagnosis.
    work = Path(tempfile.mkdtemp(prefix='build-', dir=base))
    print(f'Kraken companion build: {work}', flush=True)
    sources = json.loads(PINS.read_text())
    supplied = args.sources_dir.resolve() if args.sources_dir else None
    trees = {name: source(name, spec, work, supplied) for name, spec in sources.items()}
    # Never patch a Homebrew resource or the caller's source in place.
    suite = work / 'suite'
    shutil.copytree(trees['suite'], suite)
    native = suite / 'heimdall_v2'
    shutil.copytree(trees['uwebsockets'], native / 'uWebSockets', dirs_exist_ok=True)
    shutil.copytree(trees['usockets'], native / 'uWebSockets/uSockets', dirs_exist_ok=True)
    patch = work / 'heimdall-v2-macos.patch'
    channel_gain_patch = work / 'heimdall-v2-coherent-channel-gains.patch'
    for name in PATCHES['suite']:
        staged = work / name
        shutil.copy2(ROOT / 'script/patches' / name, staged)
        apply_source_patch(suite, staged)
    driver_source = work / 'rtlsdr' if macos else suite / 'librtlsdr'
    if driver_source.exists():
        shutil.rmtree(driver_source)
    shutil.copytree(trees['rtlsdr'], driver_source)
    driver_patch = work / 'kraken-librtlsdr-macos.patch'
    shutil.copy2(ROOT / 'script/patches/kraken-librtlsdr-macos.patch', driver_patch)
    apply_source_patch(driver_source, driver_patch)
    if args.test:
        for test in ('test/macos/kraken_driver/test_biastee_error_propagation.py',
                     'test/macos/kraken/gain_oracle_test.py'):
            run([sys.executable, ROOT / test, '--source', driver_source])
        run([sys.executable, ROOT / 'test/macos/kraken/convergence_oracle_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/watchdog_oracle_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/tcp_data_send_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/tcp_data_backpressure_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/socket_lifecycle_test.py', '--source', native])
        run([sys.executable, ROOT / 'test/macos/kraken/control_reader_test.py'])
        run([sys.executable, ROOT / 'test/macos/kraken/driver_events_test.py'])
        run([sys.executable, ROOT / 'test/macos/kraken/shutdown_process_test.py'])
    prefix = work / 'driver'
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', f'-DCMAKE_PREFIX_PATH={brew}']
    driver_build = work / 'driver-build' if macos else driver_source / 'build'
    run(['cmake', '-S', driver_source, '-B', driver_build, *common,
         f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DINSTALL_UDEV_RULES=OFF',
         '-DDETACH_KERNEL_DRIVER=' + ('OFF' if macos else 'ON'), '-DWITH_RPC=OFF'])
    run(['cmake', '--build', driver_build, '--parallel', args.jobs])
    run(['cmake', '--install', driver_build])
    run(['make', 'WITH_SSL=0', f'-j{args.jobs}'], cwd=native / 'uWebSockets/uSockets')
    env = dict(os.environ, PKG_CONFIG_PATH=f'{prefix}/lib/pkgconfig:{brew}/lib/pkgconfig')
    run(['cmake', '-S', native, '-B', work / 'heimdall-build', *common], env=env)
    run(['cmake', '--build', work / 'heimdall-build', '--parallel', args.jobs])
    destination = args.output_dir.resolve()
    for directory in ('bin', 'lib/kraken', 'share/heimdall', 'share/heimdall/licenses'):
        (destination / directory).mkdir(parents=True, exist_ok=True)
    executable = destination / 'bin/heimdall'
    shutil.copy2(work / 'heimdall-build/heimdall', executable)
    artifacts = [executable]
    if macos:
        library = destination / 'lib/kraken/librtlsdr.0.dylib'
        shutil.copy2((prefix / 'lib/librtlsdr.0.dylib').resolve(), library)
        run(['install_name_tool', '-change', '@rpath/librtlsdr.0.dylib',
             '@loader_path/../lib/kraken/librtlsdr.0.dylib', executable])
        artifacts.append(library)
        for binary in artifacts:
            run(['codesign', '--force', '--sign', '-', binary])
    else:
        dynamic = subprocess.check_output(['readelf', '-d', executable], text=True)
        if 'librtlsdr' in dynamic:
            raise ValueError('Linux companion must embed the pinned static RTL driver')
    shutil.copy2(native / 'index.html', destination / 'share/heimdall/index.html')
    shutil.copytree(prefix / 'include', destination / 'share/heimdall/include', dirs_exist_ok=True)
    for name, paths in [('rtlsdr', ['COPYING']), ('uwebsockets', ['LICENSE']), ('usockets', ['LICENSE'])]:
        for filename in paths:
            path = trees[name] / filename
            if path.is_file():
                shutil.copy2(path, destination / 'share/heimdall/licenses' / (name + '-' + filename))
    pffft_license = native / 'external/pffft/LICENSE.txt'
    if not pffft_license.is_file():
        raise ValueError('Pinned Suite is missing the statically linked pffft license')
    shutil.copy2(pffft_license, destination / 'share/heimdall/licenses/pffft-LICENSE.txt')
    receipt = dict(schema=2, sources=sources, platform=sys.platform, architecture=platform.machine(),
                   protocol='MCHT-v1;MCHQ-v2', sourceManifestSha256=digest(PINS),
                   toolchain={'compiler': subprocess.check_output([os.environ.get('CXX', 'c++'), '--version'], text=True).splitlines()[0],
                              'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0]},
                   buildFlags=common + ['DETACH_KERNEL_DRIVER=' + ('OFF' if macos else 'ON')],
                   binaryReproducibilityClaimed=False,
                   patches={name: digest(ROOT / 'script/patches' / name)
                            for group in PATCHES.values() for name in group},
                   artifacts={str(p.relative_to(destination)): digest(p) for p in artifacts},
                   driverLinkage='private-dynamic' if macos else 'pinned-static',
                   patchSha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                   coherentChannelGainsPatchSha256=hashlib.sha256(channel_gain_patch.read_bytes()).hexdigest(),
                   driverPatchSha256=hashlib.sha256(driver_patch.read_bytes()).hexdigest(),
                   hardwareTested=False, suiteLicense='No repository-wide license file supplied by this pinned upstream revision.')
    (destination / 'share/heimdall/build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    sbom = {'bomFormat': 'CycloneDX', 'specVersion': '1.5', 'version': 1,
            'components': [{'type': 'library', 'name': name, 'version': item['revision'],
                            'hashes': [{'alg': 'SHA-256', 'content': item['sha256']}],
                            'externalReferences': [{'type': 'distribution', 'url': item['url']}]}
                           for name, item in sources.items()]}
    (destination / 'share/heimdall/sbom.cdx.json').write_text(json.dumps(sbom, indent=2) + '\n')
    print(f'Built native Kraken companion at {destination}. No receiver was opened.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(f'build-kraken-macos: {error}')
