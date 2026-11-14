/* Fallback MIPS interpreter for guest code that static recompilation did not discover
 * (e.g. an indirect call target in a game mode never exercised during analysis).
 * It runs until control returns to the caller (jr to the entry-time $ra) and calls straight
 * back into recompiled functions whenever it reaches one.  Each address that needs it is
 * logged so it can be added to analysis/extra_entries.txt for the next recompile. */
#include "psx.h"

FuncPtr func_lookup(uint32_t addr);

static inline int32_t s16(uint32_t x) { return (int16_t)(x & 0xffff); }

/* execute one non-control instruction; returns 0 if it is a control-flow instruction */
static int exec_simple(uint32_t pc, uint32_t w) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    uint32_t imm = w & 0xffff; int32_t si = s16(w);
    #define SET(r, v) do { uint32_t _v = (v); if (r) R[r] = _v; } while (0)
    switch (op) {
    case 0:
        switch (w & 63) {
        case 0: SET(rd, R[rt] << sa); return 1;
        case 2: SET(rd, R[rt] >> sa); return 1;
        case 3: SET(rd, (uint32_t)((int32_t)R[rt] >> sa)); return 1;
        case 4: SET(rd, R[rt] << (R[rs] & 31)); return 1;
        case 6: SET(rd, R[rt] >> (R[rs] & 31)); return 1;
        case 7: SET(rd, (uint32_t)((int32_t)R[rt] >> (R[rs] & 31))); return 1;
        case 8: case 9: return 0;
        case 12: op_syscall(pc); return 1;
        case 13: op_break(pc, (w >> 6) & 0xfffff); return 1;
        case 16: SET(rd, HI); return 1;
        case 17: HI = R[rs]; return 1;
        case 18: SET(rd, LO); return 1;
        case 19: LO = R[rs]; return 1;
        case 24: { int64_t p = (int64_t)(int32_t)R[rs] * (int32_t)R[rt]; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); return 1; }
        case 25: { uint64_t p = (uint64_t)R[rs] * R[rt]; LO = (uint32_t)p; HI = (uint32_t)(p >> 32); return 1; }
        case 26: op_div(R[rs], R[rt]); return 1;
        case 27: op_divu(R[rs], R[rt]); return 1;
        case 32: case 33: SET(rd, R[rs] + R[rt]); return 1;
        case 34: case 35: SET(rd, R[rs] - R[rt]); return 1;
        case 36: SET(rd, R[rs] & R[rt]); return 1;
        case 37: SET(rd, R[rs] | R[rt]); return 1;
        case 38: SET(rd, R[rs] ^ R[rt]); return 1;
        case 39: SET(rd, ~(R[rs] | R[rt])); return 1;
        case 42: SET(rd, (int32_t)R[rs] < (int32_t)R[rt]); return 1;
        case 43: SET(rd, R[rs] < R[rt]); return 1;
        }
        break;
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: return 0;
    case 8: case 9: SET(rt, R[rs] + (uint32_t)si); return 1;
    case 10: SET(rt, (int32_t)R[rs] < si); return 1;
    case 11: SET(rt, R[rs] < (uint32_t)si); return 1;
    case 12: SET(rt, R[rs] & imm); return 1;
    case 13: SET(rt, R[rs] | imm); return 1;
    case 14: SET(rt, R[rs] ^ imm); return 1;
    case 15: SET(rt, imm << 16); return 1;
    case 16:
        if (w & (1u << 25)) { if ((w & 63) == 16) cop0_rfe(); return 1; }
        if (rs == 0) SET(rt, cop0_read((int)rd)); else if (rs == 4) cop0_write((int)rd, R[rt]);
        return 1;
    case 18:
        if (w & (1u << 25)) { gte_command(w & 0x1ffffff); return 1; }
        switch (rs) {
        case 0: SET(rt, gte_read_data((int)rd)); return 1;
        case 2: SET(rt, gte_read_ctrl((int)rd)); return 1;
        case 4: gte_write_data((int)rd, R[rt]); return 1;
        case 6: gte_write_ctrl((int)rd, R[rt]); return 1;
        }
        break;
    case 32: SET(rt, (uint32_t)(int8_t)rd8(R[rs] + (uint32_t)si)); return 1;
    case 33: SET(rt, (uint32_t)(int16_t)rd16(R[rs] + (uint32_t)si)); return 1;
    case 34: SET(rt, op_lwl(R[rs] + (uint32_t)si, R[rt])); return 1;
    case 35: SET(rt, rd32(R[rs] + (uint32_t)si)); return 1;
    case 36: SET(rt, rd8(R[rs] + (uint32_t)si)); return 1;
    case 37: SET(rt, rd16(R[rs] + (uint32_t)si)); return 1;
    case 38: SET(rt, op_lwr(R[rs] + (uint32_t)si, R[rt])); return 1;
    case 40: wr8(R[rs] + (uint32_t)si, R[rt]); return 1;
    case 41: wr16(R[rs] + (uint32_t)si, R[rt]); return 1;
    case 42: op_swl(R[rs] + (uint32_t)si, R[rt]); return 1;
    case 43: wr32(R[rs] + (uint32_t)si, R[rt]); return 1;
    case 46: op_swr(R[rs] + (uint32_t)si, R[rt]); return 1;
    case 50: gte_write_data((int)rt, rd32(R[rs] + (uint32_t)si)); return 1;
    case 58: wr32(R[rs] + (uint32_t)si, gte_read_data((int)rt)); return 1;
    }
    #undef SET
    op_unimpl(pc, w);
    return 1;
}

