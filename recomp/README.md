# recomp: the game as C, now

The decompilation will take a long time. This directory gets the game
running natively in the meantime: `tools/recomp.py` translates the whole
executable (MIPS R3000A) into C, mechanically, and the runtime here is the
PlayStation around it. Decompiled functions can replace recompiled ones one
at a time later; the runtime stays.

Nothing generated is committed: the C is derived from the game, so it is
produced at build time from your own `SLUS_008.84` (and, for the movie
player overlay, your disc image).

## The recompiler (`tools/recomp.py`)

- **Functions**: the split's entry points (`config/recomp_entries.us.txt`,
  addresses only), the PSY-Q symbols, every `jal` target, and every return
  point of a call to the BIOS `setjmp` stub (the kernel "longjmps" back there
  on an interrupt). Code is what is *reachable* from those: tables and strings
  that sit in `.text` stay data. Where code branches into the middle of a
  neighbour (a mis-split, or hand-written asm sharing tails) the two become
  one C function with several entry points.
- **Instructions**: registers are C locals, written back to `g_cpu` around
  calls and returns. Branch delay slots and the R3000's load delay slot are
  honoured; `jr $ra` returns, other `jr`/`jalr` go through a switch over the
  function's address-taken labels or the runtime's dispatcher. GTE
  instructions call psyz's software GTE.
- **Memory accesses**: each load and store checks where its address lands
  (RAM, else the scratchpad, else I/O). A value flow over each function (a
  few constants or an unsigned range per register, following the C the
  recompiler writes) proves the accesses that always reach the scratchpad,
  and those skip the check (`LWk`, ...; `RT_DEBUG` builds verify the proof).
  A local call's `jr $ra` goes back only to the return sites of the calls
  whose entry reaches it (anything else reaches `rt_jump_ra`, which reports
  it). `--no-value-flow` turns it off for comparisons.
- **GTE FLAG**: every command rewrites FLAG, and TPM reads it once (right
  after `RotTransPers`' RTPS). When every read has a command before it on
  each path inside its function, a command none of them is reachable from
  leaves FLAG alone (`GTE_CMDNF`), and what only fed it is not computed.
- **Time**: every loop back-edge counts down a budget; at zero the runtime
  advances its clock, runs the hardware (vblank, CD, SPU) and takes
  interrupts there.
- **Overlays**: with `--disc`, `FMV.CPE` (the movie player, loaded at
  `0x80160000`) is pulled out of `FILE.000` through the game's own file table
  and recompiled too.
- **HLE**: `hle.us.txt` lists functions replaced by native code
  (`runtime/rt_hle.c`): the BIOS patches, libmcrd (on a card image file, psyz's `mcd`), the
  movie player's play function (`runtime/rt_movie.c`: the movies natively,
  see below), and a wrapper on `PadInitDirect`.
- **Movies**: the overlay streams STR files through libcd's ring, which
  holds one frame: the game only keeps up at the hardware's exact pace. So
  its play function is replaced: the movie's sectors are read at double
  speed in emulated time, each frame goes through the game's own (recompiled)
  VLC decoder and the MDEC's decoding (`rt_mdec.c`) into VRAM, and the XA
  audio sectors are decoded into the SPU's CD input. Start or Cross skips;
  `RT_MOVIES=0` (or `rt_set_movies(0)`) skips them all, as the overlay's
  absence (no `--disc`) does.

## The runtime (`runtime/`)

| file | |
|---|---|
| `recomp_rt.h` | what generated code sees: `g_cpu`, memory accessors, `DIV`, hooks |
| `rt_core.c` | dispatch, traps, COP0, interrupt delivery (`HookEntryInt` / `ReturnFromException`), the clock, boot |
| `rt_bios.c` | the A0/B0/C0 calls: C library, heap, kernel events, printf |
| `rt_hw.c` | I/O space: interrupt controller, DMA, root counters, SPU (psyz's register-level emulator), SIO stand-in |
| `rt_mdec.c` | the MDEC: commands, tables, run-length decoding, IDCT, YUV to 15/24-bit macroblocks, DMA0/1 |
| `rt_movie.c` | the movies natively (above), XA-ADPCM decoding into the SPU's CD input |
| `rt_gpu.c` | GP0/GP1 ports, VRAM transfers, display state, in front of psyz's software rasterizer |
| `rt_cdrom.c` | the CD-ROM controller over a raw `.bin` (commands, INT1-5, data FIFO, XA audio sectors skipped) |
| `rt_hle.c` | the native replacements, and the pad buffers filled every vblank |
| `rt_platform.h` | what a platform provides (sectors, vblank, pads, audio, time, memory) and gets |

The platforms: `host/main.c` (headless desktop runner) and tab5-emu's
`components/tpm` (the M5Stack Tab5).

## The desktop runner

```sh
git submodule update --init recomp/psyz
make -C recomp DISC="<your disc>.bin"          # -> build/recomp/tpm-host
build/recomp/tpm-host disks/us/SLUS_008.84 "<your disc>.bin" \
    --frames 1800 --dump out/ --every 60 --wav out/audio.wav \
    --press 300:cross --press 600:cross --press 900:start
```

It runs in emulated time (as fast as the host can, deterministic), writes
the display as PNGs and the SPU's output as a WAV, and presses buttons on
cue. The default build keeps a shadow call stack (`DEBUG=1`): fatal errors
print the guest call chain, `RT_TRACE=1` logs CD / DMA / interrupt traffic,
`RT_WATCH=<addr>` logs every store to one word.

## Status

Boots through the memory-card notice, the loading screens and the level
intro into the first level (Trade Federation ship), in 3D: on the desktop
(~1100 fps, release build) and on the M5Stack Tab5 (ESP32-P4, ~12 fps of the
game's 30, with the GPU on the second core; see tab5-emu's
`components/tpm/README.md`). The SPU produces sound (measured, not yet
listened to).

The emulated timeline is deterministic (emulated time advances with the work
done; real-time mode only throttles), so the same `--press` script gives the
same frames on every machine: `--gpu-trace N` on the desktop and the board's
GPU trace can be diffed line by line.

Not yet: root-counter interrupts. (XA audio outside the
movies: TPM streams none.)

Runner options: `--realtime` (pace to the wall clock), `--async-gpu` (the
GPU's command stream on a second thread, as on the board), `--gpu-trace N`,
`--memcard FILE` (a memory card: a raw 128 KB image as PC emulators use,
made if missing; without it the ports are empty). The menus read the pad
slowly: `RT_PRESS_HOLD=20` holds each press for 20 vblanks instead of 10.
