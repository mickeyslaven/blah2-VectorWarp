#!/usr/bin/env python3
"""Freeze, explicitly sign, audit, and package a reviewed dual-architecture app.

No keychain discovery, notarization, installation, or credential handling occurs.
An ad-hoc result is only a local development fixture, never a distribution pass.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('standalone_packager', ROOT / 'script/package-macos-standalone.py')
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
ENTITLEMENTS = ROOT / 'packaging/macos/entitlements'
EXECUTABLES = {'bin/blah2', 'bin/blah2-gpu-worker', 'bin/node', 'python/bin/python3'}
IDENTIFIER = 'io.github.mickeyslaven.vectorwarp'


def run(*argv):
    return subprocess.check_output([str(a) for a in argv], stderr=subprocess.PIPE, text=True).strip()


def code_details(path):
    result = subprocess.run(['/usr/bin/codesign', '-dvvv', str(path)], capture_output=True, text=True, check=True)
    return result.stderr + result.stdout


def entitlement_values(path):
    result = subprocess.run(['/usr/bin/codesign', '-d', '--entitlements', ':-', str(path)], capture_output=True)
    if result.returncode:
        raise ValueError(f'cannot inspect entitlements: {path}')
    try:
        return plistlib.loads(result.stdout) if result.stdout else {}
    except plistlib.InvalidFileException as error:
        raise ValueError(f'invalid signed entitlements: {path}') from error


def read_json(path):
    return json.loads(path.read_text())


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')


def native_kind(path):
    # otool inspects all fat slices; reject mixed object types and unexpected code.
    headers = run('/usr/bin/otool', '-hv', path)
    types = {'MH_' + name for name in re.findall(r'\b(EXECUTE|DYLIB|BUNDLE|OBJECT|DYLINKER|KEXT_BUNDLE)\b', headers)}
    if len(types) != 1 or types.isdisjoint({'MH_EXECUTE', 'MH_DYLIB', 'MH_BUNDLE'}):
        raise ValueError(f'unsupported Mach-O header: {path}: {types}')
    return types.pop()


def native_inventory(app, manifests):
    contents = app / 'Contents'
    launcher = contents / 'MacOS/VectorWarp'
    if not package.macho(launcher) or native_kind(launcher) != 'MH_EXECUTE':
        raise ValueError('unexpected universal launcher')
    if set(run('/usr/bin/lipo', '-archs', launcher).split()) != set(package.ARCHES):
        raise ValueError('launcher architecture mismatch')
    objects = []
    for arch in package.ARCHES:
        runtime = contents / 'Resources/runtime' / arch
        seen = set()
        for path in sorted(runtime.rglob('*')):
            if not package.macho(path):
                continue
            relative = path.relative_to(runtime).as_posix()
            kind = native_kind(path)
            if (relative in EXECUTABLES) != (kind == 'MH_EXECUTE'):
                raise ValueError(f'unexpected native executable/type: {arch}/{relative}: {kind}')
            slices = set(run('/usr/bin/lipo', '-archs', path).split())
            if arch not in slices or slices - set(package.ARCHES) or (kind == 'MH_EXECUTE' and slices != {arch}):
                raise ValueError(f'unexpected native architectures: {arch}/{relative}: {slices}')
            seen.add(relative)
            objects.append((path, kind, relative, arch))
        if not EXECUTABLES.issubset(seen) or len(seen) != manifests[arch]['native_files']:
            raise ValueError(f'native inventory mismatch for {arch}')
    extra = [path for path in contents.rglob('*') if package.macho(path) and
             path != launcher and not any(path.is_relative_to(contents / 'Resources/runtime' / a) for a in package.ARCHES)]
    if extra:
        raise ValueError(f'unexpected native code outside runtimes: {extra[0]}')
    for path in contents.rglob('*'):
        if path.is_dir() and path.suffix in ('.app', '.framework', '.xpc', '.appex'):
            raise ValueError(f'unsupported nested code bundle: {path}')
    return objects, launcher


def validate_kit(runtime):
    kit_path = runtime / 'receiver-source/rspduo/kit.json'
    if not kit_path.is_file() or kit_path.is_symlink():
        raise ValueError('missing RSPduo source kit')
    kit = read_json(kit_path)
    if not isinstance(kit.get('sources'), dict) or not kit['sources']:
        raise ValueError('invalid RSPduo source inventory')
    for name, sha in kit['sources'].items():
        source = package.inside(kit_path.parent / name, kit_path.parent)
        if not source.is_file() or package.digest(source) != sha:
            raise ValueError(f'RSPduo source mismatch: {name}')
    core = runtime / 'bin/libblah2-capture-core.dylib'
    if package.digest(core) != kit.get('core_sha256'):
        raise ValueError('RSPduo capture core binding mismatch')
    return kit_path, kit, core


def check_input(app):
    if app.is_symlink() or not app.is_dir() or app.name != 'VectorWarp.app':
        raise ValueError('expected real VectorWarp.app')
    package.safe_tree(app)
    contents = app / 'Contents'
    info = plistlib.loads((contents / 'Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != IDENTIFIER or info.get('CFBundleExecutable') != 'VectorWarp':
        raise ValueError('unexpected app identity')
    if not re.fullmatch(r'\d+(?:\.\d+){0,2}', info.get('CFBundleVersion', '')):
        raise ValueError('invalid numeric package version')
    runtimes = {a: contents / 'Resources/runtime' / a for a in package.ARCHES}
    manifests = {a: package.audit_runtime(p, a) for a, p in runtimes.items()}
    if len({m['source_id'] for m in manifests.values()}) != 1:
        raise ValueError('source identities differ')
    if len({package.os_version(m['minimum_os']) for m in manifests.values()}) != 1:
        raise ValueError('deployment targets differ')
    notices = contents / 'Resources/ThirdPartyNotices'
    _, notice_sha = package.validated_notices(notices, manifests, runtimes)
    native_inventory(app, manifests)
    for runtime in runtimes.values():
        validate_kit(runtime)
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)
    return info, manifests, notice_sha


def output_location(source, output):
    if not output.is_absolute():
        raise ValueError('output must be an absolute path outside synced folders')
    package.separate_output(output, [source])
    if output.exists() or output.is_symlink():
        raise ValueError('output already exists')
    parent = output.parent.resolve(strict=True)
    home = Path.home().resolve()
    forbidden = (home / 'Documents', home / 'Desktop', home / 'Library/Mobile Documents',
                 home / 'Library/CloudStorage')
    if any(parent == folder or folder in parent.parents for folder in forbidden):
        raise ValueError('output is in a commonly synced folder')


def profile(relative):
    if relative == 'bin/node':
        return ENTITLEMENTS / 'node-jit.plist'
    if relative == 'bin/blah2':
        return ENTITLEMENTS / 'processor-plugins.plist'
    return None


def sign(path, identity, mode, identifier, executable=False, entitlements=None):
    args = ['/usr/bin/codesign', '--force', '--sign', identity, '--identifier', identifier]
    if mode == 'developer-id':
        args.append('--timestamp')
    if executable and mode == 'developer-id':
        args += ['--options', 'runtime']
    if entitlements:
        args += ['--entitlements', entitlements]
    run(*args, path)


def verify_code(path, mode, team, executable=False, entitlements=None):
    run('/usr/bin/codesign', '--verify', '--strict', path)
    details = code_details(path)
    fields = dict(re.findall(r'^([^=\n]+)=(.*)$', details, re.MULTILINE))
    if mode == 'developer-id':
        if fields.get('TeamIdentifier') != team or not fields.get('Timestamp'):
            raise ValueError(f'missing team/timestamp: {path}')
        if 'Authority=Developer ID Application:' not in details:
            raise ValueError(f'not a Developer ID Application signature: {path}')
    elif fields.get('TeamIdentifier') != 'not set' or 'Signature=adhoc' not in details:
        raise ValueError(f'not an ad-hoc development signature: {path}')
    hardened = bool(re.search(r'CodeDirectory [^\n]*flags=[^\n]*\bruntime\b', details))
    if executable and mode == 'developer-id' and not hardened:
        raise ValueError(f'missing hardened runtime option: {path}')
    if mode == 'ad-hoc' and hardened:
        raise ValueError(f'ad-hoc development code unexpectedly hardened: {path}')
    expected = plistlib.loads(entitlements.read_bytes()) if entitlements else {}
    if entitlement_values(path) != expected:
        raise ValueError(f'unexpected entitlements: {path}')


def verify_installer_signature_report(report, team):
    status = re.search(r'^\s*Status:\s*(.+?)\s*$', report, re.MULTILINE)
    if not status or status.group(1) not in {
            'signed by a certificate trusted by macOS',
            'signed by a certificate trusted by Mac OS X'}:
        raise ValueError('installer certificate is not reported trusted')
    leaf = re.search(r'^\s*1\.\s+(.+?)\s*$', report, re.MULTILINE)
    if not leaf or not re.fullmatch(r'Developer ID Installer: .+ \(' + re.escape(team) + r'\)', leaf.group(1)):
        raise ValueError('installer leaf certificate/team mismatch')


def finalize(args):
    if args.app.is_symlink():
        raise ValueError('input app must not be a symlink')
    source = args.app.resolve(strict=True)
    output_location(source, args.output)
    if args.ad_hoc:
        if args.application_identity or args.installer_identity or args.team_id:
            raise ValueError('ad-hoc mode does not accept distribution identities')
        mode, identity = 'ad-hoc', '-'
    else:
        if not all((args.application_identity, args.installer_identity, args.team_id)) or not re.fullmatch(r'[A-Z0-9]{10}', args.team_id):
            raise ValueError('Developer ID Application, Installer and public 10-character Team ID required')
        fingerprint = lambda value: bool(re.fullmatch(r'[0-9A-Fa-f]{40}', value))
        if not (args.application_identity.startswith('Developer ID Application:') or fingerprint(args.application_identity)) or not (
                args.installer_identity.startswith('Developer ID Installer:') or fingerprint(args.installer_identity)):
            raise ValueError('expected Developer ID certificate names or fingerprints')
        if args.application_identity == args.installer_identity:
            raise ValueError('Application and Installer identities must differ')
        mode, identity = 'developer-id', args.application_identity
    info, originals, notice_sha = check_input(source)
    output = package.fresh(args.output)
    app = output / 'VectorWarp.app'
    # ditto preserves the bundle, symlinks, permissions and xattrs; never edits input.
    run('/usr/bin/ditto', source, app)
    receipts = output / 'receipts'
    receipts.mkdir()
    for arch in package.ARCHES:
        shutil.copy2(source / 'Contents/Resources/runtime' / arch / 'standalone.json', receipts / f'original-{arch}-standalone.json')
    shutil.copy2(source / 'Contents/Resources/ThirdPartyNotices/notices.json', receipts / 'original-notices.json')
    copied_info, copied, copied_notice_sha = check_input(app)
    if copied_info != info or copied_notice_sha != notice_sha:
        raise ValueError('copied input changed')
    for attr in ('com.apple.FinderInfo', 'com.apple.ResourceFork'):
        run('/usr/bin/xattr', '-dr', attr, app)
    objects, launcher = native_inventory(app, copied)
    receipt = {'schema': 1, 'transform': 'explicit-native-signing', 'mode': mode,
               'source_id': originals['arm64']['source_id'],
               'input_app': str(source), 'input_notice_sha256': notice_sha,
               'input_runtime_manifest_sha256': {a: package.digest(receipts / f'original-{a}-standalone.json') for a in package.ARCHES},
               'candidate_entitlement_sha256': {p.stem: package.digest(p) for p in sorted(ENTITLEMENTS.glob('*.plist'))},
               'applied_entitlement_sha256': ({p.stem: package.digest(p) for p in sorted(ENTITLEMENTS.glob('*.plist'))}
                                              if mode == 'developer-id' else {}),
               'hardened_runtime_enabled': mode == 'developer-id',
               'application_identity': identity if mode == 'developer-id' else 'ad-hoc',
               'installer_identity': args.installer_identity if mode == 'developer-id' else None,
               'team_id': args.team_id if mode == 'developer-id' else None,
               'notarized': False,
               'runtime_hardened_validation': 'not-performed' if mode == 'developer-id' else 'not-applicable-ad-hoc'}
    # Libraries first; the runtime's four process executables per architecture next.
    original_kits = {arch: validate_kit(app / 'Contents/Resources/runtime' / arch)[1] for arch in package.ARCHES}
    for path, kind, relative, arch in objects:
        if kind != 'MH_EXECUTE':
            code_id = f'{IDENTIFIER}.runtime.{arch}.' + hashlib.sha256(relative.encode()).hexdigest()[:20]
            sign(path, identity, mode, code_id)
    for path, kind, relative, arch in objects:
        if kind == 'MH_EXECUTE':
            code_id = f'{IDENTIFIER}.runtime.{arch}.' + relative.replace('/', '.')
            sign(path, identity, mode, code_id, executable=True,
                 entitlements=profile(relative) if mode == 'developer-id' else None)
    for arch in package.ARCHES:
        runtime = app / 'Contents/Resources/runtime' / arch
        kit_path = runtime / 'receiver-source/rspduo/kit.json'
        kit = read_json(kit_path)
        if kit != original_kits[arch]:
            raise ValueError('RSPduo kit metadata changed during signing')
        core = runtime / 'bin/libblah2-capture-core.dylib'
        kit['core_sha256'] = package.digest(core)
        write_json(kit_path, kit)
        manifest_path = runtime / 'standalone.json'
        metadata = read_json(manifest_path)
        metadata['files'] = package.files_manifest(runtime)
        metadata['signing'] = mode
        metadata['signing_transform'] = {'schema': 1, 'original_manifest_sha256': receipt['input_runtime_manifest_sha256'][arch],
                                         'original_source_id': originals[arch]['source_id']}
        write_json(manifest_path, metadata)
    notice_path = app / 'Contents/Resources/ThirdPartyNotices/notices.json'
    notice = read_json(notice_path)
    notice['runtime_manifest_sha256'] = {a: package.digest(app / 'Contents/Resources/runtime' / a / 'standalone.json') for a in package.ARCHES}
    notice['signing_transform'] = {'schema': 1, 'original_notice_sha256': notice_sha}
    write_json(notice_path, notice)
    for arch in package.ARCHES:
        package.audit_runtime(app / 'Contents/Resources/runtime' / arch, arch)
        validate_kit(app / 'Contents/Resources/runtime' / arch)
    package.validated_notices(notice_path.parent,
                              {a: read_json(app / 'Contents/Resources/runtime' / a / 'standalone.json') for a in package.ARCHES},
                              {a: app / 'Contents/Resources/runtime' / a for a in package.ARCHES})
    sign(app, identity, mode, IDENTIFIER, executable=True)
    native_inventory(app, copied)
    for path, kind, relative, arch in objects:
        verify_code(path, mode, args.team_id, kind == 'MH_EXECUTE',
                    profile(relative) if mode == 'developer-id' and kind == 'MH_EXECUTE' else None)
    verify_code(app, mode, args.team_id, executable=True)
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)
    for arch in package.ARCHES:
        package.audit_runtime(app / 'Contents/Resources/runtime' / arch, arch)
    # Keep the app in the output folder and package the same bytes without a second copy.
    with tempfile.TemporaryDirectory(prefix='vectorwarp-pkg-', dir=output) as temp:
        temp = Path(temp)
        scripts = temp / 'scripts'; scripts.mkdir()
        shutil.copy2(ROOT / 'packaging/macos/preinstall', scripts / 'preinstall')
        (scripts / 'preinstall').chmod(0o755)
        component = temp / 'component.plist'
        component.write_bytes(plistlib.dumps([{'RootRelativeBundlePath': 'VectorWarp.app',
            'BundleIsRelocatable': False, 'BundleHasStrictIdentifier': True,
            'BundleOverwriteAction': 'upgrade'}]))
        payload = temp / 'payload'; payload.mkdir()
        app.rename(payload / app.name)
        try:
            command = ['/usr/bin/pkgbuild', '--root', payload, '--component-plist', component,
                       '--scripts', scripts, '--install-location', '/Applications',
                       '--identifier', IDENTIFIER, '--version', info['CFBundleVersion']]
            if mode == 'developer-id':
                command += ['--sign', args.installer_identity, '--timestamp']
            pkg = output / ('VectorWarp-signed.pkg' if mode == 'developer-id' else 'VectorWarp-local-ad-hoc.pkg')
            run(*command, pkg)
        finally:
            (payload / app.name).rename(app)
    if mode == 'developer-id':
        signature = run('/usr/sbin/pkgutil', '--check-signature', pkg)
        verify_installer_signature_report(signature, args.team_id)
    receipt['output_runtime_manifest_sha256'] = {a: package.digest(app / 'Contents/Resources/runtime' / a / 'standalone.json') for a in package.ARCHES}
    receipt['output_notice_sha256'] = package.digest(notice_path)
    receipt['app_launcher_sha256'] = package.digest(launcher)
    receipt['pkg_sha256'] = package.digest(pkg)
    receipt['status'] = 'signed-unnotarized' if mode == 'developer-id' else 'ad-hoc-development-only'
    write_json(receipts / 'signing-transform.json', receipt)
    print(json.dumps({'app': str(app), 'pkg': str(pkg), 'receipt': str(receipts / 'signing-transform.json'), 'status': receipt['status']}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True, help='previously audited dual-architecture VectorWarp.app')
    parser.add_argument('--output', type=Path, required=True, help='new absolute path outside synced storage')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--ad-hoc', action='store_true', help='local development only; no distribution acceptance')
    mode.add_argument('--developer-id', action='store_true', help='local identity signing; no notarization')
    parser.add_argument('--application-identity')
    parser.add_argument('--installer-identity')
    parser.add_argument('--team-id')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('macOS host required')
    finalize(args)


if __name__ == '__main__':
    main()
