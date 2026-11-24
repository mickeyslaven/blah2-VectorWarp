#!/usr/bin/env python3
"""CI-only Fedora 43 introduction baseline; never published as a release asset.

Build the pinned preceding release unmodified. Its external packaging entrypoint
adapts only target lookup and the RPM suffix; all installed bytes and maintainer
scripts come from the clean preceding checkout. The normal upgrade harness then
installs this baseline and exercises the exact candidate package.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
PLAN = runpy.run_path(str(ROOT / 'script/artifact-plan.py'))
BASELINE = '7e872c0d26ff2d69501732c139f385e1c0a397e5'
PACKAGER_SHA256 = '294778b8dcb70f47dbb25aa2c94db986dae004cdd96feb4054493ac295bcaa47'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def request(distro, arch, candidate, tag, manifest):
    require(distro == 'fedora43' and arch == 'x86_64', 'no bootstrap for this target')
    introduced = PLAN['PLAN']['linux_distros'][distro]['introduced']
    require(introduced == '0.1.39' and candidate in (introduced, '9999.0.0'),
            'bootstrap is limited to the Fedora 43 introduction candidate')
    require(tag == 'v0.1.38' and manifest.get('version') == '0.1.38'
            and manifest.get('source_commit') == BASELINE,
            'bootstrap requires the pinned preceding published source')
    filenames = [entry.get('filename') for entry in manifest['packages']]
    require(len(filenames) == len(set(filenames)) and
            set(filenames) == PLAN['release_filenames']('0.1.38'),
            'preceding release inventory is incomplete or changed')
    require(not any(entry.get('distro') == 'fedora' and entry.get('distro_version') == '43'
                    and entry.get('arch') == arch for entry in manifest['packages']),
            'use the published prior target package when it exists')
    return dict(schema=1, distro=distro, arch=arch, candidate=candidate, tag=tag,
                sourceCommit=BASELINE, version='0.1.38', manifest=manifest)


def adapted_packager(source, checkout):
    require(hashlib.sha256(source.encode()).hexdigest() == PACKAGER_SHA256,
            'preceding packaging entrypoint differs from its reviewed bytes')
    start = source.index('case "$DISTRO" in\n')
    end = source.index('\nesac', start) + len('\nesac')
    replacements = {
        'SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)':
            'SOURCE_DIR=' + shlex.quote(str(checkout)),
        source[start:end]:
            'target=$(python3 ' + shlex.quote(str(ROOT / 'script/artifact-plan.py')) +
            ' target "$DISTRO" --format "$FORMAT" --node-arch "$NODE_ARCH"'
            ' --os-id "${ID:-}" --os-version "${VERSION_ID:-}") || exit 1\n'
            'read -r DISTRO_NAME DISTRO_VERSION CODENAME PACKAGE_ARCH <<<"$target"\n'
            '[[ $CODENAME != - ]] || CODENAME=',
        '.fc44': '.fc${DISTRO_VERSION}',
    }
    for old, new in replacements.items():
        require(source.count(old) == (2 if old == '.fc44' else 1),
                'preceding packaging adapter no longer matches the reviewed transition')
        source = source.replace(old, new)
    return source


def build(value, output):
    expected = request(value['distro'], value['arch'], value['candidate'], value['tag'], value['manifest'])
    require(value == expected, 'bootstrap request changed after resolution')
    output = output.resolve()
    require(not any(output.glob('*.rpm')), 'bootstrap output already contains a package')
    with tempfile.TemporaryDirectory(prefix='vectorwarp-fc43-baseline-') as temporary:
        directory = Path(temporary)
        checkout = directory / 'source'
        subprocess.run(['git', 'clone', '--no-checkout', '--filter=blob:none',
                        'https://github.com/mickeyslaven/blah2-VectorWarp.git', str(checkout)], check=True)
        subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', value['tag']], check=True)
        actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
        require(actual == BASELINE, 'preceding release tag moved')
        require(not subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain'],
                                           text=True).strip(), 'preceding checkout is dirty')
        original = (checkout / 'script/package-native.sh').read_text()
        adapted = directory / 'package-native.sh'
        adapted.write_text(adapted_packager(original, checkout))
        package = PLAN['native_package'](SimpleNamespace(format='rpm', distro='fedora43',
            node_arch='x64', version=value['version']), root=checkout, package_script=adapted,
            output_dir=checkout / 'build/upgrade-output')
        filename = f"vectorwarp-{value['version']}-1.fc43.x86_64.rpm"
        require(package.name == filename, 'baseline package target mismatch')
        metadata = json.loads(Path(str(package) + '.manifest.json').read_text())
        for field, wanted in dict(version=value['version'], format='rpm', distro='fedora',
                                 distro_version='43', arch='x86_64', release='1.fc43').items():
            require(metadata.get(field) == wanted, 'baseline manifest mismatch: ' + field)
        digest = hashlib.sha256(package.read_bytes()).hexdigest()
        require(metadata.get('sha256') == digest and metadata.get('size') == package.stat().st_size,
                'baseline package bytes differ from its manifest')
        gate = runpy.run_path(str(ROOT / 'script/verify-release-soak.py'))
        extracted = directory / 'extracted'
        extracted.mkdir()
        gate['unpack'](package, extracted)
        artifact = extracted / 'opt/vectorwarp/current'
        fields = PLAN['key_values'](artifact / '.vectorwarp-build')
        for field, wanted in dict(source_commit=BASELINE, build_os_id='fedora', build_os_version='43',
                                 build_arch='x86_64', backend='all', test_only='false').items():
            require(fields.get(field) == wanted, 'baseline installed identity mismatch: ' + field)
        receipt = json.loads((artifact / 'BUILD-RECEIPT.json').read_text())
        require(receipt.get('sourceCommit') == BASELINE and receipt.get('version') == value['version'],
                'baseline installed receipt mismatch')
        payload = dict(artifactSha256=gate['identity'].artifact_hash(artifact), sourceClean=True)
        require(not subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain',
                                             '--untracked-files=normal'], text=True).strip(),
                'preceding source changed during bootstrap')
        shutil.copy2(package, output / filename)
        (output / 'bootstrap-receipt.json').write_text(json.dumps({
            **value, 'baselineOnly': True, 'packagingScriptSha256': PACKAGER_SHA256,
            'adaptedPackagingScriptSha256': hashlib.sha256(adapted.read_bytes()).hexdigest(),
            'filename': filename, 'sha256': digest, **payload}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    prepare = commands.add_parser('prepare')
    for name in ('distro', 'arch', 'candidate', 'tag', 'manifest', 'output'):
        prepare.add_argument('--' + name, required=True)
    builder = commands.add_parser('build')
    builder.add_argument('--request', type=Path, required=True)
    builder.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'prepare':
        value = request(args.distro, args.arch, args.candidate, args.tag,
                        json.loads(Path(args.manifest).read_text()))
        Path(args.output).write_text(json.dumps(value, indent=2) + '\n')
    else:
        build(json.loads(args.request.read_text()), args.output)


if __name__ == '__main__':
    main()
