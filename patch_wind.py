"""FF7 Rebirth foliage wind distance patch (v3).

Foliage wind in Rebirth is a baked animation whose strength the mesh shaders fade as
    weight = saturate(1 - AnimationDecayRatio * distance_cm)
so wind disappears at 1 / AnimationDecayRatio cm from the camera. AnimationDecayRatio
lives at +0x8C of every 144-byte FMassiveEnvironmentPrimitiveInfo record.

This patch hooks the place where those records are uploaded to the GPU ("PrimitiveInfos"
structured buffer, stride 0x90) and clamps AnimationDecayRatio to at most 1 / (meters * 100),
so every plant keeps its wind out to at least `meters`. Plants that already reach farther are
left alone. Clamping is idempotent, so re-uploads are safe.

The uploaded object is a resource array of class vtable 0x145628828:
    GetResourceData()     = [this+0x58]
    GetResourceDataSize() = [this+0x60]   (BYTES, so records = size / 0x90)
The routine only acts on that class; any other object is skipped and its vtable logged.

Diagnostic counters are written into the unused tail of .data and can be read from the
running game with wind_stats.ps1.

Usage (run with the game closed):
    python patch_wind.py apply [meters]     default 60; 0 = never fade (diagnostic)
    python patch_wind.py restore
    python patch_wind.py status
"""
import hashlib, os, shutil, struct, sys

EXE = os.path.join(os.environ.get("FF7R_GAME", r"C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY VII REBIRTH"), r"End\Binaries\Win64\ff7rebirth_.exe")
BACKUP = EXE + ".windmod-original"

# Game build of 2025-10-28 (exe size 124317952, sha256 752807180c4e...).
HOOK_VA, HOOK_ORIG = 0x14163AE06, bytes.fromhex("488b4b08488d055f8c0504")  # mov rcx,[rbx+8]; lea rax,[rip+X]
HOOK_RET, HOOK_LEA_TARGET = 0x14163AE11, 0x145693A70
VTABLE = 0x145628828
VTABLE_SLOT1 = bytes.fromhex("488b4158c3")      # GetResourceData:     mov rax,[rcx+0x58]; ret
VTABLE_SLOT2 = bytes.fromhex("8b4160c3")        # GetResourceDataSize: mov eax,[rcx+0x60]; ret
CAVE_TEXT_OFFSET = 0x5566108   # inside .text raw data, past VirtualSize 0x5566102 (zero padding)
LOADER_TEXT_OFFSET = 0x55661D0 # DLL loader stub (26 bytes + DLL name, must end by 0x5566200)
DLL_NAME = "FinalFoliage.dll"  # max 21 characters
ENTRY_RVA_ORIG = 0x216C0A4
IAT_LOADLIBRARYA = 0x145568488
TEXT_VSIZE_ORIG, TEXT_VSIZE_NEW = 0x5566102, 0x5566200   # new = SizeOfRawData; same page
# Diagnostic counters live in the unused tail of .data's last page (zero-filled, writable).
DATA_VSIZE_ORIG, DATA_VSIZE_NEW = 0x2608F34, 0x2609000
STATS_VA = 0x149299F40         # RVA 0x9299F40 (.data ends at RVA 0x9299F34, .pdata starts 0x929A000)
# STATS layout: +0 hook hits, +4 (unused), +8 objects of the known class, +0xC records seen,
#   +0x10 records clamped, +0x14 records with decay == 0, +0x18 max decay seen (float bits),
#   +0x20 last unknown vtable (u64), +0x28 last record array pointer (u64)


class PE:
    def __init__(self, data):
        self.d = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", data, opt + 24)[0]
        self.secs = {}
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode()
            vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
            self.secs[name] = dict(hdr=o, vsz=vsz, va=va, rsz=rsz, raw=raw)

    def va2off(self, v):
        r = v - self.base
        for s in self.secs.values():
            if s["va"] <= r < s["va"] + s["rsz"]:
                return s["raw"] + r - s["va"]
        raise ValueError(hex(v))


class Asm:
    """Tiny label-aware byte emitter."""
    def __init__(self, va):
        self.va, self.out, self.labels, self.fix, self.fix32 = va, bytearray(), {}, [], []

    def b(self, hexstr): self.out += bytes.fromhex(hexstr)
    def u32(self, v): self.out += struct.pack("<I", v)
    def label(self, n): self.labels[n] = len(self.out)
    def jcc8(self, op, n): self.out += bytes([op, 0]); self.fix.append((len(self.out) - 1, n))
    def jz32(self, n): self.out += b"\x0f\x84\0\0\0\0"; self.fix32.append((len(self.out) - 4, n))
    def jmp32(self, n): self.out += b"\xe9\0\0\0\0"; self.fix32.append((len(self.out) - 4, n))
    def rip(self, target):  # rel32 for a RIP-relative operand / branch that ends right after the field
        self.out += struct.pack("<i", target - (self.va + len(self.out) + 4))

    def done(self):
        for pos, n in self.fix:
            rel = self.labels[n] - (pos + 1)
            assert -128 <= rel <= 127, f"short jump to {n} out of range"
            self.out[pos] = rel & 0xFF
        for pos, n in self.fix32:
            struct.pack_into("<i", self.out, pos, self.labels[n] - (pos + 4))
        return bytes(self.out)


