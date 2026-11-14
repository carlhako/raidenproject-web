#!/usr/bin/env python3
"""
Static recompiler: PS-X EXE (MIPS R3000A) -> C.

Seeds function entries from the REA/Ghidra procedure inventory
(analysis/rea_functions.txt), the EXE entry point, every `jal` target and any
extra entries listed in analysis/extra_entries.txt (discovered at runtime when the
dispatcher meets an unknown address).

Every function becomes `void f_XXXXXXXX(void)` operating on the global CPU
state.  Control flow inside a function is gotos; calls are native calls;
branches/jumps that reach another function entry become tail calls.
"""
import os, re, struct, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# usage: recomp.py <name> <path of EXE relative to extracted/>   (one run per program)
NAME = sys.argv[1]
EXE = os.path.join(ROOT, 'extracted', sys.argv[2])
OUT = os.path.join(ROOT, 'gen')

data = open(EXE, 'rb').read()
PC0, GP0, BASE, SIZE = struct.unpack('<4I', data[0x10:0x20])
IMG = data[0x800:0x800 + SIZE]
END = BASE + SIZE
import zlib
CRC = zlib.crc32(IMG) & 0xffffffff
# code range: REA's procedure inventory brackets the code (the rest of the image is data)
_rea_addrs = [int(l.split()[0], 16) for l in open(os.path.join(ROOT, 'analysis', f'rea_{NAME}.txt'))]
CODE_LO = min(_rea_addrs + [PC0]) & ~3
CODE_HI = min(END, max(_rea_addrs + [PC0]) + 0x2000)

def word(a):
    o = a - BASE
    return struct.unpack_from('<I', IMG, o)[0]

def in_code(a):
    return CODE_LO <= a < CODE_HI and (a & 3) == 0

def s16(x): return x - 0x10000 if x & 0x8000 else x

RN = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
      's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']

class I:
    __slots__ = ('a','w','op','rs','rt','rd','sa','imm','simm','tgt','kind','name')

