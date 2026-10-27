/* Native stand-ins for library code whose hardware is not emulated:
 * libpad's direct-mode controller polling (SIO0), the BIOS patches that
 * search kernel code, and libmcrd (the memory card is an image file). Listed in
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

/* ---- libmcrd: the memory card in an image file (psyz/mcd.h) ----
 * The library's functions all go to the card file directly; the
 * asynchronous ones finish at once and MemCardSync reports them. The game
 * (0x8001F324) polls MemCardSync and uses no callback. Arguments past the
 * fourth are on the guest stack (sp + 0x10, 0x14). */
#include <psyz/mcd.h>

enum {
    McErrNone, McErrCardNotExist, McErrCardInvalid, McErrNewCard, McErrNotFormat,
    McErrFileNotExist, McErrAlreadyExist, McErrBlockFull,
};
enum { McFuncExist = 1, McFuncAccept, McFuncReadFile, McFuncWriteFile, McFuncReadData, McFuncWriteData };
static uint32_t mc_cmd, mc_result, mc_pending;
static uint32_t mc_new[2];            /* MemCardExist reports a new card until MemCardAccept */
static char mc_open_name[MCD_NAME + 1]; /* MemCardOpen's file (the port in mc_open_port) */
static int mc_open_port = -1;

#define A3 g_cpu.r[7]
static uint32_t mc_stack_arg(int i) { return LW(g_cpu.r[29] + 0x10 + 4 * (uint32_t)i); }
static const char* mc_path(uint32_t chan) {
    if ((chan & 0xF) != 0) return NULL; /* no multitap */
    return plat_memcard_path((int)(chan >> 4) & 1);
}
static void mc_guest_name(uint32_t a, char out[MCD_NAME + 1]) {
    int n = 0;
    for (; n < MCD_NAME && LBU(a + (uint32_t)n); n++) out[n] = (char)LBU(a + (uint32_t)n);
    out[n] = 0;
}
/* the card's state as libmcrd reports it */
static uint32_t mc_card(const char* path) {
    if (!path) return McErrCardNotExist;
    switch (Mcd_Check(path, 1)) {
    case MCD_OK: return McErrNone;
    case MCD_ERR_UNFORMATTED: return McErrNotFormat;
    default: return McErrCardInvalid;
    }
}
static uint32_t mc_err(int r) {
    switch (r) {
    case MCD_OK: return McErrNone;
    case MCD_ERR_UNFORMATTED: return McErrNotFormat;
    case MCD_ERR_NOFILE: return McErrFileNotExist;
    case MCD_ERR_EXISTS: return McErrAlreadyExist;
    case MCD_ERR_FULL: return McErrBlockFull;
    default: return McErrCardInvalid;
    }
}
static void mc_done(uint32_t cmd, uint32_t result) {
    mc_cmd = cmd;
    mc_result = result;
    mc_pending = 1;
    V0 = 1;
}
/* guest RAM <-> the card, in whole 128-byte frames as the library requires */
static uint32_t mc_data(uint32_t chan, const char* name, uint32_t adrs, uint32_t ofs, uint32_t bytes, int write) {
    const char* path = mc_path(chan);
    uint32_t r = mc_card(path);
    if (r != McErrNone) return r;
    const int32_t at = ram_off(adrs);
    if (at < 0 || (uint32_t)at + bytes > RAM_SIZE) return McErrCardInvalid;
    r = mc_err(write ? Mcd_Write(path, name, ofs, g_ram + at, bytes) : Mcd_Read(path, name, ofs, g_ram + at, bytes));
    RT_LOG("memory card: %s %s +%u, %u bytes: %u", write ? "write" : "read", name, (unsigned)ofs, (unsigned)bytes,
           (unsigned)r);
    return r;
}

