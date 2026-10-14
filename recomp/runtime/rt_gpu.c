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

/* VRAM (1 MB) comes from plat_alloc at boot, like the rest of the big
 * buffers: the board hands its memory to one game at a time. */
u16* g_rt_vram;
#ifndef SOFT_RASTER_IRAM
#define SOFT_RASTER_IRAM /* internal RAM is the board's scarce memory */
#endif
#if defined(ESP_PLATFORM) && defined(TPM_SPAN_IRAM)
#include "esp_attr.h"
#define SOFT_SPAN_IRAM IRAM_ATTR
#endif

#define SOFT_RASTER_STANDALONE
#define SOFT_RASTER_NO_FALLBACK
#ifdef ESP_PLATFORM
#define SOFT_CLUT_CACHE /* per core there; the desktop's would be shared */
/* in PSRAM: cached PSRAM measured faster for the cores than internal RAM */
#define SOFT_CLUT_CACHE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#ifdef RT_HOST_CLUT_CACHE
#define SOFT_CLUT_CACHE /* (testing the board's path; one worker) */
#endif
#define SOFT_RASTER_HOST_CORES /* two workers on threads: per-thread state */
__thread int psyz_host_core;
#endif
#ifdef ESP_PLATFORM
#define SOFT_RASTER_RECIP      /* RV32: no 64-bit divide (GCC nested function) */
#define SOFT_RASTER_NO_STATS   /* psyz's per-row / per-primitive counters (MGS's
                                * telemetry) cost the board ~3% of its drawing */
#define SOFT_RASTER_FLOAT_GRAD /* ...and the P4 has an FPU: gradients in float
                                * (exact fix-up included; the MGS port's setting) */
#endif
#if defined(ESP_PLATFORM) || defined(RT_HOST_TILED_TEX)
#define SOFT_TILED_TEX /* texels from an 8x8-tiled shadow of VRAM: a texture
                        * walked along v changes cache lines every 8 texels,
                        * not every texel (rows of VRAM are 2 KB apart) */
#endif
/* the core's ESP32 diagnostics name the MGS port's VRAM: point them at ours */
#define g_RawVram g_rt_vram
#include "platform/soft_raster.inc.c"
#undef g_RawVram

#ifdef ESP_PLATFORM
/* psyz's own frame-buffer feedback test is the MGS port's (its frame buffer
 * layout): this file makes its own, below */
void Psyz_GpuBarrier(void) {}
#endif

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
    vram = g_rt_vram;
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
        tw_c[c].uand = tw_c[c].vand = 0xFF;
        tw_c[c].uor = tw_c[c].vor = 0;
    }
}


/* ---- state ----
 * The GP0 stream is parsed where it is executed. With two workers (see
 * rt_gpu_set_async) both parse and execute the whole stream, each drawing
 * only its band of rows; everything below that is per worker lives in a
 * Parser, indexed like the rasterizer's own per-core state (soft_core()). */
static void gpu_drain(void);
#define RING_ENTRIES (1u << 16)
static uint32_t* ring;             /* the async queue: [kind, value] pairs */
static uint32_t ring_w, ring_r[2]; /* entries; producer / each worker (published) */
static uint32_t ring_r_local[2];   /* each worker's own position */
static int async_on;               /* 0: drawing on the CPU's thread; 1, 2 workers */
static volatile int prod_wait;     /* the CPU's thread waits for ring space */

typedef struct {
    uint32_t cmd[16];
    int cmd_len, cmd_need;
    int poly_line; /* receiving a polyline: vertices until 0x5xxx5xxx */
    uint32_t pl_color, pl_prev_color, pl_prev_xy;
    int pl_have_prev, pl_expect_color, pl_gouraud;
    /* CPU -> VRAM (A0) and VRAM -> CPU (C0); only the lead worker keeps
     * the data, the other counts the words */
    RECT xfer_rect;
    uint32_t xfer_words, xfer_pos;
    int xfer_in, xfer_out;
    uint32_t gpuread_latch;
    uint32_t stat_draw; /* GPUSTAT bits 0-10 from E1 + mask bits */
    uint32_t e2, e3, e4, e5;
    int ax0, ay0, ax1, ay1; /* the draw area */
    uint32_t tpage;         /* E1 */
} Parser;
static Parser P[2];
static uint16_t* xfer_buf;
static uint32_t dma_dir;
static int disp_off = 1, disp_x, disp_y, disp_mode, field;
static int disp_y1 = 16, disp_y2 = 256; /* GP1 07, scanlines (NTSC: 16..256) */
/* frameskip (below) */
static int fs_max, fs_skipping, fs_run, fs_dropped, fs_drew;
static uint32_t vbl_queued, vbl_done;
typedef struct {
    int x0, y0, x1, y1; /* a draw area the game used (E3 / E4) */
    int shown;          /* the display has started inside it */
    int stale;          /* primitives into it were dropped since its E3 */
} FsBuf;
#define FS_BUFS 4
static FsBuf fs_buf[FS_BUFS];
static int fs_cur = -1, fs_next;
static void fs_reset(void);

