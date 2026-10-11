/* What the recompiled game needs from the machine it runs on. The desktop
 * runner (recomp/host) and the Tab5 component (tab5-emu components/tpm)
 * implement these. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One raw 2352-byte sector of the disc (LBA 0 = 00:02:00). Returns 0 on
 * success. */
int plat_read_sector(uint32_t lba, uint8_t* out);
/* Sectors on the disc. */
uint32_t plat_disc_sectors(void);

/* Called at every emulated vertical blank, on the game's thread: present the
 * display (rt_gpu_display / g_RawVram) and sample input. */
void plat_vblank(void);

/* Pad buttons for port 0/1, PlayStation bit layout, 1 = pressed:
 * 0 select, 3 start, 4 up, 5 right, 6 down, 7 left, 8 L2, 9 R2, 10 L1,
 * 11 R1, 12 triangle, 13 circle, 14 cross, 15 square. */
uint16_t plat_pad_buttons(int port);

/* Stereo 44.1 kHz audio from the SPU, interleaved L/R, as it is produced. */
void plat_audio(const int16_t* samples, int frames);

/* Microseconds since some fixed point; only used when the runtime paces
 * itself in real time (rt_set_realtime(1)). */
uint64_t plat_time_us(void);

/* Large allocations (guest RAM, transfer buffers): PSRAM on the board. */
void* plat_alloc(uint32_t size);

/* Called on the game's thread while a pause is requested (rt_pause(1)):
 * block until it is lifted. */
void plat_pause_wait(void);

/* ---- what the runtime offers the platform ---- */

/* Load a PS-X EXE into guest RAM and run it. Returns only after
 * rt_request_exit(). */
void rt_boot(const uint8_t* exe, uint32_t size);
/* Ask the game's thread to leave rt_boot at its next poll. */
void rt_request_exit(void);
/* Pause the game's thread at its next poll (it calls plat_pause_wait()), or
 * let it go again; time does not advance while paused. */
void rt_pause(int on);
/* 1: emulated time follows the host clock (the board); 0: emulated time
 * advances with the work done (deterministic, as fast as the host can). */
void rt_set_realtime(int on);

typedef struct {
    int x, y;    /* top-left in VRAM, in 16-bit units */
    int w, h;    /* pixels */
    int rgb24;   /* 24-bit display mode */
    int enabled;
} RtDisplay;
void rt_gpu_display(RtDisplay* d);
extern uint16_t g_RawVram[512 * 1024];

uint32_t rt_frame_count(void);

#ifdef __cplusplus
}
#endif
