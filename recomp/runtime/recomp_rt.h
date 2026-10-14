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

/* Guest RAM: 2 MB. The 1 KB scratchpad is its own array at a fixed
 * address (a constant base for the generated code, no pointer to reload). */
#define RAM_SIZE 0x200000u
#define RAM_ALLOC RAM_SIZE
#define SCRATCH_SIZE 0x400u
/* the generated code sees the pointer as const (it is set before any of
 * that runs): GCC need not reload it after every store */
#ifdef RT_RAM_OWNER
extern uint8_t* g_ram;
#else
extern uint8_t* const g_ram;
#endif
extern uint8_t g_scratch[SCRATCH_SIZE];

uint32_t mem_read_slow(uint32_t a, int size);
void mem_write_slow(uint32_t a, uint32_t v, int size);

#if defined(__GNUC__)
#define RT_LIKELY(x) __builtin_expect(!!(x), 1)
/* (always_inline on everything measured no faster on the board and doubled
 * the code, back when every call also moved all 32 registers) */
#define RT_INLINE static inline
/* the plain loads and stores: GCC left them as calls in the large generated
 * functions, and a call there spills the caller-saved registers that hold
 * guest registers */
#define RT_ACCESS static inline __attribute__((always_inline))
#else
#define RT_LIKELY(x) (x)
#define RT_INLINE static inline
#define RT_ACCESS static inline
#endif
/* Guest loads and stores are aligned (the R3000 traps otherwise; LWL/LWR
 * work byte-wise), so they are plain typed accesses: through memcpy an RV32
 * compiler cannot assume alignment and calls the library. */
#define RAM32(o) (*(uint32_t*)(g_ram + (o)))
#define RAM16(o) (*(uint16_t*)(g_ram + (o)))
#define SCR32(o) (*(uint32_t*)(g_scratch + (o)))
#define SCR16(o) (*(uint16_t*)(g_scratch + (o)))
#define RAM8(o) (g_ram[o])
#define SCR8(o) (g_scratch[o])

/* Where a guest address lands in g_ram, or -1 for the rest. KUSEG, KSEG0
 * and KSEG1 all see the same 2 MB (mirrored over 8 MB). */
RT_ACCESS int32_t ram_off(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (RT_LIKELY(p < 0x00800000u)) return (int32_t)(p & (RAM_SIZE - 1));
    return -1;
}
/* The scratchpad offset of a guest address, or >= SCRATCH_SIZE. The
 * generated code addresses it as 0x1F800000 (a site that uses a mirror
 * takes the slow path's own check). */
RT_ACCESS uint32_t scr_off(uint32_t a) { return a - 0x1F800000u; }
/* the rest: the scratchpad's mirrors and I/O */
RT_ACCESS int32_t scr_off_any(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if ((p & 0xFFFFFC00u) == 0x1F800000u) return (int32_t)(p & 0x3FFu);
    return -1;
}

#ifdef RT_DEBUG
extern uint32_t g_watch; /* RT_WATCH=addr: log stores to that word */
void rt_watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(a, v, n) do { if (((a) & 0x1FFFFC) == g_watch) rt_watch_hit(a, v, n); } while (0)
#else
#define WATCH(a, v, n) do { } while (0)
#endif

/* Each access comes in two orders: RAM first (LW, ...), and scratchpad
 * first (LWs, ...) for the sites the recompiler was told go there
 * (--scratch-sites: measured on the desktop, see tools/sites.py). Both
 * are correct for any address; the order only decides which is cheap.
 * RT_SITES builds record what each site touched instead. */
#ifdef RT_SITES
void rt_site(uint32_t pc, uint32_t a);
#define SITE(pc, a) rt_site(pc, a)
#else
#define SITE(pc, a) ((void)0)
#endif

#define RT_LOADS(X) X(W, 4, uint32_t, RAM32, SCR32, ) X(HU, 2, uint32_t, RAM16, SCR16, ) \
    X(BU, 1, uint32_t, RAM8, SCR8, )
#define RT_LOAD_FNS(S, N, T, RAMACC, SCRACC, _)                                   \
    RT_ACCESS uint32_t L##S(uint32_t a) {                                         \
        int32_t o = ram_off(a);                                                   \
        if (RT_LIKELY(o >= 0)) return RAMACC(o);                                  \
        o = scr_off_any(a);                                                       \
        if (o >= 0) return SCRACC(o);                                             \
        return mem_read_slow(a, N) & (N == 4 ? 0xFFFFFFFFu : (1u << (N * 8)) - 1); \
    }                                                                             \
    RT_ACCESS uint32_t L##S##s(uint32_t a) {                                      \
        uint32_t s = scr_off(a);                                                  \
        if (RT_LIKELY(s < SCRATCH_SIZE)) return SCRACC(s);                        \
        return L##S(a);                                                           \
    }
RT_LOADS(RT_LOAD_FNS)
RT_ACCESS uint32_t LH(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LHU(a); }
RT_ACCESS uint32_t LB(uint32_t a) { return (uint32_t)(int32_t)(int8_t)LBU(a); }
RT_ACCESS uint32_t LHs(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LHUs(a); }
RT_ACCESS uint32_t LBs(uint32_t a) { return (uint32_t)(int32_t)(int8_t)LBUs(a); }

