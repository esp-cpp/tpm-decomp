/* The GTE's commands on rt_gte.h's register file. The arithmetic is psyz's
 * (libgte.c), result for result: RT_GTE_CHECK builds compare the two. */
#include <string.h>

#include "rt_gte.h"

/* (internal RAM measured slower than cached PSRAM for the P4's cores: these
 * stay in the bundle's .bss, which goes to PSRAM) */
#define RT_HOT
RT_HOT RtGte g_gte;

#define FLAG_MAC1_OVF_POS (1u << 30)
#define FLAG_MAC2_OVF_POS (1u << 29)
#define FLAG_MAC3_OVF_POS (1u << 28)
#define FLAG_MAC1_OVF_NEG (1u << 27)
#define FLAG_MAC2_OVF_NEG (1u << 26)
#define FLAG_MAC3_OVF_NEG (1u << 25)
#define FLAG_IR1_SAT (1u << 24)
#define FLAG_IR2_SAT (1u << 23)
#define FLAG_IR3_SAT (1u << 22)
#define FLAG_COL_R_SAT (1u << 21)
#define FLAG_COL_G_SAT (1u << 20)
#define FLAG_COL_B_SAT (1u << 19)
#define FLAG_SZ3_OTZ_SAT (1u << 18)
#define FLAG_DIV_OVF (1u << 17)
#define FLAG_MAC0_OVF_POS (1u << 16)
#define FLAG_MAC0_OVF_NEG (1u << 15)
#define FLAG_SX2_SAT (1u << 14)
#define FLAG_SY2_SAT (1u << 13)
#define FLAG_IR0_SAT (1u << 12)
#define FLAG_ERROR_MASK 0x7F87E000u
#define FLAG_ERROR (1u << 31)

typedef int64_t s64;
#define G (&g_gte)

void rt_gte_reset(void) {
    __builtin_memset(&g_gte, 0, sizeof g_gte);
}

/* the flag word is built in a local and stored once per command */
#define LIKELY(x) __builtin_expect(!!(x), 1)
/* (the fast paths are inlined into each command word, the overflow paths
 * are not) */
#define GTE_FAST static inline __attribute__((always_inline))
#define GTE_SLOW static __attribute__((noinline))

static const uint8_t unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA,
    0xE8, 0xE6, 0xE4, 0xE3, 0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5,
    0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, 0xC6, 0xC5, 0xC3, 0xC1,
    0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F,
    0x9E, 0x9C, 0x9B, 0x9A, 0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90,
    0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86, 0x85, 0x84, 0x83, 0x82,
    0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68,
    0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D,
    0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x53, 0x52,
    0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E,
    0x3D, 0x3C, 0x3C, 0x3B, 0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35,
    0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, 0x2E, 0x2E, 0x2D, 0x2C,
    0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D,
    0x1C, 0x1B, 0x1B, 0x1A, 0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15,
    0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, 0x10, 0x0F, 0x0F, 0x0E,
    0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02,
    0x01, 0x01, 0x00, 0x00, 0x00};


/* the leading zeros of a 16-bit value (1..0xFFFF) as a 16-bit number. An
 * RV32 core without Zbb (the P4) calls the library for __builtin_clz; the
 * exponent of the value as a float is the same answer in three instructions */
GTE_FAST uint32_t clz16(uint32_t v) {
#if defined(__riscv) && !defined(__riscv_zbb) && defined(__riscv_flen)
    const float f = (float)v;
    uint32_t b;
    memcpy(&b, &f, 4);
    return 142u - (b >> 23);
#else
    return (uint32_t)__builtin_clz(v) - 16u;
#endif
}

GTE_FAST uint32_t gte_divide(uint32_t h, uint32_t sz3, uint32_t* flag) {
    if (h >= sz3 * 2) {
        *flag |= FLAG_DIV_OVF;
        return 0x1FFFF;
    }
    uint32_t z = clz16(sz3);
    uint32_t n = h << z;
    uint32_t d = sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    uint64_t r = ((uint64_t)n * d + 0x8000ull) >> 16;
    return r > 0x1FFFFu ? 0x1FFFFu : (uint32_t)r;
}

