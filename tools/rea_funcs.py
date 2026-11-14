#!/usr/bin/env python3
"""Turn rea search JSON into analysis/rea_<name>.txt ('0xADDR name' per line)."""
import json, sys, re
src, dst = sys.argv[1:3]
d = json.load(open(src)); d = d.get('data', d)
out = {}
for it in d['normalized_result']:
    a = it.get('address') or it.get('entry') or ''
    m = re.search(r'0x[0-9a-fA-F]+', str(a)) or re.search(r'[0-9a-fA-F]{8}', str(a))
    if not m: continue
    v = int(m.group(0), 16)
    out[v] = it.get('value', 'FUN')
with open(dst, 'w') as o:
    for a in sorted(out): o.write(f'0x{a:08x} {out[a]}\n')
print(dst, len(out))
