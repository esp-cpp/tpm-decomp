# Plan

The end point is the game running natively on the Tab5, like Metal Gear
Solid does in tab5-emu: decompiled C compiled for RISC-V, with psyz standing in
for the PlayStation (GTE in software, GPU rasterised in software, SPU mixed in
software, the CD read from files on the SD card).

MGS had a finished community decompilation to start from. This game has
none, so the work is the decompilation itself. The size of the job:

| | functions | bytes |
|---|---|---|
| game C (`src/game*.c`) | ~823 | ~290 KB |
| hand-written asm in `.text` | a few dozen | ~25 KB |
| GTE/OT asm after the data (`gte_asm`) | ~16 | 8 KB |
| PSY-Q libraries | ~300 | ~70 KB: **not** decompiled: psyz replaces them |
| `FMV.CPE` overlay | ? | 84 KB: not needed (movies can go through psyz's MDEC or be skipped) |

## Phase 0: the ground (done)

- [x] disc layout, the game's file table, the load-screen format
- [x] splat config: byte-identical rebuild from asm
- [x] C build with `INCLUDE_ASM` stubs (GCC 2.8.1 + maspsx), still byte-identical
- [x] PSY-Q library identification by signature (~105 objects)

## Phase 1: names and structure

- [ ] per-function signatures for the unmatched library objects (libgpu
      `SYS.OBJ`, libcd, libetc `INTR.OBJ`), so every library call is named
- [ ] split `src/game.c` into translation units (splat's
      `find_file_boundaries` suggestions + alignment padding + the order of
      string literals in `.rodata`)
- [ ] name the engine's top level: `main` (`0x800236D8`), the task manager
      ("Taskman"), the frame loop, the file loader (it reads the file table at
      `0x800715A0`: everything that touches it is the CD/file layer), the
      level loader (`.LVL`), the renderer entry
- [ ] structures: actors / tasks, from the PC engine notes in OpenPhantom
      (`legacy/src/common/engine_types.h`) as a guide

## Phase 2: decompile toward the port, matching where it is cheap

The port needs **correct** C, not matching C. Matching is the best proof of
correctness, so the rule is: try to match; if a function will not match after
reasonable effort, keep the correct non-matching C under `NON_MATCHING`
(the asm stays the default for the matching build) and move on. Order:

1. the file / CD layer (small, and the port replaces its bottom half with SD
   reads, as `virtual_cd.c` does for MGS)
2. the frame loop, task manager, pad input, memory allocator
3. the renderer (ordering tables, primitives) and the GTE asm (`gte_asm`):
   these 16 hand-written routines have to be rewritten in C (with psyz's
   software GTE), as MGS's port did for its asm
4. level loading, then actors / AI / gameplay (the bulk), level by level:
   the Naboo swamp level (`SWAMP.LVL`) first

## Phase 3: the port build

In tab5-emu (`components/tpm`, branch `feat/tpm`):

- [x] skeleton component + cart, behind `CONFIG_TPM_ENGINE` (default off)
- [ ] data on the SD card: `tpm/FILE.000` ... (or the named files from
      `tools/tpm_files.py`), a "virtual CD" resolving the file table
- [ ] load screens: show `LOADING.RGB` (320x256x24) through the HAL: proves
      the data path and the display path before any game code runs
- [ ] build the decompiled C against psyz with `TPM_PORT` defined; every
      function still `INCLUDE_ASM` becomes a link error, which is the to-do list
- [ ] reuse from the MGS port: psyz build list, software rasteriser (two
      cores), audio pull task, pause/save states, the address-test lessons
      (no hard-coded PSX/ESP32-S3 memory ranges)

## Running now: static recompilation (2026-10-11)

`tools/recomp.py` + `recomp/runtime` (see [recomp/README.md](../recomp/README.md))
run the whole game: the executable is translated to C mechanically and the
runtime is the PlayStation around it (BIOS calls, interrupts, DMA, CD-ROM,
GPU ports on psyz's rasterizer, psyz's GTE and SPU). It boots into the first
level on the desktop; the Tab5 build (tab5-emu `components/tpm`) is the same
code. This changes the order of the work above: the port no longer waits for
the decompilation, and decompiled C can replace recompiled functions one at
a time (same entry address, same register conventions).

Next for the recompiled game:

- [ ] the Tab5: first run, then performance (rasterizer on the second core,
      as the MGS port does)
- [x] MDEC (movies) and XA-ADPCM (the movies' audio; in-game speech and
      music come through ordinary reads): the movies play natively
      (`recomp/runtime/rt_movie.c`)
- [x] memory card (save files on the SD card): libmcrd natively on a 128 KB card image (psyz mcd)
- [ ] root-counter interrupts, if anything uses them
