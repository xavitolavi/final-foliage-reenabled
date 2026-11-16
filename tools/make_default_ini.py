"""Write the DLL's built-in default FoliageBend.ini (DEFAULT_INI in dll/foliage.inc) to a file and
compare its settings with another ini (e.g. the one installed in the game).
usage: python tools/make_default_ini.py <out.ini> [compare.ini]"""
import ast, difflib, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = open(os.path.join(ROOT, 'dll', 'foliage.inc'), encoding='utf-8').read()
a = s.index('static const char* DEFAULT_INI =') + len('static const char* DEFAULT_INI =')
b = s.index(';\n', a)
lits = re.findall(r'"((?:[^"\\]|\\.)*)"', s[a:b])
txt = ''.join(ast.literal_eval('"' + l + '"') for l in lits)
with open(sys.argv[1], 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(txt)
print('wrote', sys.argv[1], len(txt), 'chars')
if len(sys.argv) > 2:
    def kv(t):
        return [l.strip() for l in t.splitlines() if l.strip() and not l.strip().startswith(';')]
    other = open(sys.argv[2], encoding='utf-8').read()
    d = list(difflib.unified_diff(kv(other), kv(txt), sys.argv[2], 'default', lineterm=''))
    print('\n'.join(d) if d else 'settings identical')
