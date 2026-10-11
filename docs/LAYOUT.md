# SLUS_008.84: what is where

Star Wars Episode I: The Phantom Menace, PlayStation, USA **Rev 1**
(`SLUS_008.84`, sha1 `02f73ad2115f2fbb5bd1880615db0f9ac7b5055c`, 487 424 bytes).
The original USA release (`8dda79b3…`) is a different link of the same game:
the two executables differ in about 400 000 bytes, so Rev 1 is the only target for now.

Developer: Big Ape Productions, for LucasArts, 1999. The internal codename is
**Zanzibar**: paths are `c:\zanzibar\data\...`, and the disc's padding file is
`ZANZIBAR.000`. The engine's subsystems are called `Taskman`, `Bapworld`,
`Bapview`, `Bapsound` and `Bappush` ("BAP" = Big Ape Productions).

## Executable image

| vram | file | what |
|---|---|---|
| `0x80010000` | `0x800` | `.text`: game code (C), 823 functions (`src/game*.c`) |
| `0x80057734` ... `0x8005D970` | | hand-written asm modules with data inside `.text` (raw pieces: `D_80057734`, `D_8005B678`, `D_8005C168`) |
| `0x8005E210` | `0x4EA10` | `__start` (PSY-Q startup: clears `gp..0x801FF000`, heap = `0x800CBCBC`, jumps to `main` `0x800236D8`) |
| `0x8006044C` | `0x50C4C` | PSY-Q libraries (first signature-matched object; see below) |
| `0x80071590` | `0x61D90` | game `.rodata` / `.data` (file table at `0x800715A0`, stream table at `0x80071FA8`) |
| `0x800838E4` | `0x740E4` | second `.text`: hand-written GTE / ordering-table routines (`asm/us/gte_asm.s`) |
| `0x8008585C` | `0x7605C` | library `.rdata` / `.data` |
| `0x8008630C` | `0x76B0C` | `gp`; `.sbss` (zeros, still in the file image up to `0x80086800`) |
| `0x80086800` | | `.bss` to `0x800CBCBC`; the heap follows |

The game is compiled `-G0`: no `$gp`-relative data in C code (5 uses, all in
hand-written asm).

## Compiler

PSY-Q 4.3's CCPSX: **GCC 2.8.1** + ASPSX 2.79 (emulated by maspsx). Evidence:
the library objects are PSY-Q 4.3 (`libgpu sys.c 1.140, 1998/01/12`), and
GCC 2.8.1 reproduces the game's code shape (explicit `%hi/%lo` pairs into the
destination register) where 2.7.2 does not (`lui $at` macro expansions). No
function is fully matched yet; `func_80010000` is one instruction off
(register allocation of a loop bound), which is a source-shape question.

No GCC 2.x vtables (8-byte `{delta, index, pfn}` entries) were found in the
data, and the code-pointer arrays look like switch tables: the PSX port is
probably C, although the PC game is C++.

## PSY-Q libraries

`tools/psyq_match.py` with lab313ru's signatures finds ~105 objects, mostly
4.3 with `libpad` (4.5/4.7), some `libcd`/`libds` objects from 4.7 and
`DSCB.OBJ` from 4.0: the game links a mix. Matched: libapi, libc, libetc
(VSync, intr_dma, intr_vb), libgte (InitGeom, RotTransPers, ratan2, ...),
libgpu (AddPrim, GetClut, SetDrawTPage), libspu (most of it), libds
(DsReadySystem, DsControl, ...), libpad, libmcrd, libcard.

Not matched as whole objects (they are there, judging by their strings):
libgpu `SYS.OBJ` (ResetGraph, DrawSync, ClearOTag...), libcd (`CD_sync`,
`DiskError`), libetc `INTR.OBJ`. Per-function signatures would place them.

## Disc

| file | sectors | what |
|---|---|---|
| `FILE.000` | 23.. | all game data, addressed by name through the file table (`tools/tpm_files.py`) |
| `FILE.001` | 17225.. | streamed audio (stream table) |
| `FILE.002` | 64868.. | streamed data, starts with a header naming `BigCity01SkyDock` |
| `FILE.003` | 164678.. | movies (MDEC STR: `0x80010160` sector headers) |
| `SLUS_008.84` | 272563 | the executable |
| `ZANZIBAR.000` | 272802.. | 28 MB of zeros (padding to the outer edge) |

### The file table (`0x800715A0`, 107 entries)

```c
struct FileEntry { u8 msf[3]; u8 pad; u32 size; char name[16]; };
```

`msf` is BCD minute:second:frame *inside FILE.000*, with the 2 s lead-in
(00:02:00 = first sector). Contents: 17 `.PCV` (sound banks), 11 `.LVL`
(levels), 13 `.VRM` + 12 `.VRJ` (VRAM images), 11 `.MAP`, 44 `.RGB` (load
screens), `TPAGE1.BTP`, `TITLE.VRM`, `FRONT.VRM`, `FMV.CPE`.

### The stream table (`0x80071FA8`)

```c
struct StreamEntry { u8 msf[3]; u8 pad; u32 length; };
```

Several hundred short streams (voice / music cues), positions relative to
their archive.

## Data formats (first look)

- `.RGB` 245 760 bytes = **320x256, 24-bit RGB** (load screens, title logos;
  displayed in the GPU's 24-bit mode).
- `.LVL` starts with a table of section sizes/offsets; contains `.baf` names
  (models + animation sets per character: `queen.baf`, `r2d2.baf`, ...).
- `.PCV` sound bank: `u32 size, u32 count`, then 16-byte SPU voice
  attribute records (`7f40 xx00 ...`), then ADPCM.
- `.VRM` / `.VRJ`: VRAM uploads (`.VRJ` looks like 15-bit pixel data; `.VRM`
  carries a record list first).
- `FMV.CPE`: **code** (an overlay, the movie player; starts with a MIPS
  prologue). It will need its own splat segment once its load address is known.

## The PC version

The PSX game is a port of the PC game (`WMAIN.EXE`), whose engine
OpenPhantom (<https://github.com/OpenPhantom/OpenPhantom>) has been patching:
`legacy/src/common/engine_types.h` documents structures from the PC engine
(actors, camera, sound) that the PSX code should share in spirit, if not in
layout. Item / weapon / level / message strings match the PC game's.
