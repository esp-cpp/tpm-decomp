/* Private interfaces between the runtime's pieces. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define RT_LOG(fmt, ...) printf("[rt] " fmt "\n", ##__VA_ARGS__)
extern int g_trace; /* RT_TRACE=1: log hardware traffic */
#define RT_TRACE(fmt, ...) do { if (g_trace) printf("[trace] " fmt "\n", ##__VA_ARGS__); } while (0)
void rt_fatal(const char* fmt, ...) __attribute__((noreturn));
const char* rt_name(uint32_t addr);
const char* rt_where(void); /* current guest call chain (RT_DEBUG) */

#define CPU_HZ 33868800u
extern uint64_t g_now; /* emulated CPU cycles */
void rt_idle(uint32_t cycles);

/* rt_core.c */
void rt_check_irq(void);
int rt_irq_enabled(void);
void rt_set_hook(uint32_t buf);
void rt_return_from_exception(void);

/* rt_bios.c */
void rt_bios_init(void);
void rt_bios(uint32_t table, uint32_t fn);

/* rt_hw.c: interrupt controller, DMA, timers, SPU/SIO/MDEC stand-ins */
void hw_reset(void);
void hw_free(void);
void hw_update(void);
uint32_t hw_istat(void);
uint32_t hw_imask(void);
void hw_irq(int line); /* raise I_STAT bit `line` */
void hw_vsync_poll(void); /* VSync(-1): skip a busy wait to the vblank */
enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4,
       IRQ_TMR1 = 5, IRQ_TMR2 = 6, IRQ_SIO = 7, IRQ_SPU = 9 };

/* rt_gpu.c */
void gpu_reset(void);
int gpu_alloc(void);
void gpu_free(void);
void gpu_present(void);
void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);

/* rt_cdrom.c */
void cd_reset(void);
uint8_t cd_read8(uint32_t reg);
void cd_write8(uint32_t reg, uint8_t v);
void cd_dma(uint32_t dst, uint32_t words); /* data FIFO -> guest RAM */
void cd_update(void);

/* rt_hle.c */
void hle_vblank(void);
