"""Patch every deform-capable MassiveEnvironment base-pass / pre-pass mesh shader and assemble+validate them."""
import sys, os, subprocess, collections, json
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(P, 'tools'))
from bend_patch import patch_ir, PatchError
LL = os.path.join(P, 'extract/shaders/ms/ll')
OUT = os.path.join(P, 'build/bend'); os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT): os.remove(os.path.join(OUT, f))      # stale outputs would end up in the archive
DXASM = os.path.join(P, 'tools/dxasm/dxasm.exe'); DXC = os.path.join(P, 'tools/dxc/bin/x64/dxcompiler.dll')
params = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
stats = collections.Counter(); fails = []
for f in sorted(os.listdir(LL)):
    src = open(os.path.join(LL, f)).read()
    head = src[:src.find('{')] if '{' in src else src
    entry = src.split('define void @', 1)[1].split('(', 1)[0] if 'define void @' in src else ''
    if entry not in ('MainInBasePass', 'MainInPrePass') or ('RenderingGroups' not in src and 'CullingGroups' not in src):
        continue                            # only the batched permutations are drawn
    idx = f[:-3]
    try:
        out, info = patch_ir(src, params)
    except PatchError as e:
        stats['patch_fail_' + entry] += 1; fails.append((idx, entry, str(e))); continue
    ll = os.path.join(OUT, idx + '.ll'); dx = os.path.join(OUT, idx + '.dxil')
    open(ll, 'w').write(out)
    r = subprocess.run([DXASM, DXC, ll, dx], capture_output=True, text=True)
    if r.returncode != 0:
        stats['asm_fail_' + entry] += 1; fails.append((idx, entry, (r.stdout + r.stderr).strip()[-300:])); continue
    stats['ok_' + entry] += 1
print(dict(stats))
for x in fails[:10]: print('FAIL', x)