GTE_FAST s64 mac44(s64 v, int i, uint32_t* flag) {
    if (__builtin_expect(v > 0x7FFFFFFFFFFLL, 0)) *flag |= FLAG_MAC1_OVF_POS >> i;
    if (__builtin_expect(v < -0x80000000000LL, 0)) *flag |= FLAG_MAC1_OVF_NEG >> i;
    return (s64)((uint64_t)v << 20) >> 20;
}
GTE_FAST int32_t mac0_check(s64 v, uint32_t* flag) {
    if (v > 0x7FFFFFFFLL) *flag |= FLAG_MAC0_OVF_POS;
    if (v < -0x80000000LL) *flag |= FLAG_MAC0_OVF_NEG;
    return (int32_t)v;
}
GTE_FAST int16_t ir_sat(int32_t v, int lm, uint32_t satflag, uint32_t* flag) {
    const int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { *flag |= satflag; return (int16_t)lo; }
    if (v > 0x7FFF) { *flag |= satflag; return 0x7FFF; }
    return (int16_t)v;
}
GTE_FAST int16_t sat_s16(int32_t v) {
    return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}
GTE_FAST void set_ir123(int lm, uint32_t* flag) {
    G->ir[1] = ir_sat(G->mac[1], lm, FLAG_IR1_SAT, flag);
    G->ir[2] = ir_sat(G->mac[2], lm, FLAG_IR2_SAT, flag);
    G->ir[3] = ir_sat(G->mac[3], lm, FLAG_IR3_SAT, flag);
}

/* ---- 32-bit fast paths ----
 * RV32 has no 64-bit arithmetic: a 44-bit MAC sum is a chain of carries and
 * its overflow checks are 64-bit compares. Instead, (T << 12) + the three
 * products is carried as q = T + (products >> 12) and the low 12 bits, all
 * in 32 bits: the sum is within the 44-bit range exactly when q fits in 32
 * bits, and then needs no wrap. When it does not, the 64-bit code runs. */
typedef struct {
    int32_t q;   /* sum >> 12 */
    uint32_t lo; /* sum & 0xFFF */
} Mac32;
GTE_FAST int mac32(int32_t t, int32_t p0, int32_t p1, int32_t p2, Mac32* r) {
    const uint32_t lo = (uint32_t)(p0 & 0xFFF) + (uint32_t)(p1 & 0xFFF) + (uint32_t)(p2 & 0xFFF);
    const int32_t hi = (p0 >> 12) + (p1 >> 12) + (p2 >> 12) + (int32_t)(lo >> 12);
    r->lo = lo & 0xFFF;
    return !__builtin_add_overflow(t, hi, &r->q);
}
/* the MAC register: sum >> 12 (sf) or the sum's low 32 bits */
GTE_FAST int32_t mac32_reg(const Mac32* m, int sf) {
    return sf ? m->q : (int32_t)(((uint32_t)m->q << 12) | m->lo);
}

