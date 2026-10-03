#!/usr/bin/env python3
"""Build a private standalone test APK from local original game files.

All extracted game code/assets and build products stay in the supplied lab.
Only the Unity Activity's onCreate receives a call to our Android overlay.
"""
import argparse
import os
import re
import secrets
import shutil
import subprocess
import zipfile
from pathlib import Path
from patch_manifest import patch, diagnostic_launcher
from patch_unity_resources import patch_unity_resources

ROOT=Path(__file__).resolve().parents[1]

def run(args):
    print('Running:',Path(str(args[0])).name,flush=True)
    subprocess.run([str(a) for a in args],check=True)

def build(lab: Path, iteration: int):
    tools=lab/'tools';build=lab/'build';generated=build/'generated'
    changes,total=patch_unity_resources(lab/'decoded-base')
    print('Unity resource lookup patches:',changes,'total:',total,flush=True)
    activity=lab/'decoded-base/smali_classes4/com/unity3d/player/UnityPlayerActivity.smali'
    source=activity.read_text()
    hook='    invoke-static {p0}, Lorg/modlab/granny/ModOverlay;->attach(Landroid/app/Activity;)V\n\n'
    if hook not in source:
        start=source.index('.method protected onCreate(Landroid/os/Bundle;)V')
        end=source.index('.end method',start)
        body=source[start:end]
        assert body.count('    return-void')==1
        source=source[:start]+body.replace('    return-void',hook+'    return-void')+source[end:]
        activity.write_text(source)
    startup='    invoke-static {p0}, Lorg/modlab/granny/CrashJournal;->unityStarting(Landroid/content/Context;)V\n\n'
    if iteration >= 3 and startup not in source:
        source=activity.read_text();start=source.index('.method protected onCreate(Landroid/os/Bundle;)V')
        position=source.index('    .locals 2',start)+len('    .locals 2')
        activity.write_text(source[:position]+'\n\n'+startup+source[position:])
    classes=build/'java/classes';dex=build/'java/dex'
    shutil.rmtree(classes,ignore_errors=True);shutil.rmtree(dex,ignore_errors=True)
    classes.mkdir(parents=True,exist_ok=True);dex.mkdir(parents=True,exist_ok=True)
    android=tools/'android-35/android.jar'
    run(['java','com.sun.tools.javac.Main','-source','8','-target','8','-Xlint:-options','-encoding','UTF-8','-classpath',android,'-d',classes,
         *sorted((ROOT/'android/org/modlab/granny').glob('*.java')),generated/'Weapons.java'])
    classfiles=sorted(classes.rglob('*.class'))
    run(['java','-cp',tools/'android-15/lib/d8.jar','com.android.tools.r8.D8','--lib',android,'--min-api','24','--output',dex,*classfiles])
    raw=build/'game-rebuilt.apk'
    run(['java','-jar',tools/'apktool.jar','build','--output',raw,lab/'decoded-base'])
    stripped=build/'stripped';stripped.mkdir(exist_ok=True)
    libraries=[]
    for source in [build/'native-shadow/libgranny_csgo.so',build/'native-shadow/shadowhook/libshadowhook.so',build/'native-shadow/shadowhook/libshadowhook_nothing.so']:
        library=stripped/source.name;shutil.copyfile(source,library);libraries.append(library)
        run([tools/'android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip','--strip-unneeded',library])
    unsigned=build/'iteration-01-unsigned.apk'
    with zipfile.ZipFile(raw) as game,zipfile.ZipFile(lab/'xapk/config.arm64_v8a.apk') as native,zipfile.ZipFile(unsigned,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as output:
        for entry in game.infolist():
            name=entry.filename
            if name=='stamp-cert-sha256' or (name.startswith('META-INF/') and (name.endswith(('.SF','.RSA','.DSA','.EC')) or name=='META-INF/MANIFEST.MF')):continue
            data=game.read(entry)
            if name=='AndroidManifest.xml':data,changes=patch(data,label=f'Granny Tactical Lab · {iteration}',version_code=91+iteration);print('Manifest changes:',changes,flush=True)
            if name=='AndroidManifest.xml' and iteration >= 3:data=diagnostic_launcher(data)
            output.writestr(entry,data)
        for entry in native.infolist():
            if entry.filename.startswith('lib/') and entry.filename.endswith('.so'):output.writestr(entry,native.read(entry))
        for library in libraries:output.write(library,'lib/arm64-v8a/'+library.name)
        output.write(dex/'classes.dex','classes6.dex',compress_type=zipfile.ZIP_STORED)
        for asset in (generated/'assets').rglob('*'):
            if asset.is_file():output.write(asset,'assets/'+str(asset.relative_to(generated/'assets')),compress_type=zipfile.ZIP_STORED)
    aligned=build/'iteration-01-aligned.apk'
    run([tools/'android-15/zipalign','-P','16','-f','4',unsigned,aligned])
    private=lab/'private';private.mkdir(mode=0o700,exist_ok=True)
    password=private/'signing-password';keystore=private/'granny-test.p12'
    if not password.exists():password.write_text(secrets.token_urlsafe(32));password.chmod(0o600)
    if not keystore.exists():
        keytool=shutil.which('keytool')
        command=[keytool] if keytool else ['java','sun.security.tools.keytool.Main']
        run(command+['-genkeypair','-alias','granny-lab','-keyalg','RSA','-keysize','2048','-validity','3650','-dname','CN=Granny Tactical Lab, O=Private Test, C=XX',
                     '-keystore',keystore,'-storepass:file',password,'-keypass:file',password])
        keystore.chmod(0o600)
    artifacts=lab/'artifacts';artifacts.mkdir(exist_ok=True)
    final=artifacts/f'granny-tactical-iteration-{iteration:02d}.apk'
    run([tools/'android-15/apksigner','sign','--ks',keystore,'--ks-key-alias','granny-lab','--ks-pass','file:'+str(password),
         '--min-sdk-version','24','--v1-signing-enabled','true','--v2-signing-enabled','true','--out',final,aligned])
    run([tools/'android-15/apksigner','verify','--verbose',final])
    run([tools/'android-15/zipalign','-c','-P','16','4',final])
    print('APK:',final,'bytes:',final.stat().st_size,flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--lab',type=Path,default=Path('/workspace/granny-lab'));parser.add_argument('--iteration',type=int,default=1);args=parser.parse_args();build(args.lab,args.iteration)