def build_cave(cave_va, limit_bits):
    a, S = Asm(cave_va), STATS_VA
    a.b("ff05"); a.rip(S + 0x0)                          # inc dword [S+0]        hook hits
    a.b("5051524150")                                    # push rax,rcx,rdx,r8
    a.b("488b4b08")                                      # mov rcx,[rbx+8]        resource array object
    a.b("4885c9"); a.jz32("out")                         # test rcx,rcx ; jz out
    a.b("488b01")                                        # mov rax,[rcx]          vtable
    a.b("488d15"); a.rip(VTABLE)                         # lea rdx,[rip+VTABLE]
    a.b("4839d0"); a.jcc8(0x74, "ok")                    # cmp rax,rdx ; je ok
    a.b("488905"); a.rip(S + 0x20)                       # mov [S+0x20],rax       log unknown vtable
    a.jmp32("out")                                       # jmp out
    a.label("ok")
    a.b("ff05"); a.rip(S + 0x8)                          # inc dword [S+8]
    a.b("8b4160")                                        # mov eax,[rcx+0x60]     data size in bytes
    a.b("31d2")                                          # xor edx,edx
    a.b("41b890000000")                                  # mov r8d,0x90
    a.b("41f7f0")                                        # div r8d                eax = record count
    a.b("4189c0")                                        # mov r8d,eax
    a.b("488b5158")                                      # mov rdx,[rcx+0x58]     record array
    a.b("488915"); a.rip(S + 0x28)                       # mov [S+0x28],rdx
    a.b("4885d2"); a.jcc8(0x74, "out")                   # test rdx,rdx ; jz out
    a.b("440105"); a.rip(S + 0xC)                        # add [S+0xC],r8d        records seen
    a.label("loop")
    a.b("4585c0"); a.jcc8(0x74, "out")                   # test r8d,r8d ; jz out
    a.b("8b828c000000")                                  # mov eax,[rdx+0x8c]     AnimationDecayRatio
    a.b("85c0"); a.jcc8(0x75, "nz")                      # test eax,eax ; jnz nz
    a.b("ff05"); a.rip(S + 0x14)                         # inc dword [S+0x14]     decay == 0
    a.label("nz")
    a.b("3b05"); a.rip(S + 0x18)                         # cmp eax,[S+0x18]
    a.jcc8(0x76, "nomax")                                # jbe nomax
    a.b("8905"); a.rip(S + 0x18)                         # mov [S+0x18],eax       max decay
    a.label("nomax")
    a.b("3d"); a.u32(limit_bits)                         # cmp eax,limit
    a.jcc8(0x7E, "skip")                                 # jle skip (non-negative floats compare as ints)
    a.b("c7828c000000"); a.u32(limit_bits)               # mov dword [rdx+0x8c],limit
    a.b("ff05"); a.rip(S + 0x10)                         # inc dword [S+0x10]     clamped
    a.label("skip")
    a.b("4881c290000000"); a.b("41ffc8")                 # add rdx,0x90 ; dec r8d
    a.jcc8(0xEB, "loop")                                 # jmp loop
    a.label("out")
    a.b("41585a5958")                                    # pop r8,rdx,rcx,rax
    a.b("488b4b08")                                      # mov rcx,[rbx+8]        (replayed original)
    a.b("488d05"); a.rip(HOOK_LEA_TARGET)                # lea rax,[rip+X]        (replayed original)
    a.b("e9"); a.rip(HOOK_RET)                           # jmp back
    return a.done()


