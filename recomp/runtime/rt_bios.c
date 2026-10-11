/* The BIOS calls (A0 / B0 / C0 tables) the executable makes through its
 * PSY-Q stubs, implemented natively over guest memory. Arguments in a0-a3
 * (and the stack), result in v0, as the real kernel does. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

#define A0 g_cpu.r[4]
#define A1 g_cpu.r[5]
#define A2 g_cpu.r[6]
#define A3 g_cpu.r[7]
#define V0 g_cpu.r[2]

static uint32_t heap_base, heap_size, heap_next;
static uint32_t rand_seed = 1;

static void events_reset(void);
void rt_bios_init(void) {
    events_reset();
    heap_base = heap_size = heap_next = 0;
    rand_seed = 1;
}

static uint32_t gstrlen(uint32_t s) {
    uint32_t n = 0;
    while (LBU(s + n)) n++;
    return n;
}

/* printf over guest memory: enough of the format language for debug text */
static int gformat(char* out, size_t cap, uint32_t fmt, int argn) {
    size_t o = 0;
    char spec[16];
#define ARG() (argn < 4 ? g_cpu.r[4 + argn++] : LW(g_cpu.r[29] + 4 * argn++))
#define PUT(c) do { if (o + 1 < cap) out[o++] = (char)(c); } while (0)
    for (;;) {
        uint8_t c = (uint8_t)LBU(fmt++);
        if (!c) break;
        if (c != '%') { PUT(c); continue; }
        int k = 0;
        spec[k++] = '%';
        for (;;) {
            c = (uint8_t)LBU(fmt++);
            if (!c || k >= 12) break;
            if (strchr("-+ #0123456789.lh", c)) { if (c != 'l' && c != 'h') spec[k++] = (char)c; continue; }
            break;
        }
        if (!c) break;
        char tmp[256];
        spec[k++] = (char)c;
        spec[k] = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(tmp, sizeof tmp, spec, (int)ARG());
            break;
        case 'p':
            snprintf(tmp, sizeof tmp, "%08x", (unsigned)ARG());
            break;
        case 's': {
            uint32_t s = ARG();
            char str[200];
            size_t n = 0;
            while (s && n + 1 < sizeof str && LBU(s + n)) { str[n] = (char)LBU(s + n); n++; }
            str[n] = 0;
            snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        case '%': strcpy(tmp, "%"); break;
        default: snprintf(tmp, sizeof tmp, "%%%c", c); break;
        }
        for (char* t = tmp; *t; t++) PUT(*t);
    }
    out[o] = 0;
    return (int)o;
#undef ARG
#undef PUT
}

static void bios_printf(int first_arg) {
    char buf[512];
    gformat(buf, sizeof buf, g_cpu.r[4], first_arg);
    printf("[game] %s", buf);
    size_t n = strlen(buf);
    if (n && buf[n - 1] != '\n') printf("\n");
}

