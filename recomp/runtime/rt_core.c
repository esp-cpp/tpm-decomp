/* Core of the recompiled-code runtime: register file, memory, dispatch,
 * traps, COP0, and interrupt delivery. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

Cpu g_cpu;
int g_trace;
#ifdef RT_DEBUG
uint32_t g_shadow[256];
int g_shadow_n;
uint32_t g_watch = 0xFFFFFFFFu;
void rt_watch_hit(uint32_t a, uint32_t v, int size) {
    printf("[watch] %08X <- %0*X (%d bytes) frame %u in %s\n", (unsigned)a, size * 2, (unsigned)v,
           size, (unsigned)rt_frame_count(), rt_where());
}
#endif

/* the innermost guest functions, for logs (RT_DEBUG builds) */
const char* rt_where(void) {
#ifdef RT_DEBUG
    static char buf[400];
    size_t o = 0;
    buf[0] = 0;
    for (int i = g_shadow_n - 1; i >= 0 && i >= g_shadow_n - 10 && o < sizeof buf - 20; i--)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s", o ? " < " : "",
                              rt_name(i < 256 ? g_shadow[i] : 0));
    return buf;
#else
    return "";
#endif
}
uint8_t* g_ram;

/* ---- names, for logs ---- */
const char* rt_name(uint32_t a) {
    static char buf[4][16];
    static int k;
    char key[9];
    snprintf(key, sizeof key, "%08X", (unsigned)a);
    for (unsigned i = 0; i < g_recomp_nnames; i++)
        if (!strcmp(g_recomp_names[i][0], key)) return g_recomp_names[i][1];
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "%08X", (unsigned)a);
    return buf[k];
}

