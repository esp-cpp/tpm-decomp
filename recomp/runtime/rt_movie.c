/* The cutscene movies, natively.
 *
 * TPM's movie player (the FMV.CPE overlay) streams STR files with libcd's
 * ring and decodes them through the MDEC; it depends on the CD, the DMAs
 * and the decoder keeping the hardware's pace to the sector (the ring holds
 * one frame: the VLC decode must be done before the next frame's first
 * sector lands). Here its play function, 0x80160C7C (movie index, table of
 * 8-byte entries whose first three bytes are the movie's BCD position), is
 * replaced: the movie's sectors are read from the disc at double speed in
 * emulated time, each frame goes through the game's own VLC decoder (the
 * recompiled 0x80161D08, its tables built by 0x80161C94) and the MDEC's
 * decoding (rt_mdec.c), and into VRAM through the GPU queue; the XA audio
 * sectors are decoded into the SPU's CD input (Psyz_CdPullSamples). Start
 * or Cross skips. Emulated time runs on through rt_poll, so pausing, the
 * menu and save states work as anywhere else. */
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

#define A0 g_cpu.r[4]
#define A1 g_cpu.r[5]
#define V0 g_cpu.r[2]

/* the overlay's functions, recompiled only with the disc (recomp.py --disc) */
void f_80161C94(void) __attribute__((weak)); /* DecDCTvlcBuild: tables to the scratchpad */
void f_80161CE0(void) __attribute__((weak)); /* DecDCTvlcSize */
void f_80161D08(void) __attribute__((weak)); /* DecDCTvlc */
unsigned int Psyz_SpuRead(unsigned int reg);
void Psyz_SpuWrite(unsigned int reg, unsigned short value);

/* ---- XA ADPCM: CD audio for the SPU (psx-spx, "CDROM XA Audio ADPCM") ---- */
#define XA_RING 16384u /* stereo frames at 44.1 kHz */
static int16_t xa_ring[XA_RING * 2];
static uint32_t xa_w, xa_r;
static int32_t xa_old[2], xa_older[2];
static uint32_t xa_phase;  /* resampler position, 16.16 */
static int16_t xa_prev[2]; /* the last input frame, for the interpolation */

void xa_reset(void) {
    xa_w = xa_r = 0;
    memset(xa_old, 0, sizeof xa_old);
    memset(xa_older, 0, sizeof xa_older);
    xa_phase = 0;
    xa_prev[0] = xa_prev[1] = 0;
}

/* the SPU takes the CD's samples from here (rt_psyz_glue.c) */
size_t xa_pull(int16_t* out, size_t frames) {
    size_t n = 0;
    while (n < frames && xa_r != xa_w) {
        const uint32_t i = (xa_r % XA_RING) * 2;
        out[2 * n] = xa_ring[i];
        out[2 * n + 1] = xa_ring[i + 1];
        xa_r++;
        n++;
    }
    return n;
}

static inline int16_t clamp16s(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* one input frame at `rate` Hz out to the 44.1 kHz ring, linearly */
static void xa_put(int16_t l, int16_t r, uint32_t rate) {
    const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100u);
    while (xa_phase < 0x10000u) {
        if (xa_w - xa_r >= XA_RING) break; /* full: the SPU fell behind */
        const int32_t f = (int32_t)xa_phase;
        const uint32_t i = (xa_w % XA_RING) * 2;
        xa_ring[i] = (int16_t)(xa_prev[0] + (((l - xa_prev[0]) * f) >> 16));
        xa_ring[i + 1] = (int16_t)(xa_prev[1] + (((r - xa_prev[1]) * f) >> 16));
        xa_w++;
        xa_phase += step;
    }
    xa_phase -= 0x10000u;
    xa_prev[0] = l;
    xa_prev[1] = r;
}

/* one raw 2352-byte audio sector */
void xa_decode_sector(const uint8_t* s) {
    static const int pos[4] = {0, 60, 115, 98}, neg[4] = {0, 0, -52, -55};
    const uint8_t ci = s[19];
    const int stereo = (ci & 3) == 1, eight = ((ci >> 4) & 3) == 1;
    const uint32_t rate = ((ci >> 2) & 3) == 1 ? 18900u : 37800u;
    const int blocks = eight ? 4 : 8;
    const uint8_t* d = s + 24;
    for (int g = 0; g < 18; g++, d += 128) {
        int16_t pcm[8][28];
        for (int b = 0; b < blocks; b++) {
            const uint8_t h = d[4 + b];
            int shift = h & 0x0F;
            if (shift > 12) shift = 9;
            const int filt = (h >> 4) & 3;
            const int ch = stereo ? (b & 1) : 0;
            for (int j = 0; j < 28; j++) {
                int32_t t;
                if (eight) {
                    t = (int32_t)(int8_t)d[16 + j * 4 + b] << 8;
                } else {
                    const uint8_t byte = d[16 + j * 4 + (b >> 1)];
                    t = (int32_t)(int16_t)((uint16_t)((b & 1) ? (byte >> 4) : (byte & 15)) << 12);
                }
                int32_t v = (t >> shift) + ((xa_old[ch] * pos[filt] + xa_older[ch] * neg[filt] + 32) >> 6);
                v = clamp16s(v);
                xa_older[ch] = xa_old[ch];
                xa_old[ch] = v;
                pcm[b][j] = (int16_t)v;
            }
        }
        if (stereo) {
            for (int b = 0; b < blocks; b += 2)
                for (int j = 0; j < 28; j++) xa_put(pcm[b][j], pcm[b + 1][j], rate);
        } else {
            for (int b = 0; b < blocks; b++)
                for (int j = 0; j < 28; j++) xa_put(pcm[b][j], pcm[b][j], rate);
        }
    }
}

