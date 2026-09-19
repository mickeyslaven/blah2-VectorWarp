#!/usr/bin/env python3
"""Build and audit local standalone macOS runtimes and a dual-architecture app.

Build hosts may use Homebrew; the resulting runtime may not link to it. This
tool does not download, install, sign with a developer identity, or publish.
Local artifacts are ad-hoc signed and are not approved for redistribution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ARCHES = ('arm64', 'x86_64')
MACH_MAGIC = {bytes.fromhex(h) for h in ('feedface', 'cefaedfe', 'feedfacf',
             'cffaedfe', 'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}
SYSTEM = ('/usr/lib/', '/System/Library/')


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True, stderr=subprocess.PIPE).strip()


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACH_MAGIC


def inside(path, root):
    path.resolve(strict=True).relative_to(root.resolve(strict=True))
    return path


def fresh(path):
    path = path.absolute()
    if path.exists() or path.is_symlink():
        raise ValueError(f'refusing to replace existing output: {path}')
    path.mkdir(parents=True)
    return path


def separate_output(output, inputs):
    output = output.resolve()
    for source in inputs:
        source = source.resolve()
        if output == source or source in output.parents or output in source.parents:
            raise ValueError('output and input trees must not overlap')


def commands(path):
    text = run('/usr/bin/otool', '-l', path)
    rpaths = re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', text)
    ids = re.findall(r'cmd LC_ID_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset', text)
    minimum = re.findall(r'cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform \S+\s+minos ([\d.]+)', text)
    minimum += re.findall(r'cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)', text)
    links = []
    for line in run('/usr/bin/otool', '-L', path).splitlines()[1:]:
        if ' (compatibility version ' in line:
            name = line.strip().split(' (compatibility version ', 1)[0]
            if name not in ids:
                links.append(name)
    return links, rpaths, ids, minimum


def safe_tree(root):
    for path in root.rglob('*'):
        if path.is_symlink():
            if os.path.isabs(os.readlink(path)):
                raise ValueError(f'absolute symlink: {path.relative_to(root)}')
            inside(path, root)
        elif not (path.is_dir() or path.is_file()):
            raise ValueError(f'special file: {path.relative_to(root)}')


def files_manifest(root):
    result = {}
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root).as_posix()
        if relative == 'standalone.json':
            continue
        if path.is_symlink():
            result[relative] = {'symlink': os.readlink(path)}
        elif path.is_file():
            result[relative] = {'sha256': digest(path), 'mode': path.stat().st_mode & 0o777}
    return result


class Relocator:
    def __init__(self, root, arch, executable_dirs):
        self.root, self.arch = root, arch
        self.executable_dirs = executable_dirs
        self.originals = {}
        self.destinations = {}
        self.dependencies = set()
        self.inherited = {}

    def register(self, source, target):
        source = source.resolve(strict=True)
        self.originals[target] = source
        self.destinations[source] = target

    def copy_tree(self, source, target, ignore=None):
        shutil.copytree(source, target, symlinks=True, ignore=ignore)
        for path in target.rglob('*'):
            if macho(path):
                self.register(source / path.relative_to(target), path)

    def copy_native(self, source, target):
        source = source.resolve(strict=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        target.chmod(target.stat().st_mode | 0o200)
        self.register(source, target)
        return target

    def expanded_rpaths(self, owner, rpaths):
        result = []
        for value in rpaths:
            if value == '@loader_path':
                result.append(owner.parent)
            elif value.startswith('@loader_path/'):
                result.append(owner.parent / value[len('@loader_path/'):])
            elif value == '@executable_path':
                result += self.executable_dirs
            elif value.startswith('@executable_path/'):
                result += [p / value[len('@executable_path/'):] for p in self.executable_dirs]
            elif value.startswith('/'):
                result.append(Path(value))
            else:
                raise ValueError(f'unsupported runpath: {value}')
        return result

    def resolve(self, link, owner, rpaths, inherited=()):
        def expand(value):
            if value == '@loader_path':
                return [owner.parent]
            if value == '@executable_path':
                return list(self.executable_dirs)
            if value.startswith('@loader_path/'):
                return [owner.parent / value[len('@loader_path/'):]]
            if value.startswith('@executable_path/'):
                return [folder / value[len('@executable_path/'):] for folder in self.executable_dirs]
            if value.startswith('/'):
                return [Path(value)]
            return []
        candidates = expand(link)
        if link.startswith('@rpath/'):
            candidates += [p / link[len('@rpath/'):] for p in
                           [*self.expanded_rpaths(owner, rpaths), *inherited]]
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        raise ValueError(f'unresolved dependency {link} from {owner.name}')

    def relocate(self):
        done = set()
        while pending := [p for p in self.originals if p not in done]:
            for target in pending:
                source = self.originals[target]
                links, rpaths, ids, _ = commands(source)
                inherited = self.inherited.get(source, [])
                descendant_paths = self.expanded_rpaths(source, rpaths) + inherited
                args = []
                for link in links:
                    if link.startswith(SYSTEM):
                        continue
                    dependency = self.resolve(link, source, rpaths, inherited)
                    if 'sdrplay' in dependency.name.lower() or dependency.name == 'heimdall':
                        raise ValueError('proprietary SDK/Heimdall payload must not be bundled')
                    destination = self.destinations.get(dependency)
                    if destination is None:
                        # Content-derived names avoid collisions and private build paths.
                        destination = self.root / 'lib' / f'{digest(dependency)[:12]}-{dependency.name}'
                        self.copy_native(dependency, destination)
                        self.dependencies.add(dependency)
                    known = self.inherited.setdefault(dependency, [])
                    known += [p for p in descendant_paths if p not in known]
                    relative = os.path.relpath(destination, target.parent)
                    args += ['-change', link, '@loader_path/' + relative]
                for rpath in dict.fromkeys(rpaths):
                    args += ['-delete_rpath', rpath]
                if ids:
                    args += ['-id', '@loader_path/' + target.name]
                if args:
                    run('/usr/bin/install_name_tool', *args, target)
                done.add(target)
        for path in self.originals:
            run('/usr/bin/codesign', '--force', '--sign', '-', path)


def native_audit(root, arch, minimum_os):
    safe_tree(root)
    count = 0
    for path in root.rglob('*'):
        if not macho(path):
            continue
        count += 1
        slices = run('/usr/bin/lipo', '-archs', path).split()
        if arch not in slices:
            raise ValueError(f'wrong architecture: {path.relative_to(root)}: {slices}')
        links, rpaths, _, minimums = commands(path)
        if rpaths:
            raise ValueError(f'unexpected rpath: {path.relative_to(root)}: {rpaths}')
        def version(value):
            return tuple((list(map(int, value.split('.'))) + [0, 0, 0])[:3])
        if not minimums or any(version(value) > version(minimum_os) for value in minimums):
            raise ValueError(f'unsupported deployment target: {path.relative_to(root)}: {minimums}')
        for link in links:
            if link.startswith(SYSTEM):
                continue
            if not link.startswith('@loader_path/'):
                raise ValueError(f'non-local dependency: {path.relative_to(root)}: {link}')
            inside(path.parent / link[len('@loader_path/'):], root)
        run('/usr/bin/codesign', '--verify', '--strict', path)
    if not count:
        raise ValueError('runtime contains no native executables')
    return count


def os_version(value):
    return tuple((list(map(int, value.split('.'))) + [0, 0, 0])[:3])


def stage(args):
    revision = run('git', '-C', ROOT, 'rev-parse', 'HEAD')
    if args.source_id != revision:
        raise ValueError('--source-id must match the checked-out full commit')
    dirty = bool(run('git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=normal'))
    if dirty and not args.allow_dirty:
        raise ValueError('source tree is dirty; --allow-dirty permits local development staging only')
    source_id = revision
    if dirty:
        identity = hashlib.sha256()
        paths = run('git', '-C', ROOT, 'ls-files', '--cached', '--others', '--exclude-standard', '-z').split('\0')
        for name in sorted(set(filter(None, paths))):
            source = ROOT / name
            identity.update(name.encode() + b'\0')
            if source.is_symlink():
                identity.update(os.readlink(source).encode())
            elif source.is_file():
                identity.update(digest(source).encode())
            else:
                identity.update(b'deleted')
        source_id = identity.hexdigest()
    artifact = args.artifact.resolve(strict=True)
    if (artifact / 'bin/heimdall').exists() or any(artifact.glob('bin/*rspduo*.dylib')):
        raise ValueError('use a fresh open-SDK artifact without Heimdall or SDRplay binaries')
    python_info = json.loads(run(args.python, '-I', '-c',
        'import json,sys,sysconfig;print(json.dumps({"prefix":sys.base_prefix,"stdlib":sysconfig.get_path("stdlib"),"version":f"{sys.version_info.major}.{sys.version_info.minor}"}))'))
    separate_output(args.output, [artifact, Path(python_info['prefix']), args.node.parent])
    root = fresh(args.output)
    relocator = Relocator(root, args.arch, [artifact / 'bin', args.node.resolve().parent,
                                         args.python.resolve().parent])
    for name in ('bin', 'api', 'html', 'config', 'script', 'receiver-source'):
        if (artifact / name).is_dir():
            relocator.copy_tree(artifact / name, root / name)
    shutil.copy2(artifact / 'LICENSE', root / 'LICENSE')
    relocator.copy_native(args.node, root / 'bin/node')
    relocator.dependencies.add(args.node.resolve())
    # Homebrew's bin/python is a launcher that posix_spawns the framework app.
    # Bundle the real interpreter so it does not retain that lazy path dependency.
    interpreter = Path(python_info['prefix']) / 'Resources/Python.app/Contents/MacOS/Python'
    if not interpreter.is_file():
        interpreter = args.python
    relocator.copy_native(interpreter, root / 'python/bin/python3')
    relocator.dependencies.add(interpreter.resolve())
    relocator.copy_tree(Path(python_info['stdlib']), root / 'python/lib' / ('python' + python_info['version']),
        ignore=shutil.ignore_patterns('site-packages', '__pycache__', 'test', 'tests', 'idlelib', 'tkinter', 'ensurepip', 'config-*'))
    for name in ('vectorwarp-standalone', 'vectorwarp-macos'):
        target = root / 'script' / name
        shutil.copy2(ROOT / 'packaging/macos/vectorwarp-standalone', target)
        target.chmod(0o755)
    if args.moltenvk:
        implementation = relocator.copy_native(args.moltenvk, root / 'lib/libMoltenVK.dylib')
        relocator.dependencies.add(args.moltenvk.resolve())
        icd = root / 'share/vulkan/icd.d/MoltenVK_icd.json'
        icd.parent.mkdir(parents=True)
        icd.write_text(json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'library_path': os.path.relpath(implementation, icd.parent),
            'api_version': '1.2.0', 'is_portability_driver': True}}, indent=2) + '\n')
    if (root / 'bin/blah2-gpu-worker').exists() and not args.moltenvk:
        raise ValueError('GPU artifact requires explicit --moltenvk implementation')
    relocator.relocate()
    # Local adapter ABI receipt must bind to the relocated, final signed core.
    kit = root / 'receiver-source/rspduo/kit.json'
    if kit.is_file():
        value = json.loads(kit.read_text())
        for name, expected in value['sources'].items():
            if digest(inside(kit.parent / name, kit.parent)) != expected:
                raise ValueError(f'local adapter source changed: {name}')
        value['core_sha256'] = digest(root / 'bin/libblah2-capture-core.dylib')
        kit.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    count = native_audit(root, args.arch, args.minimum_os)
    # Preserve exact dependency names for local audit, never absolute host paths.
    components = []
    for source in sorted(relocator.dependencies):
        parts = source.parts
        if 'Cellar' in parts:
            index = parts.index('Cellar')
            components.append({'formula': parts[index + 1], 'version': parts[index + 2],
                               'file': '/'.join(parts[index + 3:]), 'sha256': digest(source)})
        else:
            components.append({'file': source.name, 'sha256': digest(source)})
    metadata = {'schema': 1, 'arch': args.arch, 'source_id': source_id,
        'source_revision': revision, 'source_dirty': dirty,
        'minimum_os': args.minimum_os, 'distribution': 'local-development-only',
        'redistribution_status': 'not-reviewed-not-approved', 'signing': 'ad-hoc',
        'native_files': count, 'dependencies': components, 'files': files_manifest(root)}
    (root / 'standalone.json').write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n')
    print(json.dumps({key: metadata[key] for key in ('arch', 'source_id', 'native_files', 'signing')}))


def audit_runtime(root, arch=None):
    root = root.resolve(strict=True)
    metadata = json.loads((root / 'standalone.json').read_text())
    if metadata.get('schema') != 1 or metadata.get('arch') not in ARCHES:
        raise ValueError('invalid standalone metadata')
    if arch and metadata['arch'] != arch:
        raise ValueError('runtime architecture does not match its destination')
    if files_manifest(root) != metadata['files']:
        raise ValueError('runtime files changed since staging')
    native_audit(root, metadata['arch'], metadata['minimum_os'])
    return metadata


def assemble(args):
    sources = {arch: Path(getattr(args, arch)).resolve(strict=True) for arch in ARCHES}
    separate_output(args.output, sources.values())
    manifests = {arch: audit_runtime(path, arch) for arch, path in sources.items()}
    if len({data['source_id'] for data in manifests.values()}) != 1:
        raise ValueError('both architectures must come from the same source identity')
    if len({os_version(data['minimum_os']) for data in manifests.values()}) != 1:
        raise ValueError('both architectures must use the same deployment target')
    minimum = manifests['arm64']['minimum_os']
    output = fresh(args.output)
    app = output / 'VectorWarp.app'
    contents = app / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    for arch, path in sources.items():
        shutil.copytree(path, contents / 'Resources/runtime' / arch, symlinks=True)
    launcher = contents / 'MacOS/VectorWarp'
    run('/usr/bin/clang', '-Wall', '-Wextra', '-Werror', '-arch', 'arm64', '-arch', 'x86_64',
        '-mmacosx-version-min=' + minimum, ROOT / 'packaging/macos/launcher.c', '-o', launcher)
    info = {'CFBundleExecutable': 'VectorWarp', 'CFBundleIdentifier': 'io.github.mickeyslaven.vectorwarp',
        'CFBundleName': 'VectorWarp', 'CFBundlePackageType': 'APPL', 'CFBundleVersion': args.version,
        'CFBundleShortVersionString': args.version, 'LSMinimumSystemVersion': minimum,
        'LSUIElement': True, 'NSHighResolutionCapable': True}
    with (contents / 'Info.plist').open('wb') as stream:
        plistlib.dump(info, stream)
    # Runtime executables are already signed; seal the launcher/app last.
    run('/usr/bin/codesign', '--force', '--sign', '-', app)
    run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)
    if set(run('/usr/bin/lipo', '-archs', launcher).split()) != set(ARCHES):
        raise ValueError('launcher must contain both architectures')
    # The only script blocks replacement of a running app. No login item,
    # process termination, capture or shared-state modification is performed.
    component = output / 'component.plist'
    with component.open('wb') as stream:
        plistlib.dump([{'RootRelativeBundlePath': 'VectorWarp.app',
                       'BundleIsRelocatable': False, 'BundleHasStrictIdentifier': True,
                       'BundleOverwriteAction': 'upgrade'}], stream)
    scripts = output / 'installer-scripts'
    scripts.mkdir()
    shutil.copy2(ROOT / 'packaging/macos/preinstall', scripts / 'preinstall')
    (scripts / 'preinstall').chmod(0o755)
    payload = output / 'installer-root'
    payload.mkdir()
    staged_app = payload / app.name
    app.rename(staged_app)
    try:
        run('/usr/bin/pkgbuild', '--root', payload, '--component-plist', component, '--scripts', scripts,
            '--install-location', '/Applications', '--identifier', info['CFBundleIdentifier'],
            '--version', args.version, output / 'VectorWarp-universal-local.pkg')
    finally:
        staged_app.rename(app)
        payload.rmdir()
    component.unlink()
    shutil.rmtree(scripts)
    print(json.dumps({'app': str(app), 'installer': str(output / 'VectorWarp-universal-local.pkg'),
                      'source_id': manifests['arm64']['source_id'], 'signing': 'ad-hoc',
                      'redistribution_status': 'not-reviewed-not-approved'}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    stage_parser = sub.add_parser('stage')
    for name in ('artifact', 'node', 'python', 'output'):
        stage_parser.add_argument('--' + name, type=Path, required=True)
    stage_parser.add_argument('--arch', choices=ARCHES, required=True)
    stage_parser.add_argument('--source-id', required=True)
    stage_parser.add_argument('--minimum-os', default='15.0')
    stage_parser.add_argument('--moltenvk', type=Path)
    stage_parser.add_argument('--allow-dirty', action='store_true',
                              help='record a working-source digest for local development only')
    audit_parser = sub.add_parser('audit')
    audit_parser.add_argument('--runtime', type=Path, required=True)
    assemble_parser = sub.add_parser('assemble')
    for name in (*ARCHES, 'output'):
        assemble_parser.add_argument('--' + name, type=Path, required=True)
    assemble_parser.add_argument('--version', default='0.1.7')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('macOS host required')
    if args.command == 'stage':
        if not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', args.source_id):
            parser.error('source identity must be a full revision or source digest')
        if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', args.minimum_os):
            parser.error('minimum OS must be numeric')
        stage(args)
    elif args.command == 'assemble':
        if not re.fullmatch(r'\d+(?:\.\d+){0,2}', args.version):
            parser.error('package version must contain one to three numbers')
        assemble(args)
    else:
        metadata = audit_runtime(args.runtime)
        print(json.dumps({'accepted': True, 'arch': metadata['arch'], 'source_id': metadata['source_id']}))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        raise SystemExit(f'package-macos-standalone: {error}')
