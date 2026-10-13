/* Desktop runner for the recompiled game: headless, deterministic.
 *
 *   tpm-host <SLUS_008.84> <disc.bin> [options]
 *     --frames N        stop after N frames (default 600)
 *     --dump DIR        write the display as PNG every --every frames
 *     --every N         (default 30)
 *     --vram            dump all of VRAM (1024x512) instead of the display
 *     --wav FILE        record the SPU's output
 *     --press F:BUTTONS hold buttons from frame F for 10 frames
 *                       (BUTTONS: start,cross,circle,square,triangle,up,...)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt_platform.h"

static FILE* disc;
static uint32_t disc_sectors;
static int max_frames = 600, every = 30, dump_vram;
static const char* dump_dir;

typedef struct {
    uint32_t frame;
    uint16_t buttons;
} Press;
static Press presses[64];
static int npresses;

int plat_read_sector(uint32_t lba, uint8_t* out) {
    if (lba >= disc_sectors) return -1;
    if (fseek(disc, (long)lba * 2352, SEEK_SET)) return -1;
    return fread(out, 1, 2352, disc) == 2352 ? 0 : -1;
}
uint32_t plat_disc_sectors(void) { return disc_sectors; }
void* plat_alloc(uint32_t size) { return calloc(1, size); }
void plat_free(void* p) { free(p); }
void plat_pause_wait(void) {}

static FILE* wav;
static uint32_t wav_frames;
void plat_audio(const int16_t* samples, int frames) {
    if (!wav) return;
    fwrite(samples, 4, (size_t)frames, wav);
    wav_frames += (uint32_t)frames;
}
static void wav_header(void) {
    uint32_t data = wav_frames * 4;
    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                     0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a'};
    uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
}

uint64_t plat_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint16_t plat_pad_buttons(int port) {
    if (port) return 0;
    uint32_t f = rt_frame_count();
    uint16_t b = 0;
    for (int i = 0; i < npresses; i++)
        if (f >= presses[i].frame && f < presses[i].frame + 10) b |= presses[i].buttons;
    return b;
}

/* ---- a tiny PNG writer (stored deflate blocks) ---- */
static uint32_t crc_table[256];
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc((const uint8_t*)type, 4, 0);
    c = crc(data, n, c);
    be32(h, c);
    fwrite(h, 1, 4, f);
}
static void write_png(const char* path, const uint8_t* rgb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + 3 * (size_t)w);
    uint8_t* raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[y * (1 + 3 * w)] = 0;
        memcpy(raw + y * (1 + 3 * w) + 1, rgb + (size_t)y * 3 * w, (size_t)3 * w);
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t* z = malloc(z_n);
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw_n; p += 65535) {
        size_t n = raw_n - p < 65535 ? raw_n - p : 65535;
        z[o++] = p + n >= raw_n;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + p, n);
        o += n;
    }
    be32(z + o, (b << 16) | a);
    o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static void dump(uint32_t frame) {
    char path[512];
    RtDisplay d;
    rt_gpu_display(&d);
    int w, h;
    uint8_t* rgb;
    if (dump_vram) {
        w = 1024; h = 512;
        rgb = malloc((size_t)w * h * 3);
        for (int i = 0; i < w * h; i++) {
            uint16_t p = g_rt_vram[i];
            rgb[3 * i] = (uint8_t)((p & 31) << 3);
            rgb[3 * i + 1] = (uint8_t)(((p >> 5) & 31) << 3);
            rgb[3 * i + 2] = (uint8_t)(((p >> 10) & 31) << 3);
        }
    } else {
        w = d.w; h = d.h;
        rgb = calloc((size_t)w * h, 3);
        for (int y = 0; y < h && d.enabled; y++) {
            int vy = (d.y + y) & 511;
            if (d.rgb24) {
                const uint8_t* row = (const uint8_t*)&g_rt_vram[vy * 1024];
                for (int x = 0; x < w; x++) {
                    int bx = (d.x * 2 + x * 3) % 2048;
                    memcpy(rgb + 3 * (y * w + x), row + bx, 3);
                }
            } else {
                for (int x = 0; x < w; x++) {
                    uint16_t p = g_rt_vram[vy * 1024 + ((d.x + x) & 1023)];
                    uint8_t* o = rgb + 3 * (y * w + x);
                    o[0] = (uint8_t)((p & 31) << 3);
                    o[1] = (uint8_t)(((p >> 5) & 31) << 3);
                    o[2] = (uint8_t)(((p >> 10) & 31) << 3);
                }
            }
        }
    }
    snprintf(path, sizeof path, "%s/frame_%05u.png", dump_dir, (unsigned)frame);
    write_png(path, rgb, w, h);
    free(rgb);
    printf("[host] frame %u: display %dx%d at (%d,%d)%s%s -> %s\n", (unsigned)frame, d.w, d.h,
           d.x, d.y, d.rgb24 ? " 24-bit" : "", d.enabled ? "" : " (off)", path);
}

void plat_vblank(void) {
    uint32_t f = rt_frame_count();
    if (dump_dir && f % (uint32_t)every == 0) dump(f);
    if ((int)f >= max_frames) {
        if (dump_dir) dump(f);
        if (wav) wav_header();
#ifdef RT_DEBUG
        extern int g_shadow_max;
        printf("[host] deepest guest call chain: %d\n", g_shadow_max);
#endif
        printf("[host] %d frames: done\n", max_frames);
        fflush(stdout);
        exit(0);
    }
}

static uint16_t parse_buttons(const char* s) {
    static const char* names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    uint16_t b = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char* t = strtok(buf, ","); t; t = strtok(NULL, ","))
        for (int i = 0; i < 16; i++)
            if (!strcmp(t, names[i])) b |= (uint16_t)(1u << i);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <SLUS_008.84> <disc.bin> [--frames N] [--dump DIR] [--every N] [--vram] [--press F:BTN]\n", argv[0]);
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vram")) dump_vram = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (wav) fseek(wav, 44, SEEK_SET);
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npresses < 64) {
            char* arg = argv[++i];
            char* colon = strchr(arg, ':');
            if (!colon) continue;
            presses[npresses].frame = (uint32_t)atoi(arg);
            presses[npresses].buttons = parse_buttons(colon + 1);
            npresses++;
        }
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* exe = malloc((size_t)n);
    if (fread(exe, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    disc = fopen(argv[2], "rb");
    if (!disc) { perror(argv[2]); return 1; }
    fseek(disc, 0, SEEK_END);
    disc_sectors = (uint32_t)(ftell(disc) / 2352);
    setvbuf(stdout, NULL, _IOLBF, 0);
    rt_boot(exe, (uint32_t)n);
    rt_shutdown();
    return 0;
}