def patch(data, meters):
    pe = PE(bytearray(data))
    d = pe.d
    o = pe.va2off(HOOK_VA)
    if bytes(d[o:o + len(HOOK_ORIG)]) != HOOK_ORIG:
        sys.exit(f"Unexpected bytes at {HOOK_VA:#x}: different game build. Not patching.")
    slot1, slot2 = struct.unpack_from("<QQ", d, pe.va2off(VTABLE) + 8)
    if (bytes(d[pe.va2off(slot1):pe.va2off(slot1) + 5]) != VTABLE_SLOT1 or
            bytes(d[pe.va2off(slot2):pe.va2off(slot2) + 4]) != VTABLE_SLOT2):
        sys.exit("Resource-array class layout differs in this build. Not patching.")
    t, ds, pd = pe.secs[".text"], pe.secs[".data"], pe.secs[".pdata"]
    if t["vsz"] != TEXT_VSIZE_ORIG or t["rsz"] != TEXT_VSIZE_NEW:
        sys.exit("Unexpected .text layout: different game build. Not patching.")
    if ds["vsz"] != DATA_VSIZE_ORIG or ds["va"] + DATA_VSIZE_NEW != pd["va"]:
        sys.exit("Unexpected .data layout: different game build. Not patching.")
    cave_file = t["raw"] + CAVE_TEXT_OFFSET
    if any(d[cave_file:t["raw"] + t["rsz"]]):
        sys.exit("Cave area is not empty. Not patching.")
    limit = 0.0 if meters == 0 else 1.0 / (meters * 100.0)
    limit_bits = struct.unpack("<I", struct.pack("<f", limit))[0]
    cave_va = pe.base + t["va"] + CAVE_TEXT_OFFSET
    code = build_cave(cave_va, limit_bits)
    assert CAVE_TEXT_OFFSET + len(code) <= t["rsz"], "cave too large"
    d[cave_file:cave_file + len(code)] = code
    d[o:o + 11] = b"\xe9" + struct.pack("<i", cave_va - (HOOK_VA + 5)) + b"\x90" * 6
    # ---- DLL loader: new entry point calls LoadLibraryA(DLL_NAME) then jumps to the original entry ----
    assert len(code) <= LOADER_TEXT_OFFSET - CAVE_TEXT_OFFSET, "wind cave overlaps loader"
    opt = struct.unpack_from("<I", d, 0x3C)[0] + 24
    ep_rva = struct.unpack_from("<I", d, opt + 16)[0]
    if ep_rva != ENTRY_RVA_ORIG:
        sys.exit("Unexpected entry point: different game build. Not patching.")
    lva = pe.base + t["va"] + LOADER_TEXT_OFFSET
    name_va = lva + 26
    stub = (b"\x48\x83\xec\x28"                                                   # sub rsp,0x28
            + b"\x48\x8d\x0d" + struct.pack("<i", name_va - (lva + 4 + 7))           # lea rcx,[rip+name]
            + b"\xff\x15" + struct.pack("<i", IAT_LOADLIBRARYA - (lva + 11 + 6))     # call [rip+LoadLibraryA]
            + b"\x48\x83\xc4\x28"                                                   # add rsp,0x28
            + b"\xe9" + struct.pack("<i", (pe.base + ep_rva) - (lva + 21 + 5))       # jmp original entry
            + DLL_NAME.encode() + b"\0")
    assert LOADER_TEXT_OFFSET + len(stub) <= t["rsz"], "loader too large"
    lfile = t["raw"] + LOADER_TEXT_OFFSET
    d[lfile:lfile + len(stub)] = stub
    struct.pack_into("<I", d, opt + 16, t["va"] + LOADER_TEXT_OFFSET)            # AddressOfEntryPoint
    struct.pack_into("<I", d, t["hdr"] + 8, TEXT_VSIZE_NEW)    # cover the cave (same page)
    struct.pack_into("<I", d, ds["hdr"] + 8, DATA_VSIZE_NEW)   # cover the stats (same page)
    return bytes(d), limit, len(code)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "status":
        print("backup present:", os.path.exists(BACKUP))
        data = open(EXE, "rb").read()
        print("exe patched:", data[PE(data).va2off(HOOK_VA)] == 0xE9)
        return
    if cmd == "restore":
        if not os.path.exists(BACKUP):
            sys.exit("No backup found; nothing to restore.")
        shutil.copy2(BACKUP, EXE)
        print("Restored original exe.")
        return
    if cmd == "apply":
        meters = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
        out = sys.argv[3] if len(sys.argv) > 3 else EXE
        if out == EXE:
            if not os.path.exists(BACKUP):
                shutil.copy2(EXE, BACKUP)
                print("Backed up original to", BACKUP)
            src = open(BACKUP, "rb").read()   # always patch from the pristine copy
        else:                                 # dry run to another file: game folder untouched
            src = open(BACKUP if os.path.exists(BACKUP) else EXE, "rb").read()
        patched, limit, size = patch(src, meters)
        open(out, "wb").write(patched)
        print(f"Patched {out}: wind kept to at least {meters:g} m "
              f"(AnimationDecayRatio <= {limit:.8g}); cave {size} of 248 bytes")
        print("sha256:", hashlib.sha256(patched).hexdigest())
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
