# tpm-decomp

A decompilation of **Star Wars Episode I: The Phantom Menace** for the
PlayStation (Big Ape Productions / LucasArts, 1999), USA Rev 1
(`SLUS_008.84`).

Two goals, in this order:

1. a **matching** decompilation: C that compiles with the original compiler
   (PSY-Q 4.3's GCC 2.8.1) back to the byte-identical executable;
2. a **native port** of that C, built against
   [psyz](https://github.com/Xeeynamo/psyz) (a PlayStation SDK
   reimplementation with a software GPU) for the M5Stack Tab5 (ESP32-P4) in
   [tab5-emu](https://github.com/esp-cpp/tab5-emu), the way its Metal Gear
   Solid port runs [mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing).

**The game runs** (natively, on the desktop and the Tab5) through a static
recompilation of the whole executable: see [recomp/](recomp/README.md). The
decompilation below continues alongside it; decompiled functions can replace
recompiled ones one at a time.

Decompilation status: the executable is split (splat) and rebuilds byte-identical from
`INCLUDE_ASM` stubs; PSY-Q library functions are identified by signature; the
disc's file system is understood. First C: `main` and the file layer
(`File_Init` and its table rebasing), correct but a few instructions off
matching, so they build under `NON_MATCHING` (the port build) only. See
[docs/LAYOUT.md](docs/LAYOUT.md) for everything known about the image and the
disc, and [docs/PLAN.md](docs/PLAN.md) for the road map.

## You need your own copy of the game

Nothing from the game is in this repository: no code, no data, no names
derived from the disc beyond what is needed to describe its layout. Bring your
own disc image.

## Building

```sh
tools/setup_toolchain.sh                 # venv, mips binutils, GCC 2.8.1, maspsx
python3 tools/extract_disc.py "<your disc>.bin" disc/   # ISO9660 -> files
cp disc/SLUS_008.84 disks/us/
make setup                               # split the executable
make                                     # rebuild and compare: "OK"
```

`make setup` regenerates `asm/` and, **only for files that do not exist yet**,
the `src/*.c` stubs: splat never overwrites C.

To find the PSY-Q functions again (after changing the split, say):

```sh
git clone https://github.com/lab313ru/psx_psyq_signatures /tmp/sigs
tools/psyq_match.py disks/us/SLUS_008.84 /tmp/sigs --union --symbols config/symbols.psyq.us.txt
```

## Decompiling a function

1. pick an `INCLUDE_ASM` line in `src/*.c`; its asm is in
   `asm/us/nonmatchings/<file>/<func>.s`;
2. replace it with C (m2c is a good first draft:
   `m2c asm/us/nonmatchings/game/func_XXXXXXXX.s`);
3. `make`: it must still print `OK`. `tools/fdiff.py <func>` shows the built
   function against the original, instruction by instruction.
4. if it will not match after reasonable effort, keep the C anyway, guarded:
   `#ifndef NON_MATCHING` / `INCLUDE_ASM(...)` / `#else` / C / `#endif`.
   The port builds with `NON_MATCHING`; correct C is what it needs.

Renaming a function: add it to `config/symbols.us.txt`, rename its
`INCLUDE_ASM` line in `src/`, then `rm -rf asm && make setup` (splat files
the asm of functions it does not see in an `INCLUDE_ASM` under
`asm/us/matchings/`).

## Tools

| | |
|---|---|
| `tools/extract_disc.py` | list / extract the ISO9660 files of a raw `.bin` (Form 2 files as raw 2336-byte sectors) |
| `tools/tpm_files.py` | the game's own file table: list / extract `FILE.000` by name |
| `tools/psyq_match.py` | find PSY-Q library objects by signature, write splat symbols |
| `tools/gte_words.py` | post-split pass: GTE commands as raw `.word`s (their encodings are not all canonical) |
| `tools/setup_toolchain.sh` | fetch / build the toolchain |
| `tools/fdiff.py` | one function of the build against the original, instruction by instruction |
| `tools/recomp.py` | the static recompiler (see [recomp/](recomp/README.md)) |

## License

MIT (see [LICENSE](LICENSE)) for the code in this repository. The game
itself is not covered: it stays the property of its owners and is not here.

## Credits

splat / spimdisasm / rabbitizer (decompals, ethteck, AngheloAlf), maspsx
(mkst), old-gcc (decompals), psx_psyq_signatures (lab313ru), and the
OpenPhantom project's work on the PC version.