/* ---- RTPS / RTPT ---- */
GTE_SLOW void rtps_slow(int k, int sf, int lm, int depth_cue, uint32_t* flag);
GTE_FAST void rtps(int k, int sf, int lm, int depth_cue, uint32_t* flag) {
    RtGte* g = G;
    const int vx = g->v[k][0], vy = g->v[k][1], vz = g->v[k][2];
    Mac32 a, b, c;
    if (__builtin_expect(!mac32(g->tr[0], g->rt[0][0] * vx, g->rt[0][1] * vy, g->rt[0][2] * vz, &a) ||
                         !mac32(g->tr[1], g->rt[1][0] * vx, g->rt[1][1] * vy, g->rt[1][2] * vz, &b) ||
                         !mac32(g->tr[2], g->rt[2][0] * vx, g->rt[2][1] * vy, g->rt[2][2] * vz, &c),
                         0)) {
        rtps_slow(k, sf, lm, depth_cue, flag);
        return;
    }
    const int32_t mac1 = mac32_reg(&a, sf), mac2 = mac32_reg(&b, sf), mac3 = mac32_reg(&c, sf);
    g->mac[1] = mac1;
    g->mac[2] = mac2;
    g->mac[3] = mac3;
    const int16_t ir1 = ir_sat(mac1, lm, FLAG_IR1_SAT, flag);
    const int16_t ir2 = ir_sat(mac2, lm, FLAG_IR2_SAT, flag);
    g->ir[1] = ir1;
    g->ir[2] = ir2;
    const int32_t z12 = c.q;
    {
        int32_t v = mac3;
        if (z12 < -0x8000 || z12 > 0x7FFF) *flag |= FLAG_IR3_SAT;
        const int32_t lo = lm ? 0 : -0x8000;
        if (v < lo) v = lo;
        if (v > 0x7FFF) v = 0x7FFF;
        g->ir[3] = (int16_t)v;
    }
    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];
    int32_t szv = z12;
    if (szv < 0 || szv > 0xFFFF) {
        *flag |= FLAG_SZ3_OTZ_SAT;
        szv = szv < 0 ? 0 : 0xFFFF;
    }
    g->sz[3] = (uint16_t)szv;
    const int32_t div = (int32_t)gte_divide(g->h, (uint32_t)szv, flag);
    g->sx[0] = g->sx[1]; g->sy[0] = g->sy[1];
    g->sx[1] = g->sx[2]; g->sy[1] = g->sy[2];
    /* MAC0 = div * IR + offset: in 32 bits while div < 0x10000 (|product|
     * < 2^31) and the sum does not overflow (which would flag MAC0) */
    int32_t m0x, m0y;
    if (__builtin_expect(div > 0xFFFF || __builtin_add_overflow(div * ir1, g->ofx, &m0x) ||
                             __builtin_add_overflow(div * ir2, g->ofy, &m0y), 0)) {
        s64 mac0 = (s64)div * ir1 + (s64)g->ofx;
        g->mac[0] = mac0_check(mac0, flag);
        m0x = (int32_t)(mac0 >> 16);
        mac0 = (s64)div * ir2 + (s64)g->ofy;
        g->mac[0] = mac0_check(mac0, flag);
        m0y = (int32_t)(mac0 >> 16);
    } else {
        g->mac[0] = m0y;
        m0x >>= 16;
        m0y >>= 16;
    }
    if (m0x < -0x400 || m0x > 0x3FF) {
        *flag |= FLAG_SX2_SAT;
        m0x = m0x < -0x400 ? -0x400 : 0x3FF;
    }
    if (m0y < -0x400 || m0y > 0x3FF) {
        *flag |= FLAG_SY2_SAT;
        m0y = m0y < -0x400 ? -0x400 : 0x3FF;
    }
    g->sx[2] = g->sx[3] = (int16_t)m0x;
    g->sy[2] = g->sy[3] = (int16_t)m0y;
    if (depth_cue) {
        int32_t m0;
        if (__builtin_expect(div > 0xFFFF || __builtin_add_overflow(div * g->dqa, g->dqb, &m0), 0)) {
            const s64 m = (s64)div * g->dqa + (s64)g->dqb;
            g->mac[0] = mac0_check(m, flag);
            int32_t ir0 = (int32_t)(m >> 12);
            if (ir0 < 0 || ir0 > 0x1000) {
                *flag |= FLAG_IR0_SAT;
                ir0 = ir0 < 0 ? 0 : 0x1000;
            }
            g->ir[0] = (int16_t)ir0;
        } else {
            g->mac[0] = m0;
            int32_t ir0 = m0 >> 12;
            if (ir0 < 0 || ir0 > 0x1000) {
                *flag |= FLAG_IR0_SAT;
                ir0 = ir0 < 0 ? 0 : 0x1000;
            }
            g->ir[0] = (int16_t)ir0;
        }
    }
}

