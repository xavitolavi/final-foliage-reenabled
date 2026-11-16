"""Build the release folder + zip.
usage: python tools/make_release.py <version>   (e.g. v1.0)
Needs: the pristine exe backup (for the patch bytes), dll/FinalFoliage.dll, build/zz_FoliageBend_P.pak."""
import hashlib, os, shutil, struct, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import patch_wind  # noqa: E402

VERSION = sys.argv[1] if len(sys.argv) > 1 else 'v1.0'
GAME = os.environ.get('FF7R_GAME', r'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH')
# an untouched copy of the game exe (the patch bytes are derived from it; it is never shipped)
_w64 = os.path.join(GAME, r'End\Binaries\Win64')
ORIG = next((p for p in (os.environ.get('FF7R_ORIGINAL_EXE', ''),
                         os.path.join(_w64, 'ff7rebirth_.exe.windmod-original'),
                         os.path.join(_w64, 'ff7rebirth_.exe.finalfoliage-original'),
                         os.path.join(_w64, 'ff7rebirth_.exe')) if p and os.path.exists(p)), None)
assert ORIG, 'set FF7R_GAME (game folder) or FF7R_ORIGINAL_EXE (unmodified ff7rebirth_.exe)'
NAME = 'FinalFoliageReenabled_' + VERSION
OUT = os.path.join(ROOT, 'release', NAME)

orig = open(ORIG, 'rb').read()
sha = hashlib.sha256(orig).hexdigest()
assert sha.startswith('752807180c4e'), sha
p80, _, _ = patch_wind.patch(orig, 80.0)
p60, _, _ = patch_wind.patch(orig, 60.0)

pe = patch_wind.PE(orig)
t = pe.secs['.text']
cave_off = t['raw'] + patch_wind.CAVE_TEXT_OFFSET
cave_end = t['raw'] + t['rsz']
assert not any(orig[cave_off:cave_end]), 'cave not empty in original'
cave = p80[cave_off:cave_end]
# every other changed byte range is a small (offset, old, new) patch
patches = []
i = 0
while i < len(orig):
    if cave_off <= i < cave_end:
        i = cave_end; continue
    if orig[i] != p80[i]:
        j = i
        while j < len(orig) and not (cave_off <= j < cave_end) and orig[j] != p80[j]:
            j += 1
        patches.append((i, orig[i:j].hex(), p80[i:j].hex())); i = j
    else:
        i += 1
diff = [k for k in range(len(orig)) if p80[k] != p60[k]]
lim80 = struct.pack('<f', 1.0 / 8000.0)
limit_offs =[o for o in range(cave_off, cave_end - 3) if p80[o:o + 4] == lim80]
assert len(limit_offs) == 2 and all(any(o <= d < o + 4 for o in limit_offs) for d in diff), (limit_offs, diff)
print('patches:', [(hex(o), len(n) // 2) for o, _, n in patches], 'cave', hex(cave_off), len(cave), 'limits', [hex(o) for o in limit_offs])

# ---- folder
if os.path.exists(OUT):
    shutil.rmtree(OUT)
os.makedirs(os.path.join(OUT, 'mod'))
tpl = open(os.path.join(ROOT, 'tools', 'setup_template.ps1'), encoding='utf-8').read()
ptxt = ',\n'.join("    @{ Off = 0x%X; Old = '%s'; New = '%s' }" % (o, a, b) for o, a, b in patches)
ps = (tpl.replace('@VERSION@', VERSION).replace('@ORIGSHA@', sha).replace('@PATCHES@', ptxt)
      .replace('@CAVEOFF@', '0x%X' % cave_off).replace('@CAVEHEX@', cave.hex())
      .replace('@LIMITOFFS@', ', '.join('0x%X' % o for o in limit_offs)))
assert '@' not in ps.replace('@(', '').replace('@{', ''), 'unfilled placeholder'
with open(os.path.join(OUT, 'FinalFoliage-Setup.ps1'), 'w', encoding='utf-8-sig', newline='\r\n') as f:
    f.write(ps)
for name, act in (('Install.bat', 'install'), ('Uninstall.bat', 'uninstall')):
    with open(os.path.join(OUT, name), 'w', newline='\r\n') as f:
        f.write('@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%%~dp0FinalFoliage-Setup.ps1" -Action %s %%*\r\npause\r\n' % act)
shutil.copy2(os.path.join(ROOT, 'dll', 'FinalFoliage.dll'), os.path.join(OUT, 'mod'))
shutil.copy2(os.path.join(ROOT, 'build', 'zz_FoliageBend_P.pak'), os.path.join(OUT, 'mod', 'zz_FinalFoliage_P.pak'))
subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'make_default_ini.py'), os.path.join(OUT, 'mod', 'FinalFoliage.ini')])
shutil.copy2(os.path.join(ROOT, 'release', 'README.txt'), OUT)
shutil.copy2(os.path.join(ROOT, 'release', 'source_files', 'LICENSE'), os.path.join(OUT, 'LICENSE.txt'))

# ---- zip
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