def decode(a):
    w = word(a)
    i = I(); i.a = a; i.w = w
    op = w >> 26; i.op = op
    i.rs = (w >> 21) & 31; i.rt = (w >> 16) & 31; i.rd = (w >> 11) & 31; i.sa = (w >> 6) & 31
    i.imm = w & 0xffff; i.simm = s16(i.imm)
    i.kind = 'n'; i.tgt = None; i.name = '?'
    if op == 0:
        f = w & 63
        names = {0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',12:'syscall',13:'break',
                 16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',24:'mult',25:'multu',26:'div',27:'divu',
                 32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu'}
        i.name = names.get(f, 'bad')
        if i.name == 'jr': i.kind = 'ret' if i.rs == 31 else 'jr'
        elif i.name == 'jalr': i.kind = 'jalr'
    elif op == 1:
        r = i.rt
        i.name = {0:'bltz',1:'bgez',16:'bltzal',17:'bgezal'}.get(r, 'bad')
        if i.name != 'bad':
            i.kind = 'br'; i.tgt = (a + 4 + (i.simm << 2)) & 0xffffffff
            if r >= 16: i.kind = 'bral'
    elif op in (2, 3):
        i.name = 'j' if op == 2 else 'jal'
        i.tgt = ((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
        i.kind = 'j' if op == 2 else 'jal'
    elif op in (4, 5, 6, 7):
        i.name = ['beq','bne','blez','bgtz'][op-4]
        i.kind = 'br'; i.tgt = (a + 4 + (i.simm << 2)) & 0xffffffff
    elif 8 <= op <= 15:
        i.name = ['addi','addiu','slti','sltiu','andi','ori','xori','lui'][op-8]
    elif op == 16:
        if w & (1 << 25):
            i.name = 'rfe' if (w & 63) == 16 else 'bad'
        else:
            i.name = {0:'mfc0',4:'mtc0'}.get(i.rs, 'bad')
    elif op == 18:
        if w & (1 << 25): i.name = 'cop2'
        else: i.name = {0:'mfc2',2:'cfc2',4:'mtc2',6:'ctc2'}.get(i.rs, 'bad')
    elif op in (32,33,34,35,36,37,38,40,41,42,43,46,50,58):
        i.name = {32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',40:'sb',41:'sh',
                  42:'swl',43:'sw',46:'swr',50:'lwc2',58:'swc2'}[op]
    else:
        i.name = 'bad'
    if i.name == 'bad': i.kind = 'bad'
    return i

def is_branchy(i):
    return i.kind in ('br','bral','j','jal','jr','jalr','ret')

# ---------------------------------------------------------------- seeds
entries = set([PC0])
rea = os.path.join(ROOT, 'analysis', f'rea_{NAME}.txt')
for line in open(rea):
    a = int(line.split()[0], 16)
    if in_code(a): entries.add(a)
extra = os.path.join(ROOT, 'analysis', f'extra_{NAME}.txt')
if os.path.exists(extra):
    for line in open(extra):
        line = line.split('#')[0].strip()
        if line: entries.add(int(line, 16))
# hand-written replacements (analysis/patch_<name>.c): functions that cannot be translated 1:1
# (stack switching / longjmp-style control flow).  They keep their table slot, the generated
# body is skipped and the patch file is compiled with the program.
PATCH = os.path.join(ROOT, 'analysis', f'patch_{NAME}.c')
overrides = set()
if os.path.exists(PATCH):
    for m in re.finditer(r'void\s+' + NAME + r'_([0-9a-f]{8})\s*\(\s*void\s*\)\s*\{', open(PATCH).read()):
        overrides.add(int(m.group(1), 16))
    entries |= overrides
# ---------------------------------------------------------------- jump tables
def resolve_const(reg, a, lo, depth=0):
    """Find the constant value of `reg` just before address a, scanning backwards linearly."""
    if reg == 0: return 0
    if depth > 6: return None
    b = a - 4
    while b >= lo and a - b < 0x1000:
        i = decode(b)
        if i.kind not in ('bad',) and not is_branchy(i):
            dst = i.rd if i.op == 0 else i.rt
            if i.name in ('sw','sh','sb','swl','swr','swc2','mult','multu','div','divu','mthi','mtlo','mtc0','mtc2','ctc2','cop2','break','syscall'):
                dst = None
            if dst == reg:
                if i.name == 'lui': return i.imm << 16
                if i.name in ('addiu','addi'):
                    v = resolve_const(i.rs, b, lo, depth+1)
                    return None if v is None else (v + i.simm) & 0xffffffff
                if i.name == 'ori':
                    v = resolve_const(i.rs, b, lo, depth+1)
                    return None if v is None else v | i.imm
                if i.name in ('addu','or') and (i.rs == 0 or i.rt == 0):
                    return resolve_const(i.rs or i.rt, b, lo, depth+1)
                return None
        b -= 4
    return None

def find_jump_table(fentry, jr):
    rx = jr.rs
    lw = None
    a = jr.a - 4
    for _ in range(12):
        i = decode(a)
        if i.name == 'lw' and i.rt == rx: lw = i; break
        a -= 4
    if not lw: return None, None
    table = None
    # base register: lw rx, off(rb) with rb = addu(x, y)
    a = lw.a - 4
    for _ in range(16):
        i = decode(a)
        if i.name == 'addu' and i.rd == lw.rs:
            for r in (i.rs, i.rt):
                v = resolve_const(r, a, max(BASE, min(fentry, jr.a - 0x400)))
                if v is not None and BASE <= v < END + 0x10000:
                    table = (v + lw.simm) & 0xffffffff; break
            break
        a -= 4
    if table is None: return None, None
    n = None
    a = jr.a - 4
    for _ in range(24):
        i = decode(a)
        if i.name == 'sltiu': n = i.imm; break
        a -= 4
    tgts = []
    k = 0
    while True:
        if n is not None and k >= n: break
        if k >= 1024: break
        ta = table + 4 * k
        if not (BASE <= ta < END): break
        v = word(ta)
        if n is None and not (in_code(v) and abs(v - jr.a) < 0x8000): break
        if not in_code(v): break
        tgts.append(v); k += 1
    return table, tgts

# ---------------------------------------------------------------- function discovery
class Func:
    def __init__(self, e):
        self.e = e; self.addrs = set(); self.labels = set(); self.jt = {}
        self.calls = set(); self.tails = set(); self.bad = []

def discover(e):
    f = Func(e)
    work = [e]
    seen = set()
    def target(t):
        if t != e and t in entries:
            f.tails.add(t); return
        if not in_code(t):
            f.bad.append(t); return
        f.labels.add(t)
        work.append(t)
    while work:
        a = work.pop()
        while True:
            if a in seen: break
            if a != e and a in entries:
                f.tails.add(a); break
            if not in_code(a): f.bad.append(a); break
            seen.add(a); f.addrs.add(a)
            i = decode(a)
            if i.kind == 'bad':
                break
            if is_branchy(i):
                # delay slot is part of the function body
                d = a + 4
                f.addrs.add(d)
                if i.kind == 'br':
                    target(i.tgt); target(a + 8); break
                if i.kind == 'bral':
                    # bltzal/bgezal: treat as conditional call
                    if in_code(i.tgt): new_entries.add(i.tgt)
                    target(a + 8); break
                if i.kind == 'j':
                    target(i.tgt); break
                if i.kind == 'jal':
                    if in_code(i.tgt): f.calls.add(i.tgt)
                    target(a + 8); break
                if i.kind == 'jalr':
                    target(a + 8); break
                if i.kind == 'ret': break
                if i.kind == 'jr':
                    tb, tg = find_jump_table(e, i)
                    if tg:
                        f.jt[a] = tg
                        for t in tg: target(t)
                    break
            if i.name == 'break':
                break
            a += 4
            if a in seen or (a in entries and a != e):
                # fall through into an already-seen/other block -> needs label/tail
                if a in entries and a != e: f.tails.add(a)
                else: f.labels.add(a)
                break
    return f

new_entries = set()
funcs = {}
pending = sorted(entries)
while pending:
    for e in pending:
        funcs[e] = discover(e)
    added = set()
    for f in funcs.values():
        for c in f.calls:
            if c not in entries: added.add(c)
    added |= (new_entries - entries)
    new_entries.clear()
    for a in added: entries.add(a)
    if added:
        # re-discover everything (new entries may split existing functions)
        pending = sorted(entries)
        funcs = {}
    else:
        pending = []

# ---------------------------------------------------------------- code pointer harvesting
def looks_like_code(a, n=24):
    """Cheap validity check: decode forward until a return/jump; reject on bad words."""
    for k in range(n):
        if not in_code(a + 4 * k): return False
        i = decode(a + 4 * k)
        if i.kind == 'bad': return False
        if i.kind in ('ret', 'j', 'jr'): return True
    return True

def harvest():
    body = set()
    for f in funcs.values(): body |= f.addrs
    cands = set()
    # (a) words in the image that point into code
    for off in range(0, len(IMG) - 3, 4):
        v = struct.unpack_from('<I', IMG, off)[0]
        if in_code(v): cands.add(v)
    # (b) lui/addiu constant pairs inside discovered code
    for f in funcs.values():
        for a in f.addrs:
            i = decode(a)
            if i.name == 'lui':
                for k in range(1, 6):
                    j = decode(a + 4 * k)
                    if j.name in ('addiu', 'ori') and j.rs == i.rt:
                        v = ((i.imm << 16) + (j.simm if j.name == 'addiu' else j.imm)) & 0xffffffff
                        if in_code(v): cands.add(v)
                        break
    # (c) gap functions: undiscovered code right after a function's `jr ra` + delay slot
    for f in funcs.values():
        for a in f.addrs:
            if decode(a).kind == 'ret':
                g = a + 8
                while in_code(g) and word(g) == 0 and g not in body: g += 4
                if in_code(g): cands.add(g)
    new = set()
    for v in cands:
        if v in entries or v in body: continue
        if looks_like_code(v): new.add(v)
    return new

for _round in range(8):
    extra_found = harvest()
    if not extra_found: break
    entries |= extra_found
    funcs = {}
    pending = sorted(entries)
    while pending:
        for e in pending:
            funcs[e] = discover(e)
        added = set()
        for f in funcs.values():
            for c in f.calls:
                if c not in entries: added.add(c)
        added |= (new_entries - entries)
        new_entries.clear()
        entries |= added
        if added:
            pending = sorted(entries); funcs = {}
        else:
            pending = []
    print(f'harvest round {_round}: +{len(extra_found)} code-pointer entries')

# ---------------------------------------------------------------- code generation
def R(n): return '0' if n == 0 else f'R[{n}]'
def W(n): return None if n == 0 else f'R[{n}]'

def emit_simple(i):
    """Return C statement(s) for a non-control instruction."""
    rs, rt, rd, sa = R(i.rs), R(i.rt), i.rd, i.sa
    n = i.name
    def set_(r, e):
        return f'R[{r}] = {e};' if r else f'(void)({e});'
    if n == 'sll':
        if i.w == 0: return ';'
        return set_(rd, f'{rt} << {sa}')
    if n == 'srl': return set_(rd, f'{rt} >> {sa}')
    if n == 'sra': return set_(rd, f'(uint32_t)((int32_t){rt} >> {sa})')
    if n == 'sllv': return set_(rd, f'{rt} << ({rs} & 31)')
    if n == 'srlv': return set_(rd, f'{rt} >> ({rs} & 31)')
    if n == 'srav': return set_(rd, f'(uint32_t)((int32_t){rt} >> ({rs} & 31))')
    if n == 'syscall': return f'PC = 0x{i.a:08x}; op_syscall(0x{i.a:08x});'
    if n == 'break': return f'op_break(0x{i.a:08x}, {(i.w >> 6) & 0xfffff});'
    if n == 'mfhi': return set_(rd, 'HI') if rd else ';'
    if n == 'mflo': return set_(rd, 'LO') if rd else ';'
    if n == 'mthi': return f'HI = {rs};'
    if n == 'mtlo': return f'LO = {rs};'
    if n == 'mult': return f'{{ int64_t p = (int64_t)(int32_t){rs} * (int32_t){rt}; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }}'
    if n == 'multu': return f'{{ uint64_t p = (uint64_t){rs} * {rt}; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); }}'
    if n == 'div': return f'op_div({rs}, {rt});'
    if n == 'divu': return f'op_divu({rs}, {rt});'
    if n in ('add','addu'): return set_(rd, f'{rs} + {rt}')
    if n in ('sub','subu'): return set_(rd, f'{rs} - {rt}')
    if n == 'and': return set_(rd, f'{rs} & {rt}')
    if n == 'or': return set_(rd, f'{rs} | {rt}')
    if n == 'xor': return set_(rd, f'{rs} ^ {rt}')
    if n == 'nor': return set_(rd, f'~({rs} | {rt})')
    if n == 'slt': return set_(rd, f'(int32_t){rs} < (int32_t){rt}')
    if n == 'sltu': return set_(rd, f'{rs} < {rt}')
    t = i.rt
    if n in ('addi','addiu'): return set_(t, f'{rs} + (uint32_t){i.simm}')
    if n == 'slti': return set_(t, f'(int32_t){rs} < {i.simm}')
    if n == 'sltiu': return set_(t, f'{rs} < (uint32_t){i.simm}')
    if n == 'andi': return set_(t, f'{rs} & 0x{i.imm:x}')
    if n == 'ori': return set_(t, f'{rs} | 0x{i.imm:x}')
    if n == 'xori': return set_(t, f'{rs} ^ 0x{i.imm:x}')
    if n == 'lui': return set_(t, f'0x{i.imm << 16:x}')
    ea = f'{rs} + (uint32_t){i.simm}' if i.simm else rs
    if n == 'lb': return set_(t, f'(uint32_t)(int8_t)rd8({ea})')
    if n == 'lbu': return set_(t, f'rd8({ea})')
    if n == 'lh': return set_(t, f'(uint32_t)(int16_t)rd16({ea})')
    if n == 'lhu': return set_(t, f'rd16({ea})')
    if n == 'lw': return set_(t, f'rd32({ea})')
    if n == 'lwl': return set_(t, f'op_lwl({ea}, {R(t)})')
    if n == 'lwr': return set_(t, f'op_lwr({ea}, {R(t)})')
    if n == 'sb': return f'wr8({ea}, {R(t)});'
    if n == 'sh': return f'wr16({ea}, {R(t)});'
    if n == 'sw': return f'wr32({ea}, {R(t)});'
    if n == 'swl': return f'op_swl({ea}, {R(t)});'
    if n == 'swr': return f'op_swr({ea}, {R(t)});'
    if n == 'lwc2': return f'gte_write_data({t}, rd32({ea}));'
    if n == 'swc2': return f'wr32({ea}, gte_read_data({t}));'
    if n == 'mfc0': return set_(t, f'cop0_read({i.rd})')
    if n == 'mtc0': return f'cop0_write({i.rd}, {R(t)});'
    if n == 'rfe': return 'cop0_rfe();'
    if n == 'mfc2': return set_(t, f'gte_read_data({i.rd})')
    if n == 'cfc2': return set_(t, f'gte_read_ctrl({i.rd})')
    if n == 'mtc2': return f'gte_write_data({i.rd}, {R(t)});'
    if n == 'ctc2': return f'gte_write_ctrl({i.rd}, {R(t)});'
    if n == 'cop2': return f'gte_command(0x{i.w & 0x1ffffff:x});'
    return f'op_unimpl(0x{i.a:08x}, 0x{i.w:08x});'

def fname(a): return f'{NAME}_{a:08x}'

def gen_func(f):
    out = []
    p = out.append
    e = f.e
    # emission order: sorted addresses, skipping delay slots unless labelled
    delay = set()
    for a in f.addrs:
        if a in delay: continue
    branchy = {}
    for a in sorted(f.addrs):
        i = decode(a)
        if is_branchy(i): branchy[a] = i; delay.add(a + 4)
    order = [a for a in sorted(f.addrs) if a not in delay or a in f.labels or a == e]
    # labels needed: f.labels + entry; plus fallthrough fixups computed below
    need = set(f.labels)
    lines = []
    def xfer(t, back=False):
        """transfer control to address t (goto label or tail call)"""
        if t == e or (t in f.addrs and t not in entries):
            need.add(t)
            return (f'CHECK_EVENTS(); ' if back else '') + f'goto L_{t:08x};'
        if t in entries:
            return f'{fname(t)}(); return;'
        return f'op_badjump(0x{t:08x}); return;'
    idx = {a: k for k, a in enumerate(order)}
    blk = 0
    for k, a in enumerate(order):
        i = decode(a)
        nxt = order[k + 1] if k + 1 < len(order) else None
        lines.append(('L', a))
        cmt = f'/* {a:08x}: {i.w:08x} {i.name} */'
        if i.kind == 'bad':
            lines.append(f'op_unimpl(0x{a:08x}, 0x{i.w:08x}); return; {cmt}'); continue
        if i.name == 'break':
            lines.append(f'{emit_simple(i)} return; {cmt}')
            continue
        if not is_branchy(i):
            lines.append(f'{emit_simple(i)} {cmt}')
            ft = a + 4
            if ft not in f.addrs or (ft in delay and ft not in f.labels):
                # fallthrough leaves the function body (into another entry or nowhere)
                if ft in entries and ft != e:
                    lines.append(f'{fname(ft)}(); return;')
                elif ft == e:
                    lines.append(xfer(ft, True))
                else:
                    lines.append(f'op_badjump(0x{ft:08x}); return;')
            elif nxt != ft:
                lines.append(xfer(ft, ft <= a))
            continue
        d = decode(a + 4)
        ds = emit_simple(d) if not is_branchy(d) else f'op_unimpl(0x{a+4:08x}, 0x{d.w:08x}); /* branch in delay slot */'
        ds = f'{ds} /* {a+4:08x}: {d.w:08x} {d.name} (delay) */'
        rs, rt = R(i.rs), R(i.rt)
        if i.kind in ('br', 'bral'):
            n = i.name
            cond = {'beq': f'{rs} == {rt}', 'bne': f'{rs} != {rt}',
                    'blez': f'(int32_t){rs} <= 0', 'bgtz': f'(int32_t){rs} > 0',
                    'bltz': f'(int32_t){rs} < 0', 'bgez': f'(int32_t){rs} >= 0',
                    'bltzal': f'(int32_t){rs} < 0', 'bgezal': f'(int32_t){rs} >= 0'}[n]
            lines.append(f'{{ int c = {cond}; {cmt}')
            if i.kind == 'bral':
                lines.append(f'R[31] = 0x{a+8:08x};')
            lines.append(ds)
            if i.kind == 'bral':
                lines.append(f'if (c) {{ {fname(i.tgt)}(); }} }}')
                ft = a + 8
                lines.append(xfer(ft, ft <= a) if nxt != ft else ';')
                continue
            always = (n == 'beq' and i.rs == i.rt) or (n == 'bgez' and i.rs == 0)
            if always:
                lines.append(xfer(i.tgt, i.tgt <= a) + ' }')
                continue
            lines.append(f'if (c) {{ {xfer(i.tgt, i.tgt <= a)} }} }}')
            ft = a + 8
            if nxt != ft:
                lines.append(xfer(ft, ft <= a))
            continue
        if i.kind == 'j':
            lines.append(cmt); lines.append(ds)
            lines.append(xfer(i.tgt, i.tgt <= a))
            continue
        if i.kind == 'jal':
            lines.append(f'R[31] = 0x{a+8:08x}; {cmt}')
            lines.append(ds)
            if in_code(i.tgt):
                lines.append(f'CALL_CHECK({fname(i.tgt)}, 0x{i.tgt:08x}, 0x{a:08x});')
            else:
                lines.append(f'call_addr(0x{i.tgt:08x});')
            ft = a + 8
            if nxt != ft: lines.append(xfer(ft, ft <= a))
            continue
        if i.kind == 'jalr':
            lines.append(f'{{ uint32_t t = {rs}; R[{i.rd}] = 0x{a+8:08x}; {cmt}')
            lines.append(ds)
            lines.append(f'CALL_CHECK_IND(t, 0x{a:08x}); }}')
            ft = a + 8
            if nxt != ft: lines.append(xfer(ft, ft <= a))
            continue
        if i.kind == 'ret':
            lines.append(cmt); lines.append(ds); lines.append('return;')
            continue
        if i.kind == 'jr':
            lines.append(f'{{ uint32_t t = {rs}; {cmt}')
            lines.append(ds)
            if a in f.jt:
                lines.append('switch (t) {')
                for t in sorted(set(f.jt[a])):
                    if t in entries and t != e:
                        lines.append(f'case 0x{t:08x}: {fname(t)}(); return;')
                    else:
                        need.add(t)
                        lines.append(f'case 0x{t:08x}: goto L_{t:08x};')
                lines.append('}')
                lines.append('JR_CALL(t); }')
            else:
                lines.append('JR_CALL(t); }')
            continue
    # assemble with labels
    p(f'void {fname(e)}(void) {{')
    p(f'  FENTRY(0x{e:08x});')
    p('  uint32_t ra_in = R[31]; (void)ra_in;')
    # the body is emitted in address order and may start below the entry point
    p(f'  goto L_{e:08x};')
    # cycle accounting: CYC(n) at the start of every basic block
    markers = [k for k, ln in enumerate(lines) if isinstance(ln, tuple)]
    starts = set()
    prev_br = True
    for k in markers:
        a = lines[k][1]
        if a == e or a in need or prev_br: starts.add(k)
        prev_br = is_branchy(decode(a)) or decode(a).name == 'break'
    cyc = {}
    cur = None
    for k in markers:
        if k in starts: cur = k; cyc[cur] = 0
        cyc[cur] += 2 if is_branchy(decode(lines[k][1])) else 1
    for k, ln in enumerate(lines):
        if isinstance(ln, tuple):
            a = ln[1]
            if a == e:
                p(f'L_{a:08x}: CHECK_EVENTS();')
            elif a in need:
                p(f'L_{a:08x}:;')
            if k in cyc: p(f'  CYC({cyc[k]});')
            continue
        p('  ' + ln)
    p('}')
    return '\n'.join(out), need - f.addrs

# ---------------------------------------------------------------- write output
os.makedirs(OUT, exist_ok=True)
allf = sorted(funcs)
bodies = []
problems = []
for e in allf:
    if e in overrides: continue
    src, missing = gen_func(funcs[e])
    if missing: problems.append((e, missing))
    bodies.append((e, src))

hdr = ['#pragma once', '#include "../runtime/recomp.h"']
for e in allf: hdr.append(f'void {fname(e)}(void);')
hdr_extra = [e for e in overrides if e not in funcs]
for e in hdr_extra: hdr.append(f'void {fname(e)}(void);')
open(os.path.join(OUT, f'{NAME}_funcs.h'), 'w').write('\n'.join(hdr) + '\n')

NFILES = max(1, min(12, (len(bodies) + 199) // 200))
chunk = (len(bodies) + NFILES - 1) // NFILES
for k in range(NFILES):
    part = bodies[k*chunk:(k+1)*chunk]
    with open(os.path.join(OUT, f'{NAME}_code_{k:02d}.c'), 'w') as o:
        o.write(f'#include "{NAME}_funcs.h"\n#pragma GCC diagnostic ignored "-Wunused-label"\n\n')
        for e, s_ in part: o.write(s_ + '\n\n')

with open(os.path.join(OUT, f'{NAME}_dispatch.c'), 'w') as o:
    o.write(f'#include "{NAME}_funcs.h"\n\nstatic const FuncEntry tbl[] = {{\n')
    for e in sorted(set(allf) | overrides): o.write(f'  {{0x{e:08x}, {fname(e)}}},\n')
    o.write('};\n')
    o.write(f'const Program prog_{NAME} = {{"{NAME}", 0x{PC0:08x}, 0x{BASE:08x}, 0x{SIZE:x}, 0x{CRC:08x}u, tbl, {len(set(allf) | overrides)}}};\n')
with open(os.path.join(OUT, f'{NAME}.files'), 'w') as o:
    o.write(' '.join([f'gen/{NAME}_code_{k:02d}.c' for k in range(NFILES)] + [f'gen/{NAME}_dispatch.c'] + ([f'gen/{NAME}_patch.c'] if overrides else [])))
if overrides:
    import shutil
    shutil.copy(PATCH, os.path.join(OUT, f'{NAME}_patch.c'))

ninsn = sum(len(f.addrs) for f in funcs.values())
print(f'[{NAME}] functions: {len(allf)}  emitted instructions: {ninsn}  jump tables: {sum(len(f.jt) for f in funcs.values())}  code {CODE_LO:08x}-{CODE_HI:08x} crc {CRC:08x}')
bad = [(f.e, f.bad) for f in funcs.values() if f.bad]
for e, b in bad[:10]: print(f'  {e:08x}: flow leaves image -> {[hex(x) for x in b[:4]]}')
for e, m in problems[:10]: print(f'  {e:08x}: missing labels {[hex(x) for x in m]}')
unk = collections.Counter()
for f in funcs.values():
    for a in f.addrs:
        if decode(a).kind == 'bad': unk[f.e] += 1
print('functions containing undecodable words:', len(unk), [hex(x) for x in list(unk)[:20]])
jrs = [(f.e, a) for f in funcs.values() for a in f.addrs if decode(a).kind == 'jr' and a not in f.jt]
print('unresolved jr (dynamic):', len(jrs), [(hex(x), hex(y)) for x, y in jrs[:20]])