GTE_SLOW void rtps_slow(int k, int sf, int lm, int depth_cue, uint32_t* flag) {
    RtGte* g = G;
    const int vx = g->v[k][0], vy = g->v[k][1], vz = g->v[k][2];
    /* each product fits in 32 bits; the sums with TR << 12 need 44 */
    s64 m1 = ((s64)g->tr[0] << 12) + (s64)(g->rt[0][0] * vx) + (s64)(g->rt[0][1] * vy) + (s64)(g->rt[0][2] * vz);
    s64 m2 = ((s64)g->tr[1] << 12) + (s64)(g->rt[1][0] * vx) + (s64)(g->rt[1][1] * vy) + (s64)(g->rt[1][2] * vz);
    s64 m3 = ((s64)g->tr[2] << 12) + (s64)(g->rt[2][0] * vx) + (s64)(g->rt[2][1] * vy) + (s64)(g->rt[2][2] * vz);
    m1 = mac44(m1, 0, flag);
    m2 = mac44(m2, 1, flag);
    m3 = mac44(m3, 2, flag);
    const int shift = sf ? 12 : 0;
    g->mac[1] = (int32_t)(m1 >> shift);
    g->mac[2] = (int32_t)(m2 >> shift);
    g->mac[3] = (int32_t)(m3 >> shift);
    g->ir[1] = ir_sat(g->mac[1], lm, FLAG_IR1_SAT, flag);
    g->ir[2] = ir_sat(g->mac[2], lm, FLAG_IR2_SAT, flag);
    const int32_t z12 = (int32_t)(m3 >> 12);
    {
        /* IR3: clamped by lm, flagged on MAC3 >> 12 against the s16 range */
        int32_t v = g->mac[3];
        if (z12 < -0x8000 || z12 > 0x7FFF) *flag |= FLAG_IR3_SAT;
        const int32_t lo = lm ? 0 : -0x8000;
        if (v < lo) v = lo;
        if (v > 0x7FFF) v = 0x7FFF;
        g->ir[3] = (int16_t)v;
    }
    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];
    int32_t szv = z12;
    if (szv < 0 || szv > 0xFFFF) {
        *flag |= FLAG_SZ3_OTZ_SAT;
        szv = szv < 0 ? 0 : 0xFFFF;
    }
    g->sz[3] = (uint16_t)szv;
    const int32_t div = (int32_t)gte_divide(g->h, g->sz[3], flag);
    g->sx[0] = g->sx[1]; g->sy[0] = g->sy[1];
    g->sx[1] = g->sx[2]; g->sy[1] = g->sy[2];
    s64 mac0 = (s64)div * g->ir[1] + (s64)g->ofx;
    g->mac[0] = mac0_check(mac0, flag);
    s64 sxf = mac0 >> 16;
    if (sxf < -0x400 || sxf > 0x3FF) {
        *flag |= FLAG_SX2_SAT;
        sxf = sxf < -0x400 ? -0x400 : 0x3FF;
    }
    g->sx[2] = (int16_t)sxf;
    mac0 = (s64)div * g->ir[2] + (s64)g->ofy;
    g->mac[0] = mac0_check(mac0, flag);
    s64 syf = mac0 >> 16;
    if (syf < -0x400 || syf > 0x3FF) {
        *flag |= FLAG_SY2_SAT;
        syf = syf < -0x400 ? -0x400 : 0x3FF;
    }
    g->sy[2] = (int16_t)syf;
    g->sx[3] = g->sx[2];
    g->sy[3] = g->sy[2];
    if (depth_cue) {
        s64 m0 = (s64)div * g->dqa + (s64)g->dqb;
        g->mac[0] = mac0_check(m0, flag);
        int32_t ir0 = (int32_t)(m0 >> 12);
        if (ir0 < 0 || ir0 > 0x1000) {
            *flag |= FLAG_IR0_SAT;
            ir0 = ir0 < 0 ? 0 : 0x1000;
        }
        g->ir[0] = (int16_t)ir0;
    }
}

