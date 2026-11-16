"""Insert a foliage bend into an FF7 Rebirth MassiveEnvironment mesh shader (DXIL IR text from `dxc -dumpbin`).

Milestone 1: vertices of plants that have baked sway (NumDeformKeys > 0) are pushed horizontally away from a
point CENTER_DIST cm in front of the camera, with a radial falloff and a height factor (rotated local Z above
the plant pivot), so roots stay planted. Works in translated-world space (camera at View_TranslatedWorldCameraOrigin).
The same offset is added to the previous-frame position (motion vectors) so bent foliage doesn't smear in TAA.

patch_ir(text, params) -> (new_text, info)   raises PatchError when the shader doesn't match the expected shape.
"""
import re, struct


class PatchError(Exception):
    pass


DEFAULTS = dict(radius=105.0, max_angle=0.7, trample_r=40.0, squash=0.35, foot=90.0, vgate=100.0,
                size_full=150.0, size_none=250.0, attack_gain=1.0, attack_vgate=250.0)

# Contract with FoliageBend.dll (View cbuffer registers = byte offset / 16)
VIEW_REG_PREVIEWTRANSLATION = 78                      # View_PreViewTranslation (1248)
VIEW_REGS_SLOTS = [251, 252, 253, 254, 255, 256, 257, 290]  # MobileSkyIrradianceEnvironmentMap[0..6], XRPassthroughCameraUVs[0]
VIEW_REG_HEADER = 291                                  # XRPassthroughCameraUVs[1]
MAGIC = (12345.5, -6789.25, 31.75, 7.0)
VIEW_REGS_ATTACK = [(79 + 2 * j, 80 + 2 * j) for j in range(10)]   # View regs 79..98 (Prev* matrices, unused)

VIEW_REGS_WAKE = [(40, 41), (42, 43), (64, 65), (66, 67)]   # View_ViewToClipNoAA, View_MobileMultiviewShadowTransform (unused)
VIEW_REG_WIND = 8            # View_TranslatedWorldToOrthographicClip row 0 (unused): (dir x, dir y, strength, speed)
VIEW_REG_WIND2 = 9           # row 1: (fade start cm, 1 / fade length, 0, 0)
VIEW_REG_GAMETIME = 172      # View_GameTime is .z (offset 2760)
VIEW_REG_FORWARD = 68        # View_ViewForward                 (offset 1088)
VIEW_REG_CAMORIGIN = 76      # View_TranslatedWorldCameraOrigin (offset 1216)
# NOTE: only unused float4x4 View matrices are safe scratch space - arrays (e.g. FogContext*[4]) are indexed dynamically.
VIEW_REG_CLASS_GAINS = 123   # 4 regs (123..126, PrevCameraViewToTranslatedWorld): gain per foliage class 0..15
VIEW_REG_CLASS_TABLE = 103   # table segment A: regs 103..114 (3 unused Prev* matrices), dwords 0..47
VIEW_REG_CLASS_TABLE_B = 146 # table segment B: regs 146..153 (ProjectionToWorld / WorldToProjection), dwords 48..79
CLASS_TABLE_SEG_A_REGS = 12
CLASS_TABLE_DWORDS = 80
PRIM_NUMDEFORMKEYS = 48      # FMassiveEnvironmentPrimitiveInfo.NumDeformKeys
PRIM_DECAY = 140             # FMassiveEnvironmentPrimitiveInfo.AnimationDecayRatio
PRIM_SPHERE_RADIUS = 108     # FMassiveEnvironmentPrimitiveInfo.BoundingSphere.w

DEF_RE = re.compile(r'^\s+(%[\w.]+) = (.*)$')


def fconst(x):
    """LLVM IR float constant (double-hex of the exactly representable float)."""
    f = struct.unpack('<f', struct.pack('<f', x))[0]
    return '0x%016X' % struct.unpack('<Q', struct.pack('<d', f))[0]


def strip(rhs):
    return re.sub(r',?\s*!dx\.precise !\d+', '', rhs).split(';')[0].strip()


class IR:
    """Function body of a single-entry DXIL shader with defs, CFG and dominators."""

    def __init__(self, text):
        self.L = L = text.split('\n')
        self.fstart = next(i for i, l in enumerate(L) if l.startswith('define void @'))
        self.fend = next(i for i in range(self.fstart, len(L)) if L[i] == '}')
        self.entry = re.search(r'@(\w+)', L[self.fstart]).group(1)
        cur, succ, order, defs = 'entry', {'entry': []}, ['entry'], {}
        for i in range(self.fstart + 1, self.fend):
            l = L[i]
            m = re.match(r'^; <label>:(\d+)', l) or re.match(r'^(\d+):', l)
            if m:
                cur = '%' + m.group(1); order.append(cur); succ[cur] = []
                continue
            d = DEF_RE.match(l)
            if d:
                defs[d.group(1)] = (i, cur, strip(d.group(2)))
            s = l.strip()
            if s.startswith('br ') or s.startswith('switch '):
                succ[cur] += re.findall(r'label (%\d+)', l)
        preds = {b: [] for b in order}
        for b in order:
            for s in succ[b]:
                preds.setdefault(s, []).append(b)
        dom = {b: set(order) for b in order}
        dom['entry'] = {'entry'}
        changed = True
        while changed:
            changed = False
            for b in order[1:]:
                ps = [dom[q] for q in preds.get(b, []) if q in dom]
                new = (set.intersection(*ps) if ps else set()) | {b}
                if new != dom[b]:
                    dom[b] = new; changed = True
        self.defs, self.dom = defs, dom

    def available(self, v, line, block):
        if not v.startswith('%'):
            return True
        if v not in self.defs:
            return False
        dl, db, _ = self.defs[v]
        return (db == block and dl < line) or (db != block and db in self.dom[block])

    def cb_extract(self, v):
        """If v = extractvalue(cbufferLoadLegacy(handle, reg), comp) return (handle, reg, comp)."""
        if v not in self.defs:
            return None
        m = re.match(r'extractvalue %dx\.types\.CBufRet\.f32 (%[\w.]+), (\d)', self.defs[v][2])
        if not m or m.group(1) not in self.defs:
            return None
        m2 = re.search(r'cbufferLoadLegacy\.f32\(i32 59, %dx\.types\.Handle (%[\w.]+), i32 (\d+)\)', self.defs[m.group(1)][2])
        return (m2.group(1), int(m2.group(2)), int(m.group(2))) if m2 else None

    def output_index(self, name):
        """Signature element id of a vertex output (order of the 'Vertex Output signature' table)."""
        text = '\n'.join(self.L[:self.fstart])
        if '; Vertex Output signature:' not in text:
            return None
        sec = text.split('; Vertex Output signature:')[1].split('; Buffer Definitions')[0]
        names = [a + b for a, b in re.findall(r'^; ([A-Za-z_]+)\s+(\d+)', sec, re.M)]
        return names.index(name) if name in names else None

    def projection(self, element):
        """Follow the x component stored to output `element` back through
        fma(1, m3, fma(Z, m2, fma(Y, m1, fmul(X, m0)))) and return (X, Y, Z, handle, [regs])."""
        defs = self.defs
        st = [l for l in self.L[self.fstart:self.fend]
              if 'storeVertexOutput.f32(i32 171, i32 %d, i32 0, i8 0, float ' % element in l]
        if not st:
            raise PatchError('no store for output %d' % element)
        v = re.search(r'i8 0, float (%[\w.]+)', st[0]).group(1)
        for _ in range(3):   # unwrap phi / select wrappers
            rhs = defs.get(v, (0, 0, ''))[2]
            if rhs.startswith('phi'):
                inc = re.findall(r'\[ (%[\w.]+), %\w+ \]', rhs)
                if len(inc) != 1:
                    raise PatchError('unexpected phi on output %d' % element)
                v = inc[0]
            elif rhs.startswith('select'):
                ms = re.match(r'select i1 %[\w.]+, float (%[\w.]+), float ', rhs)
                if not ms:
                    raise PatchError('unexpected select on output %d' % element)
                v = ms.group(1)
            else:
                break
        chain, t = [], v
        for k in range(3):
            m = re.match(r'call float @dx\.op\.tertiary\.f32\(i32 46, float ([^,]+), float ([^,]+), float ([^)]+)\)',
                         defs.get(t, (0, 0, ''))[2])
            if not m:
                raise PatchError('projection chain: no fma at step %d' % k)
            a, b, c = m.groups()
            if self.cb_extract(b):
                chain.append((a, self.cb_extract(b)))
            elif self.cb_extract(a):
                chain.append((b, self.cb_extract(a)))
            else:
                raise PatchError('projection chain: no matrix operand')
            t = c
        m = re.match(r'fmul (?:fast )?float (%[\w.]+), (%[\w.]+)', defs.get(t, (0, 0, ''))[2])
        if not m:
            raise PatchError('projection chain: no leading fmul')
        a, b = m.groups()
        if self.cb_extract(b):
            chain.append((a, self.cb_extract(b)))
        elif self.cb_extract(a):
            chain.append((b, self.cb_extract(a)))
        else:
            raise PatchError('projection chain: fmul without matrix')
        (_, m3), (Z, m2), (Y, m1), (X, m0) = chain
        if len({m0[0], m1[0], m2[0], m3[0]}) != 1:
            raise PatchError('projection matrix rows come from different buffers')
        return X, Y, Z, m0[0], [m0[1], m1[1], m2[1], m3[1]]


