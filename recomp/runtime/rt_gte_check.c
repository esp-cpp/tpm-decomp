/* RT_GTE_CHECK (desktop): psyz's GTE runs in lockstep with rt_gte.c and
 * every register is compared after every operation. Not built otherwise. */
#ifdef RT_GTE_CHECK
#include <stdio.h>
#include "rt_gte.h"

unsigned int Psyz_GteDataRead(unsigned idx);
void Psyz_GteDataWrite(unsigned idx, unsigned int v);
unsigned int Psyz_GteCtrlRead(unsigned idx);
void Psyz_GteCtrlWrite(unsigned idx, unsigned int v);
void Psyz_GteCommand(unsigned int cmd);
uint32_t rt_frame_count(void);

static unsigned reported;
static void compare(const char* what, uint32_t arg) {
    if (reported >= 60) return;
    /* data 28..31 (IRGB/ORGB/LZCS/LZCR) psyz does not have; control 24/25
     * psyz keeps without their fraction */
    for (unsigned r = 0; r < 28; r++) {
        if (r == 15) continue; /* psyz's SXYP goes stale when SXY2 is written */
        uint32_t a = rt_gte_read(r), b = Psyz_GteDataRead(r);
        if (a != b && reported++ < 60)
            printf("[gte] after %s %08X: data r%u %08X, psyz %08X (frame %u)\n", what, (unsigned)arg, r,
                   (unsigned)a, (unsigned)b, (unsigned)rt_frame_count());
    }
    for (unsigned r = 0; r < 32; r++) {
        if (r == 24 || r == 25) continue;
        uint32_t a = rt_gte_cread(r), b = Psyz_GteCtrlRead(r);
        if (a != b && reported++ < 60)
            printf("[gte] after %s %08X: ctrl r%u %08X, psyz %08X (frame %u)\n", what, (unsigned)arg, r,
                   (unsigned)a, (unsigned)b, (unsigned)rt_frame_count());
    }
}
static unsigned uses[32];
uint32_t rt_gtec_dr(unsigned idx) {
    if (idx >= 28 && !uses[idx]++) printf("[gte] the game reads data r%u\n", idx);
    return rt_gte_read(idx);
}
void rt_gtec_dw(unsigned idx, uint32_t v) {
    if ((idx == 15 || idx >= 28) && !uses[idx]++) printf("[gte] the game writes data r%u\n", idx);
    rt_gte_write(idx, v);
    if (idx == 28) { /* psyz has no IRGB: give it the IR1..3 it implies */
        for (unsigned r = 9; r <= 11; r++) Psyz_GteDataWrite(r, rt_gte_read(r));
    } else if (idx == 15) { /* nor the SXYP push */
        for (unsigned r = 12; r <= 15; r++) Psyz_GteDataWrite(r, rt_gte_read(r));
    } else {
        Psyz_GteDataWrite(idx, v);
    }
    compare("mtc2", idx);
}
uint32_t rt_gtec_cr(unsigned idx) { return rt_gte_cread(idx); }
void rt_gtec_cw(unsigned idx, uint32_t v) {
    rt_gte_cwrite(idx, v);
    Psyz_GteCtrlWrite(idx, v);
    if ((idx == 24 || idx == 25) && (v & 0xFFFF) && !uses[0]++)
        printf("[gte] the screen offset has a fraction: %08X\n", (unsigned)v);
    compare("ctc2", idx);
}
void rt_gtec_cmd(uint32_t cmd) {
    rt_gte_command(cmd);
    Psyz_GteCommand(cmd);
    compare("cop2", cmd);
}
#endif
