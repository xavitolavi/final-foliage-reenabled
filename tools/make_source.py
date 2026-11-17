"""Collect the files needed to rebuild Final Foliage Reenabled into release/FinalFoliageReenabled_source
(the folder to publish on GitHub). Game files, extracted shaders, logs and third-party binaries are not included."""
import os, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'release', 'FinalFoliageReenabled_source')
FILES = [
    'dll/FoliageBend.cpp', 'dll/foliage.inc', 'dll/rustle.inc', 'dll/interact.inc', 'dll/proxy_wind.inc', 'dll/build.bat', 'dll/version.rc',
    'dll/winmm.def', 'dll/winmm_proxy.asm', 'dll/winmm_proxy.inc', 'dll/windpatch_data.inc',
    'tools/gen_winmm_proxy.py', 'tools/gen_windpatch.py',
    'tools/sign_dll.ps1', 'tools/FinalFoliage-codesign.cer',
    'tools/shaderarc.py', 'tools/extract_shaders.py', 'tools/bend_patch.py', 'tools/build_bend.py', 'tools/make_archive.py',
    'tools/dxasm/dxasm.cpp',
    'patch_wind.py',
    'tools/make_release.py', 'tools/make_default_ini.py', 'tools/adopt_ini.py', 'tools/make_source.py',
    'release/README.txt',
]
os.makedirs(OUT, exist_ok=True)
for e in os.listdir(OUT):          # empty it (keep the folder itself, it may be open somewhere)
    if e == '.git':
        continue
    pth = os.path.join(OUT, e)
    shutil.rmtree(pth) if os.path.isdir(pth) else os.remove(pth)
for f in FILES:
    dst = os.path.join(OUT, f)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.join(ROOT, f), dst)
    if f.endswith('.py'):   # no machine-specific game path in the published copy
        s = open(dst, encoding='utf-8').read()
        s = s.replace(r'C:\Program Files (x86)\Steam\steamapps\common', r'C:\Program Files (x86)\Steam\steamapps\common')
        s = s.replace('EXE = r"C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY VII REBIRTH\\End\\Binaries\\Win64\\ff7rebirth_.exe"',
                      'EXE = os.path.join(os.environ.get("FF7R_GAME", r"C:\\Program Files (x86)\\Steam\\steamapps\\common\\FINAL FANTASY VII REBIRTH"), r"End\\Binaries\\Win64\\ff7rebirth_.exe")')
        open(dst, 'w', encoding='utf-8').write(s)
for f in ('source_README.md', 'BUILDING.md', 'LICENSE', '.gitignore'):
    src = os.path.join(ROOT, 'release', 'source_files', f)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(OUT, 'README.md' if f == 'source_README.md' else f))
for dp, _, fs in os.walk(OUT):
    for fn in fs:
        print(os.path.relpath(os.path.join(dp, fn), OUT))
