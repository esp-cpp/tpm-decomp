/* The GTE (cop2) for the recompiled code.
 *
 * The register file is one struct in fast memory, and the register moves
 * (mfc2 / mtc2 / cfc2 / ctc2 / lwc2 / swc2) are inline functions of a
 * register number the recompiler always knows: each compiles to a load or a
 * store, plus the few registers with side effects (the screen FIFOs, IRGB /
 * ORGB, LZCS / LZCR). The commands are in rt_gte.c.
 *
 * The arithmetic is psyz's (libgte.c), result for result, on this layout;
 * RT_GTE_CHECK builds run psyz's GTE alongside and compare every register
 * after every operation. Where psyz differs from the hardware (psx-spx) this
 * follows the hardware: SXYP writes push the screen XY FIFO, IRGB / ORGB /
 * LZCS / LZCR exist, the screen offset keeps its fraction.
 */
#pragma once
#include <stdint.h>

typedef struct {
    /* data registers */
    int16_t v[3][4];       /* V0..V2: x, y, z (padded) */
    uint32_t rgbc;         /* 6 */
    uint16_t otz;          /* 7 */
    int16_t ir[4];         /* 8..11: IR0..IR3 */
    int16_t sx[4], sy[4];  /* 12..15: SXY0, SXY1, SXY2, SXYP (= SXY2) */
    uint16_t sz[4];        /* 16..19: SZ0..SZ3 */
    uint32_t rgb[3];       /* 20..22: RGB0..RGB2 */
    uint32_t res1;         /* 23 */
    int32_t mac[4];        /* 24..27: MAC0..MAC3 */
    int32_t lzcs, lzcr;    /* 30, 31 */
    /* control registers */
    int16_t rt[3][3];      /* 0..4 rotation */
    int32_t tr[3];         /* 5..7 translation */
    int16_t llm[3][3];     /* 8..12 light matrix */
    int32_t bk[3];         /* 13..15 background colour */
    int16_t lcm[3][3];     /* 16..20 light colour matrix */
    int32_t fc[3];         /* 21..23 far colour */
    int32_t ofx, ofy;      /* 24, 25: 16.16 */
    uint16_t h;            /* 26 */
    int16_t dqa;           /* 27 */
    int32_t dqb;           /* 28 */
    int16_t zsf3, zsf4;    /* 29, 30 */
    uint32_t flag;         /* 31 */
} RtGte;
extern RtGte g_gte;

void rt_gte_command(uint32_t cmd);
void rt_gte_reset(void);

/* each command on its own (the generated code's command words are constants:
 * rt_gte_cmd folds to one direct call) */
#define RT_GTE_OPS(X)                                                          \
    X(01) X(06) X(0C) X(10) X(11) X(12) X(13) X(14) X(16) X(1B) X(1C) X(1E)    \
    X(20) X(28) X(29) X(2A) X(2D) X(2E) X(30) X(3D) X(3E) X(3F)
#define RT_GTE_DECL(o) void rt_gte_op_##o(uint32_t cmd);
RT_GTE_OPS(RT_GTE_DECL)
/* and each command word the game uses on its own, its options (sf, lm, the
 * MVMVA operands) constants: TPM's, from the generated code */
#define RT_GTE_WORDS(X)                                                        \
    X(0180001) X(0280030) X(1400006) X(0486012) X(049E012) X(04C6012)          \
    X(04AA012) X(041E012) X(04BE012) X(0496012) X(048E012) X(0480012)          \
    X(043E012) X(0406012) X(04A6012) X(04DE012) X(04B6012) X(04AE012)          \
    X(04A2012) X(198003D) X(190003D) X(0A00428) X(0A80428) X(1780010)          \
    X(0780010) X(0980011) X(170000C) X(178000C) X(1A8003E) X(158002D)          \
    X(168002E) X(0F8002A) X(0680029)
