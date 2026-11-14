/* Hand-written replacement for the game's task launcher (stack-switching "CallTask").
 *
 * 0x8001d8b4 saves ra/s0-s7/sp/fp in globals, runs the task function a1(a0) on the same stack
 * (sp -= 128) and, when the task ends, restores them.  A task can also leave early by calling
 * 0x8001d918 from anywhere deep in its call chain: that restores the saved registers and
 * returns straight to the caller of the launcher, discarding the task's frames.  Native code
 * has no way to resume the caller's frame, so the early exit is a longjmp. */
#include "../runtime/recomp.h"
#include <setjmp.h>

static jmp_buf *task_jb;
static unsigned task_epoch;   /* task_jb is only valid within the Exec() generation that set it */

static void restore_context(void) {
    R[1] = 0x80100000;            /* $at is left holding the high half of the save area address */
    R[31] = rd32(0x80102944);
    for (int i = 0; i < 8; i++) R[16 + i] = rd32(0x80102948 + 4 * i);
    R[29] = rd32(0x80102968);
    R[30] = rd32(0x8010296c);
}

void jj2h_8001d8b4(void) {
    R[1] = 0x80100000;
    wr32(0x80102944, R[31]);
    for (int i = 0; i < 8; i++) wr32(0x80102948 + 4 * i, R[16 + i]);
    wr32(0x80102968, R[29]);
    wr32(0x8010296c, R[30]);
    R[29] -= 128;
    jmp_buf jb, *prev = task_epoch == exec_epoch ? task_jb : NULL;
    task_jb = &jb; task_epoch = exec_epoch;
    if (setjmp(jb) == 0) {
        R[31] = 0x8001d918;
        R[16] = R[4];
        call_addr(R[5]);
    }
    task_jb = prev;
    restore_context();
}

void jj2h_8001d918(void) {
    if (task_jb && task_epoch == exec_epoch) longjmp(*task_jb, 1);
    restore_context();
}