def insert_and_rewire(L, fend, ins, new_lines, rep):
    """Insert new_lines at index ins and replace uses of rep keys in all later body lines."""
    out = L[:ins] + new_lines + L[ins:]
    pat = re.compile('(' + '|'.join(re.escape(k) for k in rep) + r')(?![\w.])')
    n = 0
    for i in range(ins + len(new_lines), fend + len(new_lines)):
        line = out[i]
        if line.lstrip().startswith(';'):
            continue
        new = pat.sub(lambda m: rep[m.group(1)], line)
        if new != line:
            out[i] = new; n += 1
    return out, n


def patch_ir(text, params=None):
    p = dict(DEFAULTS, **(params or {}))
    ir = IR(text)
    defs = ir.defs

    # ---- 1. current translated-world position feeding SV_Position ----
    X, Y, Z, view_handle, regs = ir.projection(0)
    if regs != [0, 1, 2, 3]:
        raise PatchError('not the main-view TranslatedWorldToClip (regs %s)' % regs)

    # ---- 2. split each component into rotated-local vector + plant pivot (offset has a cbuffer term) ----
    def is_offset(v):
        mm = re.match(r'fadd (?:fast )?float (%[\w.]+), (%[\w.]+)', defs.get(v, (0, 0, ''))[2])
        return bool(mm and (ir.cb_extract(mm.group(1)) or ir.cb_extract(mm.group(2))))

    def split(v, axis):
        mm = re.match(r'fadd (?:fast )?float (%[\w.]+), (%[\w.]+)', defs[v][2])
        if not mm:
            raise PatchError('%s is not an fadd' % axis)
        a_, b_ = mm.groups()
        if is_offset(a_) and not is_offset(b_):
            return b_, a_
        if is_offset(b_) and not is_offset(a_):
            return a_, b_
        raise PatchError('cannot split %s into rotated local + pivot' % axis)
    RX, OX = split(X, 'X')
    RY, OY = split(Y, 'Y')
    RZ, OZ = split(Z, 'Z')

    # ---- 3. insertion point and primitive-record access ----
    blks = {defs[x][1] for x in (X, Y, Z)}
    if len(blks) != 1:
        raise PatchError('X/Y/Z defined in different blocks')
    blk = blks.pop()
    ins = max(defs[x][0] for x in (X, Y, Z)) + 1
    cands = []
    for l in ir.L[ir.fstart:ir.fend]:
        mh = re.search(r'rawBufferLoad\.(?:f32|i32)\(i32 139, %dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 (\d+),', l)
        if mh and int(mh.group(3)) in (PRIM_NUMDEFORMKEYS, PRIM_DECAY, 96, 124):
            h = mh.group(1)
            if h in defs and 'StructuredBuffer<stride=144>' in ir.L[defs[h][0]]:
                cands.append((h, mh.group(2)))
    ok = [(h, i) for h, i in dict.fromkeys(cands) if ir.available(h, ins, blk) and ir.available(i, ins, blk)]
    if ok:
        H, IDX = ok[0]
    elif not cands:
        H = IDX = None                      # no-sway permutation: no plant record, gated by the DLL's per-material class
    else:
        raise PatchError('no primitive-record handle/index available at insertion point')
    # groups cbuffer (MassiveEnvironmentRenderingGroups 4096 B / CullingGroups 8192 B): RenderDataPositions[i].w = class-table slot
    GH = [m.group(1) for m in re.finditer(r'(%[\w.]+) = call %dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle %[\w.]+, %dx\.types\.ResourceProperties \{ i32 13, i32 (?:4096|8192) \}\)', text)]
    PLOAD = YV = None
    for m in re.finditer(r'(%[\w.]+) = call %dx\.types\.CBufRet\.f32 @dx\.op\.cbufferLoadLegacy\.f32\(i32 59, %dx\.types\.Handle (%[\w.]+), i32 (%?[\w.]+)\)', text):
        if m.group(2) in GH and ir.available(m.group(1), ins, blk):
            PLOAD = m.group(1); break
    MH = [m.group(1) for m in re.finditer(r'(%[\w.]+) = call %dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle %[\w.]+, %dx\.types\.ResourceProperties \{ i32 10, i32 517 \}\)', text)]
    for m in re.finditer(r'(%[\w.]+) = call %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\(i32 68, %dx\.types\.Handle (%[\w.]+),', text):
        if m.group(2) in MH:
            ym = re.search(r'(%[\w.]+) = extractvalue %dx\.types\.ResRet\.i32 ' + re.escape(m.group(1)) + r', 1\n', text)
            if ym and ir.available(ym.group(1), ins, blk):
                YV = ym.group(1); break
    if PLOAD is None or YV is None:
        raise PatchError('no groups position load / meshlet word (not a batched permutation)')
    for need in (view_handle, RX, RY, RZ, OX, OY, OZ):
        if not ir.available(need, ins, blk):
            raise PatchError('%s not available at insertion point' % need)

    # ---- 4. the bend ----
    pm = re.search(r'!dx\.precise (!\d+)', text)
    prec = (', !dx.precise ' + pm.group(1)) if pm else ''
    c = fconst
    g = []

    def e(s, precise=True):
        g.append('  ' + s + (prec if precise else ''))
    def cb(name, reg):
        e('%%%s = call %%dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %%dx.types.Handle %s, i32 %d)' % (name, view_handle, reg), False)
        for k, comp in enumerate('xyzw'):
            e('%%%s_%s = extractvalue %%dx.types.CBufRet.f32 %%%s, %d' % (name, comp, name, k), False)
    # header written by FoliageBend.dll: only bend when it matches exactly
    cb('gb_hd', VIEW_REG_HEADER)
    for comp, val in zip('xyzw', MAGIC):
        e('%%gb_hd_ok%s = fcmp oeq float %%gb_hd_%s, %s' % (comp, comp, c(val)), False)
    e('%gb_hd_a = and i1 %gb_hd_okx, %gb_hd_oky', False)
    e('%gb_hd_b = and i1 %gb_hd_okz, %gb_hd_okw', False)
    e('%gb_valid = and i1 %gb_hd_a, %gb_hd_b', False)
    # per-material foliage class from the DLL's table: slot = RenderDataPositions[i].w (0 = none), material = meshlet word.y >> 20
    e('%%gb_pw = extractvalue %%dx.types.CBufRet.f32 %s, 3' % PLOAD, False)
    e('%gb_slot = bitcast float %gb_pw to i32', False)
    e('%%gb_mat = lshr i32 %s, 20' % YV, False)
    e('%gb_hasslot = icmp ne i32 %gb_slot, 0', False)
    e('%gb_sm1 = add i32 %gb_slot, -1', False)
    e('%gb_md = lshr i32 %gb_mat, 3', False)
    e('%gb_d0 = add i32 %gb_sm1, %gb_md', False)
    e('%%gb_dc = call i32 @dx.op.binary.i32(i32 40, i32 %%gb_d0, i32 %d)' % (CLASS_TABLE_DWORDS - 1), False)   # UMin
    e('%gb_dr = lshr i32 %gb_dc, 2', False)
    e('%%gb_tsa = icmp ult i32 %%gb_dr, %d' % CLASS_TABLE_SEG_A_REGS, False)
    e('%%gb_tbase = select i1 %%gb_tsa, i32 %d, i32 %d' % (VIEW_REG_CLASS_TABLE, VIEW_REG_CLASS_TABLE_B - CLASS_TABLE_SEG_A_REGS), False)
    e('%gb_treg = add i32 %gb_dr, %gb_tbase', False)
    e('%%gb_tl = call %%dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %%dx.types.Handle %s, i32 %%gb_treg)' % view_handle, False)
    for k in range(4):
        e('%%gb_t%d = extractvalue %%dx.types.CBufRet.i32 %%gb_tl, %d' % (k, k), False)
    e('%gb_dk = and i32 %gb_dc, 3', False)
    e('%gb_dk1 = icmp eq i32 %gb_dk, 1', False)
    e('%gb_dk2 = icmp eq i32 %gb_dk, 2', False)
    e('%gb_dk3 = icmp eq i32 %gb_dk, 3', False)
    e('%gb_tw1 = select i1 %gb_dk1, i32 %gb_t1, i32 %gb_t0', False)
    e('%gb_tw2 = select i1 %gb_dk2, i32 %gb_t2, i32 %gb_tw1', False)
    e('%gb_tw = select i1 %gb_dk3, i32 %gb_t3, i32 %gb_tw2', False)
    e('%gb_sh0 = and i32 %gb_mat, 7', False)
    e('%gb_sh = shl i32 %gb_sh0, 2', False)
    e('%gb_cv = lshr i32 %gb_tw, %gb_sh', False)
    e('%gb_cls = and i32 %gb_cv, 15', False)
    e('%gb_gr0 = lshr i32 %gb_cls, 2', False)
    e('%%gb_gr = add i32 %%gb_gr0, %d' % VIEW_REG_CLASS_GAINS, False)
    e('%%gb_gl = call %%dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %%dx.types.Handle %s, i32 %%gb_gr)' % view_handle, False)
    for k in range(4):
        e('%%gb_g%d = extractvalue %%dx.types.CBufRet.f32 %%gb_gl, %d' % (k, k), False)
    e('%gb_gk = and i32 %gb_cls, 3', False)
    e('%gb_gk1 = icmp eq i32 %gb_gk, 1', False)
    e('%gb_gk2 = icmp eq i32 %gb_gk, 2', False)
    e('%gb_gk3 = icmp eq i32 %gb_gk, 3', False)
    e('%gb_gw1 = select i1 %gb_gk1, float %gb_g1, float %gb_g0', False)
    e('%gb_gw2 = select i1 %gb_gk2, float %gb_g2, float %gb_gw1', False)
    e('%gb_gws = select i1 %gb_gk3, float %gb_g3, float %gb_gw2', False)
    e('%%gb_partc = fcmp olt float %%gb_gws, %s' % c(0.0), False)
    e('%gb_part = and i1 %gb_partc, %gb_hasslot', False)
    e('%gb_gw = call float @dx.op.unary.f32(i32 6, float %gb_gws)', False)        # |gain|
    # distances are always measured from the plant pivot (per-vertex distances read as fish-eye); "shear" mode only
    # changes how the lean is applied: bent from the ground by height instead of rotating the plant about its pivot
    e('%%gb_refx = fadd float %s, %s' % (OX, c(0.0)))
    e('%%gb_refy = fadd float %s, %s' % (OY, c(0.0)))
    cb('gb_pvt', VIEW_REG_PREVIEWTRANSLATION)
    # Per-plant rigid lean: distance/direction are measured from the plant pivot (OX,OY,OZ), so every vertex of a
    # plant gets the same rotation (no tearing or stretching). Plants right under a character are squashed instead.
    acc_x, acc_y, acc_q = c(0.0), c(0.0), c(0.0)
    for i, reg in enumerate(VIEW_REGS_SLOTS):
        n = 'gb_s%d' % i
        cb(n, reg)
        e('%%%s_tx = fadd float %%%s_x, %%gb_pvt_x' % (n, n))
        e('%%%s_ty = fadd float %%%s_y, %%gb_pvt_y' % (n, n))
        e('%%%s_tz = fadd float %%%s_z, %%gb_pvt_z' % (n, n))
        e('%%%s_vx = fsub float %%gb_refx, %%%s_tx' % (n, n))
        e('%%%s_vy = fsub float %%gb_refy, %%%s_ty' % (n, n))
        e('%%%s_vz0 = fsub float %s, %%%s_tz' % (n, OZ, n))
        e('%%%s_vz = fadd float %%%s_vz0, %s' % (n, n, c(p['foot'])))            # pivot height relative to the feet
        e('%%%s_vxx = fmul float %%%s_vx, %%%s_vx' % (n, n, n))
        e('%%%s_vyy = fmul float %%%s_vy, %%%s_vy' % (n, n, n))
        e('%%%s_d2 = fadd float %%%s_vxx, %%%s_vyy' % (n, n, n))
        e('%%%s_d2c = call float @dx.op.binary.f32(i32 35, float %%%s_d2, float %s)' % (n, n, c(1.0)))
        e('%%%s_d = call float @dx.op.unary.f32(i32 24, float %%%s_d2c)' % (n, n))
        e('%%%s_fr = fmul float %%%s_d, %s' % (n, n, c(1.0 / p['radius'])))
        e('%%%s_f0 = fsub float %s, %%%s_fr' % (n, c(1.0), n))
        e('%%%s_t = call float @dx.op.unary.f32(i32 7, float %%%s_f0)' % (n, n))
        e('%%%s_f1 = fmul float %%%s_t, %%%s_t' % (n, n, n))
        e('%%%s_adz = call float @dx.op.unary.f32(i32 6, float %%%s_vz)' % (n, n))
        e('%%%s_gz0 = fsub float %s, %%%s_adz' % (n, c(p['vgate'] + 100.0), n))
        e('%%%s_gz1 = fmul float %%%s_gz0, %s' % (n, n, c(0.01)))
        e('%%%s_gz = call float @dx.op.unary.f32(i32 7, float %%%s_gz1)' % (n, n))
        e('%%%s_wc = call float @dx.op.unary.f32(i32 7, float %%%s_w)' % (n, n))
        e('%%%s_f2 = fmul float %%%s_f1, %%%s_gz' % (n, n, n))
        e('%%%s_f = fmul float %%%s_f2, %%%s_wc' % (n, n, n))
        e('%%%s_k = fdiv float %%%s_f, %%%s_d' % (n, n, n))
        e('%%%s_mx = fmul float %%%s_vx, %%%s_k' % (n, n, n))
        e('%%%s_my = fmul float %%%s_vy, %%%s_k' % (n, n, n))
        e('%%%s_ax = fadd float %s, %%%s_mx' % (n, acc_x, n))
        e('%%%s_ay = fadd float %s, %%%s_my' % (n, acc_y, n))
        e('%%%s_tr0 = fmul float %%%s_d, %s' % (n, n, c(1.0 / p['trample_r'])))
        e('%%%s_tr1 = fsub float %s, %%%s_tr0' % (n, c(1.0), n))
        e('%%%s_tr = call float @dx.op.unary.f32(i32 7, float %%%s_tr1)' % (n, n))
        e('%%%s_qf = fmul float %%%s_tr, %%%s_f' % (n, n, n))
        e('%%%s_aq = fadd float %s, %%%s_qf' % (n, acc_q, n))
        acc_x, acc_y, acc_q = '%%%s_ax' % n, '%%%s_ay' % n, '%%%s_aq' % n
    # Wake segments from FoliageBend.dll: A = (x, y, z, strength) newer end, B = (x, y, z, strength) older end.
    # A plant leans by its distance to the closest point on the segment (strength interpolated along it), outward from
    # the path and, right on the path, along the direction of travel. The wake only fills what the characters' own bend
    # leaves (room = 1 - |characters|), so it slows the spring-back without adding to the initial push.
    e('%%gb_cxx = fmul float %s, %s' % (acc_x, acc_x))
    e('%%gb_cyy = fmul float %s, %s' % (acc_y, acc_y))
    e('%gb_cl2 = fadd float %gb_cxx, %gb_cyy')
    e('%gb_cl = call float @dx.op.unary.f32(i32 24, float %gb_cl2)')
    wx, wy, wm = c(0.0), c(0.0), c(0.0)
    for j, (ra, rb) in enumerate(VIEW_REGS_WAKE):
        n = 'gb_w%d' % j
        cb(n + 'a', ra)
        cb(n + 'b', rb)
        e('%%%s_ax = fadd float %%%sa_x, %%gb_pvt_x' % (n, n))
        e('%%%s_ay = fadd float %%%sa_y, %%gb_pvt_y' % (n, n))
        e('%%%s_az = fadd float %%%sa_z, %%gb_pvt_z' % (n, n))
        e('%%%s_ex = fsub float %%%sb_x, %%%sa_x' % (n, n, n))
        e('%%%s_ey = fsub float %%%sb_y, %%%sa_y' % (n, n, n))
        e('%%%s_ez = fsub float %%%sb_z, %%%sa_z' % (n, n, n))
        e('%%%s_vx = fsub float %%gb_refx, %%%s_ax' % (n, n))
        e('%%%s_vy = fsub float %%gb_refy, %%%s_ay' % (n, n))
        e('%%%s_exx = fmul float %%%s_ex, %%%s_ex' % (n, n, n))
        e('%%%s_eyy = fmul float %%%s_ey, %%%s_ey' % (n, n, n))
        e('%%%s_ee0 = fadd float %%%s_exx, %%%s_eyy' % (n, n, n))
        e('%%%s_ee = call float @dx.op.binary.f32(i32 35, float %%%s_ee0, float %s)' % (n, n, c(1.0)))
        e('%%%s_dp0 = fmul float %%%s_vx, %%%s_ex' % (n, n, n))
        e('%%%s_dp1 = fmul float %%%s_vy, %%%s_ey' % (n, n, n))
        e('%%%s_dp = fadd float %%%s_dp0, %%%s_dp1' % (n, n, n))
        e('%%%s_t0 = fdiv float %%%s_dp, %%%s_ee' % (n, n, n))
        e('%%%s_t = call float @dx.op.unary.f32(i32 7, float %%%s_t0)' % (n, n))
        e('%%%s_qx0 = fmul float %%%s_ex, %%%s_t' % (n, n, n))
        e('%%%s_qy0 = fmul float %%%s_ey, %%%s_t' % (n, n, n))
        e('%%%s_qz0 = fmul float %%%s_ez, %%%s_t' % (n, n, n))
        e('%%%s_ox = fsub float %%%s_vx, %%%s_qx0' % (n, n, n))           # plant - closest point (horizontal)
        e('%%%s_oy = fsub float %%%s_vy, %%%s_qy0' % (n, n, n))
        e('%%%s_qz = fadd float %%%s_az, %%%s_qz0' % (n, n, n))
        e('%%%s_oxx = fmul float %%%s_ox, %%%s_ox' % (n, n, n))
        e('%%%s_oyy = fmul float %%%s_oy, %%%s_oy' % (n, n, n))
        e('%%%s_od20 = fadd float %%%s_oxx, %%%s_oyy' % (n, n, n))
        e('%%%s_od2 = call float @dx.op.binary.f32(i32 35, float %%%s_od20, float %s)' % (n, n, c(1.0)))
        e('%%%s_od = call float @dx.op.unary.f32(i32 24, float %%%s_od2)' % (n, n))
        e('%%%s_sd = fsub float %%%sb_w, %%%sa_w' % (n, n, n))
        e('%%%s_st0 = fmul float %%%s_sd, %%%s_t' % (n, n, n))
        e('%%%s_s = fadd float %%%sa_w, %%%s_st0' % (n, n, n))
        e('%%%s_fr = fmul float %%%s_od, %s' % (n, n, c(1.0 / p['radius'])))
        e('%%%s_f0 = fsub float %s, %%%s_fr' % (n, c(1.0), n))
        e('%%%s_fs = call float @dx.op.unary.f32(i32 7, float %%%s_f0)' % (n, n))
        e('%%%s_ff = fmul float %%%s_fs, %%%s_fs' % (n, n, n))
        e('%%%s_vz0 = fsub float %s, %%%s_qz' % (n, OZ, n))
        e('%%%s_vz = fadd float %%%s_vz0, %s' % (n, n, c(p['foot'])))
        e('%%%s_adz = call float @dx.op.unary.f32(i32 6, float %%%s_vz)' % (n, n))
        e('%%%s_gz0 = fsub float %s, %%%s_adz' % (n, c(p['vgate'] + 100.0), n))
        e('%%%s_gz1 = fmul float %%%s_gz0, %s' % (n, n, c(0.01)))
        e('%%%s_gz = call float @dx.op.unary.f32(i32 7, float %%%s_gz1)' % (n, n))
        e('%%%s_m0 = fmul float %%%s_ff, %%%s_gz' % (n, n, n))
        e('%%%s_sc = call float @dx.op.unary.f32(i32 7, float %%%s_s)' % (n, n))
        e('%%%s_m = fmul float %%%s_m0, %%%s_sc' % (n, n, n))
        # direction: outward from the path, blending into the travel direction (A - B) on the path itself
        e('%%%s_side0 = fmul float %%%s_od, %s' % (n, n, c(1.0 / 40.0)))
        e('%%%s_side = call float @dx.op.unary.f32(i32 7, float %%%s_side0)' % (n, n))
        e('%%%s_el = call float @dx.op.unary.f32(i32 24, float %%%s_ee)' % (n, n))
        e('%%%s_on = fsub float %s, %%%s_side' % (n, c(1.0), n))
        e('%%%s_tk = fdiv float %%%s_on, %%%s_el' % (n, n, n))           # on the path: lean back, as the character left it
        e('%%%s_sk = fdiv float %%%s_side, %%%s_od' % (n, n, n))
        e('%%%s_dx0 = fmul float %%%s_ox, %%%s_sk' % (n, n, n))
        e('%%%s_dx1 = fmul float %%%s_ex, %%%s_tk' % (n, n, n))
        e('%%%s_dx = fadd float %%%s_dx0, %%%s_dx1' % (n, n, n))
        e('%%%s_dy0 = fmul float %%%s_oy, %%%s_sk' % (n, n, n))
        e('%%%s_dy1 = fmul float %%%s_ey, %%%s_tk' % (n, n, n))
        e('%%%s_dy = fadd float %%%s_dy0, %%%s_dy1' % (n, n, n))
        e('%%%s_dxx = fmul float %%%s_dx, %%%s_dx' % (n, n, n))
        e('%%%s_dyy = fmul float %%%s_dy, %%%s_dy' % (n, n, n))
        e('%%%s_dl20 = fadd float %%%s_dxx, %%%s_dyy' % (n, n, n))
        e('%%%s_dl2 = call float @dx.op.binary.f32(i32 35, float %%%s_dl20, float %s)' % (n, n, c(1e-8)))
        e('%%%s_dl = call float @dx.op.unary.f32(i32 24, float %%%s_dl2)' % (n, n))
        e('%%%s_mk = fdiv float %%%s_m, %%%s_dl' % (n, n, n))
        e('%%%s_mx = fmul float %%%s_dx, %%%s_mk' % (n, n, n))
        e('%%%s_my = fmul float %%%s_dy, %%%s_mk' % (n, n, n))
        # strongest segment wins (summing made plants near a segment joint tilt a second time)
        e('%%%s_gt = fcmp ogt float %%%s_m, %s' % (n, n, wm), False)
        e('%%%s_wx = select i1 %%%s_gt, float %%%s_mx, float %s' % (n, n, n, wx), False)
        e('%%%s_wy = select i1 %%%s_gt, float %%%s_my, float %s' % (n, n, n, wy), False)
        e('%%%s_wm = select i1 %%%s_gt, float %%%s_m, float %s' % (n, n, n, wm), False)
        wx, wy, wm = '%%%s_wx' % n, '%%%s_wy' % n, '%%%s_wm' % n
    # max-combine: add only the part of the wake that exceeds the characters' own bend (no second push, no extra
    # bend around a character standing still); the wake takes over continuously as a character walks away
    e('%%gb_wxx = fmul float %s, %s' % (wx, wx))
    e('%%gb_wyy = fmul float %s, %s' % (wy, wy))
    e('%gb_wl20 = fadd float %gb_wxx, %gb_wyy')
    e('%%gb_wl2 = call float @dx.op.binary.f32(i32 35, float %%gb_wl20, float %s)' % c(1e-8))
    e('%gb_wl = call float @dx.op.unary.f32(i32 24, float %gb_wl2)')
    e('%gb_wex0 = fsub float %gb_wl, %gb_cl')
    e('%%gb_wex = call float @dx.op.binary.f32(i32 35, float %%gb_wex0, float %s)' % c(0.0))
    e('%gb_wsc = fdiv float %gb_wex, %gb_wl')
    e('%%gb_wkx = fmul float %s, %%gb_wsc' % wx)
    e('%%gb_wky = fmul float %s, %%gb_wsc' % wy)
    e('%%gb_cwx = fadd float %s, %%gb_wkx' % acc_x)
    e('%%gb_cwy = fadd float %s, %%gb_wky' % acc_y)
    acc_x, acc_y = '%gb_cwx', '%gb_cwy'
    # Attack sources from FoliageBend.dll (weapon swing gusts; later shockwaves):
    #   A = (x, y, z world, radius)   B = (dirx, diry, strength, ring)
    # dir == 0 -> radial push away from the source; otherwise push along dir. Empty slots have radius 0 -> no effect.
    for j, (ra, rb) in enumerate(VIEW_REGS_ATTACK):
        n = 'gb_a%d' % j
        cb(n + 'p', ra)
        cb(n + 'q', rb)
        e('%%%s_tx = fadd float %%%sp_x, %%gb_pvt_x' % (n, n))
        e('%%%s_ty = fadd float %%%sp_y, %%gb_pvt_y' % (n, n))
        e('%%%s_tz = fadd float %%%sp_z, %%gb_pvt_z' % (n, n))
        e('%%%s_vx = fsub float %%gb_refx, %%%s_tx' % (n, n))
        e('%%%s_vy = fsub float %%gb_refy, %%%s_ty' % (n, n))
        e('%%%s_vz = fsub float %s, %%%s_tz' % (n, OZ, n))
        e('%%%s_vxx = fmul float %%%s_vx, %%%s_vx' % (n, n, n))
        e('%%%s_vyy = fmul float %%%s_vy, %%%s_vy' % (n, n, n))
        e('%%%s_d2 = fadd float %%%s_vxx, %%%s_vyy' % (n, n, n))
        e('%%%s_d2c = call float @dx.op.binary.f32(i32 35, float %%%s_d2, float %s)' % (n, n, c(1.0)))
        e('%%%s_d = call float @dx.op.unary.f32(i32 24, float %%%s_d2c)' % (n, n))
        e('%%%s_rc = call float @dx.op.binary.f32(i32 35, float %%%sp_w, float %s)' % (n, n, c(1e-3)))
        e('%%%s_fr = fdiv float %%%s_d, %%%s_rc' % (n, n, n))
        e('%%%s_f0 = fsub float %s, %%%s_fr' % (n, c(1.0), n))
        e('%%%s_t = call float @dx.op.unary.f32(i32 7, float %%%s_f0)' % (n, n))
        e('%%%s_tt = fmul float %%%s_t, %%%s_t' % (n, n, n))
        e('%%%s_adz = call float @dx.op.unary.f32(i32 6, float %%%s_vz)' % (n, n))
        e('%%%s_gz0 = fsub float %s, %%%s_adz' % (n, c(p['attack_vgate'] + 100.0), n))
        e('%%%s_gz1 = fmul float %%%s_gz0, %s' % (n, n, c(0.01)))
        e('%%%s_gz = call float @dx.op.unary.f32(i32 7, float %%%s_gz1)' % (n, n))
        e('%%%s_s = call float @dx.op.unary.f32(i32 7, float %%%sq_z)' % (n, n))
        e('%%%s_f1 = fmul float %%%s_tt, %%%s_gz' % (n, n, n))
        e('%%%s_f2 = fmul float %%%s_f1, %%%s_s' % (n, n, n))
        e('%%%s_f = fmul float %%%s_f2, %s' % (n, n, c(p['attack_gain'])))
        e('%%%s_rx = fdiv float %%%s_vx, %%%s_d' % (n, n, n))
        e('%%%s_ry = fdiv float %%%s_vy, %%%s_d' % (n, n, n))
        e('%%%s_zx = fcmp oeq float %%%sq_x, %s' % (n, n, c(0.0)), False)
        e('%%%s_zy = fcmp oeq float %%%sq_y, %s' % (n, n, c(0.0)), False)
        e('%%%s_rad = and i1 %%%s_zx, %%%s_zy' % (n, n, n), False)
        e('%%%s_ux = select i1 %%%s_rad, float %%%s_rx, float %%%sq_x' % (n, n, n, n), False)
        e('%%%s_uy = select i1 %%%s_rad, float %%%s_ry, float %%%sq_y' % (n, n, n, n), False)
        e('%%%s_mx = fmul float %%%s_ux, %%%s_f' % (n, n, n))
        e('%%%s_my = fmul float %%%s_uy, %%%s_f' % (n, n, n))
        e('%%%s_ax = fadd float %s, %%%s_mx' % (n, acc_x, n))
        e('%%%s_ay = fadd float %s, %%%s_my' % (n, acc_y, n))
        acc_x, acc_y = '%%%s_ax' % n, '%%%s_ay' % n
    if H is None:
        # Procedural wind for plants without baked sway (gated later by the material class gain, like the bend):
        # lean = dir * strength * gust(t, phase) + small cross-wind flutter; phase from the plant's WORLD position so
        # neighbours move slightly out of step; fades out with camera distance (pivot is camera-relative).
        cb('gb_wd_wnd', VIEW_REG_WIND)
        cb('gb_wd_wnd2', VIEW_REG_WIND2)
        cb('gb_wd_tm', VIEW_REG_GAMETIME)
        e('%gb_wd_wpx = fsub float ' + OX + ', %gb_pvt_x')
        e('%gb_wd_wpy = fsub float ' + OY + ', %gb_pvt_y')
        e('%%gb_wd_ph0 = fmul float %%gb_wd_wpx, %s' % c(0.0031))
        e('%%gb_wd_ph1 = fmul float %%gb_wd_wpy, %s' % c(0.0043))
        e('%gb_wd_ph = fadd float %gb_wd_ph0, %gb_wd_ph1')
        e('%gb_wd_ts = fmul float %gb_wd_tm_z, %gb_wd_wnd_w')
        e('%%gb_wd_a1 = fmul float %%gb_wd_ts, %s' % c(1.7))
        e('%gb_wd_a1p = fadd float %gb_wd_a1, %gb_wd_ph')
        e('%gb_wd_s1 = call float @dx.op.unary.f32(i32 13, float %gb_wd_a1p)')
        e('%%gb_wd_a2 = fmul float %%gb_wd_ts, %s' % c(0.43))
        e('%%gb_wd_ph2 = fmul float %%gb_wd_ph, %s' % c(0.37))
        e('%gb_wd_a2p = fadd float %gb_wd_a2, %gb_wd_ph2')
        e('%gb_wd_s2 = call float @dx.op.unary.f32(i32 13, float %gb_wd_a2p)')
        e('%%gb_wd_a3 = fmul float %%gb_wd_ts, %s' % c(3.1))
        e('%%gb_wd_ph3 = fmul float %%gb_wd_ph, %s' % c(1.9))
        e('%gb_wd_a3p = fadd float %gb_wd_a3, %gb_wd_ph3')
        e('%gb_wd_s3 = call float @dx.op.unary.f32(i32 13, float %gb_wd_a3p)')
        # gust = (0.55 + 0.45 sin1) * (0.65 + 0.35 sin2) in [0.065, 1]
        e('%%gb_wd_g10 = fmul float %%gb_wd_s1, %s' % c(0.45))
        e('%%gb_wd_g1 = fadd float %%gb_wd_g10, %s' % c(0.55))
        e('%%gb_wd_g20 = fmul float %%gb_wd_s2, %s' % c(0.35))
        e('%%gb_wd_g2 = fadd float %%gb_wd_g20, %s' % c(0.65))
        e('%gb_wd_gu = fmul float %gb_wd_g1, %gb_wd_g2')
        # distance fade
        e('%gb_wd_cxx2 = fmul float ' + OX + ', ' + OX)
        e('%gb_wd_cyy2 = fmul float ' + OY + ', ' + OY)
        e('%gb_wd_czz2 = fmul float ' + OZ + ', ' + OZ)
        e('%gb_wd_cd20 = fadd float %gb_wd_cxx2, %gb_wd_cyy2')
        e('%gb_wd_cd2 = fadd float %gb_wd_cd20, %gb_wd_czz2')
        e('%gb_wd_cdist = call float @dx.op.unary.f32(i32 24, float %gb_wd_cd2)')
        e('%gb_wd_fd0 = fsub float %gb_wd_cdist, %gb_wd_wnd2_x')
        e('%gb_wd_fd1 = fmul float %gb_wd_fd0, %gb_wd_wnd2_y')
        e('%%gb_wd_fd2 = fsub float %s, %%gb_wd_fd1' % c(1.0))
        e('%gb_wd_fade = call float @dx.op.unary.f32(i32 7, float %gb_wd_fd2)')
        e('%gb_wd_wamp0 = fmul float %gb_wd_wnd_z, %gb_wd_fade')
        e('%gb_wd_wl = fmul float %gb_wd_wamp0, %gb_wd_gu')
        e('%%gb_wd_wf0 = fmul float %%gb_wd_wamp0, %s' % c(0.2))
        e('%gb_wd_wf = fmul float %gb_wd_wf0, %gb_wd_s3')          # cross-wind flutter
        e('%gb_wd_wx0 = fmul float %gb_wd_wnd_x, %gb_wd_wl')
        e('%gb_wd_wx1 = fmul float %gb_wd_wnd_y, %gb_wd_wf')
        e('%gb_wd_wvx = fsub float %gb_wd_wx0, %gb_wd_wx1')
        e('%gb_wd_wy0 = fmul float %gb_wd_wnd_y, %gb_wd_wl')
        e('%gb_wd_wy1 = fmul float %gb_wd_wnd_x, %gb_wd_wf')
        e('%gb_wd_wvy = fadd float %gb_wd_wy0, %gb_wd_wy1')
        e('%%gb_awx = fadd float %s, %%gb_wd_wvx' % acc_x)
        e('%%gb_awy = fadd float %s, %%gb_wd_wvy' % acc_y)
        acc_x, acc_y = '%gb_awx', '%gb_awy'
    e('%%gb_axx = fmul float %s, %s' % (acc_x, acc_x))
    e('%%gb_ayy = fmul float %s, %s' % (acc_y, acc_y))
    e('%gb_al2 = fadd float %gb_axx, %gb_ayy')
    e('%%gb_al2c = call float @dx.op.binary.f32(i32 35, float %%gb_al2, float %s)' % c(1e-8))
    e('%gb_al = call float @dx.op.unary.f32(i32 24, float %gb_al2c)')
    e('%%gb_alc = call float @dx.op.binary.f32(i32 36, float %%gb_al, float %s)' % c(1.0))   # fmin
    e('%gb_inv = fdiv float 1.000000e+00, %gb_al')
    e('%%gb_ux = fmul float %s, %%gb_inv' % acc_x)
    e('%%gb_uy = fmul float %s, %%gb_inv' % acc_y)
    # gates: plant size (record +108), plant has baked sway, DLL header valid
    if H is None:
        # no-sway permutation (no plant record): bend only materials the DLL classified as foliage
        e('%%gb_cg = select i1 %%gb_hasslot, float %%gb_gw, float %s' % c(0.0), False)
        e('%%gb_gs = select i1 %%gb_valid, float %%gb_cg, float %s' % c(0.0), False)
    else:
        e('%%gb_rr = call %%dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %%dx.types.Handle %s, i32 %s, i32 %d, i8 1, i32 4)' % (H, IDX, PRIM_SPHERE_RADIUS), False)
        e('%gb_rad = extractvalue %dx.types.ResRet.f32 %gb_rr, 0', False)
        e('%%gb_gs0 = fsub float %s, %%gb_rad' % c(p['size_none']))
        e('%%gb_gs1 = fmul float %%gb_gs0, %s' % c(1.0 / (p['size_none'] - p['size_full'])))
        e('%gb_gs2 = call float @dx.op.unary.f32(i32 7, float %gb_gs1)')
        e('%%gb_nkr = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %s, i32 %s, i32 %d, i8 1, i32 4)' % (H, IDX, PRIM_NUMDEFORMKEYS), False)
        e('%gb_nk = extractvalue %dx.types.ResRet.i32 %gb_nkr, 0', False)
        e('%gb_has = icmp ne i32 %gb_nk, 0', False)
        e('%gb_on = and i1 %gb_has, %gb_valid' if not p.get('test_all') else '%gb_on = and i1 %gb_valid, %gb_valid', False)
        e('%%gb_gsr = select i1 %%gb_on, float %%gb_gs2, float %s' % c(0.0), False)
        e('%%gb_cg = select i1 %%gb_hasslot, float %%gb_gw, float %s' % c(1.0), False)
        e('%gb_gs = fmul float %gb_gsr, %gb_cg')
    # lean angle and trample squash
    e('%%gb_th0 = fmul float %%gb_alc, %s' % c(p['max_angle']))
    e('%gb_th = fmul float %gb_th0, %gb_gs')
    e('%%gb_q0 = call float @dx.op.binary.f32(i32 36, float %s, float %s)' % (acc_q, c(1.0)))
    e('%%gb_q1 = fmul float %%gb_q0, %s' % c(p['squash']))
    e('%gb_q = fmul float %gb_q1, %gb_gs')
    e('%%gb_qs = fsub float %s, %%gb_q' % c(1.0))
    e('%%gb_rzs = fmul float %s, %%gb_qs' % RZ)
    # Rodrigues rotation of (RX,RY,RZs) about the horizontal axis k = (-uy, ux, 0) by th
    e('%gb_sin = call float @dx.op.unary.f32(i32 13, float %gb_th)')
    e('%gb_cos = call float @dx.op.unary.f32(i32 12, float %gb_th)')
    e('%%gb_omc = fsub float %s, %%gb_cos' % c(1.0))
    e('%%gb_kr0 = fmul float %%gb_ux, %s' % RY)
    e('%%gb_kr1 = fmul float %%gb_uy, %s' % RX)
    e('%gb_kdr = fsub float %gb_kr0, %gb_kr1')
    e('%gb_kdo = fmul float %gb_kdr, %gb_omc')
    e('%gb_zs = fmul float %gb_rzs, %gb_sin')
    e('%%gb_nx0 = fmul float %s, %%gb_cos' % RX)
    e('%gb_nx1 = fmul float %gb_ux, %gb_zs')
    e('%gb_nx2 = fmul float %gb_uy, %gb_kdo')
    e('%gb_nx3 = fadd float %gb_nx0, %gb_nx1')
    e('%gb_nx = fsub float %gb_nx3, %gb_nx2')
    e('%%gb_ny0 = fmul float %s, %%gb_cos' % RY)
    e('%gb_ny1 = fmul float %gb_uy, %gb_zs')
    e('%gb_ny2 = fmul float %gb_ux, %gb_kdo')
    e('%gb_ny3 = fadd float %gb_ny0, %gb_ny1')
    e('%gb_ny = fadd float %gb_ny3, %gb_ny2')
    e('%gb_nz0 = fmul float %gb_rzs, %gb_cos')
    e('%%gb_nz1 = fmul float %%gb_uy, %s' % RY)
    e('%%gb_nz2 = fmul float %%gb_ux, %s' % RX)
    e('%gb_nz3 = fadd float %gb_nz1, %gb_nz2')
    e('%gb_nz4 = fmul float %gb_nz3, %gb_sin')
    e('%gb_nz = fsub float %gb_nz0, %gb_nz4')
    # displacement relative to the original vertex (zero exactly when th = 0 and no squash)
    e('%%gb_rmx = fsub float %%gb_nx, %s' % RX)
    e('%%gb_rmy = fsub float %%gb_ny, %s' % RY)
    e('%%gb_rmz = fsub float %s, %%gb_nz' % RZ)
    # shear mode: bend by the vertex height above the patch base (tips move, the base stays planted everywhere)
    e('%%gb_ph0 = call float @dx.op.binary.f32(i32 35, float %s, float %s)' % (RZ, c(0.0)))
    e('%gb_psh = fmul float %gb_ph0, %gb_sin')
    e('%gb_pmx = fmul float %gb_ux, %gb_psh')
    e('%gb_pmy = fmul float %gb_uy, %gb_psh')
    e('%gb_pmz0 = fmul float %gb_ph0, %gb_omc')
    e('%%gb_pq = fsub float %s, %%gb_rzs' % RZ)                                       # squash
    e('%gb_pmz = fadd float %gb_pmz0, %gb_pq')
    e('%gb_mx = select i1 %gb_part, float %gb_pmx, float %gb_rmx', False)
    e('%gb_my = select i1 %gb_part, float %gb_pmy, float %gb_rmy', False)
    e('%gb_mz = select i1 %gb_part, float %gb_pmz, float %gb_rmz', False)
    e('%%gb_X = fadd float %s, %%gb_mx' % X)
    e('%%gb_Y = fadd float %s, %%gb_my' % Y)
    e('%%gb_Z = fsub float %s, %%gb_mz' % Z)
    L, n_cur = insert_and_rewire(ir.L, ir.fend, ins, g, {X: '%gb_X', Y: '%gb_Y', Z: '%gb_Z'})
    if n_cur < 3:
        raise PatchError('too few uses rewired (%d)' % n_cur)
    txt = '\n'.join(L)

    # ---- 5. previous-frame position (motion vectors), if this permutation outputs one ----
    prev = 'none'
    ir2 = IR(txt)
    pe = ir2.output_index('PREVIOUS_CLIP_SPACE_POSITION0')
    if pe is not None:
        PX, PY, PZ, ph, pregs = ir2.projection(pe)
        if ph != view_handle or pregs == [0, 1, 2, 3] or pregs != list(range(pregs[0], pregs[0] + 4)):
            raise PatchError('unexpected previous-position projection %s %s' % (ph, pregs))
        if any(x.startswith('%gb_') for x in (PX, PY, PZ)):
            prev = 'shared'                       # already uses the bent current position
        else:
            pb = {ir2.defs[x][1] for x in (PX, PY, PZ)}
            pins = max(ir2.defs[x][0] for x in (PX, PY, PZ)) + 1
            pblk = ir2.defs[PX][1]
            if len(pb) != 1 or not all(ir2.available(v, pins, pblk) for v in ('%gb_mx', '%gb_my', '%gb_mz')):
                raise PatchError('bend offset not available at previous-position site')
            g2 = ['  %%gb_PX = fadd float %s, %%gb_mx%s' % (PX, prec),
                  '  %%gb_PY = fadd float %s, %%gb_my%s' % (PY, prec),
                  '  %%gb_PZ = fsub float %s, %%gb_mz%s' % (PZ, prec)]
            L2, n_prev = insert_and_rewire(ir2.L, ir2.fend, pins, g2, {PX: '%gb_PX', PY: '%gb_PY', PZ: '%gb_PZ'})
            if n_prev < 3:
                raise PatchError('too few previous-position uses rewired (%d)' % n_prev)
            txt = '\n'.join(L2)
            prev = 'bent (%d uses)' % n_prev

    # ---- 6. declarations ----
    need = {
        'dx.op.unary.f32': 'declare float @dx.op.unary.f32(i32, float) #%s',
        'dx.op.binary.f32': 'declare float @dx.op.binary.f32(i32, float, float) #%s',
        'dx.op.cbufferLoadLegacy.f32': 'declare %%dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %%dx.types.Handle, i32) #%s',
        'dx.op.rawBufferLoad.i32': 'declare %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %%dx.types.Handle, i32, i32, i8, i32) #%s',
        'dx.op.cbufferLoadLegacy.i32': 'declare %%dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %%dx.types.Handle, i32) #%s',
        'dx.op.binary.i32': 'declare i32 @dx.op.binary.i32(i32, i32, i32) #%s',
    }
    attrs = dict(re.findall(r'attributes #(\d+) = \{ ([^}]*) \}', txt))
    readnone = next((k for k, v in attrs.items() if 'readnone' in v), None)
    readonly = next((k for k, v in attrs.items() if 'readonly' in v), None)
    decls, types = [], []
    for fn, decl in need.items():
        if re.search(r'declare [^\n]*@' + re.escape(fn) + r'\(', txt) is None:
            a = readonly if fn.endswith('Load.i32') or 'cbuffer' in fn else readnone
            if a is None:
                raise PatchError('no attribute group for ' + fn)
            decls.append(decl % a)
    if '%dx.types.ResRet.i32 = type' not in txt:
        types.append('%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }')
    if '%dx.types.CBufRet.i32 = type' not in txt:
        types.append('%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }')
    if decls:
        k = txt.index('\ndeclare ')
        txt = txt[:k] + '\n' + '\n'.join(decls) + txt[k:]
    if types:
        k = txt.index('\ndefine void')
        txt = txt[:k] + '\n' + '\n'.join(types) + '\n' + txt[k:]
    return txt, dict(entry=ir.entry, X=X, Y=Y, Z=Z, RZ=RZ, rewired=n_cur, prev=prev, inserted=len(g))
