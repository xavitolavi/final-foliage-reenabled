"""Build the release folder + zip.
usage: python tools/make_release.py <version>   (e.g. v1.0)

No installer and no change to ff7rebirth_.exe. The zip mirrors the game folder (End\\...), so players copy
the "End" folder into the game folder. FinalFoliage.ini is not shipped: the DLL writes the default one on first
run, so updating the mod never overwrites a player's settings.
Needs: dll/winmm.dll (built + signed), build/zz_FoliageBend_P.pak."""
import os, shutil, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = sys.argv[1] if len(sys.argv) > 1 else 'v1.0'
NAME = 'FinalFoliageReenabled_' + VERSION
OUT = os.path.join(ROOT, 'release', NAME)
DLL = os.path.join(ROOT, 'dll', 'winmm.dll')
PAK = os.path.join(ROOT, 'build', 'zz_FoliageBend_P.pak')

# don't ship a DLL without its version metadata or signature
cmd = ("$ErrorActionPreference='Stop'; $f='%s';"
       "Write-Output ([bool]((Get-Item -LiteralPath $f).VersionInfo.ProductName));"
       "Write-Output ([bool]((Get-AuthenticodeSignature -LiteralPath $f).SignerCertificate))" % DLL.replace("'", "''"))
meta = subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True)
assert meta.stdout.split()[:2] == ['True', 'True'], (
    'winmm.dll is missing version metadata or a signature - run dll\\build.bat then tools\\sign_dll.ps1. (%r)'
    % (meta.stdout + meta.stderr))

if os.path.exists(OUT):
    shutil.rmtree(OUT)
w64 = os.path.join(OUT, 'End', 'Binaries', 'Win64')
paks = os.path.join(OUT, 'End', 'Content', 'Paks')
os.makedirs(w64); os.makedirs(paks)
shutil.copy2(DLL, os.path.join(w64, 'winmm.dll'))
shutil.copy2(PAK, os.path.join(paks, 'zz_FinalFoliage_P.pak'))
shutil.copy2(os.path.join(ROOT, 'release', 'README.txt'), OUT)
shutil.copy2(os.path.join(ROOT, 'release', 'source_files', 'LICENSE'), os.path.join(OUT, 'LICENSE.txt'))

zp = OUT + '.zip'
with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
    for dp, _, fs in os.walk(OUT):
        for fn in fs:
            full = os.path.join(dp, fn)
            z.write(full, os.path.join(NAME, os.path.relpath(full, OUT)))
print('zip:', zp, os.path.getsize(zp) // 1024, 'KB')
for dp, _, fs in os.walk(OUT):
    for fn in fs:
        full = os.path.join(dp, fn)
        print('  ', os.path.relpath(full, OUT), os.path.getsize(full))
