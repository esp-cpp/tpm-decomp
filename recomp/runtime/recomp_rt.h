/* Runtime for the statically recompiled SLUS_008.84 (tools/recomp.py).
 *
 * The generated code sees the PlayStation through this header: a register
 * file (g_cpu), memory accessors, the GTE (psyz's software GTE), and a few
 * hooks for what has no plain C equivalent (calls through pointers, traps,
 * interrupts). Everything behind the hooks -- BIOS, interrupt controller,
 * DMA, GPU, CD-ROM, timers -- is in the rt_*.c files; the board or desktop
 * shell around it implements rt_platform.h.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t r[32];
    uint32_t hi, lo;
    int32_t budget; /* back-edges until the next rt_poll() */
} Cpu;
extern Cpu g_cpu;

/* Guest RAM: 2 MB, with the 1 KB scratchpad right after it. */
#define RAM_SIZE 0x200000u
#define SCRATCH_OFF RAM_SIZE
#define RAM_ALLOC (RAM_SIZE + 0x400u)
extern uint8_t* g_ram;

uint32_t mem_read_slow(uint32_t a, int size);
void mem_write_slow(uint32_t a, uint32_t v, int size);

#if defined(__GNUC__)
#define RT_LIKELY(x) __builtin_expect(!!(x), 1)
/* (always_inline measured no faster on the board and doubled the code) */
#define RT_INLINE static inline
#else
#define RT_LIKELY(x) (x)
#define RT_INLINE static inline
#endif
/* Guest loads and stores are aligned (the R3000 traps otherwise; LWL/LWR
 * work byte-wise), so they are plain typed accesses: through memcpy an RV32
 * compiler cannot assume alignment and calls the library. */
#define RAM32(o) (*(uint32_t*)(g_ram + (o)))
#define RAM16(o) (*(uint16_t*)(g_ram + (o)))

/* Where a guest address lands in g_ram, or -1 for I/O / unmapped. KUSEG,
 * KSEG0 and KSEG1 all see the same 2 MB (mirrored over 8 MB). */
RT_INLINE int32_t ram_off(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (RT_LIKELY(p < 0x00800000u)) return (int32_t)(p & (RAM_SIZE - 1));
    if ((p & 0xFFFFFC00u) == 0x1F800000u) return (int32_t)(SCRATCH_OFF + (p & 0x3FFu));
    return -1;
}

RT_INLINE uint32_t LW(uint32_t a) {
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) return RAM32(o);
    return mem_read_slow(a, 4);
}
RT_INLINE uint32_t LHU(uint32_t a) {
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) return RAM16(o);
    return mem_read_slow(a, 2) & 0xFFFFu;
}
RT_INLINE uint32_t LH(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LHU(a); }
RT_INLINE uint32_t LBU(uint32_t a) {
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) return g_ram[o];
    return mem_read_slow(a, 1) & 0xFFu;
}
RT_INLINE uint32_t LB(uint32_t a) { return (uint32_t)(int32_t)(int8_t)LBU(a); }

#ifdef RT_DEBUG
extern uint32_t g_watch; /* RT_WATCH=addr: log stores to that word */
void rt_watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(a, v, n) do { if (((a) & 0x1FFFFC) == g_watch) rt_watch_hit(a, v, n); } while (0)
#else
#define WATCH(a, v, n) do { } while (0)
#endif

RT_INLINE void SW(uint32_t a, uint32_t v) {
    WATCH(a, v, 4);
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) { RAM32(o) = v; return; }
    mem_write_slow(a, v, 4);
}
RT_INLINE void SH(uint32_t a, uint32_t v) {
    WATCH(a, v, 2);
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) { RAM16(o) = (uint16_t)v; return; }
    mem_write_slow(a, v & 0xFFFFu, 2);
}
RT_INLINE void SB(uint32_t a, uint32_t v) {
    WATCH(a, v, 1);
    int32_t o = ram_off(a);
    if (RT_LIKELY(o >= 0)) { g_ram[o] = (uint8_t)v; return; }
    mem_write_slow(a, v & 0xFFu, 1);
}

/* unaligned word access, little-endian R3000 semantics */
RT_INLINE uint32_t LWL(uint32_t a, uint32_t rt) {
    uint32_t w = LW(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFFu) | (w << 24);
    case 1: return (rt & 0x0000FFFFu) | (w << 16);
    case 2: return (rt & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
RT_INLINE uint32_t LWR(uint32_t a, uint32_t rt) {
    uint32_t w = LW(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000u) | (w >> 8);
    case 2: return (rt & 0xFFFF0000u) | (w >> 16);
    default: return (rt & 0xFFFFFF00u) | (w >> 24);
    }
}
RT_INLINE void SWL(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = LW(al);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    SW(al, w);
}
RT_INLINE void SWR(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = LW(al);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FFu) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;
    default: w = (w & 0x00FFFFFFu) | (rt << 24); break;
    }
    SW(al, w);
}

