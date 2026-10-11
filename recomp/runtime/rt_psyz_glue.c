/* The few psyz symbols the rasterizer core and libgte.c reference that the
 * runtime does not otherwise link (psyz's logger, and the trig tables of
 * libgte's matrix helpers, which the game never reaches: it has its own). */
#include <stdarg.h>
#include <stdio.h>

#include <psyz.h>
#include <psyz/log.h>

LOG_LEVEL psyz_logLevel = LOG_LEVEL_W;

void psyz_log(unsigned int level, const char* file, unsigned int line, const char* func, const char* fmt, ...) {
    va_list ap;
    (void)level;
    (void)file;
    (void)line;
    printf("[psyz] %s: ", func);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int rsin(int a) { (void)a; return 0; }
int rcos(int a) { (void)a; return 4096; }
long SquareRoot0_impl(long a) { (void)a; return 0; }
long SquareRoot12_impl(long a) { (void)a; return 0; }

/* psyz_spu.c's links into the rest of psyz */
#include <stddef.h>
#include "libspu_private.h" /* psyz/decomp/src/libspu */
static union SpuUnion spu_regs;
union SpuUnion* _spu_RXX = &spu_regs;
volatile unsigned psyz_spu_irq_raised, psyz_spu_irq_caught_up;
void Psyz_RcntAdd(int frames) { (void)frames; }
/* CD-DA / XA audio into the SPU mix: none yet */
size_t Psyz_CdPullSamples(short* out, size_t frames) { (void)out; (void)frames; return 0; }
