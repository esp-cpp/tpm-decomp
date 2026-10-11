/* The GPU's two ports (GP0 commands / GP1 control) in front of psyz's
 * software rasterizer (soft_raster.inc.c, the same core the MGS and SOTN
 * ports use). The rasterizer draws primitives; this file splits the word
 * stream into packets, runs VRAM transfers, keeps the display state and
 * answers GPUSTAT / GPUREAD. */
#include <psyz.h>
#include <libgpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "draw.h"
#include "rt_internal.h"
#include "rt_platform.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
EXT_RAM_BSS_ATTR u16 g_RawVram[VRAM_H * VRAM_W];
#ifndef SOFT_RASTER_IRAM
#define SOFT_RASTER_IRAM IRAM_ATTR
#endif
#else
u16 g_RawVram[VRAM_H * VRAM_W];
#endif

#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#define SOFT_CLUT_CACHE
#ifdef ESP_PLATFORM
#define SOFT_RASTER_RECIP /* RV32: no 64-bit divide (GCC nested function) */
#endif
#include "platform/soft_raster.inc.c"

/* telemetry symbols the rasterizer core references */
unsigned sotn_prim_cycles;
unsigned sotn_prim_census[8];
unsigned sotn_prim_cycles_by[8];
unsigned sotn_poly_kind[4];
unsigned sotn_move_count;
unsigned sotn_exeque_count;
unsigned sotn_push_count;
unsigned sotn_pkt_drops;

void Draw_Reset(void) {
    vram = g_RawVram;
    Draw_TiledInit();
    for (int c = 0; c < 2; c++) {
        SoftState* s = &st_c[c];
        s->x0 = 0;
        s->x1 = VRAM_W - 1;
        s->ox = s->oy = 0;
        s->area_y0 = s->band_y0 = 0;
        s->area_y1 = s->band_y1 = VRAM_H - 1;
        s->y0 = 0;
        s->y1 = VRAM_H - 1;
        s->mask_or = 0;
        s->mask_check = 0;
    }
}

/* ---- state ---- */
static uint32_t cmd[16];
static int cmd_len, cmd_need;
static int poly_line; /* receiving a polyline: vertices until 0x5xxx5xxx */
static uint32_t pl_color, pl_prev_color, pl_prev_xy;
static int pl_have_prev, pl_expect_color, pl_gouraud;

/* CPU -> VRAM (A0) and VRAM -> CPU (C0) */
static uint16_t* xfer_buf;
static RECT xfer_rect;
static uint32_t xfer_words, xfer_pos;
static int xfer_in, xfer_out;

static uint32_t gpuread_latch;
static uint32_t stat_draw; /* GPUSTAT bits 0-10 from E1 + mask bits */
static uint32_t e2, e3, e4, e5;
static uint32_t dma_dir;
static int disp_off = 1, disp_x, disp_y, disp_mode, field;

void rt_gpu_display(RtDisplay* d) {
    static const int hres[4] = {256, 320, 512, 640};
    d->x = disp_x;
    d->y = disp_y;
    d->w = (disp_mode & 0x40) ? 368 : hres[disp_mode & 3];
    d->h = (disp_mode & 0x04) && (disp_mode & 0x20) ? 480 : 240;
    d->rgb24 = (disp_mode >> 4) & 1;
    d->enabled = !disp_off;
}

void gpu_reset(void) {
    Draw_Reset();
    cmd_len = cmd_need = 0;
    poly_line = 0;
    xfer_in = xfer_out = 0;
    stat_draw = 0;
    disp_off = 1;
    if (!xfer_buf) xfer_buf = (uint16_t*)malloc(VRAM_W * VRAM_H * 2);
}

void gpu_vblank(void) { field ^= 1; }

static int s11v(uint32_t v) { return (int)(((v & 0x7FF) ^ 0x400) - 0x400); }

/* words in a GP0 packet, from its first word */
static int packet_len(uint32_t w) {
    uint32_t c = w >> 24;
    if (c >= 0x20 && c < 0x40) {
        int n = (c & 8) ? 4 : 3;
        int tex = (c & 4) != 0, gou = (c & 0x10) != 0;
        return 1 + n * (1 + tex) + (gou ? n - 1 : 0);
    }
    if (c >= 0x40 && c < 0x60) return (c & 0x10) ? 4 : 3; /* single lines */
    if (c >= 0x60 && c < 0x80) {
        int tex = (c & 4) != 0;
        int var = ((c >> 3) & 3) == 0;
        return 2 + tex + var;
    }
    if (c >= 0x80 && c < 0xA0) return 4;
    if (c >= 0xA0 && c < 0xE0) return 3;
    if (c == 0x02) return 3;
    return 1;
}

