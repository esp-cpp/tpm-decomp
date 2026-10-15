/* The MDEC: the movie decoder (psx-spx, "MDEC"). The game decodes the
 * movie's variable-length codes itself (libpress DecDCTvlc) and hands the
 * MDEC the run-length coefficients of a frame through DMA0; DMA1 takes the
 * macroblocks back out as 15- or 24-bit pixels (or 4/8-bit grey blocks).
 *
 * A decode command's words are copied in as DMA0 delivers them; the
 * macroblocks are decoded only as DMA1 (or a read of the data port) asks
 * for them, straight into guest RAM. A DMA1 started before its input
 * arrived waits until DMA0 brings it. Results need not match the hardware's
 * IDCT bit for bit, only look like it. */
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

/* the compressed words of the decode command in progress */
#define IN_WORDS (64 * 1024u)
static uint32_t* in_buf;
static uint32_t in_len, in_need, in_pos; /* words held, still to come, read */

static uint32_t cmd;      /* the command being received or executed */
static int cmd_kind;      /* 0 none, 1 decode, 2 quant, 3 scale */
static uint32_t ctrl;     /* 0x1F801824 writes: DMA enables */
static uint8_t qt_y[64], qt_uv[64];
static int16_t scale[64];
static uint32_t param_n;  /* table bytes/halfwords received */

/* the DMAs in progress (below) */
typedef struct {
    int on;
    uint32_t addr, words, done;
} Xfer;
static Xfer xin, xout;
static int mdec_pump(int self);

/* output: one macroblock at a time */
static uint32_t out_mb[192];
static uint32_t out_len, out_pos;


