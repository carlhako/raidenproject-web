#!/usr/bin/env python3 -I
"""Copy the disc image to dist/data/disc.img and write disc.json (sector count, size, sha256)."""
import hashlib, json, os, re, sys
src = sys.argv[1] if len(sys.argv) > 1 else None
if not src:
    import glob; src = sorted(glob.glob('disc/*.bin'))[0]
n = os.path.getsize(src) // 2352 * 2352; lba = n // 2352
os.makedirs('dist/data', exist_ok=True)
h = hashlib.sha256()
with open(src, 'rb') as f, open('dist/data/disc.img', 'wb') as o:
    left = n
    while left:
        b = f.read(min(left, 1 << 22))
        if not b: break
        h.update(b); o.write(b); left -= len(b)
meta = {'sectors': lba, 'size': n, 'sha256': h.hexdigest()}
json.dump(meta, open('dist/data/disc.json', 'w'))
print(meta)