void hle_MemCardInit(void) { mc_new[0] = mc_new[1] = 1; }
void hle_MemCardEnd(void) {}
void hle_MemCardStart(void) {}
void hle_MemCardStop(void) { mc_open_port = -1; }
void hle_MemCardExist(void) {
    const uint32_t chan = A0, port = (chan >> 4) & 1;
    uint32_t r = mc_path(chan) ? McErrNone : McErrCardNotExist;
    if (r == McErrNone && mc_new[port]) r = McErrNewCard;
    mc_done(McFuncExist, r);
}
void hle_MemCardAccept(void) {
    const uint32_t chan = A0, port = (chan >> 4) & 1;
    uint32_t r = mc_card(mc_path(chan));
    if (r == McErrNone && mc_new[port]) r = McErrNewCard;
    if (r == McErrNone || r == McErrNewCard || r == McErrNotFormat) mc_new[port] = 0;
    RT_LOG("memory card: accept port %u: %u", (unsigned)port, (unsigned)r);
    mc_done(McFuncAccept, r);
}
void hle_MemCardOpen(void) {
    /* MemCardOpen(chan, file, flag) */
    const char* path = mc_path(A0);
    char name[MCD_NAME + 1];
    mc_guest_name(A1, name);
    uint32_t r = mc_card(path);
    if (r == McErrNone) r = mc_err(Mcd_Find(path, name, NULL));
    if (r == McErrNone) {
        memcpy(mc_open_name, name, sizeof name);
        mc_open_port = (int)A0;
    }
    V0 = r;
}
void hle_MemCardClose(void) { mc_open_port = -1; }
void hle_MemCardReadData(void) {
    /* MemCardReadData(adrs, ofs, bytes) */
    if ((A2 & 0x7F) || mc_open_port < 0) { V0 = 0; return; }
    mc_done(McFuncReadData, mc_data((uint32_t)mc_open_port, mc_open_name, A0, A1, A2, 0));
}
void hle_MemCardWriteData(void) {
    if ((A2 & 0x7F) || mc_open_port < 0) { V0 = 0; return; }
    mc_done(McFuncWriteData, mc_data((uint32_t)mc_open_port, mc_open_name, A0, A1, A2, 1));
}
void hle_MemCardReadFile(void) {
    /* MemCardReadFile(chan, file, adrs, ofs, bytes) */
    const uint32_t bytes = mc_stack_arg(0);
    if (bytes & 0x7F) { V0 = 0; return; }
    char name[MCD_NAME + 1];
    mc_guest_name(A1, name);
    mc_done(McFuncReadFile, mc_data(A0, name, A2, A3, bytes, 0));
}
void hle_MemCardWriteFile(void) {
    const uint32_t bytes = mc_stack_arg(0);
    if (bytes & 0x7F) { V0 = 0; return; }
    char name[MCD_NAME + 1];
    mc_guest_name(A1, name);
    mc_done(McFuncWriteFile, mc_data(A0, name, A2, A3, bytes, 1));
}
void hle_MemCardGetDirentry(void) {
    /* MemCardGetDirentry(chan, name, struct DIRENTRY* dir, long* files, ofs, max):
     * DIRENTRY is name[20], attr, size, next, system[8] (40 bytes) */
    const char* path = mc_path(A0);
    char pat[MCD_NAME + 1];
    mc_guest_name(A1, pat);
    const uint32_t dir = A2, files = A3, ofs = mc_stack_arg(0), max = mc_stack_arg(1);
    uint32_t r = mc_card(path), n = 0;
    if (r == McErrNone) {
        McdEntry e[MCD_FILES];
        const int count = Mcd_List(path, e);
        if (count < 0) r = mc_err(count);
        for (int i = 0, seen = 0; i < count && n < max; i++) {
            if (!Mcd_NameMatch(pat, e[i].name)) continue;
            if ((uint32_t)seen++ < ofs) continue;
            const uint32_t d = dir + 40 * n++;
            for (uint32_t k = 0; k < 40; k++) SB(d + k, 0);
            for (uint32_t k = 0; k < MCD_NAME && e[i].name[k]; k++) SB(d + k, (uint8_t)e[i].name[k]);
            SW(d + 20, 0x51); /* attr: the directory state */
            SW(d + 24, e[i].size);
            SW(d + 32, (uint32_t)e[i].first);
        }
    }
    if (files) SW(files, n);
    RT_LOG("memory card: directory '%s': %u files (%u)", pat, (unsigned)n, (unsigned)r);
    V0 = r;
}
void hle_MemCardCreateFile(void) {
    /* MemCardCreateFile(chan, file, blocks) */
    const char* path = mc_path(A0);
    char name[MCD_NAME + 1];
    mc_guest_name(A1, name);
    uint32_t r = mc_card(path);
    if (r == McErrNone) r = mc_err(Mcd_Create(path, name, (int)A2));
    RT_LOG("memory card: create %s (%u blocks): %u", name, (unsigned)A2, (unsigned)r);
    V0 = r;
}
void hle_MemCardDeleteFile(void) {
    const char* path = mc_path(A0);
    char name[MCD_NAME + 1];
    mc_guest_name(A1, name);
    uint32_t r = mc_card(path);
    if (r == McErrNone) r = mc_err(Mcd_Delete(path, name));
    RT_LOG("memory card: delete %s: %u", name, (unsigned)r);
    V0 = r;
}
void hle_MemCardFormat(void) {
    const char* path = mc_path(A0);
    V0 = path ? mc_err(Mcd_Format(path)) : McErrCardNotExist;
    RT_LOG("memory card: format: %u", (unsigned)V0);
}
void hle_MemCardUnformat(void) {
    const char* path = mc_path(A0);
    V0 = path && Mcd_Unformat(path) == MCD_OK ? 1 : 0;
}
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
