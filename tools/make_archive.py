"""Build the patched global shader archive from build/bend/*.dxil."""
import sys, os, struct
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(P, 'tools'))
from shaderarc import ShaderArchive
src = os.path.join(P, 'extract/shaders/End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode')
out = os.path.join(P, 'build/pak/End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode')
os.makedirs(os.path.dirname(out), exist_ok=True)
A = ShaderArchive(src)
rep = {}
for f in sorted(os.listdir(os.path.join(P, 'build/bend'))):
    if not f.endswith('.dxil'): continue
    i = int(f[:5]); code = A.code(i)
    j = code.find(b'DXBC'); old = struct.unpack_from('<I', code, j + 24)[0]
    new = open(os.path.join(P, 'build/bend', f), 'rb').read()
    rep[i] = code[:j] + new + code[j + old:]
A.write(out, rep)
B = ShaderArchive(out)
bad = 0
for i in range(len(A.entries)):
    expect = rep.get(i)
    got = B.code(i)
    if expect is not None and got != expect: bad += 1
    if expect is None and got != A.code(i): bad += 1
print('replaced', len(rep), 'entries', len(B.entries), 'mismatches', bad, 'size', os.path.getsize(out))