static void draw_line(uint32_t c0, uint32_t xy0, uint32_t c1, uint32_t xy1, int semi) {
    u_long pk[4];
    pk[0] = (u_long)((c0 & 0xFFFFFF) | ((0x50u | (semi ? 2 : 0)) << 24));
    pk[1] = xy0;
    pk[2] = c1 & 0xFFFFFF;
    pk[3] = xy1;
    Draw_PushPrim(pk, 4);
}

static void exec_packet(void) {
    uint32_t c = cmd[0] >> 24;
    if (c >= 0x20 && c < 0x80) {
        u_long pk[16];
        for (int i = 0; i < cmd_len; i++) pk[i] = cmd[i];
        Draw_PushPrim(pk, cmd_len);
        return;
    }
    if (c == 0x02) { /* fill: ignores draw area, offset and mask */
        RECT r;
        r.x = (short)(cmd[1] & 0x3F0);
        r.y = (short)((cmd[1] >> 16) & 0x1FF);
        r.w = (short)(((cmd[2] & 0x3FF) + 15) & ~15);
        r.h = (short)((cmd[2] >> 16) & 0x1FF);
        Draw_ClearImage(&r, cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF, (cmd[0] >> 16) & 0xFF);
        return;
    }
    if (c >= 0x80 && c < 0xA0) { /* VRAM -> VRAM */
        RECT r;
        r.x = (short)(cmd[1] & 0x3FF);
        r.y = (short)((cmd[1] >> 16) & 0x1FF);
        r.w = (short)(cmd[3] & 0x3FF ? cmd[3] & 0x3FF : 0x400);
        r.h = (short)((cmd[3] >> 16) & 0x1FF ? (cmd[3] >> 16) & 0x1FF : 0x200);
        Draw_MoveImage(&r, cmd[2] & 0x3FF, (cmd[2] >> 16) & 0x1FF);
        return;
    }
    if (c >= 0xA0 && c < 0xE0) {
        RECT* r = &xfer_rect;
        r->x = (short)(cmd[1] & 0x3FF);
        r->y = (short)((cmd[1] >> 16) & 0x1FF);
        r->w = (short)(((cmd[2] & 0xFFFF) - 1) & 0x3FF) + 1;
        r->h = (short)((((cmd[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
        xfer_words = ((uint32_t)r->w * r->h + 1) / 2;
        xfer_pos = 0;
        if (c < 0xC0) {
            xfer_in = 1;
        } else {
            Draw_StoreImage(r, (u_long*)xfer_buf);
            if ((r->w * r->h) & 1) xfer_buf[r->w * r->h] = 0;
            xfer_out = 1;
        }
        return;
    }
    switch (c) {
    case 0xE1:
        stat_draw = (stat_draw & ~0x7FFu) | (cmd[0] & 0x7FF);
        {
            uint32_t w = cmd[0];
            Draw_SetTexpageMode((ParamDrawTexpageMode*)&w);
        }
        break;
    case 0xE2:
        e2 = cmd[0] & 0xFFFFF;
        Draw_SetTextureWindow(e2 & 0x1F, (e2 >> 5) & 0x1F, (e2 >> 10) & 0x1F, (e2 >> 15) & 0x1F);
        break;
    case 0xE3:
        e3 = cmd[0] & 0xFFFFF;
        Draw_SetAreaStart(e3 & 0x3FF, (e3 >> 10) & 0x1FF);
        break;
    case 0xE4:
        e4 = cmd[0] & 0xFFFFF;
        Draw_SetAreaEnd(e4 & 0x3FF, (e4 >> 10) & 0x1FF);
        break;
    case 0xE5:
        e5 = cmd[0] & 0x3FFFFF;
        Draw_SetOffset(s11v(e5), s11v(e5 >> 11));
        break;
    case 0xE6:
        stat_draw = (stat_draw & ~0x1800u) | ((cmd[0] & 3) << 11);
        Draw_SetMask(cmd[0] & 1, (cmd[0] >> 1) & 1);
        break;
    default:
        break; /* 00 nop, 01 cache, 1F irq, E0/E7 */
    }
}

void gpu_gp0(uint32_t w) {
    if (xfer_in) {
        xfer_buf[2 * xfer_pos] = (uint16_t)w;
        xfer_buf[2 * xfer_pos + 1] = (uint16_t)(w >> 16);
        if (++xfer_pos >= xfer_words) {
            xfer_in = 0;
            Draw_LoadImage(&xfer_rect, (u_long*)xfer_buf);
        }
        return;
    }
    if (poly_line) {
        if ((w & 0xF000F000u) == 0x50005000u && !pl_expect_color) {
            poly_line = 0;
            return;
        }
        if (pl_expect_color) {
            pl_color = w;
            pl_expect_color = 0;
            return;
        }
        if (pl_have_prev) draw_line(pl_prev_color, pl_prev_xy, pl_color, w, (cmd[0] >> 25) & 1);
        pl_prev_color = pl_color;
        pl_prev_xy = w;
        pl_have_prev = 1;
        pl_expect_color = pl_gouraud;
        return;
    }
    if (cmd_len == 0) {
        uint32_t c = w >> 24;
        if (c >= 0x40 && c < 0x60 && (c & 0x08)) { /* polyline */
            cmd[0] = w;
            poly_line = 1;
            pl_gouraud = (c & 0x10) != 0;
            pl_color = w;
            pl_have_prev = 0;
            pl_expect_color = 0;
            return;
        }
        cmd_need = packet_len(w);
    }
    cmd[cmd_len++] = w;
    if (cmd_len >= cmd_need) {
        exec_packet();
        cmd_len = 0;
    }
}

void gpu_gp1(uint32_t w) {
    uint32_t c = (w >> 24) & 0x3F;
    switch (c) {
    case 0x00:
        cmd_len = 0;
        poly_line = xfer_in = xfer_out = 0;
        stat_draw = 0;
        disp_off = 1;
        dma_dir = 0;
        break;
    case 0x01:
        cmd_len = 0;
        poly_line = xfer_in = 0;
        break;
    case 0x02: break;
    case 0x03: disp_off = w & 1; break;
    case 0x04: dma_dir = w & 3; break;
    case 0x05:
        disp_x = w & 0x3FE;
        disp_y = (w >> 10) & 0x1FF;
        break;
    case 0x06: case 0x07: break;
    case 0x08: disp_mode = w & 0xFF; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 7) {
        case 2: gpuread_latch = e2; break;
        case 3: gpuread_latch = e3; break;
        case 4: gpuread_latch = e4; break;
        case 5: gpuread_latch = e5; break;
        case 7: gpuread_latch = 2; break; /* GPU version */
        default: break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_read(void) {
    if (xfer_out) {
        uint32_t v = xfer_buf[2 * xfer_pos] | ((uint32_t)xfer_buf[2 * xfer_pos + 1] << 16);
        if (++xfer_pos >= xfer_words) xfer_out = 0;
        return v;
    }
    return gpuread_latch;
}

uint32_t gpu_stat(void) {
    static uint32_t toggle;
    uint32_t s = stat_draw & 0x7FF;
    s |= (stat_draw & 0x1800);
    s |= 1u << 13;
    s |= (uint32_t)(disp_mode & 0x40) << 10; /* bit 16 */
    s |= (uint32_t)(disp_mode & 0x3F) << 17; /* bits 17-22 */
    s |= (uint32_t)disp_off << 23;
    s |= 1u << 26;                          /* ready for commands */
    s |= (uint32_t)(xfer_out ? 1 : 0) << 27; /* ready to send VRAM */
    s |= 1u << 28;                          /* ready for DMA blocks */
    s |= dma_dir << 29;
    switch (dma_dir) {
    case 1: s |= 1u << 25; break;
    case 2: s |= 1u << 25; break;
    case 3: s |= (uint32_t)(xfer_out ? 1 : 0) << 25; break;
    }
    /* bit 31: the field being scanned; flips every read so that loops
     * waiting for a change never stall */
    toggle ^= 1;
    s |= (field ^ toggle) << 31;
    return s;
}
