#!/usr/bin/env python3
"""Rebuild a staged APK inside GitHub Actions and print the release environment.

Two delivery formats are supported:

* ``delivery/<tag>/manifest.json`` with ``"mode": "patch"``: a bsdiff4 patch against an APK
  already attached to an earlier release. The base asset is downloaded with ``gh``.
* legacy ``delivery/manifest.json`` with ``"parts"``: the APK split into Git-sized chunks.

Every input and the reconstructed APK are checked against SHA-256 digests in the manifest.
Lines printed to stdout are ``NAME=value`` pairs for ``$GITHUB_ENV``.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def find_manifest(tag):
    if tag:
        return ROOT / 'delivery' / tag / 'manifest.json'
    staged = sorted((ROOT / 'delivery').glob('*/READY'))
    if staged:
        return staged[-1].parent / 'manifest.json'
    return ROOT / 'delivery' / 'manifest.json'


def restore(out, tag):
    manifest_path = find_manifest(tag)
    metadata = json.loads(manifest_path.read_text())
    apk = out / metadata['filename']
    if metadata.get('mode') == 'patch':
        import bsdiff4
        base = out / metadata['base_filename']
        subprocess.run(['gh', 'release', 'download', metadata['base_tag'], '--pattern', metadata['base_filename'],
                        '--dir', str(out), '--clobber'], check=True, stdout=sys.stderr)
        if sha256(base) != metadata['base_sha256']:
            raise SystemExit('Base APK checksum failed: ' + base.name)
        patch = manifest_path.parent / metadata['patch_filename']
        if sha256(patch) != metadata['patch_sha256']:
            raise SystemExit('Patch checksum failed: ' + patch.name)
        bsdiff4.file_patch(str(base), str(apk), str(patch))
    else:
        with apk.open('wb') as output:
            for part in metadata['parts']:
                data = (manifest_path.parent / 'parts' / part['filename']).read_bytes()
                if hashlib.sha256(data).hexdigest() != part['sha256']:
                    raise SystemExit('Part checksum failed: ' + part['filename'])
                output.write(data)
    if apk.stat().st_size != metadata['bytes'] or sha256(apk) != metadata['sha256']:
        raise SystemExit('APK checksum or size failed')
    Path(str(apk) + '.sha256').write_text(metadata['sha256'] + '  ' + apk.name + '\n')
    print('Verified:', apk.name, metadata['bytes'], metadata['sha256'], file=sys.stderr)
    number = metadata['tag'].split('-')[-1].lstrip('0') or '0'
    print('APK_PATH=' + str(apk))
    print('RELEASE_TAG=' + metadata['tag'])
    print('RELEASE_TITLE=Granny Tactical Lab · ' + number)
    print('RELEASE_NOTES=' + str(ROOT / 'releases' / (metadata['tag'] + '.md')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tag', default='')
    args = parser.parse_args()
    restore(args.out, args.tag)