#define RT_STORES(X) X(W, 4, uint32_t, RAM32, SCR32) X(H, 2, uint16_t, RAM16, SCR16) \
    X(B, 1, uint8_t, RAM8, SCR8)
#define RT_STORE_FNS(SZ, N, T, RAMACC, SCRACC)                                     \
    RT_ACCESS void S##SZ(uint32_t a, uint32_t v) {                                 \
        WATCH(a, v, N);                                                           \
        int32_t o = ram_off(a);                                                   \
        if (RT_LIKELY(o >= 0)) { RAMACC(o) = (T)v; return; }                      \
        o = scr_off_any(a);                                                       \
        if (o >= 0) { SCRACC(o) = (T)v; return; }                                 \
        mem_write_slow(a, N == 4 ? v : v & ((1u << (N * 8)) - 1), N);             \
    }                                                                             \
    RT_ACCESS void S##SZ##s(uint32_t a, uint32_t v) {                              \
        uint32_t s = scr_off(a);                                                  \
        if (RT_LIKELY(s < SCRATCH_SIZE)) { SCRACC(s) = (T)v; return; }            \
        S##SZ(a, v);                                                               \
    }
RT_STORES(RT_STORE_FNS)

/* And the sites the recompiler proved always reach the scratchpad (its
 * value flow: a base register that holds 0x1F800000 plus a bounded offset):
 * no check at all. RT_DEBUG builds check the proof. */
#ifdef RT_DEBUG
void rt_known_miss(uint32_t a);
#define SCR_KNOWN(a, n) (((a) - 0x1F800000u <= SCRATCH_SIZE - (n)) ? (void)0 : rt_known_miss(a), \
                         (a) - 0x1F800000u)
#else
#define SCR_KNOWN(a, n) ((a) - 0x1F800000u)
#endif
RT_ACCESS uint32_t LWk(uint32_t a) { return SCR32(SCR_KNOWN(a, 4)); }
RT_ACCESS uint32_t LHUk(uint32_t a) { return SCR16(SCR_KNOWN(a, 2)); }
RT_ACCESS uint32_t LBUk(uint32_t a) { return SCR8(SCR_KNOWN(a, 1)); }
RT_ACCESS uint32_t LHk(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LHUk(a); }
RT_ACCESS uint32_t LBk(uint32_t a) { return (uint32_t)(int32_t)(int8_t)LBUk(a); }
RT_ACCESS void SWk(uint32_t a, uint32_t v) { SCR32(SCR_KNOWN(a, 4)) = v; }
RT_ACCESS void SHk(uint32_t a, uint32_t v) { SCR16(SCR_KNOWN(a, 2)) = (uint16_t)v; }
RT_ACCESS void SBk(uint32_t a, uint32_t v) { SCR8(SCR_KNOWN(a, 1)) = (uint8_t)v; }

/* unaligned word access, little-endian R3000 semantics */
#define RT_UNALIGNED(SFX)                                                         \
    RT_INLINE uint32_t LWL##SFX(uint32_t a, uint32_t rt) {                        \
        uint32_t w = LW##SFX(a & ~3u);                                            \
        switch (a & 3) {                                                          \
        case 0: return (rt & 0x00FFFFFFu) | (w << 24);                            \
        case 1: return (rt & 0x0000FFFFu) | (w << 16);                            \
        case 2: return (rt & 0x000000FFu) | (w << 8);                             \
        default: return w;                                                        \
        }                                                                         \
    }                                                                             \
    RT_INLINE uint32_t LWR##SFX(uint32_t a, uint32_t rt) {                        \
        uint32_t w = LW##SFX(a & ~3u);                                            \
        switch (a & 3) {                                                          \
        case 0: return w;                                                         \
        case 1: return (rt & 0xFF000000u) | (w >> 8);                             \
        case 2: return (rt & 0xFFFF0000u) | (w >> 16);                            \
        default: return (rt & 0xFFFFFF00u) | (w >> 24);                           \
        }                                                                         \
    }                                                                             \
    RT_INLINE void SWL##SFX(uint32_t a, uint32_t rt) {                            \
        uint32_t al = a & ~3u, w = LW##SFX(al);                                   \
        switch (a & 3) {                                                          \
        case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;                        \
        case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;                        \
        case 2: w = (w & 0xFF000000u) | (rt >> 8); break;                         \
        default: w = rt; break;                                                   \
        }                                                                         \
        SW##SFX(al, w);                                                           \
    }                                                                             \
    RT_INLINE void SWR##SFX(uint32_t a, uint32_t rt) {                            \
        uint32_t al = a & ~3u, w = LW##SFX(al);                                   \
        switch (a & 3) {                                                          \
        case 0: w = rt; break;                                                    \
        case 1: w = (w & 0x000000FFu) | (rt << 8); break;                         \
        case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;                        \
        default: w = (w & 0x00FFFFFFu) | (rt << 24); break;                       \
        }                                                                         \
        SW##SFX(al, w);                                                           \
    }
RT_UNALIGNED()
RT_UNALIGNED(s)

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
#define GTE_CMD(c) rt_gte_cmd(c)
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
void rt_entry_outside(uint32_t entry) __attribute__((noreturn)); /* recomp.py: only local calls enter there */
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