/* ---- matrix * vector (MVMVA and the lighting commands) ---- */
/* the operands of matrix * vector */
GTE_FAST const int16_t (*mv_matrix(int mx, int16_t m[3][3]))[3] {
    RtGte* g = G;
    if (mx == 0) return g->rt;
    if (mx == 1) return g->llm;
    if (mx == 2) return g->lcm;
    int16_t r = (int16_t)((g->rgbc & 0xFF) << 4);
    m[0][0] = (int16_t)-r; m[0][1] = r; m[0][2] = g->ir[0];
    m[1][0] = m[1][1] = m[1][2] = g->rt[0][2];
    m[2][0] = m[2][1] = m[2][2] = g->rt[1][1];
    return (const int16_t (*)[3])m;
}
GTE_FAST void mv_vector(int vx, int* x, int* y, int* z) {
    RtGte* g = G;
    if (vx < 3) { *x = g->v[vx][0]; *y = g->v[vx][1]; *z = g->v[vx][2]; }
    else { *x = g->ir[1]; *y = g->ir[2]; *z = g->ir[3]; }
}
GTE_FAST void mv_trans(int cv, int32_t* t1, int32_t* t2, int32_t* t3) {
    RtGte* g = G;
    if (cv == 0) { *t1 = g->tr[0]; *t2 = g->tr[1]; *t3 = g->tr[2]; }
    else if (cv == 1) { *t1 = g->bk[0]; *t2 = g->bk[1]; *t3 = g->bk[2]; }
    else if (cv == 2) { *t1 = g->fc[0]; *t2 = g->fc[1]; *t3 = g->fc[2]; }
    else { *t1 = *t2 = *t3 = 0; }
}

/* in 64 bits, and the FC bug */
GTE_SLOW void mat_vec_slow(int sf, int lm, int mx, int vx, int cv, uint32_t* flag) {
    RtGte* g = G;
    int16_t m[3][3];
    const int16_t (*mp)[3] = mv_matrix(mx, m);
    int x, y, z;
    mv_vector(vx, &x, &y, &z);
    int32_t t1, t2, t3;
    mv_trans(cv, &t1, &t2, &t3);
    s64 m1, m2, m3;
    if (cv == 2) {
        /* the FC bug: the first column's sum is checked, then dropped */
        (void)mac44(((s64)t1 << 12) + (s64)(mp[0][0] * x), 0, flag);
        (void)mac44(((s64)t2 << 12) + (s64)(mp[1][0] * x), 1, flag);
        (void)mac44(((s64)t3 << 12) + (s64)(mp[2][0] * x), 2, flag);
        m1 = (s64)(mp[0][1] * y) + (s64)(mp[0][2] * z);
        m2 = (s64)(mp[1][1] * y) + (s64)(mp[1][2] * z);
        m3 = (s64)(mp[2][1] * y) + (s64)(mp[2][2] * z);
    } else {
        m1 = ((s64)t1 << 12) + (s64)(mp[0][0] * x) + (s64)(mp[0][1] * y) + (s64)(mp[0][2] * z);
        m2 = ((s64)t2 << 12) + (s64)(mp[1][0] * x) + (s64)(mp[1][1] * y) + (s64)(mp[1][2] * z);
        m3 = ((s64)t3 << 12) + (s64)(mp[2][0] * x) + (s64)(mp[2][1] * y) + (s64)(mp[2][2] * z);
    }
    m1 = mac44(m1, 0, flag);
    m2 = mac44(m2, 1, flag);
    m3 = mac44(m3, 2, flag);
    const int shift = sf ? 12 : 0;
    g->mac[1] = (int32_t)(m1 >> shift);
    g->mac[2] = (int32_t)(m2 >> shift);
    g->mac[3] = (int32_t)(m3 >> shift);
    set_ir123(lm, flag);
}

/* ---- matrix * vector (MVMVA and the lighting commands) ---- */
GTE_FAST void mat_vec(int sf, int lm, int mx, int vx, int cv, uint32_t* flag) {
    RtGte* g = G;
    if (cv == 2) {
        mat_vec_slow(sf, lm, mx, vx, cv, flag);
        return;
    }
    int16_t m[3][3];
    const int16_t (*mp)[3] = mv_matrix(mx, m);
    int x, y, z;
    mv_vector(vx, &x, &y, &z);
    int32_t t1, t2, t3;
    mv_trans(cv, &t1, &t2, &t3);
    Mac32 a, b, c;
    if (LIKELY(mac32(t1, mp[0][0] * x, mp[0][1] * y, mp[0][2] * z, &a) &&
               mac32(t2, mp[1][0] * x, mp[1][1] * y, mp[1][2] * z, &b) &&
               mac32(t3, mp[2][0] * x, mp[2][1] * y, mp[2][2] * z, &c))) {
        g->mac[1] = mac32_reg(&a, sf);
        g->mac[2] = mac32_reg(&b, sf);
        g->mac[3] = mac32_reg(&c, sf);
        set_ir123(lm, flag);
        return;
    }
    mat_vec_slow(sf, lm, mx, vx, cv, flag);
}