static void bios_a0(uint32_t fn) {
    switch (fn) {
    case 0x13: { /* setjmp: ra, sp, fp, s0-s7, gp */
        uint32_t b = A0;
        SW(b + 0, g_cpu.r[31]);
        SW(b + 4, g_cpu.r[29]);
        SW(b + 8, g_cpu.r[30]);
        for (int i = 0; i < 8; i++) SW(b + 12 + 4 * i, g_cpu.r[16 + i]);
        SW(b + 44, g_cpu.r[28]);
        V0 = 0;
        return;
    }
    case 0x14: rt_fatal("longjmp: not supported (from %08X)", (unsigned)g_cpu.r[31]);
    case 0x15: { /* strcat */
        uint32_t d = A0 + gstrlen(A0), s = A1;
        uint8_t c;
        do { c = (uint8_t)LBU(s++); SB(d++, c); } while (c);
        V0 = A0;
        return;
    }
    case 0x17: { /* strcmp */
        uint32_t a = A0, b = A1;
        for (;;) {
            int x = (int)LBU(a++), y = (int)LBU(b++);
            if (x != y || !x) { V0 = (uint32_t)(x - y); return; }
        }
    }
    case 0x18: { /* strncmp */
        uint32_t a = A0, b = A1, n = A2;
        V0 = 0;
        while (n--) {
            int x = (int)LBU(a++), y = (int)LBU(b++);
            if (x != y || !x) { V0 = (uint32_t)(x - y); return; }
        }
        return;
    }
    case 0x19: { /* strcpy */
        uint32_t d = A0, s = A1;
        uint8_t c;
        do { c = (uint8_t)LBU(s++); SB(d++, c); } while (c);
        V0 = A0;
        return;
    }
    case 0x1A: { /* strncpy */
        uint32_t d = A0, s = A1, n = A2;
        while (n) { uint8_t c = (uint8_t)LBU(s++); SB(d++, c); n--; if (!c) break; }
        while (n--) SB(d++, 0);
        V0 = A0;
        return;
    }
    case 0x1B: V0 = gstrlen(A0); return; /* strlen */
    case 0x1E: { /* strchr */
        uint32_t s = A0;
        for (;; s++) { uint8_t c = (uint8_t)LBU(s); if (c == (uint8_t)A1) { V0 = s; return; } if (!c) { V0 = 0; return; } }
    }
    case 0x22: { /* strstr */ V0 = 0; return; }
    case 0x25: { uint32_t c = A0 & 0xFF; V0 = (c >= 'a' && c <= 'z') ? c - 32 : c; return; } /* toupper */
    case 0x26: { uint32_t c = A0 & 0xFF; V0 = (c >= 'A' && c <= 'Z') ? c + 32 : c; return; } /* tolower */
    case 0x28: for (uint32_t i = 0; i < A1; i++) SB(A0 + i, 0); V0 = A0; return; /* bzero */
    case 0x2A: case 0x2C: { /* memcpy, memmove */
        uint32_t d = A0, s = A1, n = A2;
        if (fn == 0x2C && d > s && d < s + n) { while (n--) SB(d + n, LBU(s + n)); }
        else for (uint32_t i = 0; i < n; i++) SB(d + i, LBU(s + i));
        V0 = A0;
        return;
    }
    case 0x2B: for (uint32_t i = 0; i < A2; i++) SB(A0 + i, A1); V0 = A0; return; /* memset */
    case 0x2D: { /* memcmp */
        for (uint32_t i = 0; i < A2; i++) {
            int x = (int)LBU(A0 + i), y = (int)LBU(A1 + i);
            if (x != y) { V0 = (uint32_t)(x - y); return; }
        }
        V0 = 0;
        return;
    }
    case 0x2E: { /* memchr */
        for (uint32_t i = 0; i < A2; i++) if (LBU(A0 + i) == (A1 & 0xFF)) { V0 = A0 + i; return; }
        V0 = 0;
        return;
    }
    case 0x2F: rand_seed = rand_seed * 1103515245u + 12345u; V0 = (rand_seed >> 16) & 0x7FFF; return;
    case 0x30: rand_seed = A0; return; /* srand */
    case 0x33: { /* malloc: bump allocator over InitHeap's area */
        uint32_t n = (A0 + 7) & ~7u;
        if (heap_next + n > heap_base + heap_size) { V0 = 0; return; }
        V0 = heap_next;
        heap_next += n;
        return;
    }
    case 0x34: return; /* free */
    case 0x39: heap_base = heap_next = A0; heap_size = A1; return; /* InitHeap */
    case 0x3F: bios_printf(1); return;
    case 0x3E: { /* puts */
        char buf[256];
        uint32_t s = A0, n = 0;
        while (n + 1 < sizeof buf && LBU(s + n)) { buf[n] = (char)LBU(s + n); n++; }
        buf[n] = 0;
        printf("[game] %s\n", buf);
        return;
    }
    case 0x44: return; /* FlushCache */
    case 0x49: return; /* GPU_cw */
    case 0x72: return; /* _96_remove */
    case 0x9F: return; /* SetMem */
    case 0xA0: case 0xAB: case 0xAC: V0 = 0; return; /* _boot / _card_info / _card_load */
    default: break;
    }
    static uint8_t seen[256];
    if (!seen[fn & 0xFF]++) RT_LOG("BIOS A0:%02X not implemented (ra %08X)", (unsigned)fn, (unsigned)g_cpu.r[31]);
    V0 = 0;
}

/* Kernel events (OpenEvent ...): libspu, libcd and friends deliver them from
 * their interrupt callbacks and poll them with TestEvent. */
#define EV_MAX 32
enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000 };
typedef struct {
    uint32_t cls, spec, mode, func, status;
} Event;
static Event events[EV_MAX];

static Event* ev_get(uint32_t h) {
    uint32_t i = h & 0xFFFF;
    if ((h & 0xFFFF0000u) != 0xF1000000u || i >= EV_MAX || events[i].status == EV_FREE) return NULL;
    return &events[i];
}
static void ev_deliver(uint32_t cls, uint32_t spec) {
    RT_TRACE("DeliverEvent(%08X, %04X)", (unsigned)cls, (unsigned)spec);
    for (int i = 0; i < EV_MAX; i++) {
        Event* e = &events[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x1000) { /* EvMdINTR: run the handler */
            if (e->func) {
                Cpu saved = g_cpu;
                rt_dispatch(e->func);
                int32_t b = g_cpu.budget;
                g_cpu = saved;
                g_cpu.budget = b;
            }
        } else { /* EvMdNOINTR: mark it for TestEvent / WaitEvent */
            e->status = EV_READY;
        }
    }
}
void rt_deliver_event(uint32_t cls, uint32_t spec) { ev_deliver(cls, spec); }
static void events_reset(void) { memset(events, 0, sizeof events); }

