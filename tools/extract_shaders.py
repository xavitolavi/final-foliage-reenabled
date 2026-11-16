"""Extract the MassiveEnvironment mesh shaders from the game's global shader archive and disassemble them.

Input : extract/shaders/End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode
        (unpack it from <game>/End/Content/Paks/pakchunk0-WindowsNoEditor.pak with
         repak unpack, see BUILDING.md)
Output: extract/shaders/ms/<index>.dxil   raw DXIL container of every mesh shader entry point
        extract/shaders/ms/ll/<index>.ll  its LLVM IR text (dxc -dumpbin), which build_bend.py patches
"""
import os, subprocess, sys

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(P, 'tools'))
from shaderarc import ShaderArchive  # noqa: E402

SRC = os.path.join(P, 'extract/shaders/End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode')
OUT = os.path.join(P, 'extract/shaders/ms')
DXC = os.path.join(P, 'tools/dxc/bin/x64/dxc.exe')
os.makedirs(os.path.join(OUT, 'll'), exist_ok=True)

A = ShaderArchive(SRC)
n = 0
for i in range(len(A.entries)):
    code = A.code(i)
    if b'MainInBasePass' not in code and b'MainInPrePass' not in code:
        continue                                   # only the foliage base-pass / pre-pass mesh shaders
    j = code.find(b'DXBC')
    if j < 0:
        continue
    size = int.from_bytes(code[j + 24:j + 28], 'little')
    dx = os.path.join(OUT, '%05d.dxil' % i)
    open(dx, 'wb').write(code[j:j + size])
    ll = os.path.join(OUT, 'll', '%05d.ll' % i)
    r = subprocess.run([DXC, '-dumpbin', dx, '-Fo', ll], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(ll):
        open(ll, 'w').write(subprocess.run([DXC, '-dumpbin', dx], capture_output=True, text=True).stdout)
    n += 1
print('extracted', n, 'shaders to', OUT)