#define RT_GTE_WDECL(w) void rt_gte_w_##w(void);
RT_GTE_WORDS(RT_GTE_WDECL)
static inline void rt_gte_cmd(uint32_t cmd) {
    switch (cmd & 0x1FFFFFF) {
#define RT_GTE_WCALL(w) case 0x##w: rt_gte_w_##w(); return;
        RT_GTE_WORDS(RT_GTE_WCALL)
    default: break;
    }
    switch (cmd & 0x3F) {
#define RT_GTE_CALL(o) case 0x##o: rt_gte_op_##o(cmd); break;
        RT_GTE_OPS(RT_GTE_CALL)
    default: break;
    }
}

static inline uint32_t gte_pack(int16_t lo, int16_t hi) {
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}
static inline int rt_gte_lzc(int32_t v) {
    uint32_t u = v < 0 ? ~(uint32_t)v : (uint32_t)v;
    return u ? __builtin_clz(u) : 32;
}
static inline int16_t gte_sat5(int v) { return (int16_t)(v < 0 ? 0 : v > 0x1F ? 0x1F : v); }

static inline uint32_t rt_gte_read(unsigned idx) {
    RtGte* g = &g_gte;
    switch (idx) {
    case 0: case 2: case 4: return gte_pack(g->v[idx >> 1][0], g->v[idx >> 1][1]);
    case 1: case 3: case 5: return (uint32_t)(int32_t)g->v[idx >> 1][2];
    case 6: return g->rgbc;
    case 7: return g->otz;
    case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)g->ir[idx - 8];
    case 12: case 13: case 14: return gte_pack(g->sx[idx - 12], g->sy[idx - 12]);
    case 15: return gte_pack(g->sx[2], g->sy[2]);
    case 16: case 17: case 18: case 19: return g->sz[idx - 16];
    case 20: case 21: case 22: return g->rgb[idx - 20];
    case 23: return g->res1;
    case 24: case 25: case 26: case 27: return (uint32_t)g->mac[idx - 24];
    case 28: case 29: /* IRGB / ORGB: IR1..3 as 5-bit colour */
        return (uint32_t)gte_sat5(g->ir[1] >> 7) | ((uint32_t)gte_sat5(g->ir[2] >> 7) << 5) |
               ((uint32_t)gte_sat5(g->ir[3] >> 7) << 10);
    case 30: return (uint32_t)g->lzcs;
    default: return (uint32_t)g->lzcr;
    }
}