GTE_FAST void color_push(uint32_t* flag) {
    RtGte* g = G;
    int r = g->mac[1] >> 4, gg = g->mac[2] >> 4, b = g->mac[3] >> 4;
    if (r < 0 || r > 0xFF) { *flag |= FLAG_COL_R_SAT; r = r < 0 ? 0 : 0xFF; }
    if (gg < 0 || gg > 0xFF) { *flag |= FLAG_COL_G_SAT; gg = gg < 0 ? 0 : 0xFF; }
    if (b < 0 || b > 0xFF) { *flag |= FLAG_COL_B_SAT; b = b < 0 ? 0 : 0xFF; }
    g->rgb[0] = g->rgb[1];
    g->rgb[1] = g->rgb[2];
    g->rgb[2] = (uint32_t)r | ((uint32_t)gg << 8) | ((uint32_t)b << 16) | (g->rgbc & 0xFF000000u);
}

GTE_FAST void depth_cue(int sf, int lm, int32_t in_r, int32_t in_g, int32_t in_b, uint32_t* flag) {
    RtGte* g = G;
    const int shift = sf ? 12 : 0;
    const int16_t dr = sat_s16((int32_t)((((s64)g->fc[0] << 12) - in_r) >> shift));
    const int16_t dg = sat_s16((int32_t)((((s64)g->fc[1] << 12) - in_g) >> shift));
    const int16_t db = sat_s16((int32_t)((((s64)g->fc[2] << 12) - in_b) >> shift));
    g->mac[1] = (in_r + (int32_t)g->ir[0] * dr) >> shift;
    g->mac[2] = (in_g + (int32_t)g->ir[0] * dg) >> shift;
    g->mac[3] = (in_b + (int32_t)g->ir[0] * db) >> shift;
    set_ir123(lm, flag);
}

GTE_FAST void color_apply(int sf, int lm, uint32_t* flag) {
    RtGte* g = G;
    const int shift = sf ? 12 : 0;
    const int r = g->rgbc & 0xFF, gg = (g->rgbc >> 8) & 0xFF, b = (g->rgbc >> 16) & 0xFF;
    g->mac[1] = ((r << 4) * g->ir[1]) >> shift;
    g->mac[2] = ((gg << 4) * g->ir[2]) >> shift;
    g->mac[3] = ((b << 4) * g->ir[3]) >> shift;
    set_ir123(lm, flag);
}

GTE_FAST void depth_cue_color(int sf, int lm, uint32_t* flag) {
    RtGte* g = G;
    const int r = g->rgbc & 0xFF, gg = (g->rgbc >> 8) & 0xFF, b = (g->rgbc >> 16) & 0xFF;
    depth_cue(sf, lm, (r << 4) * g->ir[1], (gg << 4) * g->ir[2], (b << 4) * g->ir[3], flag);
}

GTE_FAST void nc_core(int kind, int sf, int lm, int v, uint32_t* flag) {
    mat_vec(sf, lm, 1, v, 3, flag);
    mat_vec(sf, lm, 2, 3, 1, flag);
    if (kind == 1) color_apply(sf, lm, flag);
    else if (kind == 2) depth_cue_color(sf, lm, flag);
    color_push(flag);
}

/* one command; `op` is a constant in each rt_gte_op_XX below, so the switch
 * folds away and each gets only its own code */
