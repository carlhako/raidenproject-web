#!/usr/bin/env python3
"""Tiny MIPS disassembler for exploring an EXE: dis.py <exe> <start> <end>"""
import struct, sys
RN=['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7','s0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
def dis(w, a):
    op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; rd=(w>>11)&31; sa=(w>>6)&31; imm=w&0xffff; si=imm-0x10000 if imm&0x8000 else imm
    r=lambda n: '$'+RN[n]
    if w==0: return 'nop'
    if op==0:
        f=w&63
        t={0:'sll',2:'srl',3:'sra'}
        if f in t: return f'{t[f]} {r(rd)},{r(rt)},{sa}'
        t={4:'sllv',6:'srlv',7:'srav'}
        if f in t: return f'{t[f]} {r(rd)},{r(rt)},{r(rs)}'
        if f==8: return f'jr {r(rs)}'
        if f==9: return f'jalr {r(rd)},{r(rs)}'
        if f==12: return 'syscall'
        if f in (16,18): return f'{"mfhi" if f==16 else "mflo"} {r(rd)}'
        if f in (17,19): return f'{"mthi" if f==17 else "mtlo"} {r(rs)}'
        t={24:'mult',25:'multu',26:'div',27:'divu'}
        if f in t: return f'{t[f]} {r(rs)},{r(rt)}'
        t={32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu'}
        if f in t: return f'{t[f]} {r(rd)},{r(rs)},{r(rt)}'
    if op==1: return f'{"bltz" if rt==0 else "bgez" if rt==1 else "bltzal" if rt==16 else "bgezal"} {r(rs)},{a+4+si*4:08x}'
    if op==2: return f'j {((a+4)&0xf0000000)|((w&0x3ffffff)<<2):08x}'
    if op==3: return f'jal {((a+4)&0xf0000000)|((w&0x3ffffff)<<2):08x}'
    if op in (4,5): return f'{"beq" if op==4 else "bne"} {r(rs)},{r(rt)},{a+4+si*4:08x}'
    if op in (6,7): return f'{"blez" if op==6 else "bgtz"} {r(rs)},{a+4+si*4:08x}'
    t={8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',14:'xori'}
    if op in t: return f'{t[op]} {r(rt)},{r(rs)},{si if op<12 else hex(imm)}'
    if op==15: return f'lui {r(rt)},{hex(imm)}'
    t={32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',40:'sb',41:'sh',42:'swl',43:'sw',46:'swr',50:'lwc2',58:'swc2'}
    if op in t: return f'{t[op]} {r(rt)},{si}({r(rs)})'
    if op==16: return f'cop0 {w:08x}'
    if op==18: return f'cop2 {w:08x}'
    return f'.word {w:08x}'
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    pc,gp,base,size=struct.unpack('<4I',d[0x10:0x20])
    img=d[0x800:]
    for a in range(int(sys.argv[2],16),int(sys.argv[3],16),4):
        w=struct.unpack_from('<I',img,a-base)[0]
        print(f'{a:08x}: {w:08x}  {dis(w,a)}')
