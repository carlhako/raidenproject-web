#!/usr/bin/env python3
"""Read a `./raiden -v` log on stdin and add every address the interpreter had to handle
("note: [prog] call XXXXXXXX not recompiled") to analysis/extra_<prog>.txt, so the next
recompile covers it.  Addresses outside the program image (overlay code) are ignored."""
import os, re, struct, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXES = {'scus': 'SCUS_944.02', 'jj2h': 'JJ2X/PSJJ2H.EXE', 'jj2v': 'JJ2X/PSJJ2V.EXE', 'jj3': 'JJ3X/PSJJ3.EXE'}
new = {}
for line in sys.stdin:
    m = re.match(r'note: \[(\w+)\] (?:call|jump) ([0-9a-f]{8}) not recompiled', line)
    if m: new.setdefault(m.group(1), set()).add(int(m.group(2), 16))
for prog, addrs in new.items():
    d = open(os.path.join(ROOT, 'extracted', EXES[prog]), 'rb').read()
    _, _, base, size = struct.unpack('<4I', d[0x10:0x20])
    path = os.path.join(ROOT, 'analysis', f'extra_{prog}.txt')
    have = set()
    if os.path.exists(path):
        for l in open(path):
            l = l.split('#')[0].strip()
            if l: have.add(int(l, 16))
    add = sorted(a for a in addrs if base <= a < base + size and a not in have and a % 4 == 0)
    with open(path, 'a') as o:
        for a in add: o.write(f'{a:08x}\n')
    print(f'{prog}: +{len(add)} entries ({len(addrs) - len(add)} outside image or known)')