static void exec_delay(uint32_t pc) {
    uint32_t w = rd32(pc);
    if (!exec_simple(pc, w)) fatal("interp: branch in delay slot at %08x", pc);
}

static int callable(uint32_t t) {
    uint32_t p = t & 0x1fffffff;
    return p == 0xa0 || p == 0xb0 || p == 0xc0 || func_lookup(t) != NULL;
}

void interp_run(uint32_t pc) {
    uint32_t sentinel = R[31];
    int budget = 0;
    for (;;) {
        if (++budget >= 32) { CYC(budget); budget = 0; CHECK_EVENTS(); }
        PC = pc;
        uint32_t w = rd32(pc);
        if (exec_simple(pc, w)) { pc += 4; continue; }
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        int32_t off = s16(w) << 2;
        uint32_t btgt = pc + 4 + (uint32_t)off;
        switch (op) {
        case 0: {
            uint32_t t = R[rs];
            if ((w & 63) == 9) {                    /* jalr */
                uint32_t rd = (w >> 11) & 31;
                if (rd) R[rd] = pc + 8;
                exec_delay(pc + 4);
                call_addr(t);
                pc += 8; continue;
            }
            exec_delay(pc + 4);                     /* jr */
            if (t == sentinel) return;
            if (callable(t)) {
                call_addr(t);
                if (rs != 31 && R[31] != sentinel) { pc = R[31]; continue; }   /* returned to a hand-made $ra */
                return;
            }
            pc = t; continue;
        }
        case 1: {
            int c = (rt & 1) ? (int32_t)R[rs] >= 0 : (int32_t)R[rs] < 0;
            if (rt & 16) R[31] = pc + 8;
            exec_delay(pc + 4);
            if (rt & 16) { if (c) call_addr(btgt); pc += 8; continue; }
            pc = c ? btgt : pc + 8; continue;
        }
        case 2: case 3: {
            uint32_t t = ((pc + 4) & 0xf0000000u) | ((w & 0x3ffffff) << 2);
            if (op == 3) {
                R[31] = pc + 8; exec_delay(pc + 4); call_addr(t); pc += 8; continue;
            }
            exec_delay(pc + 4);
            if (callable(t)) { call_addr(t); return; }
            pc = t; continue;
        }
        default: {
            int c;
            switch (op) {
            case 4: c = R[rs] == R[rt]; break;
            case 5: c = R[rs] != R[rt]; break;
            case 6: c = (int32_t)R[rs] <= 0; break;
            default: c = (int32_t)R[rs] > 0; break;
            }
            exec_delay(pc + 4);
            pc = c ? btgt : pc + 8; continue;
        }
        }
    }
}
