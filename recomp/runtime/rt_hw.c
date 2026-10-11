/* The PlayStation's I/O space, as far as the game and PSY-Q need it:
 * interrupt controller, DMA (instant transfers), root counters, GPU / CD-ROM
 * ports (forwarded), and register-file stand-ins for the SPU, SIO and MDEC.
 */
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

/* psyz's register-level SPU (psyz_spu.c) */
void Psyz_SpuInit(void);
void Psyz_SpuReset(int hot);
void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value);
unsigned short Psyz_SpuRead(unsigned int reg_offset);
void Psyz_SpuFifoWriteBulk(const unsigned char* src, unsigned int size);
void Psyz_SpuMemRead(unsigned int offset, void* dst, unsigned int size);
unsigned int Psyz_SpuGetTransferAddr(void);
void Psyz_SpuSetTransferAddr(unsigned int addr);
void Psyz_SpuPullSamples(short* out, int num_frames);
extern volatile int psyz_pending_spu_irq;

static uint32_t i_stat, i_mask;
static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
static uint32_t dpcr = 0x07654321u, dicr;
static uint32_t io_misc[0x40]; /* 0x1F801000..0x1F8010FF: memory control etc */
static uint64_t next_vblank;
static uint32_t frames;
#define VBLANK_CYC (CPU_HZ / 60u)
#define HBLANK_CYC 2172u /* cycles per scanline, NTSC */

/* ---- interrupt controller ---- */
uint32_t hw_istat(void) { return i_stat; }
uint32_t hw_imask(void) { return i_mask; }
void hw_irq(int line) { i_stat |= 1u << line; }
uint32_t rt_frame_count(void) { return frames; }

/* ---- root counters ---- */
typedef struct {
    uint64_t base; /* g_now at which the count was 0 */
    uint16_t mode, target;
} Rcnt;
static Rcnt rc[3];

static uint32_t rc_div(int n) {
    uint16_t src = (rc[n].mode >> 8) & 3;
    if (n == 1 && (src & 1)) return HBLANK_CYC;
    if (n == 2 && (src & 2)) return 8;
    return 1;
}
static uint32_t rc_value(int n) {
    uint64_t ticks = (g_now - rc[n].base) / rc_div(n);
    if ((rc[n].mode & 0x08) && rc[n].target) ticks %= (uint64_t)rc[n].target + 1;
    return (uint32_t)(ticks & 0xFFFF);
}

/* ---- DMA ---- */
static void dma_finish(int ch) {
    dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    if (dicr & (1u << (16 + ch))) {
        uint32_t before = dicr & 0x80000000u;
        dicr |= 1u << (24 + ch);
        int master = (dicr & 0x8000u) ||
                     ((dicr & 0x800000u) && ((dicr >> 24) & (dicr >> 16) & 0x7F));
        if (master) dicr |= 0x80000000u;
        if (master && !before) hw_irq(IRQ_DMA);
    }
}

static void dma_gpu(void) {
    uint32_t chcr = dma_chcr[2], addr = dma_madr[2] & 0x1FFFFCu;
    int mode = (chcr >> 9) & 3;
    if (mode == 2) { /* linked list (ordering table) */
        for (int guard = 0; guard < 0x40000; guard++) {
            uint32_t hdr = LW(0x80000000u | addr);
            uint32_t n = hdr >> 24;
            for (uint32_t i = 1; i <= n; i++) gpu_gp0(LW(0x80000000u | (addr + 4 * i)));
            if (hdr & 0x800000u) break; /* 0xFFFFFF: end of list */
            addr = hdr & 0x1FFFFCu;
        }
    } else {
        uint32_t words = mode == 1 ? (dma_bcr[2] & 0xFFFF) * (dma_bcr[2] >> 16)
                                   : (dma_bcr[2] & 0xFFFF ? dma_bcr[2] & 0xFFFF : 0x10000);
        if (chcr & 1) { /* RAM -> GPU */
            for (uint32_t i = 0; i < words; i++) gpu_gp0(LW(0x80000000u | (addr + 4 * i)));
        } else {
            for (uint32_t i = 0; i < words; i++) SW(0x80000000u | (addr + 4 * i), gpu_read());
        }
    }
}

static void dma_otc(void) {
    uint32_t addr = dma_madr[6] & 0x1FFFFCu, n = dma_bcr[6] & 0xFFFF;
    if (!n) n = 0x10000;
    for (uint32_t i = 0; i + 1 < n; i++, addr -= 4)
        SW(0x80000000u | addr, (addr - 4) & 0xFFFFFFu);
    SW(0x80000000u | addr, 0xFFFFFFu);
}