static const uint8_t zagzig[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

uint32_t mdec_mem_bytes(int* count) {
    *count += 1;
    return IN_WORDS * 4;
}

void mdec_reset(void) {
    if (!in_buf) in_buf = (uint32_t*)plat_alloc(IN_WORDS * 4);
    in_len = in_need = in_pos = 0;
    cmd = 0;
    cmd_kind = 0;
    ctrl = 0;
    param_n = 0;
    out_len = out_pos = 0;
    memset(&xin, 0, sizeof xin);
    memset(&xout, 0, sizeof xout);
}

void mdec_free(void) {
    plat_free(in_buf);
    in_buf = NULL;
}

/* ---- decoding ---- */
static int depth(void) { return (int)((cmd >> 27) & 3); } /* 0: 4, 1: 8, 2: 24, 3: 15 bits */

static inline uint16_t in_half(void) {
    const uint32_t w = in_pos >> 1 < in_len ? in_buf[in_pos >> 1] : 0xFE00FE00u;
    const uint16_t h = (in_pos & 1) ? (uint16_t)(w >> 16) : (uint16_t)w;
    in_pos++;
    return h;
}
static inline int s10(uint32_t v) { return (int)((v & 0x3FF) ^ 0x200) - 0x200; }

/* one 8x8 block: run-length to coefficients, then the IDCT */
static int rl_block(int16_t* blk, const uint8_t* qt) {
    int32_t c[64];
    memset(c, 0, sizeof c);
    uint16_t n;
    do {
        if ((in_pos >> 1) >= in_len) return 0;
        n = in_half();
    } while (n == 0xFE00); /* padding */
    const int q = n >> 10;
    int k = 0;
    int val = q ? s10(n) * qt[0] : s10(n) * 2;
    for (;;) {
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        c[q ? zagzig[k] : k] = val;
        n = in_half();
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = q ? (s10(n) * qt[k] * q + 4) / 8 : s10(n) * 2;
    }
    /* two passes of the scale table (s.15-ish cosines), each transposing */
    int32_t t[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)c[y + z * 8] * scale[x + z * 8];
            t[x + y * 8] = (int32_t)((sum + 0x8000) >> 16);
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)t[y + z * 8] * scale[x + z * 8];
            int v = (int)((sum + 0x8000) >> 16);
            blk[x + y * 8] = (int16_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    return 1;
}

static inline int clamp8s(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

/* the next macroblock into out_mb; 0 when the input is used up */
static int decode_mb(void) {
    const int d = depth();
    const int sgn = (cmd >> 26) & 1, b15 = (cmd >> 25) & 1;
    out_len = out_pos = 0;
    if (d <= 1) { /* grey: one Y block */
        int16_t y[64];
        if (!rl_block(y, qt_y)) return 0;
        if (d == 1) {
            uint8_t* o = (uint8_t*)out_mb;
            for (int i = 0; i < 64; i++) o[i] = (uint8_t)(sgn ? y[i] : y[i] + 128);
            out_len = 16;
        } else {
            uint8_t* o = (uint8_t*)out_mb;
            for (int i = 0; i < 64; i += 2) {
                const int a = (sgn ? y[i] : y[i] + 128) >> 4, b = (sgn ? y[i + 1] : y[i + 1] + 128) >> 4;
                o[i / 2] = (uint8_t)((a & 15) | (b << 4));
            }
            out_len = 8;
        }
        return 1;
    }
    int16_t cr[64], cb[64], yb[4][64];
    if (!rl_block(cr, qt_uv) || !rl_block(cb, qt_uv)) return 0;
    for (int i = 0; i < 4; i++)
        if (!rl_block(yb[i], qt_y)) return 0;
    uint8_t* o8 = (uint8_t*)out_mb;
    uint16_t* o16 = (uint16_t*)out_mb;
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            const int ci = (px >> 1) + (py >> 1) * 8;
            const int R = cr[ci], B = cb[ci];
            /* 1.402 R, -0.3437 B - 0.7143 R, 1.772 B, in 1/1024 */
            const int r = (1436 * R) >> 10, g = (-352 * B - 731 * R) >> 10, b = (1815 * B) >> 10;
            const int Y = yb[(px >> 3) + (py >> 3) * 2][(px & 7) + (py & 7) * 8];
            int rr = clamp8s(Y + r), gg = clamp8s(Y + g), bb = clamp8s(Y + b);
            if (!sgn) rr += 128, gg += 128, bb += 128;
            const int p = px + py * 16;
            if (d == 2) {
                o8[p * 3] = (uint8_t)rr;
                o8[p * 3 + 1] = (uint8_t)gg;
                o8[p * 3 + 2] = (uint8_t)bb;
            } else {
                o16[p] = (uint16_t)(((rr & 0xFF) >> 3) | (((gg & 0xFF) >> 3) << 5) | (((bb & 0xFF) >> 3) << 10) |
                                    (b15 << 15));
            }
        }
    out_len = d == 2 ? 192 : 128;
    return 1;
}

/* a decode is done once its last macroblock is out and only padding is
 * left of its input */
static void check_done(void) {
    if (cmd_kind != 1 || in_need || out_pos < out_len) return;
    while ((in_pos >> 1) < in_len) {
        const uint32_t w = in_buf[in_pos >> 1];
        if (((in_pos & 1) ? (w >> 16) : (w & 0xFFFF)) != 0xFE00) return;
        in_pos++;
    }
    cmd_kind = 0;
}

/* the next output word, decoding as needed; 0 when there is none */
static int out_word(uint32_t* w) {
    if (out_pos >= out_len) {
        if (cmd_kind != 1 || in_need || !decode_mb()) {
            check_done();
            return 0;
        }
    }
    *w = out_mb[out_pos++];
    if (out_pos >= out_len) check_done();
    return 1;
}

/* ---- commands ---- */
static void command_done(void) {
    cmd_kind = 0;
    in_need = 0;
}

static void param_word(uint32_t v) {
    switch (cmd_kind) {
    case 1:
        if (in_len < IN_WORDS) in_buf[in_len++] = v;
        if (--in_need == 0) {
            in_pos = 0;
            out_len = out_pos = 0;
        }
        break;
    case 2: { /* quant tables: 64 luma bytes, then 64 chroma (bit 0) */
        for (int i = 0; i < 4; i++, param_n++) {
            const uint8_t b = (uint8_t)(v >> (8 * i));
            if (param_n < 64) qt_y[param_n] = b;
            else qt_uv[param_n - 64] = b;
        }
        if (--in_need == 0) command_done();
        break;
    }
    case 3: /* scale table: 64 halfwords */
        scale[param_n++] = (int16_t)v;
        scale[param_n++] = (int16_t)(v >> 16);
        if (--in_need == 0) command_done();
        break;
    default: break;
    }
}

static void mdec_word(uint32_t v) {
    if (in_need) {
        param_word(v);
        return;
    }
    cmd = v;
    param_n = 0;
    RT_TRACE("mdec: command %08X", (unsigned)v);
    switch (v >> 29) {
    case 1:
        cmd_kind = 1;
        in_len = in_pos = 0;
        out_len = out_pos = 0;
        in_need = v & 0xFFFF;
        if (!in_need) cmd_kind = 0;
        break;
    case 2:
        cmd_kind = 2;
        in_need = (v & 1) ? 32 : 16;
        break;
    case 3:
        cmd_kind = 3;
        in_need = 32;
        break;
    default: cmd_kind = 0; break; /* no function */
    }
}

void mdec_write(uint32_t reg, uint32_t v) {
    if (reg == 0) {
        mdec_word(v);
        mdec_pump(-1);
        return;
    }
    if (v & 0x80000000u) { /* reset */
        cmd_kind = 0;
        in_need = 0;
        in_len = in_pos = 0;
        out_len = out_pos = 0;
        xin.on = xout.on = 0;
    }
    ctrl = v & 0x60000000u;
    RT_TRACE("mdec: control %08X", (unsigned)v);
}

uint32_t mdec_read(uint32_t reg) {
    if (reg == 0) {
        uint32_t w;
        return out_word(&w) ? w : 0;
    }
    uint32_t s = 0;
    const int have_out = out_pos < out_len || (cmd_kind == 1 && !in_need && (in_pos >> 1) < in_len);
    if (!have_out) s |= 1u << 31;
    if (cmd_kind) s |= 1u << 29;
    if (in_need && (ctrl & 0x40000000u)) s |= 1u << 28;
    if (have_out && (ctrl & 0x20000000u)) s |= 1u << 27;
    s |= ((cmd >> 25) & 0xF) << 23;
    s |= 4u << 16;
    s |= (in_need ? in_need - 1 : 0xFFFF) & 0xFFFF;
    return s;
}

/* ---- DMA ----
 * Both directions wait as the hardware's do: DMA0 until the MDEC takes its
 * words (not while a decode's output is still to be read), DMA1 until the
 * macroblocks are there. mdec_pump moves whatever can move and completes
 * the transfers that are through. */

static int can_take(void) { return !(cmd_kind == 1 && !in_need); }

/* finished: the channel to complete now, other than `self` (which the
 * caller's dma_start completes when its start returns 1) */
static int mdec_pump(int self) {
    int self_done = 0;
    for (int moved = 1; moved;) {
        moved = 0;
        while (xin.on && xin.done < xin.words && can_take()) {
            mdec_word(LW(0x80000000u | ((xin.addr + 4 * xin.done) & 0x1FFFFCu)));
            xin.done++;
            moved = 1;
        }
        if (xin.on && xin.done == xin.words) {
            xin.on = 0;
            RT_TRACE("mdec: dma0 done (%u words)", (unsigned)xin.words);
            if (self == 0) self_done = 1;
            else hw_dma_finish(0);
        }
        uint32_t w;
        while (xout.on && xout.done < xout.words && out_word(&w)) {
            SW(0x80000000u | ((xout.addr + 4 * xout.done) & 0x1FFFFCu), w);
            xout.done++;
            moved = 1;
        }
        if (xout.on && xout.done == xout.words) {
            xout.on = 0;
            RT_TRACE("mdec: dma1 done (%u words)", (unsigned)xout.words);
            if (self == 1) self_done = 1;
            else hw_dma_finish(1);
        }
    }
    return self_done;
}

/* DMA0: words from RAM into the MDEC; 1 when done now */
int mdec_dma_in(uint32_t addr, uint32_t words) {
    xin = (Xfer){1, addr, words, 0};
    return mdec_pump(0);
}

/* DMA1: words from the MDEC into RAM; 1 when done now */
int mdec_dma_out(uint32_t addr, uint32_t words) {
    xout = (Xfer){1, addr, words, 0};
    return mdec_pump(1);
}

/* ---- the native movie player (rt_movie.c) ----
 * A frame's decode command and its words, as the game's VLC decoder left
 * them, in as 15-bit pixels with the tables the game uploaded; then the
 * macroblocks out one at a time (16x16, row-major). The MDEC's own state
 * is the player's meanwhile. */
void mdec_native_begin(const uint32_t* words, uint32_t n) {
    cmd_kind = 0;
    in_need = 0;
    xin.on = xout.on = 0;
    for (uint32_t i = 0; i < n; i++)
        mdec_word(i == 0 ? (words[0] & ~0x1E000000u) | 0x18000000u : words[i]); /* 15-bit, unsigned, bit 15 0 */
}
int mdec_native_mb(uint16_t* px) {
    if (cmd_kind != 1 || in_need || !decode_mb()) return 0;
    memcpy(px, out_mb, 512);
    out_len = out_pos = 0;
    return 1;
}
