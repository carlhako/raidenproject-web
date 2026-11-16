/* Modified-mode hooks for Raiden II (jj3). Each address in analysis/hooks_jj3.txt gets a wrapper
 * (gen/jj3_hooks.c) that calls mod_jj3_<addr>() here when mod_mode is set, and the original
 * recompiled function (jj3_<addr>_orig) otherwise. A hook runs with the game's CPU state as the
 * original would (arguments in R[4..7], object pointer usually in R[4]) and may call the original.
 *
 * Game RAM layout: docs/RAM_MAP.md. Keep hooks deterministic: same inputs -> same RAM, on every
 * machine, because netplay replays only the input log. */
#include "recomp.h"

void jj3_8005a18c_orig(void);

/* Ship update. Placeholder that proves the hook plumbing; new weapons will start here. */
void mod_jj3_8005a18c(void) { jj3_8005a18c_orig(); }