static void dma_start(int ch) {
    uint32_t words;
    RT_TRACE("dma%d: madr %06X bcr %08X chcr %08X dicr %08X  [%s]", ch, (unsigned)dma_madr[ch],
             (unsigned)dma_bcr[ch], (unsigned)dma_chcr[ch], (unsigned)dicr, rt_where());
    switch (ch) {
    case 2: dma_gpu(); break;
    case 6: dma_otc(); break;
    case 3: {
        uint32_t bcr = dma_bcr[3];
        words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
        cd_dma(dma_madr[3] & 0x1FFFFCu, words);
        break;
    }
    case 4: { /* SPU RAM <-> main RAM at the SPU's transfer address */
        uint32_t bcr = dma_bcr[4];
        words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
        uint32_t src = dma_madr[4] & 0x1FFFFCu;
        if (dma_chcr[4] & 1) {
            Psyz_SpuFifoWriteBulk(g_ram + src, words * 4);
        } else {
            uint32_t ta = Psyz_SpuGetTransferAddr();
            Psyz_SpuMemRead(ta, g_ram + src, words * 4);
            Psyz_SpuSetTransferAddr(ta + words * 4);
        }
        break;
    }
    case 0: case 1: { /* MDEC: not emulated; output black */
        static int warned;
        if (!warned++) RT_LOG("MDEC DMA: not emulated (movies are black)");
        if (ch == 1) {
            uint32_t bcr = dma_bcr[1];
            words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
            for (uint32_t i = 0; i < words; i++) SW(0x80000000u | ((dma_madr[1] & 0x1FFFFCu) + 4 * i), 0);
        }
        break;
    }
    default:
        RT_LOG("DMA%d start: not emulated", ch);
        break;
    }
    dma_finish(ch);
}

/* ---- SPU: psyz's register-level emulator (psyz_spu.c) ---- */

#define SPU_HZ 44100u
static uint64_t spu_frames; /* sample frames produced so far */

static uint16_t spu_read(uint32_t off) { return Psyz_SpuRead(off); }

/* run the SPU up to g_now, in chunks */
static void spu_update(void) {
    uint64_t due = g_now * SPU_HZ / CPU_HZ;
    static short buf[2 * 256];
    while (spu_frames + 64 <= due) {
        int n = (int)(due - spu_frames);
        if (n > 256) n = 256;
        Psyz_SpuPullSamples(buf, n);
        plat_audio(buf, n);
        spu_frames += (uint64_t)n;
    }
    if (psyz_pending_spu_irq > 0) {
        psyz_pending_spu_irq = 0;
        hw_irq(IRQ_SPU);
    }
}

/* ---- the I/O dispatcher ---- */
uint32_t mem_read_slow(uint32_t a, int size) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x1F801000u && p < 0x1F803000u) {
        uint32_t o = p - 0x1F801000u;
        if (o == 0x70) return i_stat;
        if (o == 0x74) return i_mask;
        if (o >= 0x80 && o < 0xF0) {
            int ch = (o - 0x80) >> 4;
            switch (o & 0xC) {
            case 0: return dma_madr[ch];
            case 4: return dma_bcr[ch];
            case 8: return dma_chcr[ch];
            default: return 0;
            }
        }
        if (o == 0xF0) return dpcr;
        if (o == 0xF4) return dicr;
        if (o >= 0x100 && o < 0x130) {
            int n = (o - 0x100) >> 4;
            switch (o & 0xC) {
            case 0: return rc_value(n);
            case 4: { uint32_t m = rc[n].mode | 0x400; rc[n].mode &= ~0x1800; return m; }
            case 8: return rc[n].target;
            default: return 0;
            }
        }
        if (o >= 0x800 && o < 0x804) {
            uint32_t v = cd_read8(o - 0x800);
            if (size > 1) v |= (uint32_t)cd_read8(o - 0x800) << 8;
            if (size > 2) {
                v |= (uint32_t)cd_read8(o - 0x800) << 16;
                v |= (uint32_t)cd_read8(o - 0x800) << 24;
            }
            return v;
        }
        if (o == 0x810) return gpu_read();
        if (o == 0x814) return gpu_stat();
        if (o == 0x820) return 0;
        if (o == 0x824) return 0x80040000u; /* MDEC: FIFO empty, idle */
        if (o >= 0xE00 && o < 0x1000) return 0;
        if (o >= 0xC00 && o < 0xE00) {
            spu_update();
            uint32_t v = spu_read(o - 0xC00);
            if (size == 4) v |= (uint32_t)spu_read(o - 0xC00 + 2) << 16;
            return v;
        }
        if (o == 0x40) return 0xFF;   /* SIO0 data: nothing connected */
        if (o == 0x44) return 0x0005; /* SIO0 stat: TX ready */
        if (o < 0x100) return io_misc[o >> 2];
        return 0;
    }
    if (p >= 0x1F000000u && p < 0x1F800000u) return 0; /* expansion 1: no dev board */
    if (p >= 0x1FC00000u && p < 0x1FC80000u) return 0; /* BIOS ROM */
    if (p == 0x1FFE0130u) return 0;                    /* cache control */
    {
        static int n;
        if (n++ < 32) RT_LOG("read%d from unmapped %08X (ra %08X)", size * 8, (unsigned)a,
                             (unsigned)g_cpu.r[31]);
    }
    return 0;
}

