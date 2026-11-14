#!/usr/bin/env python3
"""Wrap a PS-X EXE as a MIPS32 LE ELF executable for Ghidra/REA analysis."""
import sys, struct
d = open(sys.argv[1], 'rb').read()
pc, gp, addr, size = struct.unpack('<4I', d[0x10:0x20])
bss_addr, bss_size = struct.unpack('<2I', d[0x28:0x30])
text = d[0x800:0x800+size]
shstr = b'\0.text\0.shstrtab\0'
ehsz, phsz, shsz = 52, 32, 40
text_off = 0x1000
shstr_off = text_off + len(text)
sh_off = (shstr_off + len(shstr) + 3) & ~3
eh = struct.pack('<4sBBBB8xHHIIIIIHHHHHH', b'\x7fELF', 1, 1, 1, 0,
                 2, 8, 1, pc, ehsz, sh_off, 0x50000000, ehsz, phsz, 1, shsz, 3, 2)
ph = struct.pack('<8I', 1, text_off, addr, addr, len(text), len(text) + 0x40000, 7, 0x1000)
sh0 = b'\0' * shsz
sh1 = struct.pack('<10I', 1, 1, 6 | 1, addr, text_off, len(text), 0, 0, 16, 0)
sh2 = struct.pack('<10I', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
out = bytearray(eh + ph)
out += b'\0' * (text_off - len(out)) + text + shstr
out += b'\0' * (sh_off - len(out)) + sh0 + sh1 + sh2
open(sys.argv[2], 'wb').write(out)
print(f'pc={pc:#x} gp={gp:#x} addr={addr:#x} size={size:#x}')
