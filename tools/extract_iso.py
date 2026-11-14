#!/usr/bin/env python3
"""Extract files from a PSX MODE2/2352 BIN image (ISO9660).
Form1 files -> 2048-byte user data. Files containing Form2 (XA/STR) sectors are
additionally written raw (2352 bytes/sector) as <name>.raw."""
import sys, os, struct
SEC = 2352
def main(binpath, outdir):
    f = open(binpath, 'rb')
    def sector(lba):
        f.seek(lba * SEC); return f.read(SEC)
    def data(lba):
        return sector(lba)[24:24+2048]
    pvd = data(16)
    assert pvd[1:6] == b'CD001', pvd[:8]
    root = pvd[156:156+34]
    listing = []
    def walk(lba, size, path):
        buf = b''.join(data(lba + i) for i in range((size + 2047)//2048))
        off = 0
        while off < len(buf):
            ln = buf[off]
            if ln == 0:
                off = (off // 2048 + 1) * 2048; continue
            rec = buf[off:off+ln]
            elba = struct.unpack('<I', rec[2:6])[0]
            esz = struct.unpack('<I', rec[10:14])[0]
            flags = rec[25]
            nl = rec[32]; name = rec[33:33+nl]
            off += ln
            if name in (b'\x00', b'\x01'): continue
            name = name.decode('ascii').split(';')[0]
            p = os.path.join(path, name)
            if flags & 2:
                os.makedirs(os.path.join(outdir, p), exist_ok=True)
                walk(elba, esz, p)
            else:
                nsec = (esz + 2047)//2048
                form2 = False
                out = bytearray()
                raws = []
                for i in range(nsec):
                    s = sector(elba + i); raws.append(s)
                    if s[18] & 0x20: form2 = True
                    out += s[24:24+2048]
                with open(os.path.join(outdir, p), 'wb') as o: o.write(bytes(out[:esz]))
                if form2:
                    # XA/STR: size field counts 2048/sector; dump full raw sectors
                    with open(os.path.join(outdir, p) + '.raw', 'wb') as o: o.write(b''.join(raws))
                listing.append((p, elba, esz, form2))
    walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
    with open(os.path.join(outdir, '_listing.txt'), 'w') as o:
        for p, l, s, x in listing: o.write(f'{l:8d} {s:10d} {"XA" if x else "  "} {p}\n')
    for p, l, s, x in listing: print(f'{l:8d} {s:10d} {"XA" if x else "  "} {p}')
if __name__ == '__main__': main(sys.argv[1], sys.argv[2])
