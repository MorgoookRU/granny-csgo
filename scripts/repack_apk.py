#!/usr/bin/env python3
"""Build the next test APK from a previously published one.

The published APK already contains the merged original game, the patched Unity bootstrap
smali, the preserved resource table and the original ARM64 libraries. Only the mod's own
files change between iterations, so every other entry is kept byte-for-byte by Info-ZIP:

  lib/arm64-v8a/libgranny_csgo.so   rebuilt native module (stripped)
  classes6.dex                      rebuilt Java bridge/diagnostics
  AndroidManifest.xml               launcher label and versionCode

The old signature is removed, the archive is aligned and signed with the given key.
"""
import argparse
import hashlib
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path
from patch_manifest import patch

REPLACED = ['AndroidManifest.xml', 'classes6.dex', 'lib/arm64-v8a/libgranny_csgo.so']


def run(args, **kwargs):
    print('Running:', ' '.join(str(a) for a in args[:3]), '…', flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def repack(base, native, dex, iteration, tools, keystore, password, output):
    build_tools = tools / 'android-15'
    strip = tools / 'android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
    with zipfile.ZipFile(base) as source:
        names = source.namelist()
        manifest = source.read('AndroidManifest.xml')
        signature = [n for n in names if n == 'stamp-cert-sha256' or (n.startswith('META-INF/') and (n.endswith(('.SF', '.RSA', '.DSA', '.EC')) or n == 'META-INF/MANIFEST.MF'))]
        for name in REPLACED:
            assert name in names, f'{name} missing from base APK'
    manifest, changes = patch(manifest, label=f'Granny Tactical Lab · {iteration}', version_code=91 + iteration)
    print('Manifest changes:', changes, flush=True)
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        stage = temp / 'stage'
        (stage / 'lib/arm64-v8a').mkdir(parents=True)
        library = stage / 'lib/arm64-v8a/libgranny_csgo.so'
        shutil.copyfile(native, library)
        run([strip, '--strip-unneeded', library])
        shutil.copyfile(dex, stage / 'classes6.dex')
        (stage / 'AndroidManifest.xml').write_bytes(manifest)
        unsigned = temp / 'unsigned.apk'
        shutil.copyfile(base, unsigned)
        run(['zip', '-q', '-d', unsigned, *signature, *REPLACED])
        # Same compression as the published entries: DEX stored, the rest deflated.
        run(['zip', '-q', '-X', '-0', unsigned, 'classes6.dex'], cwd=stage)
        run(['zip', '-q', '-X', '-9', unsigned, 'AndroidManifest.xml', 'lib/arm64-v8a/libgranny_csgo.so'], cwd=stage)
        aligned = temp / 'aligned.apk'
        run([build_tools / 'zipalign', '-P', '16', '-f', '4', unsigned, aligned])
        output.parent.mkdir(parents=True, exist_ok=True)
        run([build_tools / 'apksigner', 'sign', '--ks', keystore, '--ks-pass', 'file:' + str(password),
             '--min-sdk-version', '24', '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true',
             '--v3-signing-enabled', 'true', '--out', output, aligned])
    run([build_tools / 'apksigner', 'verify', '--verbose', output])
    run([build_tools / 'zipalign', '-c', '-P', '16', '4', output])
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    print('APK:', output, 'bytes:', output.stat().st_size, 'sha256:', digest, flush=True)
    return digest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', type=Path, required=True, help='previous published APK')
    parser.add_argument('--native', type=Path, required=True, help='built libgranny_csgo.so')
    parser.add_argument('--dex', type=Path, required=True, help='D8 output classes.dex')
    parser.add_argument('--iteration', type=int, required=True)
    parser.add_argument('--tools', type=Path, required=True, help='folder with android-15 build-tools and android-ndk-r27c')
    parser.add_argument('--keystore', type=Path, required=True)
    parser.add_argument('--password', type=Path, required=True, help='file with the keystore password')
    parser.add_argument('--out', type=Path, required=True)
    a = parser.parse_args()
    repack(a.base, a.native, a.dex, a.iteration, a.tools, a.keystore, a.password, a.out)