static inline __attribute__((always_inline)) void gte_exec(uint32_t cmd, unsigned op) {
    RtGte* g = G;
    const int sf = (cmd >> 19) & 1, lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;
    uint32_t flag = 0;
    switch (op) {
    case 0x01: rtps(0, sf, lm, 1, &flag); break;
    case 0x30:
        rtps(0, sf, lm, 0, &flag);
        rtps(1, sf, lm, 0, &flag);
        rtps(2, sf, lm, 1, &flag);
        break;
    case 0x06: { /* NCLIP */
        s64 m = (s64)g->sx[0] * (g->sy[1] - g->sy[2]);
        g->mac[0] = mac0_check(m, &flag);
        m += (s64)g->sx[1] * (g->sy[2] - g->sy[0]);
        g->mac[0] = mac0_check(m, &flag);
        m += (s64)g->sx[2] * (g->sy[0] - g->sy[1]);
        g->mac[0] = mac0_check(m, &flag);
        break;
    }
    case 0x0C: { /* OP */
        const int d1 = g->rt[0][0], d2 = g->rt[1][1], d3 = g->rt[2][2];
        s64 m1 = (s64)d2 * g->ir[3] - (s64)d3 * g->ir[2];
        s64 m2 = (s64)d3 * g->ir[1] - (s64)d1 * g->ir[3];
        s64 m3 = (s64)d1 * g->ir[2] - (s64)d2 * g->ir[1];
        m1 = mac44(m1, 0, &flag);
        m2 = mac44(m2, 1, &flag);
        m3 = mac44(m3, 2, &flag);
        g->mac[1] = (int32_t)(m1 >> shift);
        g->mac[2] = (int32_t)(m2 >> shift);
        g->mac[3] = (int32_t)(m3 >> shift);
        set_ir123(lm, &flag);
        break;
    }
    case 0x10: /* DPCS */
        depth_cue(sf, lm, (int32_t)(g->rgbc & 0xFF) << 16, (int32_t)((g->rgbc >> 8) & 0xFF) << 16,
                  (int32_t)((g->rgbc >> 16) & 0xFF) << 16, &flag);
        color_push(&flag);
        break;
    case 0x11: /* INTPL */
        depth_cue(sf, lm, g->ir[1] << 12, g->ir[2] << 12, g->ir[3] << 12, &flag);
        color_push(&flag);
        break;
    case 0x12: /* MVMVA */
        mat_vec(sf, lm, (cmd >> 17) & 3, (cmd >> 15) & 3, (cmd >> 13) & 3, &flag);
        break;
    case 0x13: nc_core(2, sf, lm, 0, &flag); break; /* NCDS */
    case 0x14: /* CDP */
        mat_vec(sf, lm, 2, 3, 1, &flag);
        depth_cue_color(sf, lm, &flag);
        color_push(&flag);
        break;
    case 0x16: /* NCDT */
        nc_core(2, sf, lm, 0, &flag);
        nc_core(2, sf, lm, 1, &flag);
        nc_core(2, sf, lm, 2, &flag);
        break;
    case 0x1B: nc_core(1, sf, lm, 0, &flag); break; /* NCCS */
    case 0x1C: /* CC */
        mat_vec(sf, lm, 2, 3, 1, &flag);
        color_apply(sf, lm, &flag);
        color_push(&flag);
        break;
    case 0x1E: nc_core(0, sf, lm, 0, &flag); break; /* NCS */
    case 0x20: /* NCT */
        nc_core(0, sf, lm, 0, &flag);
        nc_core(0, sf, lm, 1, &flag);
        nc_core(0, sf, lm, 2, &flag);
        break;
    case 0x28: /* SQR */
        g->mac[1] = ((int32_t)g->ir[1] * g->ir[1]) >> shift;
        g->mac[2] = ((int32_t)g->ir[2] * g->ir[2]) >> shift;
        g->mac[3] = ((int32_t)g->ir[3] * g->ir[3]) >> shift;
        set_ir123(lm, &flag);
        break;
    case 0x29: /* DCPL */
        depth_cue_color(sf, lm, &flag);
        color_push(&flag);
        break;
    case 0x2A: /* DPCT */
        for (int i = 0; i < 3; i++) {
            const uint32_t c = g->rgb[0];
            depth_cue(sf, lm, (int32_t)(c & 0xFF) << 16, (int32_t)((c >> 8) & 0xFF) << 16,
                      (int32_t)((c >> 16) & 0xFF) << 16, &flag);
            color_push(&flag);
        }
        break;
    case 0x2D: case 0x2E: { /* AVSZ3 / AVSZ4 */
        if (op == 0x2D) g->mac[0] = g->zsf3 * (g->sz[1] + g->sz[2] + g->sz[3]);
        else g->mac[0] = g->zsf4 * (g->sz[0] + g->sz[1] + g->sz[2] + g->sz[3]);
        int32_t otz = g->mac[0] >> 12;
        if (otz < 0 || otz > 0xFFFF) {
            flag |= FLAG_SZ3_OTZ_SAT;
            otz = otz < 0 ? 0 : 0xFFFF;
        }
        g->otz = (uint16_t)otz;
        break;
    }
    case 0x3D: /* GPF */
        g->mac[1] = ((int32_t)g->ir[0] * g->ir[1]) >> shift;
        g->mac[2] = ((int32_t)g->ir[0] * g->ir[2]) >> shift;
        g->mac[3] = ((int32_t)g->ir[0] * g->ir[3]) >> shift;
        set_ir123(lm, &flag);
        color_push(&flag);
        break;
    case 0x3E: { /* GPL */
        s64 m1 = ((s64)g->mac[1] << shift) + (s64)g->ir[0] * g->ir[1];
        s64 m2 = ((s64)g->mac[2] << shift) + (s64)g->ir[0] * g->ir[2];
        s64 m3 = ((s64)g->mac[3] << shift) + (s64)g->ir[0] * g->ir[3];
        m1 = mac44(m1, 0, &flag);
        m2 = mac44(m2, 1, &flag);
        m3 = mac44(m3, 2, &flag);
        g->mac[1] = (int32_t)(m1 >> shift);
        g->mac[2] = (int32_t)(m2 >> shift);
        g->mac[3] = (int32_t)(m3 >> shift);
        set_ir123(lm, &flag);
        color_push(&flag);
        break;
    }
    case 0x3F: /* NCCT */
        nc_core(1, sf, lm, 0, &flag);
        nc_core(1, sf, lm, 1, &flag);
        nc_core(1, sf, lm, 2, &flag);
        break;
    default:
        return; /* unknown: leaves everything, FLAG included, as it was */
    }
    if (flag & FLAG_ERROR_MASK) flag |= FLAG_ERROR;
    g->flag = flag;
}