/* the worker that does what only one may (VRAM moves and transfers, the
 * display), and whose parser the CPU's thread reads back */
static inline int lead_idx(void) { return async_on ? 1 : 0; }
static inline Parser* me(void) { return &P[soft_core()]; }
static inline int is_lead(void) { return soft_core() == lead_idx(); }

/* ---- two workers: bands and rendezvous ----
 * Worker 1 draws the top `split` rows of the draw area (and every VRAM row
 * above), worker 0 the rest. The split moves at vblanks, a few rows at a
 * time, toward equal busy times; both read it only between the vblank's two
 * rendezvous, so they always agree on who owns a row. */
static int nworkers;
static int split = 120;
static uint64_t busy_us[2], wait_us[2];
uint32_t g_rt_gpu_split, g_rt_gpu_barriers; /* telemetry */
uint64_t g_rt_gpu_wait_us[2];               /* each worker's time at rendezvous */

static void apply_band(Parser* p) {
    if (nworkers != 2) {
        Draw_SetBand(0, VRAM_H - 1);
        return;
    }
    int s = p->ay0 + split;
    if (s > VRAM_H) s = VRAM_H;
    if (soft_core() == 1) Draw_SetBand(0, s - 1);
    else Draw_SetBand(s, VRAM_H - 1);
}

uint32_t g_bar_why[4]; /* debug: feedback, load, move/store, vblank */
static volatile int bar_count;
static volatile unsigned bar_gen;
static void barrier_(int counted);
static void gpu_barrier(void) { barrier_(1); }
static void barrier_(int counted) {
    if (nworkers != 2) return;
    const unsigned gen = __atomic_load_n(&bar_gen, __ATOMIC_ACQUIRE);
    if (__atomic_add_fetch(&bar_count, 1, __ATOMIC_ACQ_REL) == 2) {
        __atomic_store_n(&bar_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&bar_gen, gen + 1, __ATOMIC_RELEASE);
        if (soft_core() == 1) g_rt_gpu_barriers++;
        return;
    }
    const uint64_t t0 = plat_time_us();
    uint32_t n = 0;
    while (__atomic_load_n(&bar_gen, __ATOMIC_ACQUIRE) == gen) {
        if ((++n & 0xFFF) == 0 && plat_time_us() - t0 > 2000000) {
            RT_LOG("gpu: worker %d alone at a rendezvous for 2 s", soft_core());
            break;
        }
    }
    const uint64_t waited = plat_time_us() - t0;
    if (counted) wait_us[soft_core()] += waited; /* not work */
    g_rt_gpu_wait_us[soft_core()] += waited;
}

void rt_gpu_display(RtDisplay* d) {
    static const int hres[4] = {256, 320, 512, 640};
    d->x = disp_x;
    d->y = disp_y;
    d->w = (disp_mode & 0x40) ? 368 : hres[disp_mode & 3];
    d->h = (disp_mode & 0x04) && (disp_mode & 0x20) ? 480 : 240;
    d->rgb24 = (disp_mode >> 4) & 1;
    d->enabled = !disp_off;
    const int scale = d->h == 480 ? 2 : 1;
    int top = (disp_y1 - 16) * scale, lines = (disp_y2 - disp_y1) * scale;
    if (top < 0) top = 0;
    if (top > d->h) top = d->h;
    if (lines < 0) lines = 0;
    if (top + lines > d->h) lines = d->h - top;
    d->top = top;
    d->lines = lines;
    d->stale = 0;
}

void gpu_reset(void) {
    Draw_Reset();
    memset(P, 0, sizeof P);
    for (int i = 0; i < 2; i++) {
        P[i].ax1 = VRAM_W - 1;
        P[i].ay1 = VRAM_H - 1;
    }
    split = 120;
    disp_off = 1;
    if (!xfer_buf) xfer_buf = (uint16_t*)plat_alloc(VRAM_W * VRAM_H * 2 + 4);
    if (!xfer_buf) rt_fatal("no memory for VRAM transfers");
}

int gpu_alloc(void) {
    if (!g_rt_vram) g_rt_vram = (u16*)plat_alloc(VRAM_W * VRAM_H * 2);
#ifdef SOFT_TILED_TEX
    {
        /* the rasterizer's tiled texture shadow, from the same pool */
        void** ts = Draw_TiledSlot();
        if (!*ts) *ts = plat_alloc(VRAM_W * VRAM_H * 2);
        if (!*ts) return 0;
    }
#endif
    if (async_on && !ring) ring = (uint32_t*)plat_alloc(RING_ENTRIES * 2 * sizeof(uint32_t));
    ring_w = ring_r[0] = ring_r[1] = 0;
    vbl_queued = vbl_done = 0;
    fs_skipping = fs_run = fs_dropped = fs_drew = 0;
    fs_reset();
    return g_rt_vram != NULL;
}