/* ---- dispatch ---- */
static void (*find_func(uint32_t a))(void) {
    unsigned lo = 0, hi = g_recomp_nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t m = g_recomp_funcs[mid].addr;
        if (m == a) return g_recomp_funcs[mid].fn;
        if (m < a) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void rt_fatal(const char* fmt, ...) __attribute__((noreturn));

void rt_dispatch(uint32_t t) {
    if (t == 0xA0 || t == 0xB0 || t == 0xC0) {
        rt_bios(t, g_cpu.r[9]);
        return;
    }
    void (*fn)(void) = find_func(t);
    if (!fn) fn = find_func((t & 0x1FFFFFFFu) | 0x80000000u);
    if (!fn) rt_fatal("call to %08X: no function there (ra %08X)", (unsigned)t,
                      (unsigned)g_cpu.r[31]);
    fn();
}

void rt_jump(uint32_t t, uint32_t pc) {
    if (t == 0xA0 || t == 0xB0 || t == 0xC0 || find_func(t) ||
        find_func((t & 0x1FFFFFFFu) | 0x80000000u)) {
        rt_dispatch(t); /* a tail call */
        return;
    }
    rt_fatal("jr %08X at %08X: not a function (ra %08X)", (unsigned)t,
             (unsigned)pc, (unsigned)g_cpu.r[31]);
}

void rt_fallthrough(uint32_t t) {
    rt_fatal("fell through into %08X, not a function", (unsigned)t);
}

/* ---- COP0 ---- */
static uint32_t cop0_sr = 0x00000401u; /* IEc + IM2: the BIOS leaves both on */
static uint32_t cop0_epc, cop0_cause;

int rt_irq_enabled(void) { return (cop0_sr & 0x401u) == 0x401u; }
uint32_t rt_mfc0(int reg) {
    switch (reg) {
    case 12: return cop0_sr;
    case 13: return cop0_cause;
    case 14: return cop0_epc;
    case 15: return 0x00000002u; /* PRId */
    default: return 0;
    }
}
void rt_mtc0(int reg, uint32_t v) {
    if (reg == 12) {
        if ((v ^ cop0_sr) & 0x10000u)
            RT_LOG("cop0: cache %s", (v & 0x10000u) ? "isolated" : "back");
        cop0_sr = v;
        rt_check_irq();
    } else if (reg == 13) {
        cop0_cause = (cop0_cause & ~0x300u) | (v & 0x300u);
    }
}
void rt_rfe(void) { cop0_sr = (cop0_sr & ~0xFu) | ((cop0_sr >> 2) & 0xFu); }

/* ---- traps ---- */
void rt_syscall(uint32_t pc) {
    switch (g_cpu.r[4]) {
    case 1: /* EnterCriticalSection */
        g_cpu.r[2] = (cop0_sr & 0x401u) == 0x401u;
        cop0_sr &= ~0x401u;
        break;
    case 2: /* ExitCriticalSection */
        cop0_sr |= 0x401u;
        rt_check_irq();
        break;
    default:
        RT_LOG("syscall(%u) at %08X", (unsigned)g_cpu.r[4], (unsigned)pc);
        break;
    }
}
void rt_break(uint32_t pc, uint32_t insn) {
    RT_LOG("break %05X at %08X (ra %08X)", (unsigned)((insn >> 6) & 0xFFFFF),
           (unsigned)pc, (unsigned)g_cpu.r[31]);
}
void rt_invalid(uint32_t pc, uint32_t insn) {
    rt_fatal("invalid instruction %08X at %08X (ra %08X)", (unsigned)insn,
             (unsigned)pc, (unsigned)g_cpu.r[31]);
}

/* ---- interrupts ----
 * libetc installs its dispatcher the BIOS way: setjmp(buf) in startIntr,
 * buf's sp pointed at its own interrupt stack, HookEntryInt(buf). On an
 * interrupt the BIOS "longjmps" there: setjmp returns 1 and trapIntr runs,
 * ending in ReturnFromException. Here the longjmp is a call to the setjmp
 * return point (made a function entry by recomp_entries), and
 * ReturnFromException a host longjmp back to rt_take_irq. */
static uint32_t hook_buf;
static jmp_buf irq_jb;
static int in_irq;

void rt_set_hook(uint32_t buf) { hook_buf = buf; }
void rt_return_from_exception(void) {
    if (!in_irq) {
        RT_LOG("ReturnFromException outside an interrupt (ra %08X)",
               (unsigned)g_cpu.r[31]);
        return;
    }
    longjmp(irq_jb, 1);
}

static void rt_take_irq(void) {
    static int warned;
    if (!hook_buf) {
        if (!warned++) RT_LOG("interrupt pending, no handler hooked yet");
        return;
    }
    Cpu saved = g_cpu;
    uint32_t sr = cop0_sr;
    RT_TRACE("irq: stat %03X mask %03X", (unsigned)hw_istat(), (unsigned)hw_imask());
    in_irq = 1;
    cop0_sr = (cop0_sr & ~0x3Fu) | ((cop0_sr << 2) & 0x3Cu); /* push IE/KU */
    uint32_t b = hook_buf;
    g_cpu.r[31] = LW(b + 0);
    g_cpu.r[29] = LW(b + 4);
    g_cpu.r[30] = LW(b + 8);
    for (int i = 0; i < 8; i++) g_cpu.r[16 + i] = LW(b + 12 + 4 * i);
    g_cpu.r[28] = LW(b + 44);
    g_cpu.r[2] = 1;
#ifdef RT_DEBUG
    int shadow_n = g_shadow_n;
#endif
    if (!setjmp(irq_jb)) {
        rt_dispatch(g_cpu.r[31]);
        RT_LOG("interrupt handler returned without ReturnFromException");
    }
#ifdef RT_DEBUG
    g_shadow_n = shadow_n;
#endif
    int32_t budget = g_cpu.budget;
    g_cpu = saved;
    g_cpu.budget = budget;
    cop0_sr = sr;
    in_irq = 0;
}

void rt_check_irq(void) {
    if (in_irq || !rt_irq_enabled()) return;
    /* a handler that does not acknowledge would loop forever: bound it */
    for (int n = 0; n < 8 && (hw_istat() & hw_imask()); n++) {
        if (!hook_buf) { rt_take_irq(); break; }
        rt_take_irq();
    }
}

/* ---- time ---- */
#define POLL_EDGES 32
#define CYCLES_PER_EDGE 48
static int realtime;
static uint64_t rt_epoch_us;
uint64_t g_now;

void rt_set_realtime(int on) {
    realtime = on;
    rt_epoch_us = plat_time_us() - (uint64_t)(g_now / 33.8688);
}

static volatile int exit_requested, pause_requested;
static jmp_buf boot_jb;
void rt_request_exit(void) { exit_requested = 1; }
void rt_pause(int on) { pause_requested = on; }

void rt_poll(void) {
    g_cpu.budget = POLL_EDGES;
    if (exit_requested) longjmp(boot_jb, 1);
    if (pause_requested) {
        while (pause_requested && !exit_requested) plat_pause_wait();
        if (exit_requested) longjmp(boot_jb, 1);
        rt_set_realtime(realtime); /* the pause took no emulated time */
    }
    if (realtime) {
        uint64_t t = (uint64_t)((plat_time_us() - rt_epoch_us) * 33.8688);
        g_now = t > g_now ? t : g_now + POLL_EDGES;
    } else {
        g_now += POLL_EDGES * CYCLES_PER_EDGE;
    }
    hw_update();
    rt_check_irq();
}

/* Waits the BIOS would spend: advance time explicitly */
void rt_idle(uint32_t cycles) {
    if (!realtime) g_now += cycles;
    hw_update();
}

/* ---- boot ---- */
void rt_boot(const uint8_t* exe, uint32_t size) {
    if (size < 0x800 || memcmp(exe, "PS-X EXE", 8))
        rt_fatal("not a PS-X EXE");
    uint32_t pc0, gp0, t_addr, t_size, s_addr, s_size;
    memcpy(&pc0, exe + 0x10, 4);
    memcpy(&gp0, exe + 0x14, 4);
    memcpy(&t_addr, exe + 0x18, 4);
    memcpy(&t_size, exe + 0x1C, 4);
    memcpy(&s_addr, exe + 0x30, 4);
    memcpy(&s_size, exe + 0x34, 4);
    if (!g_ram) g_ram = (uint8_t*)plat_alloc(RAM_ALLOC);
    if (!g_ram) rt_fatal("no memory for guest RAM");
    memset(g_ram, 0, RAM_ALLOC);
    exit_requested = pause_requested = 0;
    hook_buf = 0;
    in_irq = 0;
    g_now = 0;
    cop0_sr = 0x00000401u;
    g_trace = getenv("RT_TRACE") != NULL;
#ifdef RT_DEBUG
    if (getenv("RT_WATCH")) g_watch = (uint32_t)strtoul(getenv("RT_WATCH"), NULL, 16) & 0x1FFFFC;
#endif
    if (t_size > size - 0x800) t_size = size - 0x800;
    memcpy(g_ram + (t_addr & (RAM_SIZE - 1)), exe + 0x800, t_size);
    hw_reset();
    rt_bios_init();
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.r[28] = gp0;
    g_cpu.r[29] = s_addr ? s_addr + s_size : 0x801FFFF0u;
    g_cpu.r[30] = g_cpu.r[29];
    g_cpu.r[31] = 0xFFFFFFF0u; /* returning from main = a bug */
    g_cpu.budget = POLL_EDGES;
    RT_LOG("boot: pc %08X gp %08X text %08X+%X", (unsigned)pc0, (unsigned)gp0,
           (unsigned)t_addr, (unsigned)t_size);
    rt_set_realtime(realtime);
    if (setjmp(boot_jb)) {
        RT_LOG("exit");
#ifdef RT_DEBUG
        g_shadow_n = 0;
#endif
        return;
    }
    rt_dispatch(pc0);
    rt_fatal("the executable returned");
}
