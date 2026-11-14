/* Hand-written replacement for the game's task launcher (stack-switching "CallTask"), as in
 * the other executables, but the saved context lives in the scratchpad (0x1f80001c).
 * 0x800426a4 saves ra/s0/s1/sp, runs a1(a0) on the same stack (sp -= 256); 0x800426c8 restores
 * them and returns to the launcher's caller; a task may call it from deep in its call chain to
 * leave early, which native code can only do with a longjmp. */
#include "../runtime/recomp.h"
#include <setjmp.h>

static jmp_buf *task_jb;
static unsigned task_epoch;   /* task_jb is only valid within the Exec() generation that set it */

static void restore_context(void) {
    R[2] = 0x1f80001c;            /* the original restore code leaves its base pointer in $v0 */
    R[31] = rd32(0x1f80001c);
    R[16] = rd32(0x1f800020);
    R[17] = rd32(0x1f800024);
    R[29] = rd32(0x1f800028);
}

void jj3_800426a4(void) {
    R[2] = 0x1f80001c;
    wr32(0x1f80001c, R[31]);
    wr32(0x1f800020, R[16]);
    wr32(0x1f800024, R[17]);
    wr32(0x1f800028, R[29]);
    R[29] -= 256;
    jmp_buf jb, *prev = task_epoch == exec_epoch ? task_jb : NULL;
    task_jb = &jb; task_epoch = exec_epoch;
    if (setjmp(jb) == 0) {
        R[31] = 0x800426c8;
        R[16] = R[4];
        call_addr(R[5]);
    }
    task_jb = prev;
    restore_context();
}

void jj3_800426c8(void) {
    if (task_jb && task_epoch == exec_epoch) longjmp(*task_jb, 1);
    restore_context();
}