void mem_write_slow(uint32_t a, uint32_t v, int size) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x1F801000u && p < 0x1F803000u) {
        uint32_t o = p - 0x1F801000u;
        if (o == 0x70) { i_stat &= v; return; }
        if (o == 0x74) { i_mask = v & 0x7FF; rt_check_irq(); return; }
        if (o >= 0x80 && o < 0xF0) {
            int ch = (o - 0x80) >> 4;
            switch (o & 0xC) {
            case 0: dma_madr[ch] = v & 0xFFFFFF; break;
            case 4: dma_bcr[ch] = v; break;
            case 8:
                dma_chcr[ch] = v;
                if ((v & (1u << 24)) && (dpcr & (8u << (4 * ch)))) dma_start(ch);
                else if (v & (1u << 24)) dma_start(ch); /* tolerate a disabled channel */
                break;
            }
            return;
        }
        if (o == 0xF0) { dpcr = v; return; }
        if (o == 0xF4) {
            /* bits 24-30 are acknowledged by writing 1 */
            uint32_t flags = (dicr & 0x7F000000u) & ~(v & 0x7F000000u);
            dicr = (v & 0x00FF803Fu) | flags;
            int master = (dicr & 0x8000u) ||
                         ((dicr & 0x800000u) && ((dicr >> 24) & (dicr >> 16) & 0x7F));
            if (master) dicr |= 0x80000000u;
            return;
        }
        if (o >= 0x100 && o < 0x130) {
            int n = (o - 0x100) >> 4;
            switch (o & 0xC) {
            case 0: rc[n].base = g_now - (uint64_t)(v & 0xFFFF) * rc_div(n); break;
            case 4:
                rc[n].mode = (uint16_t)v;
                rc[n].base = g_now;
                if (v & 0x30) RT_LOG("root counter %d: interrupts wanted (mode %04X), not emulated", n, (unsigned)v);
                break;
            case 8: rc[n].target = (uint16_t)v; break;
            }
            return;
        }
        if (o >= 0x800 && o < 0x804) {
            cd_write8(o - 0x800, (uint8_t)v);
            return;
        }
        if (o == 0x810) { gpu_gp0(v); return; }
        if (o == 0x814) { gpu_gp1(v); return; }
        if (o == 0x820 || o == 0x824) return; /* MDEC */
        if (o >= 0xC00 && o < 0xE00) {
            uint32_t r = o - 0xC00;
            spu_update(); /* register writes take effect at the current time */
            Psyz_SpuWrite(r & ~1u, (uint16_t)v);
            if (size == 4) Psyz_SpuWrite((r & ~1u) + 2, (uint16_t)(v >> 16));
            return;
        }
        if (o >= 0xE00 && o < 0x1000) return;
        if (o < 0x100) { io_misc[o >> 2] = v; return; }
        return; /* SIO etc */
    }
    if (p == 0x1FFE0130u) return;
    if (p >= 0x1F000000u && p < 0x1F800000u) return;
    {
        static int n;
        if (n++ < 32) RT_LOG("write%d %08X to unmapped %08X (ra %08X)", size * 8,
                             (unsigned)v, (unsigned)a, (unsigned)g_cpu.r[31]);
    }
}

/* ---- the scheduler ---- */
void hw_reset(void) {
    i_stat = i_mask = 0;
    memset(dma_chcr, 0, sizeof dma_chcr);
    memset(rc, 0, sizeof rc);
    next_vblank = g_now + VBLANK_CYC;
    Psyz_SpuInit();
    Psyz_SpuReset(0);
    spu_frames = g_now * SPU_HZ / CPU_HZ;
    gpu_reset();
    cd_reset();
}

void hw_update(void) {
    if (g_now >= next_vblank) {
        next_vblank += VBLANK_CYC;
        if (next_vblank < g_now) next_vblank = g_now + VBLANK_CYC; /* fell behind */
        frames++;
        if (frames % 60 == 0) RT_TRACE("frame %u: in %s", (unsigned)frames, rt_where());
        hw_irq(IRQ_VBLANK);
        gpu_vblank();
        hle_vblank();
        plat_vblank();
    }
    cd_update();
    spu_update();
}