/* ---- the player ---- */
static int movies_on = 1;
void rt_set_movies(int on) { movies_on = on; }
int rt_movies_on(void) { return movies_on; }

/* the overlay's own buffers (its .bss, unused while this plays): the frame's
 * bitstream and the VLC decoder's output */
#define MOV_BS 0x80176218u
#define MOV_BS_MAX (24u * 2016u) /* (the original's ring: 32 sectors) */
#define MOV_OUT 0x801C955Cu
#define MOV_SECTOR_DATA 2016u
#define MOV_QUANT 0x8017445Cu /* the MDEC's quantization tables (luma, chroma) */
#define MOV_SCALE 0x801744E0u /* and its IDCT scale table */

static void spu_set(uint32_t reg, uint16_t v) { Psyz_SpuWrite(reg, v); }
static uint16_t spu_get(uint32_t reg) { return Psyz_SpuRead(reg); }

/* one 16-pixel column: rows `first`, `first + step`, ... (`rows` of them)
 * to VRAM at (x, y) */
static void upload_column(const uint16_t* colpx, int first, int step, int rows, int x, int y) {
    gpu_gp0(0xA0000000u);
    gpu_gp0(((uint32_t)y << 16) | (uint32_t)x);
    gpu_gp0(((uint32_t)rows << 16) | 16u);
    uint32_t buf[128];
    int n = 0;
    for (int i = 0; i < rows; i++) {
        const uint16_t* row = &colpx[(first + i * step) * 16];
        for (int c = 0; c < 16; c += 2) {
            buf[n++] = row[c] | ((uint32_t)row[c + 1] << 16);
            if (n == 128) {
                gpu_gp0_words(buf, (uint32_t)n);
                n = 0;
            }
        }
    }
    if (n) gpu_gp0_words(buf, (uint32_t)n);
}

static void fill_black(int y) {
    gpu_gp0(0x02000000u);
    gpu_gp0((uint32_t)y << 16);
    gpu_gp0((240u << 16) | 320u);
}

/* The frame into the 320x240 buffer at row buf_y. Up to 300 lines it is a
 * picture for the non-interlaced screen (taller ones lose the overscan,
 * evenly); taller is the interlaced screen's: every other line,
 * letterboxed. */
static void show_frame(int w, int h, int buf_y) {
    /* the VLC decoder: bitstream at MOV_BS, MDEC words to MOV_OUT */
    g_cpu.r[4] = MOV_BS;
    g_cpu.r[5] = MOV_OUT;
    g_cpu.r[31] = 0;
    f_80161D08();
    const uint32_t hdr = LW(MOV_OUT);
    const uint32_t nwords = hdr & 0xFFFF;
    if (nwords == 0 || nwords > 0x8000u) return;
    mdec_native_begin((const uint32_t*)(g_ram + (MOV_OUT & (RAM_SIZE - 1))), nwords + 1);
    /* macroblocks come column by column, top to bottom */
    const int cols = (w + 15) / 16, mbrows = (h + 15) / 16;
    const int step = h > 300 ? 2 : 1;
    int rows = (h + step - 1) / step, first = 0;
    if (rows > 240) {
        first = (rows - 240) / 2 * step;
        rows = 240;
    }
    const int y0 = buf_y + (240 - rows) / 2;
    const int x0 = w < 320 ? (320 - cols * 16) / 2 : 0;
    static uint16_t colpx[16 * 512];
    for (int c = 0; c < cols && x0 + c * 16 < 1024; c++) {
        for (int m = 0; m < mbrows && m < 32; m++) {
            uint16_t mb[256];
            if (!mdec_native_mb(mb)) memset(mb, 0, sizeof mb);
            for (int r = 0; r < 16; r++) memcpy(&colpx[(m * 16 + r) * 16], &mb[r * 16], 32);
        }
        upload_column(colpx, first, step, rows, x0 + c * 16, y0);
    }
}

static int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