/* R3000 division, including its answers for x/0 and INT_MIN/-1 */
RT_INLINE void DIV(uint32_t rs, uint32_t rt, uint32_t* hi, uint32_t* lo) {
    int32_t n = (int32_t)rs, d = (int32_t)rt;
    if (d == 0) { *hi = rs; *lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (n == INT32_MIN && d == -1) { *hi = 0; *lo = 0x80000000u; }
    else { *hi = (uint32_t)(n % d); *lo = (uint32_t)(n / d); }
}
RT_INLINE void DIVU(uint32_t rs, uint32_t rt, uint32_t* hi, uint32_t* lo) {
    if (rt == 0) { *hi = rs; *lo = 0xFFFFFFFFu; }
    else { *hi = rs % rt; *lo = rs / rt; }
}

/* The GTE (rt_gte.h): register moves inline, commands in rt_gte.c.
 * RT_GTE_CHECK builds run psyz's GTE in lockstep and report differences. */
#include "rt_gte.h"
#ifdef RT_GTE_CHECK
uint32_t rt_gtec_dr(unsigned idx);
void rt_gtec_dw(unsigned idx, uint32_t v);
uint32_t rt_gtec_cr(unsigned idx);
void rt_gtec_cw(unsigned idx, uint32_t v);
void rt_gtec_cmd(uint32_t cmd);
#define GTE_DR(n) rt_gtec_dr(n)
#define GTE_DW(n, v) rt_gtec_dw(n, v)
#define GTE_CR(n) rt_gtec_cr(n)
#define GTE_CW(n, v) rt_gtec_cw(n, v)
#define GTE_CMD(c) rt_gtec_cmd(c)
#else
#define GTE_DR(n) rt_gte_read(n)
#define GTE_DW(n, v) rt_gte_write(n, v)
#define GTE_CR(n) rt_gte_cread(n)
#define GTE_CW(n, v) rt_gte_cwrite(n, v)
#define GTE_CMD(c) rt_gte_command(c)
#endif

/* RT_DEBUG builds keep a shadow call stack of guest functions, for traces
 * and post-mortems. */
#ifdef RT_DEBUG
extern uint32_t g_shadow[256];
extern int g_shadow_n, g_shadow_max;
RT_INLINE void rt_leave_(int* d) { g_shadow_n = *d; }
#define RT_ENTER(a)                                                         \
    __attribute__((cleanup(rt_leave_))) int shadow_d_ = g_shadow_n;         \
    if (g_shadow_n < 256) g_shadow[g_shadow_n] = (a);                       \
    if (++g_shadow_n > g_shadow_max) g_shadow_max = g_shadow_n
#else
#define RT_ENTER(a) do { } while (0)
#endif

/* hooks the generated code calls */
void rt_dispatch(uint32_t target);           /* call through a pointer */
void rt_jump(uint32_t target, uint32_t pc);  /* jr to somewhere unknown */
void rt_jump_ra(uint32_t target, uint32_t pc);  /* jr $ra that does not return */
void rt_jump_tbl(uint32_t target, uint32_t pc); /* jump table leaving its function */
void rt_dispatch_ind(uint32_t target);          /* jalr: a call through a pointer */
void rt_clobber_check(const Cpu* before, unsigned long long keep, uint32_t pc); /* --check */
void rt_jump_ind(uint32_t target, uint32_t pc); /* jr: a tail call through a pointer */
void rt_fallthrough(uint32_t target);
void rt_syscall(uint32_t pc);
void rt_break(uint32_t pc, uint32_t insn);
void rt_invalid(uint32_t pc, uint32_t insn);
uint32_t rt_mfc0(int reg);
void rt_mtc0(int reg, uint32_t v);
void rt_rfe(void);
void rt_poll(void);

typedef struct {
    uint32_t addr;
    void (*fn)(void);
} RecompFunc;
extern const RecompFunc g_recomp_funcs[];
extern const unsigned g_recomp_nfuncs;
extern const char* const g_recomp_names[][2];
extern const uint32_t g_recomp_indirect[]; /* sorted; empty without the analysis */
extern const unsigned g_recomp_nindirect;
extern const unsigned g_recomp_nnames;

#ifdef __cplusplus
}
#endif