#ifdef RT_GTE_COUNT /* how many of each command: at exit, on stderr */
#include <stdio.h>
#include <stdlib.h>
static unsigned long gte_cnt[64];
static void gte_cnt_dump(void) {
    for (int i = 0; i < 64; i++) if (gte_cnt[i]) fprintf(stderr, "gtecount %02X %lu\n", i, gte_cnt[i]);
}
static void gte_count(unsigned op) {
    static int once;
    if (!once) { once = 1; atexit(gte_cnt_dump); }
    gte_cnt[op]++;
}
#define RT_GTE_OP(o) void rt_gte_op_##o(uint32_t cmd) { gte_count(0x##o); gte_exec(cmd, 0x##o); }
#else
#define RT_GTE_OP(o) void rt_gte_op_##o(uint32_t cmd) { gte_exec(cmd, 0x##o); }
#endif
RT_GTE_OPS(RT_GTE_OP)

#define RT_GTE_W(w) void rt_gte_w_##w(void) { gte_exec(0x##w, 0x##w & 0x3F); }
RT_GTE_WORDS(RT_GTE_W)

void rt_gte_command(uint32_t cmd) {
    switch (cmd & 0x1FFFFFF) {
#define RT_GTE_WCASE(w) case 0x##w: rt_gte_w_##w(); return;
        RT_GTE_WORDS(RT_GTE_WCASE)
    default: break;
    }
    switch (cmd & 0x3F) {
#define RT_GTE_CASE(o) case 0x##o: rt_gte_op_##o(cmd); break;
        RT_GTE_OPS(RT_GTE_CASE)
    default: break; /* unknown: leaves everything as it was */
    }
}
