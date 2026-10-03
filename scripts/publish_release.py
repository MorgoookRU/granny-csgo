#!/usr/bin/env python3
"""Upload the completed APK to an explicitly selected private GitHub repo."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True, help='OWNER/REPO')
    parser.add_argument('--apk', type=Path, default=Path(
        '/workspace/granny-lab/artifacts/granny-tactical-iteration-01.apk'))
    args = parser.parse_args()
    if args.repo.count('/') != 1 or args.repo.startswith('-'):
        parser.error('--repo must be OWNER/REPO')
    metadata = json.loads((ROOT / 'releases/iteration-01.json').read_text())
    if args.apk.name != metadata['filename']:
        parser.error('APK filename differs from the prepared release')
    digest = hashlib.sha256()
    with args.apk.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    if args.apk.stat().st_size != metadata['bytes'] or digest.hexdigest() != metadata['sha256']:
        parser.error('APK size or SHA-256 differs from the prepared release')
    response = subprocess.run([
        'gh', 'repo', 'view', args.repo, '--json', 'isPrivate'
    ], check=True, capture_output=True, text=True)
    if not json.loads(response.stdout)['isPrivate']:
        parser.error('Use a private repository for this test APK')
    commit = subprocess.run([
        'git', '-C', str(ROOT), 'rev-parse', 'HEAD'
    ], check=True, capture_output=True, text=True).stdout.strip()
    subprocess.run([
        'gh', 'release', 'create', metadata['tag'], str(args.apk),
        '--repo', args.repo, '--target', commit,
        '--title', 'Granny Tactical Lab · 1', '--prerelease',
        '--notes-file', str(ROOT / 'releases/iteration-01.md')
    ], check=True)


if __name__ == '__main__':
    main()