static inline void rt_gte_write(unsigned idx, uint32_t v) {
    RtGte* g = &g_gte;
    switch (idx) {
    case 0: case 2: case 4:
        g->v[idx >> 1][0] = (int16_t)v;
        g->v[idx >> 1][1] = (int16_t)(v >> 16);
        break;
    case 1: case 3: case 5: g->v[idx >> 1][2] = (int16_t)v; break;
    case 6: g->rgbc = v; break;
    case 7: g->otz = (uint16_t)v; break;
    case 8: case 9: case 10: case 11: g->ir[idx - 8] = (int16_t)v; break;
    case 12: case 13: case 14:
        g->sx[idx - 12] = (int16_t)v;
        g->sy[idx - 12] = (int16_t)(v >> 16);
        break;
    case 15: /* SXYP: push the FIFO */
        g->sx[0] = g->sx[1]; g->sy[0] = g->sy[1];
        g->sx[1] = g->sx[2]; g->sy[1] = g->sy[2];
        g->sx[2] = (int16_t)v; g->sy[2] = (int16_t)(v >> 16);
        break;
    case 16: case 17: case 18: case 19: g->sz[idx - 16] = (uint16_t)v; break;
    case 20: case 21: case 22: g->rgb[idx - 20] = v; break;
    case 23: g->res1 = v; break;
    case 24: case 25: case 26: case 27: g->mac[idx - 24] = (int32_t)v; break;
    case 28: /* IRGB */
        g->ir[1] = (int16_t)((v & 0x1F) << 7);
        g->ir[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        g->ir[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: break; /* ORGB: read-only */
    case 30:
        g->lzcs = (int32_t)v;
        g->lzcr = rt_gte_lzc((int32_t)v);
        break;
    default: break; /* LZCR: read-only */
    }
}

static inline uint32_t rt_gte_cread(unsigned idx) {
    RtGte* g = &g_gte;
    switch (idx) {
    case 0: return gte_pack(g->rt[0][0], g->rt[0][1]);
    case 1: return gte_pack(g->rt[0][2], g->rt[1][0]);
    case 2: return gte_pack(g->rt[1][1], g->rt[1][2]);
    case 3: return gte_pack(g->rt[2][0], g->rt[2][1]);
    case 4: return (uint32_t)(int32_t)g->rt[2][2];
    case 5: case 6: case 7: return (uint32_t)g->tr[idx - 5];
    case 8: return gte_pack(g->llm[0][0], g->llm[0][1]);
    case 9: return gte_pack(g->llm[0][2], g->llm[1][0]);
    case 10: return gte_pack(g->llm[1][1], g->llm[1][2]);
    case 11: return gte_pack(g->llm[2][0], g->llm[2][1]);
    case 12: return (uint32_t)(int32_t)g->llm[2][2];
    case 13: case 14: case 15: return (uint32_t)g->bk[idx - 13];
    case 16: return gte_pack(g->lcm[0][0], g->lcm[0][1]);
    case 17: return gte_pack(g->lcm[0][2], g->lcm[1][0]);
    case 18: return gte_pack(g->lcm[1][1], g->lcm[1][2]);
    case 19: return gte_pack(g->lcm[2][0], g->lcm[2][1]);
    case 20: return (uint32_t)(int32_t)g->lcm[2][2];
    case 21: case 22: case 23: return (uint32_t)g->fc[idx - 21];
    case 24: return (uint32_t)g->ofx;
    case 25: return (uint32_t)g->ofy;
    case 26: return g->h;
    case 27: return (uint32_t)(int32_t)g->dqa;
    case 28: return (uint32_t)g->dqb;
    case 29: return (uint32_t)(int32_t)g->zsf3;
    case 30: return (uint32_t)(int32_t)g->zsf4;
    default: return g->flag;
    }
}

static inline void rt_gte_cwrite(unsigned idx, uint32_t v) {
    RtGte* g = &g_gte;
    int16_t lo = (int16_t)v, hi = (int16_t)(v >> 16);
    switch (idx) {
    case 0: g->rt[0][0] = lo; g->rt[0][1] = hi; break;
    case 1: g->rt[0][2] = lo; g->rt[1][0] = hi; break;
    case 2: g->rt[1][1] = lo; g->rt[1][2] = hi; break;
    case 3: g->rt[2][0] = lo; g->rt[2][1] = hi; break;
    case 4: g->rt[2][2] = lo; break;
    case 5: case 6: case 7: g->tr[idx - 5] = (int32_t)v; break;
    case 8: g->llm[0][0] = lo; g->llm[0][1] = hi; break;
    case 9: g->llm[0][2] = lo; g->llm[1][0] = hi; break;
    case 10: g->llm[1][1] = lo; g->llm[1][2] = hi; break;
    case 11: g->llm[2][0] = lo; g->llm[2][1] = hi; break;
    case 12: g->llm[2][2] = lo; break;
    case 13: case 14: case 15: g->bk[idx - 13] = (int32_t)v; break;
    case 16: g->lcm[0][0] = lo; g->lcm[0][1] = hi; break;
    case 17: g->lcm[0][2] = lo; g->lcm[1][0] = hi; break;
    case 18: g->lcm[1][1] = lo; g->lcm[1][2] = hi; break;
    case 19: g->lcm[2][0] = lo; g->lcm[2][1] = hi; break;
    case 20: g->lcm[2][2] = lo; break;
    case 21: case 22: case 23: g->fc[idx - 21] = (int32_t)v; break;
    case 24: g->ofx = (int32_t)v; break;
    case 25: g->ofy = (int32_t)v; break;
    case 26: g->h = (uint16_t)v; break;
    case 27: g->dqa = lo; break;
    case 28: g->dqb = (int32_t)v; break;
    case 29: g->zsf3 = lo; break;
    case 30: g->zsf4 = lo; break;
    default: g->flag = v; break;
    }
}
