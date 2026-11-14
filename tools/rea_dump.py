#!/usr/bin/env python3
"""Pretty-print decompilations from an rea_mcp batch output into a .c file."""
import json, sys
out = []
for r in json.load(open(sys.argv[1])):
    res = r['result']
    sc = res.get('structuredContent') if isinstance(res, dict) else None
    if r['call']['tool'] != 'batch_decompile': continue
    if not sc:
        out.append(json.dumps(res)[:2000]); continue
    nr = sc.get('normalized_result', sc)
    items = nr.get('results') or nr.get('procedures') or nr.get('items') or nr
    if isinstance(items, dict): items = list(items.values())
    for it in items:
        if isinstance(it, dict):
            addr = it.get('address') or it.get('selector')
            code = it.get('pseudo_code') or it.get('pseudocode') or it.get('code') or it.get('text') or json.dumps(it)[:3000]
            out.append(f'// ===== {addr}\n{code}\n')
        else:
            out.append(str(it))
open(sys.argv[2], 'w').write('\n'.join(out))
