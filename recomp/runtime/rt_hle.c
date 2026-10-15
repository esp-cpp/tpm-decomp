/* Native stand-ins for library code whose hardware is not emulated:
 * libpad's direct-mode controller polling (SIO0), the BIOS patches that
 * search kernel code, and libmcrd (no memory card yet). Listed in
 * recomp/hle.us.txt; the recompiler makes each listed function call its
 * hle_ counterpart. */
#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

#define A0 g_cpu.r[4]
#define A1 g_cpu.r[5]
#define A2 g_cpu.r[6]
#define V0 g_cpu.r[2]

/* ---- libpad: PadInitDirect(u_char* port1, u_char* port2) ----
 * The game reads the receive buffers it passed in; fill them every vblank
 * the way the library's SIO interrupt would: status, id, then the buttons
 * (active low). */
static uint32_t pad_buf[2];
static int pad_on;

void o_8006044C(void); /* the recompiled PadInitDirect */
void hle_PadInitDirect(void) {
    pad_buf[0] = A0;
    pad_buf[1] = A1;
    pad_on = 1;
    RT_LOG("PadInitDirect(%08X, %08X)", (unsigned)A0, (unsigned)A1);
    o_8006044C();
}

void hle_vblank(void) {
    for (int p = 0; p < 2; p++) {
        uint32_t b = pad_buf[p];
        if (!b || !pad_on) continue;
        if (p == 1) { /* nothing in port 2 */
            SB(b, 0xFF);
            continue;
        }
        uint16_t keys = (uint16_t)~plat_pad_buttons(p);
        SB(b + 0, 0x00);
        SB(b + 1, 0x41); /* digital pad, 1 halfword of data */
        SB(b + 2, keys & 0xFF);
        SB(b + 3, keys >> 8);
    }
}

/* ---- libetc: VSync(mode) ----
 * The game's frame limiter spins on VSync(-1) until the vblank count moves.
 * The R3000 would sit in that loop until the interrupt; here emulated time
 * goes straight to it (hw_vsync_poll), which the board needs: every turn of
 * the loop costs it about as much as it cost the PlayStation (a fifth of
 * the board's game thread was this loop). */
void o_80060D38(void); /* the recompiled VSync */
void hle_VSync(void) {
    /* the frame limiter (0x800110F8), and libcd's wait for a command to
     * finish (0x8006DB58, which the loader calls in a loop: 8000 turns a
     * frame, the level load took 25 s on the board instead of 7): the step
     * stops at the first interrupt, the CD's included. Not the other libcd
     * and libgpu polls of VSync(-1) for their timeouts: skipping those
     * broke the display */
    uint32_t ra = g_cpu.r[31];
    if ((int32_t)A0 < 0 && (ra == 0x80011128u || ra == 0x80011184u || ra == 0x8006DBE0u))
        hw_vsync_poll();
    o_80060D38();
}

/* ---- BIOS patches: they rewrite kernel code we do not have ---- */
void hle__patch_gte(void) {}
void hle__patch_card(void) {}
void hle__patch_card2(void) {}
void hle__copy_memcard_patch(void) {}

/* ---- libmcrd: no card in either slot ---- */
enum { McErrNone, McErrCardNotExist, McErrCardInvalid, McErrNewCard };
static uint32_t mc_cmd, mc_result, mc_pending;

static void mc_async(uint32_t cmd) {
    mc_cmd = cmd;
    mc_result = McErrCardNotExist;
    mc_pending = 1;
    V0 = 1;
}
void hle_MemCardInit(void) {}
void hle_MemCardEnd(void) {}
void hle_MemCardStart(void) {}
void hle_MemCardStop(void) {}
void hle_MemCardExist(void) { mc_async(1); }
void hle_MemCardAccept(void) { mc_async(2); }
void hle_MemCardReadFile(void) { mc_async(3); }
void hle_MemCardWriteFile(void) { mc_async(4); }
void hle_MemCardReadData(void) { mc_async(5); }
void hle_MemCardWriteData(void) { mc_async(6); }
void hle_MemCardOpen(void) { V0 = McErrCardNotExist; }
void hle_MemCardClose(void) {}
void hle_MemCardGetDirentry(void) { V0 = McErrCardNotExist; }
void hle_MemCardCreateFile(void) { V0 = McErrCardNotExist; }
void hle_MemCardDeleteFile(void) { V0 = McErrCardNotExist; }
void hle_MemCardFormat(void) { V0 = McErrCardNotExist; }
void hle_MemCardUnformat(void) { V0 = McErrCardNotExist; }
void hle_MemCardCallback(void) { V0 = 0; }
void hle_MemCardSync(void) {
    /* MemCardSync(mode, long* cmds, long* result) */
    if (!mc_pending) { V0 = (uint32_t)-1; return; }
    if (A1) SW(A1, mc_cmd);
    if (A2) SW(A2, mc_result);
    mc_pending = 0;
    V0 = 1;
}

/* ---- 0x80083F00: call the loaded overlay (the FMV.CPE movie player) ----
 * The original saves gp/ra in the scratchpad around `jalr a0`. The overlay
 * plays its movie through rt_movie.c (hle_PlayMovie); with movies off
 * (rt_set_movies, RT_MOVIES=0) or without the recompiled overlay the call
 * is skipped (the caller ignores the result). */
#include <stdlib.h>
int rt_dispatch_known(uint32_t addr);
void hle_CallOverlay(void) {
    int movies = rt_movies_on();
    if (getenv("RT_MOVIES")) movies = atoi(getenv("RT_MOVIES"));
    if (!movies || !rt_dispatch_known(A0)) {
        RT_LOG("movie skipped");
        V0 = 0;
        return;
    }
    uint32_t gp = g_cpu.r[28], ra = g_cpu.r[31];
    SW(0x1F800000u, gp);
    SW(0x1F800004u, ra);
    g_cpu.r[31] = 0x80083F14u;
    rt_dispatch(A0);
    g_cpu.r[28] = LW(0x1F800000u);
    g_cpu.r[31] = LW(0x1F800004u);
}