static void bios_b0(uint32_t fn) {
    switch (fn) {
    case 0x07: ev_deliver(A0, A1); V0 = 1; return; /* DeliverEvent */
    case 0x08: /* OpenEvent(class, spec, mode, func) */
        for (int i = 0; i < EV_MAX; i++) {
            if (events[i].status != EV_FREE) continue;
            events[i] = (Event){A0, A1, A2, A3, EV_DISABLED};
            RT_TRACE("OpenEvent(%08X, %04X, %04X, %08X) = %d", (unsigned)A0, (unsigned)A1, (unsigned)A2, (unsigned)A3, i);
            V0 = 0xF1000000u | (uint32_t)i;
            return;
        }
        V0 = 0xFFFFFFFFu;
        return;
    case 0x09: { Event* e = ev_get(A0); if (e) e->status = EV_FREE; V0 = 1; return; } /* CloseEvent */
    case 0x0A: { /* WaitEvent */
        Event* e = ev_get(A0);
        if (!e || e->status == EV_DISABLED) { V0 = 0; return; }
        for (int guard = 0; e->status != EV_READY && guard < 10000000; guard++) rt_idle(64);
        if (e->status == EV_READY) e->status = EV_ENABLED;
        V0 = 1;
        return;
    }
    case 0x0B: { /* TestEvent */
        Event* e = ev_get(A0);
        if (e && e->status == EV_READY) { e->status = EV_ENABLED; V0 = 1; }
        else V0 = 0;
        return;
    }
    case 0x0C: { Event* e = ev_get(A0); if (e) e->status = EV_ENABLED; V0 = 1; return; } /* EnableEvent */
    case 0x0D: { Event* e = ev_get(A0); if (e) e->status = EV_DISABLED; V0 = 1; return; } /* DisableEvent */
    case 0x17: rt_return_from_exception(); return;
    case 0x18: rt_set_hook(0); return; /* ResetEntryInt */
    case 0x19: rt_set_hook(A0); RT_LOG("HookEntryInt(%08X)", (unsigned)A0); return;
    case 0x20: /* UnDeliverEvent */
        for (int i = 0; i < EV_MAX; i++)
            if (events[i].status == EV_READY && events[i].cls == A0 && events[i].spec == A1)
                events[i].status = EV_ENABLED;
        V0 = 1;
        return;
    case 0x3F: bios_printf(1); return; /* puts */
    case 0x4A: case 0x4B: case 0x4C: return; /* InitCARD / StartCARD / StopCARD */
    case 0x4D: V0 = 0; return; /* _card_info */
    case 0x4E: case 0x4F: V0 = 0; return; /* _card_write / _card_read */
    case 0x50: V0 = 0; return; /* _new_card */
    case 0x56: V0 = 0; return; /* GetC0Table */
    case 0x57: V0 = 0; return; /* GetB0Table */
    case 0x5B: return; /* ChangeClearPAD */
    case 0x5C: V0 = 0; return; /* _card_status */
    case 0x5D: V0 = 0; return; /* _card_wait */
    case 0x32: case 0x33: case 0x34: case 0x35: /* open close lseek read */
    case 0x36: case 0x41: case 0x42: case 0x43: case 0x45: case 0x46:
        V0 = 0xFFFFFFFFu;
        return;
    default: break;
    }
    static uint8_t seen[256];
    if (!seen[fn & 0xFF]++) RT_LOG("BIOS B0:%02X not implemented (ra %08X)", (unsigned)fn, (unsigned)g_cpu.r[31]);
    V0 = 0;
}

static void bios_c0(uint32_t fn) {
    switch (fn) {
    case 0x02: RT_LOG("SysEnqIntRP(%u, %08X): handler chain not emulated", (unsigned)A0, (unsigned)A1); V0 = 0; return;
    case 0x03: V0 = 0; return; /* SysDeqIntRP */
    case 0x0A: V0 = 0; return; /* ChangeClearRCnt */
    default: break;
    }
    static uint8_t seen[256];
    if (!seen[fn & 0xFF]++) RT_LOG("BIOS C0:%02X not implemented (ra %08X)", (unsigned)fn, (unsigned)g_cpu.r[31]);
    V0 = 0;
}

void rt_bios(uint32_t table, uint32_t fn) {
    switch (table) {
    case 0xA0: bios_a0(fn); break;
    case 0xB0: bios_b0(fn); break;
    default: bios_c0(fn); break;
    }
}

void rt_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[rt] FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    printf("[rt] regs:");
    for (int i = 0; i < 32; i++) printf("%s r%d=%08X", (i & 7) ? "" : "\n[rt] ", i, (unsigned)g_cpu.r[i]);
    printf("\n[rt] frame %u\n[rt] in %s\n", (unsigned)rt_frame_count(), rt_where());
    fflush(stdout);
    abort();
}
