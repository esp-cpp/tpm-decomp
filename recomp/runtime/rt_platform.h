/* What the recompiled game needs from the machine it runs on. The desktop
 * runner (recomp/host) and the Tab5 component (tab5-emu components/tpm)
 * implement these. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y;    /* top-left in VRAM, in 16-bit units */
    int w, h;    /* pixels */
    int rgb24;   /* 24-bit display mode */
    int enabled;
    /* GP1 07, the vertical display range: the picture (VRAM rows y ..
     * y + lines - 1) starts `top` lines down the h-line screen, the rest of
     * the screen is black (letterboxed cutscenes shrink it) */
    int top, lines;
    /* frameskip (rt_gpu_set_frameskip): the buffer shown was not drawn for
     * the game's latest frame in it; keep showing the previous picture */
    int stale;
} RtDisplay;

/* One raw 2352-byte sector of the disc (LBA 0 = 00:02:00). Returns 0 on
 * success. */
int plat_read_sector(uint32_t lba, uint8_t* out);
/* Sectors on the disc. */
uint32_t plat_disc_sectors(void);

/* Called at every emulated vertical blank, on the game's thread (input,
 * pacing). */
void plat_vblank(void);
/* Then, once the drawing queued before that vblank is in VRAM: present `d`
 * from g_rt_vram. On the game's thread, or on the GPU worker's with
 * rt_gpu_set_async(1). */
void plat_display_ready(const RtDisplay* d);
/* rt_gpu_set_async(n > 0) only: wake the GPU workers / wait a little (a
 * worker when idle, the game's thread when the queue is full or draining);
 * a worker that made room wakes the waiting game's thread. */
void plat_gpu_kick(void);
void plat_gpu_idle(void);
void plat_gpu_wake_producer(void);

/* Pad buttons for port 0/1, PlayStation bit layout, 1 = pressed:
 * 0 select, 3 start, 4 up, 5 right, 6 down, 7 left, 8 L2, 9 R2, 10 L1,
 * 11 R1, 12 triangle, 13 circle, 14 cross, 15 square. */
uint16_t plat_pad_buttons(int port);

/* Stereo 44.1 kHz audio from the SPU, interleaved L/R, as it is produced. */
void plat_audio(const int16_t* samples, int frames);

/* Microseconds since some fixed point; only used when the runtime paces
 * itself in real time (rt_set_realtime(1)). */
uint64_t plat_time_us(void);

/* Large allocations (guest RAM, VRAM, SPU, transfer buffers), zeroed:
 * PSRAM on the board. Released with plat_free by rt_shutdown(). */
void* plat_alloc(uint32_t size);
void plat_free(void* p);

/* Real time only: the game is this far ahead of the wall clock; sleep (about
 * that long; less is fine). */
void plat_sleep_us(uint32_t us);

/* Called on the game's thread while a pause is requested (rt_pause(1)):
 * block until it is lifted. */
void plat_pause_wait(void);

/* ---- what the runtime offers the platform ---- */

/* Load a PS-X EXE into guest RAM and run it. Returns only after
 * rt_request_exit(). */
void rt_boot(const uint8_t* exe, uint32_t size);
/* What rt_boot will take from plat_alloc: bytes, and in *count allocations.
 * The order of the allocations is fixed. */
uint32_t rt_mem_bytes(int* count);
/* Save states, which the platform takes while the game's thread waits in
 * plat_pause_wait (a snapshot of the runtime's statics, plat_alloc's memory
 * and that thread's stack, restored at the same addresses). The rasterizer's
 * caches are this run's heap memory: rt_snap_preserve lists the statics
 * that point at them (up to max), whose values the platform keeps across a
 * restore; then rt_snap_restored() marks the caches stale. */
int rt_snap_preserve(void*** out, int max);
void rt_snap_restored(void);
/* After rt_boot returned: release everything the runtime allocated. */
void rt_shutdown(void);
/* Ask the game's thread to leave rt_boot at its next poll. */
void rt_request_exit(void);
/* Pause the game's thread at its next poll (it calls plat_pause_wait()), or
 * let it go again; time does not advance while paused. */
void rt_pause(int on);
/* Emulated time advances with the work done (deterministic). 0: as fast as
 * the host can; 1: no faster than real time (the board), slower when the
 * host cannot keep up. */
void rt_set_realtime(int on);
/* Before rt_boot: run the GPU's command stream on n other threads (1 or 2),
 * which the platform gives to rt_gpu_worker_n(&stop, idx) (returns once
 * *stop is set): idx 1 always, idx 0 too with two workers, which then draw
 * a band of rows each. On the board, worker idx must run on core idx.
 * rt_gpu_worker(&stop) is rt_gpu_worker_n(&stop, 1). */
void rt_gpu_set_async(int n);
void rt_gpu_worker_n(volatile int* stop, int idx);
void rt_gpu_worker(volatile int* stop);
extern uint32_t g_rt_gpu_split, g_rt_gpu_barriers; /* two workers: telemetry */
/* Frameskip, one worker only: when the game has queued two more vblanks
 * behind the frame the worker is about to draw, that frame's primitives into
 * the display buffers are dropped (VRAM transfers, fills, state and drawing
 * elsewhere in VRAM still run) and its buffer is presented as `stale`. At
 * most `max` frames in a row; 0 = off. g_rt_gpu_skipped counts them. */
void rt_gpu_set_frameskip(int max);
extern uint32_t g_rt_gpu_skipped;
extern uint64_t g_rt_gpu_wait_us[2];
extern uint64_t g_rt_gpu_prod_wait_us; /* the CPU's thread waiting for the queue */

void rt_gpu_display(RtDisplay* d);
/* Record the GPU's command stream of vblanks [from, from + frames) to a file
 * (with VRAM as it was at the start), and replay such a recording through
 * the rasterizer: microseconds per vblank, the mean of `reps` runs. */
void rt_gpu_record(const char* path, uint32_t from, uint32_t frames);
uint32_t rt_gpu_bench(const uint32_t* rec, uint32_t words, int reps);
void rt_gpu_bench_mode(int skip); /* 1: no span pixels, 2: triangle setup only, 3: no triangles */
extern uint16_t* g_rt_vram; /* 1024 x 512, allocated by rt_boot */

uint32_t rt_frame_count(void);

/* Where the game thread's time went, in microseconds since boot. */
typedef struct {
    uint64_t gpu;    /* GP0 packets: the rasterizer, VRAM transfers */
    uint64_t spu;    /* SPU emulation (sample generation) */
    uint64_t cd;     /* sector reads */
    uint64_t vblank; /* plat_vblank */
    uint64_t sleep;  /* real time: waiting for the wall clock */
} RtProf;
extern RtProf g_rt_prof;
/* Debug: log the GPU state commands of one frame (vblank number). */
void rt_set_gpu_trace(uint32_t frame);

#ifdef __cplusplus
}
#endif
