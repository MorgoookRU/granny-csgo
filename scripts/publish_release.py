#!/usr/bin/env python3
"""Publish a verified APK directly or through GitHub Actions."""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def git(root, *args):
    return run(['git', '-c', 'credential.helper=', '-c',
                'credential.helper=!gh auth git-credential', '-C', root, *args])


def stage_actions(apk, metadata, lab):
    build = lab / 'build'
    build.mkdir(parents=True, exist_ok=True)
    worktree = Path(tempfile.mkdtemp(prefix='release-worktree-', dir=build))
    branch = f"apk-delivery-{metadata['tag']}-{int(time.time())}"
    git(ROOT, 'worktree', 'add', '--detach', worktree, 'HEAD')
    try:
        parts = worktree / 'delivery/parts'
        parts.mkdir(parents=True, exist_ok=True)
        manifest = dict(metadata)
        manifest['parts'] = []
        chunk_size = 8 * 1024 * 1024
        count = (metadata['bytes'] + chunk_size - 1) // chunk_size
        with apk.open('rb') as source:
            index = 0
            while data := source.read(chunk_size):
                name = f'part-{index:04d}.bin'
                (parts / name).write_bytes(data)
                manifest['parts'].append({'filename': name, 'bytes': len(data),
                                          'sha256': hashlib.sha256(data).hexdigest()})
                git(worktree, 'add', parts / name)
                git(worktree, 'commit', '--quiet', '-m', f'Stage APK part {index + 1}')
                git(worktree, 'push', '--quiet', 'origin', f'HEAD:refs/heads/{branch}')
                print(f'Transferred part {index + 1}/{count}', flush=True)
                index += 1
        (worktree / 'delivery/manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        (worktree / 'delivery/READY').write_text(metadata['tag'] + '\n')
        git(worktree, 'add', 'delivery/manifest.json', 'delivery/READY')
        git(worktree, 'commit', '--quiet', '-m', 'Publish verified APK')
        git(worktree, 'push', '--quiet', 'origin', f'HEAD:refs/heads/{branch}')
        print('GitHub Actions started on:', branch, flush=True)
    finally:
        git(ROOT, 'worktree', 'remove', '--force', worktree)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True, help='OWNER/REPO')
    parser.add_argument('--iteration', default='iteration-01')
    parser.add_argument('--apk', type=Path)
    parser.add_argument('--lab', type=Path, default=Path('/workspace/granny-lab'))
    parser.add_argument('--allow-public', action='store_true')
    parser.add_argument('--via-actions', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', args.repo):
        parser.error('--repo must be OWNER/REPO')
    if not re.fullmatch(r'iteration-\d{2,}', args.iteration):
        parser.error('Invalid iteration name')
    metadata = json.loads((ROOT / 'releases' / (args.iteration + '.json')).read_text())
    apk = args.apk or args.lab / 'artifacts' / metadata['filename']
    if apk.name != metadata['filename'] or metadata['tag'] != args.iteration:
        parser.error('Artifact name differs from release metadata')
    digest = hashlib.sha256()
    with apk.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    if apk.stat().st_size != metadata['bytes'] or digest.hexdigest() != metadata['sha256']:
        parser.error('APK size or SHA-256 differs from release metadata')
    response = run(['gh', 'repo', 'view', args.repo, '--json', 'isPrivate'], capture_output=True, text=True)
    if not json.loads(response.stdout)['isPrivate'] and not args.allow_public:
        parser.error('Public delivery requires --allow-public')
    commit = run(['git', '-C', ROOT, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    existing = subprocess.run(['gh', 'release', 'view', args.iteration, '--repo', args.repo],
                              capture_output=True, text=True)
    if existing.returncode:
        run(['gh', 'release', 'create', args.iteration, '--repo', args.repo,
             '--target', commit, '--title', 'Granny Tactical Lab · ' + args.iteration.split('-')[-1],
             '--prerelease', '--notes-file', ROOT / 'releases' / (args.iteration + '.md')])
    if args.via_actions:
        remote = run(['git', '-C', ROOT, 'remote', 'get-url', 'origin'], capture_output=True, text=True).stdout.strip()
        if remote not in (f'https://github.com/{args.repo}.git', f'git@github.com:{args.repo}.git'):
            parser.error('Origin must match the selected release repository')
        stage_actions(apk, metadata, args.lab)
    else:
        run(['gh', 'release', 'upload', args.iteration, apk, '--repo', args.repo])


if __name__ == '__main__':
    main()