void gpu_free(void) {
    gpu_drain();
    plat_free(ring);
    ring = NULL;
#ifdef SOFT_TILED_TEX
    {
        void** ts = Draw_TiledSlot(); /* (plat_alloc's, not the caches' own) */
        plat_free(*ts);
        *ts = NULL;
    }
#endif
#ifdef ESP_PLATFORM
    Draw_FreeCaches();
#endif
    plat_free(xfer_buf);
    plat_free(g_rt_vram);
    xfer_buf = NULL;
    g_rt_vram = NULL;
    vram = NULL;
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

/* A textured primitive whose texture page overlaps the draw area samples
 * pixels this frame is drawing: with two workers, both must have drawn
 * everything before it. Decided from the stream alone, so both agree. */
static int samples_draw_area(const Parser* p, uint32_t tpage) {
    const int px = (int)(tpage & 0xF) * 64, py = (int)((tpage >> 4) & 1) * 256;
    const int bpp = (tpage >> 7) & 3;
    const int w = bpp == 0 ? 64 : bpp == 1 ? 128 : 256;
    return px + w > p->ax0 && px <= p->ax1 && py + 256 > p->ay0 && py <= p->ay1;
}

/* rt_set_gpu_trace(n): log the state commands of frame n, with the number of
 * primitives between them (comparing a board against the desktop) */
static uint32_t trace_frame = 0xFFFFFFFFu, trace_prims;
void rt_set_gpu_trace(uint32_t frame) { trace_frame = frame; }
static void trace_packet(const Parser* p, uint32_t c) {
    if (rt_frame_count() != trace_frame || !is_lead()) return;
    if (c >= 0x20 && c < 0x80) {
        if (trace_prims++ < 6 && c < 0x40) {
            printf("[gpu] f%u prim", (unsigned)trace_frame);
            for (int i = 0; i < p->cmd_len; i++) printf(" %08X", (unsigned)p->cmd[i]);
            printf(" | ofs %d,%d clip %d..%d\n", st.ox, st.oy, st.y0, st.y1);
        }
        return;
    }
    printf("[gpu] f%u +%u prims | %02X %08X %08X %08X | band %d..%d area %d..%d clip %d..%d\n",
           (unsigned)trace_frame, (unsigned)trace_prims, (unsigned)c, (unsigned)p->cmd[0],
           (unsigned)(p->cmd_len > 1 ? p->cmd[1] : 0), (unsigned)(p->cmd_len > 2 ? p->cmd[2] : 0),
           st.band_y0, st.band_y1, st.area_y0, st.area_y1, st.y0, st.y1);
    trace_prims = 0;
}

/* g_rt_prof.gpu: the drawing's time. On a worker, its busy spells (timed
 * per ring drain: a clock read per packet cost the board ~1 us each);
 * synchronously, per packet. */
static void exec_packet_(Parser* p);
static int timing_packets = 1;
static void exec_packet(Parser* p) {
    if (!timing_packets) {
        exec_packet_(p);
        return;
    }
    uint64_t t0 = plat_time_us();
    exec_packet_(p);
    g_rt_prof.gpu += plat_time_us() - t0;
}
/* ---- frameskip (rt_gpu_set_frameskip) ----
 * Decided by the worker at each vblank marker for the drawing up to the next
 * one (the game queues a frame's whole list between two vblanks). Only
 * primitives into a display buffer are dropped: the draw areas the game set
 * are remembered, and one is a display buffer once a vblank's display
 * address fell inside it (not at its corner: letterboxed cutscenes start the
 * display lower down). A buffer is `stale` from a dropped primitive until the
 * game starts drawing it again (E3) in a drawn frame. */
uint32_t g_rt_gpu_skipped;
void rt_gpu_set_frameskip(int max) { fs_max = max < 0 ? 0 : max; }

static void fs_area_start(int x, int y) {
    int b = -1;
    for (int i = 0; i < FS_BUFS; i++)
        if (fs_buf[i].x0 == x && fs_buf[i].y0 == y) b = i;
    if (b < 0) {
        b = fs_next;
        fs_next = (fs_next + 1) % FS_BUFS;
        fs_buf[b].x0 = x;
        fs_buf[b].y0 = y;
        fs_buf[b].x1 = VRAM_W - 1;
        fs_buf[b].y1 = VRAM_H - 1;
        fs_buf[b].shown = fs_buf[b].stale = 0;
    }
    fs_cur = b;
    if (!fs_skipping) fs_buf[b].stale = 0; /* drawn afresh from here */
}
static void fs_reset(void) {
    for (int i = 0; i < FS_BUFS; i++) {
        fs_buf[i].x0 = fs_buf[i].y0 = -1;
        fs_buf[i].shown = fs_buf[i].stale = 0;
    }
    fs_cur = -1;
    fs_next = 0;
}

static void exec_packet_(Parser* p) {
    uint32_t* cmd = p->cmd;
    uint32_t c = cmd[0] >> 24;
    trace_packet(p, c);
    if (c >= 0x20 && c < 0x80) {
        if (fs_max && fs_cur >= 0 && fs_buf[fs_cur].shown) {
            if (fs_skipping) {
                fs_buf[fs_cur].stale = fs_dropped = 1;
                return;
            }
            fs_drew = 1;
        }
        if (nworkers == 2 && (c & 4)) {
            /* polygons carry their texpage in the second texcoord word */
            uint32_t tp = c < 0x40 ? cmd[(c & 0x10) ? 5 : 4] >> 16 : p->tpage;
            if (samples_draw_area(p, tp)) { if (is_lead()) g_bar_why[0]++; gpu_barrier(); }
        }
        u_long pk[16];
        for (int i = 0; i < p->cmd_len; i++) pk[i] = cmd[i];
        Draw_PushPrim(pk, p->cmd_len);
        return;
    }
    if (c == 0x02) { /* fill: ignores draw area, offset and mask; band-clipped */
        RECT r;
        r.x = (short)(cmd[1] & 0x3F0);
        r.y = (short)((cmd[1] >> 16) & 0x1FF);
        r.w = (short)(((cmd[2] & 0x3FF) + 15) & ~15);
        r.h = (short)((cmd[2] >> 16) & 0x1FF);
        Draw_ClearImage(&r, cmd[0] & 0xFF, (cmd[0] >> 8) & 0xFF, (cmd[0] >> 16) & 0xFF);
        return;
    }
    if (c >= 0x80 && c < 0xA0) { /* VRAM -> VRAM: one worker, the rows of both */
        RECT r;
        r.x = (short)(cmd[1] & 0x3FF);
        r.y = (short)((cmd[1] >> 16) & 0x1FF);
        r.w = (short)(cmd[3] & 0x3FF ? cmd[3] & 0x3FF : 0x400);
        r.h = (short)((cmd[3] >> 16) & 0x1FF ? (cmd[3] >> 16) & 0x1FF : 0x200);
        if (is_lead()) g_bar_why[2]++;
        gpu_barrier();
        if (is_lead()) Draw_MoveImage(&r, cmd[2] & 0x3FF, (cmd[2] >> 16) & 0x1FF);
        gpu_barrier();
        return;
    }
    if (c >= 0xA0 && c < 0xE0) {
        RECT* r = &p->xfer_rect;
        r->x = (short)(cmd[1] & 0x3FF);
        r->y = (short)((cmd[1] >> 16) & 0x1FF);
        r->w = (short)(((cmd[2] & 0xFFFF) - 1) & 0x3FF) + 1;
        r->h = (short)((((cmd[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
        p->xfer_words = ((uint32_t)r->w * r->h + 1) / 2;
        p->xfer_pos = 0;
        if (c < 0xC0) {
            p->xfer_in = 1;
        } else {
            gpu_barrier();
            if (is_lead()) {
                Draw_StoreImage(r, (u_long*)xfer_buf);
                if ((r->w * r->h) & 1) xfer_buf[r->w * r->h] = 0;
                p->xfer_out = 1;
            }
            gpu_barrier();
        }
        return;
    }
    switch (c) {
    case 0xE1:
        p->stat_draw = (p->stat_draw & ~0x7FFu) | (cmd[0] & 0x7FF);
        p->tpage = cmd[0] & 0xFFFF;
        {
            uint32_t w = cmd[0];
            Draw_SetTexpageMode((ParamDrawTexpageMode*)&w);
        }
        break;
    case 0xE2:
        p->e2 = cmd[0] & 0xFFFFF;
        Draw_SetTextureWindowE2(p->e2);
        break;
    case 0xE3:
        p->e3 = cmd[0] & 0xFFFFF;
        p->ax0 = p->e3 & 0x3FF;
        p->ay0 = (p->e3 >> 10) & 0x1FF;
        if (fs_max) fs_area_start(p->ax0, p->ay0);
        Draw_SetAreaStart(p->ax0, p->ay0);
        apply_band(p);
        break;
    case 0xE4:
        p->e4 = cmd[0] & 0xFFFFF;
        p->ax1 = p->e4 & 0x3FF;
        p->ay1 = (p->e4 >> 10) & 0x1FF;
        Draw_SetAreaEnd(p->ax1, p->ay1);
        if (fs_max && fs_cur >= 0) {
            fs_buf[fs_cur].x1 = p->ax1;
            fs_buf[fs_cur].y1 = p->ay1;
        }
        break;
    case 0xE5:
        p->e5 = cmd[0] & 0x3FFFFF;
        Draw_SetOffset(s11v(p->e5), s11v(p->e5 >> 11));
        break;
    case 0xE6:
        p->stat_draw = (p->stat_draw & ~0x1800u) | ((cmd[0] & 3) << 11);
        Draw_SetMask(cmd[0] & 1, (cmd[0] >> 1) & 1);
        break;
    default:
        break; /* 00 nop, 01 cache, 1F irq, E0/E7 */
    }
}

static void gp0_exec(Parser* p, uint32_t w) {
    if (p->xfer_in) {
        const int lead = is_lead();
        if (lead) {
            xfer_buf[2 * p->xfer_pos] = (uint16_t)w;
            xfer_buf[2 * p->xfer_pos + 1] = (uint16_t)(w >> 16);
        }
        if (++p->xfer_pos >= p->xfer_words) {
            p->xfer_in = 0;
            /* one worker writes it, and nobody draws past it before then:
             * it may be a texture the other band's next primitive reads, or
             * a picture the other worker is about to draw over */
            if (lead) g_bar_why[1]++;
            gpu_barrier();
            if (lead) {
                uint64_t t0 = timing_packets ? plat_time_us() : 0;
                Draw_LoadImage(&p->xfer_rect, (u_long*)xfer_buf);
                if (timing_packets) g_rt_prof.gpu += plat_time_us() - t0;
            }
            gpu_barrier();
        }
        return;
    }
    if (p->poly_line) {
        if ((w & 0xF000F000u) == 0x50005000u && !p->pl_expect_color) {
            p->poly_line = 0;
            return;
        }
        if (p->pl_expect_color) {
            p->pl_color = w;
            p->pl_expect_color = 0;
            return;
        }
        if (p->pl_have_prev) draw_line(p->pl_prev_color, p->pl_prev_xy, p->pl_color, w, (p->cmd[0] >> 25) & 1);
        p->pl_prev_color = p->pl_color;
        p->pl_prev_xy = w;
        p->pl_have_prev = 1;
        p->pl_expect_color = p->pl_gouraud;
        return;
    }
    if (p->cmd_len == 0) {
        uint32_t c = w >> 24;
        if (c >= 0x40 && c < 0x60 && (c & 0x08)) { /* polyline */
            p->cmd[0] = w;
            p->poly_line = 1;
            p->pl_gouraud = (c & 0x10) != 0;
            p->pl_color = w;
            p->pl_have_prev = 0;
            p->pl_expect_color = 0;
            return;
        }
        p->cmd_need = packet_len(w);
    }
    p->cmd[p->cmd_len++] = w;
    if (p->cmd_len >= p->cmd_need) {
        exec_packet(p);
        p->cmd_len = 0;
    }
}

/* GP1, the drawing side: what the command stream's parser and the GP0
 * state own (runs where GP0 runs) */
static void gp1_exec(Parser* p, uint32_t w) {
    uint32_t c = (w >> 24) & 0x3F;
    switch (c) {
    case 0x00:
        p->cmd_len = 0;
        p->poly_line = p->xfer_in = p->xfer_out = 0;
        p->stat_draw = 0;
        break;
    case 0x01:
        p->cmd_len = 0;
        p->poly_line = p->xfer_in = 0;
        break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 7) {
        case 2: p->gpuread_latch = p->e2; break;
        case 3: p->gpuread_latch = p->e3; break;
        case 4: p->gpuread_latch = p->e4; break;
        case 5: p->gpuread_latch = p->e5; break;
        case 7: p->gpuread_latch = 2; break; /* GPU version */
        default: break;
        }
        break;
    default: break;
    }
}

/* GP1, the display side: registers the CPU's thread reads back (GPUSTAT,
 * the display area for the vblank) */
static void gp1_display(uint32_t w) {
    switch ((w >> 24) & 0x3F) {
    case 0x00: disp_off = 1; dma_dir = 0; disp_y1 = 16; disp_y2 = 256; break;
    case 0x03: disp_off = w & 1; break;
    case 0x04: dma_dir = w & 3; break;
    case 0x05:
        disp_x = w & 0x3FE;
        disp_y = (w >> 10) & 0x1FF;
        break;
    case 0x07:
        disp_y1 = (int)(w & 0x3FF);
        disp_y2 = (int)((w >> 10) & 0x3FF);
        break;
    case 0x08: disp_mode = w & 0xFF; break;
    default: break;
    }
}

/* ---- the GPU on other threads (rt_gpu_set_async) ----
 * The real GPU works through its command FIFO while the CPU runs on; here
 * the CPU's thread queues GP0 words (and the drawing half of GP1) in a ring,
 * and one or two workers execute them on the other cores. A vblank marker
 * in the ring presents the display after exactly the drawing queued before
 * it. Reads of GPU state that depend on the drawing (GPUREAD, VRAM -> CPU)
 * wait for the ring to drain. */
enum { Q_GP0, Q_GP1, Q_VBLANK };

void rt_gpu_set_async(int n) {
    async_on = n > 0;
    nworkers = n;
}

#define KICK_BATCH 256u
static inline uint32_t ring_load(uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint32_t ring_tail(void) {
    uint32_t a = ring_load(&ring_r[1]);
    if (nworkers == 2) {
        uint32_t b = ring_load(&ring_r[0]);
        if ((int32_t)(b - a) < 0) a = b;
    }
    return a;
}

static void ring_push(uint32_t kind, uint32_t v) {
    uint32_t w = ring_w;
    if (w - ring_tail() >= RING_ENTRIES) { /* full: wait for the workers */
        plat_gpu_kick();
        prod_wait = 1;
        while (w - ring_tail() >= RING_ENTRIES) plat_gpu_idle();
        prod_wait = 0;
    }
    uint32_t i = (w & (RING_ENTRIES - 1)) * 2;
    ring[i] = kind;
    ring[i + 1] = v;
    __atomic_store_n(&ring_w, w + 1, __ATOMIC_RELEASE);
    /* a worker that ran dry is woken once a batch has built up (or at the
     * vblank, a drain, a full queue; it also looks again every tick): a
     * wake per word cost the board a context switch per word, and the
     * second worker preempts the game on its core */
    if (w + 1 - ring_load(&ring_r[1]) == KICK_BATCH ||
        (nworkers == 2 && w + 1 - ring_load(&ring_r[0]) == KICK_BATCH))
        plat_gpu_kick();
}

static void gpu_drain(void) {
    if (!async_on || !ring) return;
    plat_gpu_kick();
    if (ring_tail() != ring_w) {
        prod_wait = 1;
        while (ring_tail() != ring_w) plat_gpu_idle();
        prod_wait = 0;
    }
}

/* the vblank marker: both workers have drawn everything before it; the lead
 * moves the split and presents while the other waits */
static void vblank_marker(uint32_t kind, uint32_t v) {
    if (is_lead()) {
        g_bar_why[3]++;
        if (getenv("RT_BARLOG") && g_bar_why[3] % 60 == 0)
            printf("[gpu] barriers: feedback %u load %u move %u vblank %u\n", g_bar_why[0], g_bar_why[1], g_bar_why[2], g_bar_why[3]);
    }
    barrier_(0); /* (the vblank's waits are outside the busy spells) */
    if (is_lead()) {
        if (nworkers == 2) {
            const int64_t w1 = (int64_t)busy_us[1] - (int64_t)wait_us[1];
            const int64_t w0 = (int64_t)busy_us[0] - (int64_t)wait_us[0];
            if (w1 > w0 + 1000 && split > 16) split -= 4;
            else if (w0 > w1 + 1000 && split < 232) split += 4;
            busy_us[0] = busy_us[1] = wait_us[0] = wait_us[1] = 0;
            g_rt_gpu_split = (uint32_t)split;
        }
        RtDisplay d;
        d.x = (int)(v & 0x3FF);
        d.y = (int)((v >> 10) & 0x1FF);
        d.w = (int)((v >> 19) & 0x3FF);
        d.h = (v >> 29) & 1 ? 480 : 240;
        d.rgb24 = (int)((v >> 30) & 1);
        d.enabled = (int)((v >> 31) & 1);
        d.top = (int)((kind >> 8) & 0x3FF);
        d.lines = (int)((kind >> 20) & 0x3FF);
        d.stale = 0;
        if (fs_max && nworkers == 1) {
            /* the draw areas the display starts in */
            for (int i = 0; i < FS_BUFS; i++) {
                FsBuf* f = &fs_buf[i];
                if (d.x >= f->x0 && d.x <= f->x1 && d.y >= f->y0 && d.y <= f->y1) {
                    f->shown = 1;
                    d.stale |= f->stale;
                }
            }
            /* the interval that ended: a frame dropped or drawn */
            if (fs_dropped) {
                fs_run++;
                g_rt_gpu_skipped++;
            } else if (fs_drew) {
                fs_run = 0;
            }
            fs_dropped = fs_drew = 0;
            /* the next: dropped if the game is two vblanks further on */
            const uint32_t behind = __atomic_load_n(&vbl_queued, __ATOMIC_ACQUIRE) - ++vbl_done;
            fs_skipping = behind >= 2 && fs_run < fs_max;
        }
        plat_display_ready(&d);
    }
    barrier_(0);
    apply_band(me()); /* the split may have moved */
}

/* idx: 1 for the lead worker (with two: the top band), 0 for the second.
 * On the board each runs on the core of the same number. */
void rt_gpu_worker_n(volatile int* stop, int idx) {
#ifndef ESP_PLATFORM
    psyz_host_core = idx;
#endif
    if (soft_core() != idx) RT_LOG("gpu: worker %d is on core %d", idx, soft_core());
    Parser* p = &P[idx];
    uint64_t busy_since = 0;
    timing_packets = 0;
    apply_band(p);
    ring_r_local[idx] = ring_r[idx];
    while (!*stop) {
        uint32_t r = ring_r_local[idx];
        if (r == ring_load(&ring_w)) {
            if (ring_r[idx] != r) __atomic_store_n(&ring_r[idx], r, __ATOMIC_RELEASE);
            if (busy_since) {
                uint64_t d = plat_time_us() - busy_since;
                busy_us[idx] += d;
                if (idx == 1) g_rt_prof.gpu += d;
                busy_since = 0;
            }
            if (prod_wait) plat_gpu_wake_producer();
            plat_gpu_idle();
            continue;
        }
        if (!busy_since) busy_since = plat_time_us();
        /* everything queued so far, without an acquire load per entry (a
         * fence on this core); the read position is published every 32
         * entries and at the end (a release store is a fence too) */
        const uint32_t end = ring_load(&ring_w);
        while (r != end) {
            const uint32_t i = (r & (RING_ENTRIES - 1)) * 2;
            const uint32_t kind = ring[i], v = ring[i + 1];
            if (kind == Q_GP0) {
                gp0_exec(p, v);
            } else if (kind == Q_GP1) {
                gp1_exec(p, v);
            } else {
                if (busy_since) { /* the rendezvous is not work */
                    uint64_t d = plat_time_us() - busy_since;
                    busy_us[idx] += d;
                    if (idx == 1) g_rt_prof.gpu += d;
                }
                vblank_marker(kind, v);
                busy_since = plat_time_us();
                __atomic_store_n(&ring_r[idx], r + 1, __ATOMIC_RELEASE);
            }
            r++;
            if ((r & 31) == 0) {
                __atomic_store_n(&ring_r[idx], r, __ATOMIC_RELEASE);
                if (prod_wait && (r & 1023) == 0) plat_gpu_wake_producer();
            }
        }
        ring_r_local[idx] = r;
        __atomic_store_n(&ring_r[idx], r, __ATOMIC_RELEASE);
        if (prod_wait) plat_gpu_wake_producer();
    }
}
void rt_gpu_worker(volatile int* stop) { rt_gpu_worker_n(stop, 1); }

/* ---- recording the command stream (rt_gpu_record) and replaying it
 * (rt_gpu_bench): the same frames through the rasterizer on any machine ----
 * File: "TPMG" u32 version, then VRAM (1 MB), then [kind, value] pairs. */
static FILE* rec_f;
static uint32_t rec_from, rec_to;
static void rec_put(uint32_t kind, uint32_t v) {
    if (!rec_f) return;
    uint32_t f = rt_frame_count();
    if (f < rec_from) return;
    if (f >= rec_to) {
        fclose(rec_f);
        rec_f = NULL;
        RT_LOG("gpu: recording done");
        return;
    }
    static int started;
    if (!started) {
        started = 1;
        gpu_drain();
        const uint32_t hdr[2] = {0x47504D54u, 1};
        fwrite(hdr, 4, 2, rec_f);
        fwrite(g_rt_vram, 2, VRAM_W * VRAM_H, rec_f);
    }
    const uint32_t e[2] = {kind, v};
    fwrite(e, 4, 2, rec_f);
}
void rt_gpu_record(const char* path, uint32_t from, uint32_t frames) {
    rec_f = fopen(path, "wb");
    rec_from = from;
    rec_to = from + frames;
}

/* Replays a recording `reps` times on the calling thread (drawing state of
 * its core) and returns the microseconds per vblank of it; the VRAM it
 * leaves is the recording's last frame. */
/* 5: everything, through the per-row span dispatch (the old path) */
void rt_gpu_bench_mode(int skip) {
    /* 6: everything, row loops but no pre-modulated palettes; 7: the per-row
     * dispatch with texels from internal RAM (timing only) */
    psyz_tex_rows = skip == 5 || skip == 7 ? 0 : skip == 6 ? 2 : 1;
    psyz_bench_skip = skip == 7 ? 7 : skip >= 5 ? 0 : skip;
#ifdef ESP_PLATFORM
    if (skip == 7 && !psyz_bench_page)
        psyz_bench_page = (u16*)heap_caps_calloc(1, 4 * 2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (skip != 7 && psyz_bench_page) {
        heap_caps_free(psyz_bench_page);
        psyz_bench_page = NULL;
    }
#endif
}
uint32_t rt_gpu_bench(const uint32_t* rec, uint32_t words, int reps) {
    if (words < 2 + VRAM_W * VRAM_H / 2 || rec[0] != 0x47504D54u) return 0;
    if (!gpu_alloc()) return 0;
    gpu_reset();
    const uint32_t* e = rec + 2 + VRAM_W * VRAM_H / 2;
    const uint32_t n = (words - (uint32_t)(e - rec)) / 2;
    uint32_t vblanks = 0;
    const int saved = timing_packets;
    timing_packets = 0;
    uint64_t total = 0;
    for (int k = 0; k < reps; k++) {
        memcpy(g_rt_vram, rec + 2, VRAM_W * VRAM_H * 2);
        Draw_Reset();
        Parser* p = &P[soft_core()];
        memset(p, 0, sizeof *p);
        p->ax1 = VRAM_W - 1;
        p->ay1 = VRAM_H - 1;
        const uint64_t t0 = plat_time_us();
        for (uint32_t i = 0; i < n; i++) {
            if (e[2 * i] == Q_GP0) gp0_exec(p, e[2 * i + 1]);
            else if (e[2 * i] == Q_GP1) gp1_exec(p, e[2 * i + 1]);
            else if (k == 0) vblanks++;
        }
        total += plat_time_us() - t0;
    }
    timing_packets = saved;
    return vblanks ? (uint32_t)(total / (uint64_t)reps / vblanks) : 0;
}

/* n words of one DMA packet: with the queue, reserved and published at once
 * (one release, one wake check) instead of word by word */
void gpu_gp0_words(const uint32_t* w, uint32_t n) {
    if (!async_on || !ring || rec_f || n > 256) {
        for (uint32_t i = 0; i < n; i++) gpu_gp0(w[i]);
        return;
    }
    uint32_t wp = ring_w;
    if (wp + n - ring_tail() > RING_ENTRIES) {
        plat_gpu_kick();
        prod_wait = 1;
        while (wp + n - ring_tail() > RING_ENTRIES) plat_gpu_idle();
        prod_wait = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t j = ((wp + i) & (RING_ENTRIES - 1)) * 2;
        ring[j] = Q_GP0;
        ring[j + 1] = w[i];
    }
    __atomic_store_n(&ring_w, wp + n, __ATOMIC_RELEASE);
    /* a worker that ran dry is woken once a batch built up */
    const uint32_t r1 = ring_load(&ring_r[1]);
    if ((wp - r1 < KICK_BATCH && wp + n - r1 >= KICK_BATCH) ||
        (nworkers == 2 && wp - ring_load(&ring_r[0]) < KICK_BATCH &&
         wp + n - ring_load(&ring_r[0]) >= KICK_BATCH))
        plat_gpu_kick();
}

void gpu_gp0(uint32_t w) {
    rec_put(Q_GP0, w);
    if (async_on && ring) ring_push(Q_GP0, w);
    else gp0_exec(&P[0], w);
}

void gpu_gp1(uint32_t w) {
    gp1_display(w);
    uint32_t c = (w >> 24) & 0x3F;
    if (c == 0x00 || c == 0x01 || c >= 0x10) {
        rec_put(Q_GP1, w);
        if (async_on && ring) ring_push(Q_GP1, w);
        else gp1_exec(&P[0], w);
    }
}

/* at every vblank, after plat_vblank: present */
void gpu_present(void) {
    RtDisplay d;
    rt_gpu_display(&d);
    rec_put(Q_VBLANK, 0);
    if (async_on && ring) {
        uint32_t v = (uint32_t)(d.x & 0x3FF) | ((uint32_t)(d.y & 0x1FF) << 10) |
                     ((uint32_t)(d.w & 0x3FF) << 19) | ((uint32_t)(d.h == 480) << 29) |
                     ((uint32_t)(d.rgb24 & 1) << 30) | ((uint32_t)(d.enabled & 1) << 31);
        ring_push(Q_VBLANK | ((uint32_t)d.top << 8) | ((uint32_t)d.lines << 20), v);
        __atomic_store_n(&vbl_queued, vbl_queued + 1, __ATOMIC_RELEASE);
        plat_gpu_kick();
    } else {
        d.stale = 0;
        plat_display_ready(&d);
    }
}

uint32_t gpu_read(void) {
    gpu_drain();
    Parser* p = &P[lead_idx()];
    if (p->xfer_out) {
        uint32_t v = xfer_buf[2 * p->xfer_pos] | ((uint32_t)xfer_buf[2 * p->xfer_pos + 1] << 16);
        if (++p->xfer_pos >= p->xfer_words) p->xfer_out = 0;
        return v;
    }
    return p->gpuread_latch;
}

uint32_t gpu_stat(void) {
    static uint32_t toggle;
    const Parser* p = &P[lead_idx()];
    const int xfer_out = p->xfer_out;
    uint32_t s = p->stat_draw & 0x7FF;
    s |= (p->stat_draw & 0x1800);
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