/* 0x80160C7C: play movie `a0` of the table at `a1` */
void hle_PlayMovie(void) {
    const uint32_t idx = A0, table = A1;
    if (!f_80161C94 || !f_80161CE0 || !f_80161D08) { /* no overlay: no movie */
        V0 = 0;
        return;
    }
    const uint32_t e = table + idx * 8;
    const uint32_t lba0 = (uint32_t)((bcd(LBU(e)) * 60 + bcd(LBU(e + 1))) * 75 + bcd(LBU(e + 2))) - 150u;
    RT_LOG("movie %u at %02X:%02X:%02X (lba %u)", (unsigned)idx, LBU(e), LBU(e + 1), LBU(e + 2),
           (unsigned)lba0);
    /* the decoder's tables go to the scratchpad, which holds the caller's
     * gp/ra (0x80083F00) and more: kept and put back */
    uint8_t scratch[SCRATCH_SIZE];
    memcpy(scratch, g_scratch, sizeof scratch);
    g_cpu.r[31] = 0;
    f_80161C94();
    g_cpu.r[4] = 0;
    f_80161CE0(); /* DecDCTvlcSize(0): no limit */
    /* DecDCTReset's tables, from the overlay's data, as it sends them */
    mdec_write(1, 0x80000000u);
    mdec_write(0, 0x40000001u);
    mdec_dma_in(MOV_QUANT & 0x1FFFFCu, 32);
    mdec_write(0, 0x60000000u);
    mdec_dma_in(MOV_SCALE & 0x1FFFFCu, 32);
    /* the CD's sound into the mix for the movie */
    const uint16_t cnt0 = spu_get(0x1AA), cdl0 = spu_get(0x1B0), cdr0 = spu_get(0x1B2);
    spu_set(0x1AA, (uint16_t)(cnt0 | 1u));
    if (!cdl0 && !cdr0) {
        spu_set(0x1B0, 0x7FFF);
        spu_set(0x1B2, 0x7FFF);
    }
    xa_reset();
    /* 320x240, 15-bit, the frame in one of two buffers */
    gpu_gp1(0x08000001u);
    gpu_gp1(0x06C60260u);
    gpu_gp1(0x07040010u);
    int shown = 0;
    fill_black(0);
    fill_black(256);
    static uint8_t sec[2352];
    const uint64_t period = CPU_HZ / 150u;
    uint64_t next = g_now;
    uint32_t lba = lba0, last_frame = 0, frames = 0;
    uint16_t pad_prev = 0xFFFF;
    int w = 0, h = 0, done = 0, others = 0, skipped = 0;
    while (!done) {
        rt_poll();
        const uint16_t pad = plat_pad_buttons(0);
        if ((pad & ~pad_prev) & ((1u << 3) | (1u << 14))) { /* Start / Cross: skip */
            skipped = 1;
            break;
        }
        pad_prev = pad;
        while (!done && g_now >= next) {
            next += period;
            if (plat_read_sector(lba++, sec)) {
                done = 1;
                break;
            }
            const uint8_t sub = sec[18];
            if (sub & 0x04) { /* XA audio */
                others = 0;
                xa_decode_sector(sec);
                continue;
            }
            const uint8_t* d = sec + 24;
            if (d[0] != 0x60 || d[1] != 0x01 || d[2] != 0x01 || d[3] != 0x80) {
                if (++others > 8) done = 1; /* past the movie */
                continue;
            }
            others = 0;
            const uint32_t secno = d[4] | d[5] << 8, nsec = d[6] | d[7] << 8;
            const uint32_t frame = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
            if (frames && frame < last_frame) { /* the next movie's first frame */
                done = 1;
                break;
            }
            if (secno * MOV_SECTOR_DATA + MOV_SECTOR_DATA <= MOV_BS_MAX)
                memcpy(g_ram + ((MOV_BS + secno * MOV_SECTOR_DATA) & (RAM_SIZE - 1)), d + 32,
                       MOV_SECTOR_DATA);
            w = d[16] | d[17] << 8;
            h = d[18] | d[19] << 8;
            if (secno + 1 == nsec && w > 0 && w <= 640 && h > 0 && h <= 512) {
                last_frame = frame;
                frames++;
                const int y = shown ? 0 : 256;
                show_frame(w, h, y);
                gpu_gp1(0x05000000u | ((uint32_t)y << 10));
                gpu_gp1(0x03000000u); /* display on */
                shown ^= 1;
            }
        }
    }
    RT_LOG("movie %u: %u frames (%dx%d)%s", (unsigned)idx, (unsigned)frames, w, h, skipped ? ", skipped" : "");
    /* the rest of the audio plays out, unless skipped */
    for (int i = 0; i < 200 && !skipped && xa_r != xa_w; i++) rt_poll();
    xa_reset();
    spu_set(0x1AA, cnt0);
    spu_set(0x1B0, cdl0);
    spu_set(0x1B2, cdr0);
    memcpy(g_scratch, scratch, sizeof scratch);
    V0 = 0;
}
