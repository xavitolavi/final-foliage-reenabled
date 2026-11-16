"""Copy every setting of an ini (e.g. the tuned one in the game folder) into the DLL's built-in DEFAULT_INI
and into the per-class touch-effect defaults table. Comments and layout of DEFAULT_INI are kept.
usage: python tools/adopt_ini.py <tuned.ini>"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, 'dll', 'foliage.inc')
tuned = {}
sec = None
for l in open(sys.argv[1], encoding='utf-8').read().splitlines():
    l = l.strip()
    if not l or l.startswith(';'):
        continue
    if l.startswith('['):
        sec = l.lower(); continue
    if '=' in l:
        k, v = l.split('=', 1)
        tuned[(sec, k.strip().lower())] = v.strip()

s = open(P, encoding='utf-8').read()
a = s.index('static const char* DEFAULT_INI =')
b = s.index(';\n', a)
out, sec, changed = [], None, []
for l in s[a:b].split('\n'):
    m = re.match(r'^"(\[[^\]]+\])\\n"', l)
    if m:
        sec = m.group(1).lower()
    m = re.match(r'^"([A-Za-z0-9_]+)=(.*)\\n"(;?)$', l)
    if m and (sec, m.group(1).lower()) in tuned:
        v = tuned[(sec, m.group(1).lower())]
        if v != m.group(2):
            changed.append('%s %s: %s -> %s' % (sec, m.group(1), m.group(2), v))
            l = '"%s=%s\\n"%s' % (m.group(1), v.replace('\\', '\\\\').replace('"', '\\"'), m.group(3))
    out.append(l)
s = s[:a] + '\n'.join(out) + s[b:]

# per-class sound volume / radius / vfx numbers in CLASS_FX_DEFAULT (row k = class k)
t0 = s.index('CLASS_FX_DEFAULT[6] = {'); t1 = s.index('};', t0)
rows = s[t0:t1].split('\n'); k = -1
for i, r in enumerate(rows):
    m = re.match(r'^(\s*\{ )(\d), ([\d.]+)f, ([\d.]+)f, ("[^"]*"), ([\d.]+)f, ([\d.]+)f, ([\d.]+)f( \},.*)$', r)
    if not m:
        continue
    k += 1
    if k == 0:
        continue
    sc = '[class%d]' % k
    def g(key, cur):
        v = tuned.get((sc, key))
        try:
            return '%gf' % float(v) if v is not None else cur + 'f'
        except ValueError:
            return cur + 'f'
    size = {'small': '1', 'medium': '2', 'large': '3', 'none': '0'}.get(tuned.get((sc, 'sound'), ''), m.group(2))
    vfx = tuned.get((sc, 'vfx'))
    vfx = '"%s"' % ('' if vfx == 'none' else vfx) if vfx is not None else m.group(5)
    rows[i] = '%s%s, %s, %s, %s, %s, %s, %s%s' % (m.group(1), size, g('sound_volume', m.group(3)), g('touch_radius_cm', m.group(4)), vfx,
                                              g('vfx_scale', m.group(6)), g('vfx_height_cm', m.group(7)), g('vfx_chance', m.group(8)), m.group(9))
    rows[i] = re.sub(r'(?<![\d.])(\d+)f\b', r'\1.f', rows[i])   # 1f -> 1.f (valid C++ literal); 0.5f stays
s = s[:t0] + '\n'.join(rows) + s[t1:]
open(P, 'w', encoding='utf-8').write(s)
print('\n'.join(changed) or 'no changes')
print(s[t0:s.index('};', t0)])
