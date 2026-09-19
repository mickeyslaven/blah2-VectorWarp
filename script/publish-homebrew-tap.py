#!/usr/bin/env python3
"""Publish a tested tap from trusted main CI only; never force or rewrite history."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

SOURCE_REPO = 'mickeyslaven/blah2-VectorWarp'
SOURCE_REMOTE = f'https://github.com/{SOURCE_REPO}.git'
TAP_REMOTE = 'git@github.com:mickeyslaven/homebrew-vectorwarp.git'
RECEIPT = 'homebrew-release.json'
FILES = ('Formula/vectorwarp.rb', 'Formula/vectorwarp-heimdall.rb', 'README.md')
SHA = re.compile(r'[0-9a-f]{40}')
DIGEST = re.compile(r'[0-9a-f]{64}')


def command(args, cwd=None, check=True):
    return subprocess.run(args, cwd=cwd, check=check, text=True, capture_output=True)


def context(env, expected):
    if (env.get('GITHUB_ACTIONS') != 'true' or
            env.get('GITHUB_REPOSITORY') != SOURCE_REPO or
            env.get('GITHUB_REF') != 'refs/heads/main' or
            env.get('GITHUB_EVENT_NAME') not in ('push', 'workflow_dispatch') or
            env.get('GITHUB_SHA') != expected or not SHA.fullmatch(expected)):
        raise ValueError('Publication requires the exact trusted main workflow SHA.')


def version_key(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d+\.\d+\.\d+', value):
        raise ValueError('Expected an explicit numeric Homebrew base version.')
    return tuple(map(int, value.split('.')))


def read_receipt(root):
    path = root / RECEIPT
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 65536:
        raise ValueError('Missing or unsafe publication receipt.')
    data = json.loads(path.read_text())
    commit = data.get('source_commit', '')
    if not isinstance(commit, str) or not SHA.fullmatch(commit):
        raise ValueError('Receipt source commit is invalid.')
    if data.get('source_url') != f'https://github.com/{SOURCE_REPO}/archive/{commit}.tar.gz':
        raise ValueError('Receipt must identify the immutable source archive.')
    digest = data.get('source_sha256', '')
    if not isinstance(digest, str) or not DIGEST.fullmatch(digest):
        raise ValueError('Receipt archive checksum is invalid.')
    version_key(data.get('version'))
    if type(data.get('revision')) is not int or data['revision'] < 1:
        raise ValueError('Receipt revision must be a positive integer.')
    if set(data.get('files', {})) != set(FILES):
        raise ValueError('Receipt must cover exactly the managed tap files.')
    for name in FILES:
        file = root / name
        if file.is_symlink() or file.parent.is_symlink() or not file.is_file():
            raise ValueError(f'Unsafe or missing tap file: {name}')
        if file.stat().st_size > 1024 * 1024:
            raise ValueError('Unexpectedly large tap file.')
        if hashlib.sha256(file.read_bytes()).hexdigest() != data['files'][name]:
            raise ValueError(f'Tap file does not match its receipt: {name}')
    return data


def validate_update(candidate, previous, is_ancestor):
    if previous is None:
        return 'publish'
    if candidate['source_commit'] == previous['source_commit']:
        if candidate != previous:
            raise ValueError('Same source SHA has different publication metadata.')
        return 'unchanged'
    if not is_ancestor(previous['source_commit'], candidate['source_commit']):
        raise ValueError('Refusing a non-ancestor source replacement.')
    new_version, old_version = version_key(candidate['version']), version_key(previous['version'])
    if new_version < old_version or (new_version == old_version and
                                   candidate['revision'] <= previous['revision']):
        raise ValueError('Refusing a version or revision rollback.')
    return 'publish'


def current_main(expected):
    result = command(['git', 'ls-remote', SOURCE_REMOTE, 'refs/heads/main']).stdout.split()
    if result != [expected, 'refs/heads/main']:
        raise ValueError('Source main advanced; let its own tested run publish.')


def publish(source, files, work, expected):
    context(os.environ, expected)
    candidate = read_receipt(files)
    if candidate['source_commit'] != expected:
        raise ValueError('Tested artifact does not match the workflow commit.')
    if any(p.is_symlink() for p in files.rglob('*')):
        raise ValueError('Symlinks are not allowed in the tested tap artifact.')
    actual_files = {p.relative_to(files).as_posix() for p in files.rglob('*') if not p.is_dir()}
    if actual_files != set(FILES) | {RECEIPT}:
        raise ValueError('Unexpected files in the tested tap artifact.')
    if command(['git', 'rev-parse', 'HEAD'], source).stdout.strip() != expected:
        raise ValueError('Publisher checkout differs from the tested commit.')
    current_main(expected)
    if work.exists():
        raise ValueError('Publication needs a fresh task-owned working directory.')
    command(['git', 'clone', TAP_REMOTE, str(work)])
    branch = command(['git', 'show-ref', '--verify', '--quiet', 'refs/remotes/origin/main'], work, check=False)
    if branch.returncode == 0:
        command(['git', 'checkout', 'main'], work)
    elif not command(['git', 'ls-remote', '--heads', TAP_REMOTE]).stdout.strip():
        # Empty public tap: establish only the local unborn branch before the first bot commit.
        command(['git', 'symbolic-ref', 'HEAD', 'refs/heads/main'], work)
    else:
        raise ValueError('Existing tap has branches but no main; refusing to replace its history.')

    def ancestor(old, new):
        return command(['git', 'merge-base', '--is-ancestor', old, new], source, check=False).returncode == 0

    for attempt in range(3):
        previous = read_receipt(work) if (work / RECEIPT).exists() else None
        if previous is None and any((work / name).exists() for name in FILES if name.startswith('Formula/')):
            raise ValueError('Existing formulas lack reviewed publication metadata.')
        if validate_update(candidate, previous, ancestor) == 'unchanged':
            print('Tap already contains this exact tested source and files.')
            return
        current_main(expected)
        for name in (*FILES, RECEIPT):
            target = work / name
            if target.is_symlink() or target.parent.is_symlink():
                raise ValueError('Refusing a symlink in the destination tap.')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(files / name, target)
        command(['git', 'add', '--', *FILES, RECEIPT], work)
        command(['git', '-c', 'user.name=github-actions[bot]', '-c',
                 'user.email=41898282+github-actions[bot]@users.noreply.github.com',
                 'commit', '-m', f"VectorWarp {candidate['version']}_{candidate['revision']} ({expected[:12]})"], work)
        current_main(expected)
        pushed = command(['git', 'push', 'origin', 'HEAD:refs/heads/main'], work, check=False)
        if pushed.returncode == 0:
            published = command(['git', 'rev-parse', 'HEAD'], work).stdout.strip()
            remote = command(['git', 'ls-remote', TAP_REMOTE, 'refs/heads/main']).stdout.split()
            if remote != [published, 'refs/heads/main']:
                raise ValueError('Tap changed after publication; inspect remote state.')
            print(f'Published tested source {expected} as tap commit {published}.')
            return
        # Only this ephemeral clone is reset. Never force-push or rewrite remote history.
        command(['git', 'fetch', '--no-tags', 'origin', 'main'], work)
        command(['git', 'reset', '--hard', 'FETCH_HEAD'], work)
    raise ValueError('Tap push failed after three checked fast-forward attempts.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-repo', type=Path, required=True)
    parser.add_argument('--tap-files', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--expected-commit', required=True)
    args = parser.parse_args()
    publish(args.source_repo.resolve(), args.tap_files.resolve(), args.work_dir.resolve(), args.expected_commit)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Homebrew publication refused: {error}')
