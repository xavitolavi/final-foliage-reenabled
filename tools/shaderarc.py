"""UE 4.26 FSerializedShaderArchive (v2) reader/writer with LZ4 block codec (pure Python)."""
import struct

def lz4_decompress(src, usize):
    dst = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        dst += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4; start = len(dst) - off
        if off >= ml: dst += dst[start:start + ml]
        else:
            for k in range(ml): dst.append(dst[start + k])
    assert len(dst) == usize, (len(dst), usize)
    return bytes(dst)

def lz4_store(data):
    """Valid LZ4 block with the whole input as literals (no compression)."""
    n = len(data); out = bytearray()
    if n < 15: out.append(n << 4)
    else:
        out.append(0xF0); r = n - 15
        while r >= 255: out.append(255); r -= 255
        out.append(r)
    out += data
    return bytes(out)

class ShaderArchive:
    def __init__(self, path):
        d = self.raw = open(path, 'rb').read(); p = 0
        def u32():
            nonlocal p; v = struct.unpack_from('<I', d, p)[0]; p += 4; return v
        self.version = u32()
        n = u32(); self.map_hashes = d[p:p + 20 * n]; self.nmaps = n; p += 20 * n
        n = u32(); self.shader_hashes = d[p:p + 20 * n]; p += 20 * n
        n = u32(); self.map_entries = [struct.unpack_from('<IIII', d, p + 16 * k) for k in range(n)]; p += 16 * n
        n = u32(); self.entries = []
        for k in range(n):
            self.entries.append(list(struct.unpack_from('<QIIB', d, p))); p += 17
        n = u32(); self.preloads = [list(struct.unpack_from('<qq', d, p + 16 * k)) for k in range(n)]; p += 16 * n
        n = u32(); self.indices = list(struct.unpack_from('<%dI' % n, d, p)); p += 4 * n
        self.code_base = p

    def code(self, i):
        off, size, usize, freq = self.entries[i]
        raw = self.raw[self.code_base + off:self.code_base + off + size]
        return raw if size == usize else lz4_decompress(raw, usize)

    def write(self, path, replacements):
        """replacements: {entry index: new uncompressed code bytes}. Rebuilds offsets and preload ranges."""
        blobs = []; new_off = {}; off = 0
        for i, (o, size, usize, freq) in enumerate(self.entries):
            if i in replacements:
                code = replacements[i]; comp = lz4_store(code); ent = (len(comp), len(code))
            else:
                comp = self.raw[self.code_base + o:self.code_base + o + size]; ent = (size, usize)
            blobs.append(comp); new_off[i] = (off, ent[0], ent[1], freq); off += len(comp)
        # preload entries cover contiguous byte ranges of each shader map's shaders: recompute from members
        old_to_range = {}
        pre = [list(x) for x in self.preloads]
        for m, (idx_off, nsh, first_pre, npre) in enumerate(self.map_entries):
            if npre == 0: continue
            # map preload ranges by recomputing span of this map's shaders, grouped by original ranges
            for pk in range(first_pre, first_pre + npre):
                po, ps = self.preloads[pk]
                members = [self.indices[idx_off + s] for s in range(nsh)
                           if po <= self.entries[self.indices[idx_off + s]][0] < po + ps]
                if not members: continue
                lo = min(new_off[i][0] for i in members)
                hi = max(new_off[i][0] + new_off[i][1] for i in members)
                pre[pk] = [lo, hi - lo]
        out = bytearray()
        out += struct.pack('<I', self.version)
        out += struct.pack('<I', self.nmaps) + self.map_hashes
        out += struct.pack('<I', len(self.shader_hashes) // 20) + self.shader_hashes
        out += struct.pack('<I', len(self.map_entries)) + b''.join(struct.pack('<IIII', *e) for e in self.map_entries)
        out += struct.pack('<I', len(self.entries)) + b''.join(struct.pack('<QIIB', *new_off[i]) for i in range(len(self.entries)))
        out += struct.pack('<I', len(pre)) + b''.join(struct.pack('<qq', *x) for x in pre)
        out += struct.pack('<I', len(self.indices)) + struct.pack('<%dI' % len(self.indices), *self.indices)
        out += b''.join(blobs)
        open(path, 'wb').write(out)
