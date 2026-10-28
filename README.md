# Pokémon Platinum: Nintendo 3DS source port

A native 3DS port built from the [pret/pokeplatinum](https://github.com/pret/pokeplatinum)
decompilation. All of the game's C code (about 1000 files) is compiled for the
3DS's ARM11 CPU, together with the C parts of NitroSystem and NitroSDK. The DS
hardware those expect is provided by `platform/`.

No game data is included. You supply your own ROM (Platinum USA), and its file
system is extracted into `romfs/` at build time.

## Get it

Download the builder for your computer or phone from the
[releases](../../releases): it makes the game from your own Pokémon Platinum
(USA) ROM. Drop the ROM on it (or choose it in the Android app); it checks the
ROM file by file and writes `Platinum3HDS.cia` (install with FBI) and
`Platinum3HDS.3dsx` (Homebrew Launcher). `builder/README.txt` has the details
for each platform.

## Status

The whole game builds and links, both for the 3DS (`pokeplatinum.3dsx`) and for
the desktop test harness. On the harness it runs the decomp's own `main.c`
from boot: copyright and ESRB screens, the Game Freak intro, the opening
cutscene (3D logo, field scenes), the title screen with 3D Giratina, and on into
New Game with Professor Rowan's intro. With no save file, START on the title goes
straight to New Game; the Continue/options menu should appear once a save exists.

It runs on a New 2DS XL and in the Azahar emulator, at 60 fps in the opening
on the emulator.

| Part | State |
| --- | --- |
| 2D engines: VRAM banks, text/affine/extended/bitmap BGs, sprites, windows (OBJ window too), blending, master brightness | done |
| 3D: geometry engine (matrices, lighting, clipping, tests) and rasterizer (all texture formats, toon/highlight, alpha, fog/edge registers) | done |
| OS: VBlank wait drives frames; IRQ handlers, ticks, arenas, locks, RTC (host clock) | done |
| Saves: 512KB flash backup in a file (`sdmc:/3ds/pokeplat/pokeplat.sav`, written by a thread behind the game); start menu with CONTINUE | done |
| Touch screen, buttons | done |
| HBlank effects / HBlank DMA | approximated once per frame |
| Sound: music, sound effects, cries, reverb | done (microphone: stubs) |
| Wi-Fi, local wireless, GBA slot, voice chat | offline stubs |
| Soft reset | quits the app |
| Port options in X > OPTIONS: 3D EDGES (NORMAL / SMOOTH, supersampled 3D) | done (`sdmc:/3ds/pokeplat/settings.txt`) |

**3DS rendering.**
- 3D: polygons go through the port's DS geometry engine, then are drawn by the
  3DS GPU (`frontend/gpu3d_3ds.c`, citro3d) into a texture. The software
  rasterizer is kept for the desktop harness.
- 2D: composed on the GPU (`frontend/gpu2d_3ds.c`). The CPU only decodes each
  BG layer and the sprites into images, split over two cores on the New 3DS.
  The renderer then describes the DS blending rules as GPU passes
  (`Ntr_ComposeEngine` in `platform/src/ntr_render.c`): alpha test, a stencil
  holding each pixel's top layer, and blending. The passes are drawn straight
  into the framebuffer with the 3D frame as a texture. The OBJ window (battle
  move animations use it) is an image of its shape, drawn into the depth
  buffer, and passes for its inside or outside test the depth. Frames the
  passes do not cover (display modes other than graphics, brightness effects
  over blended sprites or 3D) are composed on the CPU instead.
- Pacing: 60 Hz on average rather than each frame to its own VBlank. The
  overworld runs its logic on every other frame, so one long frame is made up
  by the short one after it.
- BG layers whose inputs did not change (a hash of their registers, map, tiles
  and palettes) keep last frame's image and texture.
- ROM files stay open and are read through a cache in memory: up to 64MB on
  the New 3DS and 16MB on the Old, keeping 24MB of the heap free for the game
  (its blocks are one allocation taken at the start, so they do not fragment
  the heap).
  It holds 16KB blocks filled 8KB at a time, since the game reads small pieces
  of its archives all the time and every read is a request to the SD card (in
  Azahar a request of up to 8KB takes about a millisecond, a bigger one at
  least a frame). A thread reads files into it ahead of the game, in the time
  the game leaves free: first the small files, then the map, sound and text
  archives (`frontend/prefetch_list.h`, made by `tools/gen_prefetch_list.py`
  from test runs and the decomp's file table), so loads find their data in
  memory.
- Every file read and write, the save file's included, goes in pieces of at
  most 8KB (`Ntr_ReadPieces`), which Azahar finishes at once. At start the
  build checks that romfs finds every file of the ROM at its size (the list
  is made at build time by `tools/gen_romfs_list.py`) and mounts it again if
  not: in Azahar a few files were sometimes missing after the mount, and the
  game crashed when it came to them.
- Clock: the New 3DS runs at 804 MHz.
- Timing: hold SELECT + R to show frame times left of the top screen; the white
  line is 1/60 s.
- `tools/emu_run.sh` and `tools/emu_perf.py` run the build in the Azahar
  emulator and read the same numbers over its RPC server, including fixed
  benchmark windows of the opening. In the emulator they take 16.0 ms
  (copyright), 16.1 ms (3D logo), 16.0 ms (field scene) and 15.8 ms (sprite
  bands) per frame.
- `tools/emu_script.sh SCRIPT CSV [SAVE]` runs a test script in Azahar and logs
  every frame's times (game, 3D geometry and setup, file reads, 2D stages) to
  a CSV; `tools/frames_summary.py` lists the frames that missed 60 fps.
  `tools/emu_hostcost_run.sh` measures the CPU time Azahar itself spends per
  frame. Script lines `skip mix`, `skip geometry`, `skip 2d`, `skip prefetch`
  and `skip savewriter` switch parts off to measure them. Azahar's own speed
  readings are only meaningful while the Mac's screen is on and unlocked.
- `tools/emu_crash_hunt.sh SCRIPT OUT [RUNS] [SAVE]` plays a script until the
  game crashes and describes the crash (`tools/emu_crash_dump.py`).
- `tools/emu_battle_runs.sh OUT [RUNS]` plays the battle test script several
  times, with each run's frame summary, Azahar's exceptions and the file
  troubles `tools/emu_fs_watch.py` reads (romfs mounts, failed opens, short
  reads).

**Sound.** The game's sound code runs unchanged: the decomp's sound files,
NitroSystem's sound player and NitroSDK's sound interface, which sends
commands to the sound driver that ran on the DS's second CPU (the ARM7).
- The driver is NitroSDK's own ARM7 sound library (sequencer, envelopes,
  channel allocation), compiled a second time for the ARM7 with its symbols
  prefixed (`platform/include/ntr_snd7.h`). It runs beside the game and talks
  to it through the same PXI messages and shared memory
  (`platform/src/ntr_snd.c`).
- The DS sound hardware it programs is emulated in `platform/src/ntr_spu.c`:
  16 channels of PCM8, PCM16 and IMA-ADPCM samples, square waves and noise,
  the mixer, and the two capture units the game's reverb uses. Output is
  16-bit stereo at the DS rate, 32728.5 Hz.
- On the 3DS the driver runs on an audio thread and feeds NDSP. NDSP needs the
  DSP firmware, `sdmc:/3ds/dspfirm.cdc` (run DSP1 once to dump it); without
  it the game runs silent and says so on the bottom screen.
- The desktop harness and test scripts run the driver on the game's thread,
  a frame's share of the output each frame, so runs repeat exactly; the
  harness's `--wav FILE` writes the output.

**3D EDGES.** A row the port adds to the game's OPTIONS menu
(`patches/applications/options_menu.c.patch`; the rows are 14 pixels apart
instead of 16 to fit it). SMOOTH draws 3D at twice the size and lets the GPU
average each 2x2 block when composing (`frontend/gpu3d_3ds.c`): polygon edges
and 3D models (the opening's logo, Giratina) get smoother, but the overworld's
pixel-art textures, which rarely line up with the screen's pixels, get softer.
NORMAL is the default. The port's options live in their own file
(`game/port_settings.c`), so saves stay as the DS game writes them.

**Widescreen.** The 3DS build renders at the 3DS screen sizes, 1:1 pixels
(`Ntr_SetScreenSizes`; the harness's `--widescreen` does the same):
- the DS picture (256x192) is in the middle of each screen;
- 3D scenes drawn with the full-screen viewport show more of the world around
  it: the geometry engine widens clip space and box tests, so nothing beside the
  picture is culled;
- BG layers go on beside the picture (`ExtendLayer` in
  `platform/src/ntr_render.c`):
  - a layer that scrolls continues its own map;
  - a background (a layer that fills its edge, or a band across the screen)
    is widened where it is uniform, so its edge decoration moves to the
    screen's edge while its middle stays centered;
  - windows, text and pictures keep their DS size and place;
- sprites are drawn beside the picture too (above and below it only when they
  reach into it, as games park unused sprites just below the screen);
- windows that span the DS picture span the screen;
- the DS touch screen is the middle 256x192 of the bottom screen.

**Next:**
1. Speed: overworld frames with logic take about 20 ms in the emulator (the
   geometry engine is most of it), so busier scenes can still drop frames.
2. Check the widescreen extension screen by screen (battles, the Pokétch,
   the remaining menus).

## Layout

- `platform/include/ntr_shim.h`: stands in for `<nitro.h>`. The SDK's
  constants, types, prototypes and inline helpers are extracted from the
  NitroSDK headers at build time (`tools/gen_sdk_defs.py`).
- `platform/include/ntr_memmap.h`: the DS memory map. Registers, VRAM, palettes and
  OAM are host memory, so SDK register macros work as written; 3D command ports
  and divider/sqrt results are routed to the emulation.
- `platform/src/`:
  - `ntr_gx.c`: VRAM banks and GX.
  - `ntr_render.c`: 2D renderer.
  - `ntr_g3.c`: 3D geometry engine.
  - `ntr_raster.c`: 3D rasterizer.
  - `ntr_os.c`: interrupts, VBlank, ticks, DMA, message queues.
  - `ntr_snd.c`: the ARM7 sound driver beside the game (PXI, its clock and
    alarms); `ntr_spu.c`: the DS sound hardware.
  - `ntr_card.c`: save backup.
  - `ntr_tp.c`: touch.
  - `ntr_rtc.c`: real-time clock.
  - `ntr_crypto.c`: SHA-1, MD5, RC4.
  - `ntr_sys.c`: memory, file system, LZ77.
  - `ntr_sdk_asm.c`: the SDK's assembly routines in C.
- `game/`: replacements for `heap.c`, `game_overlay.c` and `boot.c`;
  `game/stubs/` is generated (`tools/update_stubs.sh`). The game's heaps are
  the system allocator, but freed memory is only reused by the heap that
  freed it and keeps its contents until then, as on the DS: the game reads
  some memory after freeing it (sprites crossing from Twinleaf Town to Route
  201 drew with a freed animation sequence, which crashed). The test script
  line `heap quarantine KB` fills freed memory with 0xdd instead, to find
  such reads.
- `patches/`: small changes applied to decomp/SDK sources when they are copied into
  the build:
  - Metrowerks-only constructs, made into standard C
    (`tools/make_compat_patches.py`, `tools/make_gcc_patches.py`);
  - duplicate definitions;
  - 64-bit pointer fixes.
- `frontend/`:
  - `main_3ds.c` (libctru + citro3d), `gpu3d_3ds.c` with `gpu3d.v.pica` (the
    3D engine on the GPU) and `gpu2d_3ds.c` with `gpu2d.v.pica` (composition
    on the GPU);
  - `main_host.c`: desktop harness with scripted input, PNG snapshots and
    timing; `compose_ref.c` runs the GPU composition passes in software for it.
- `tests/`: `obj_test.c` (sprites on real ROM icons) and `text_bg_test.c`
  (fast BG path vs. per-pixel reference, on the real game's frames).

## Build

Next to this folder:
- `../decomp`: pret/pokeplatinum;
- `../NitroSDK`: the ntrtwl NitroSDK reconstruction, at the commit in `sources.mk`;
- `../NitroSystem`: the matching NitroSystem reconstruction;
- `../decomp-gen`: the decomp's generated headers, made by
  `tools/setup_decomp_gen.sh`, which needs Python ≥ 3.12, clang, ninja,
  pkg-config and libpng.

The trainer AI script is assembled with any stock `clang`.

```sh
python3 tools/extract_rom.py "/path/to/Pokemon - Platinum Version (USA).nds" romfs
tools/setup_decomp_gen.sh
```

3DS (needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `3ds-dev`):

```sh
make            # -> pokeplatinum.3dsx, run with the Homebrew Launcher
make meta       # HOME menu icon, banner and jingle (needs the desktop harness)
make cia        # -> Platinum3HDS.cia, install with FBI or similar
```

`make meta` plays the game to its title screen on the desktop harness and
makes the art (`tools/make_app_meta.sh`): three seconds of the title music as
the banner jingle, and for the icon and banner either pictures you put in
`art/` (`cover.png`, square, averaged down to the 48x48 icon; `logo.png`,
with transparency, over a darkened band of the cover for the banner:
`tools/make_app_art_from.py`) or else the 3D Giratina of the title screen.
Nothing of the game is kept in the repository, the `art/` pictures included. `make cia` needs
[makerom](https://github.com/3DSGuy/Project_CTR) and
[bannertool](https://github.com/carstene1ns/3ds-bannertool) in `../tools-host`
(or set `MAKEROM` and `BANNERTOOL`); its settings are in `meta/app.rsf` (80MB
on the Old 3DS, 124MB and 804 MHz on the New 3DS).

Desktop test harness (wasm32, needs wasi-sdk and wasmtime in `../tools-host`):

```sh
make -f Makefile.host
mkdir -p out/run
tools/run_host.sh build-host/pokeplat_host.wasm romfs out/run --frames 1800 --every 100 \
    --press START@400 --press START@1000 --press A@1300
python3 tools/contact_sheet.py out/run/sheet.png out/run 300 1000 1500
make -f Makefile.host obj-test bg-test
```

Add `--render-all` to time rendering on every frame.

The builder (`builder/`, Go): the release's way to make the game from a ROM.
It holds the port's program packaged without the game's files (made by
`make builder-templates`: a CIA and a 3DSX with a placeholder RomFS,
original banner art, and the list of the game's files with their SHA-1).
Given a ROM, it reads the DS file system, checks every file against the list,
builds the RomFS (level 3 for the 3DSX; under its IVFC hash tree in the CIA's
NCCH) and redoes the hashes and sizes it changes: the NCCH's superblock
hashes, the ExeFS icon hash, the TMD's content record. The HOME menu icon is
made from the ROM's own DS icon.

```sh
make builder-templates     # builder/templates/ (needs romfs/ and makerom, bannertool)
tools/make_builder.sh      # dist/: macOS (arm64, x64), Linux (x64, arm64), Windows (x64, arm64)
tools/make_android.sh      # dist/: the Android app (needs the JDK and Android SDK in ../tools-host)
```

The Android app (`android/`) is built without Gradle (aapt2, javac, d8,
apksigner). It runs the same builder program, packaged as its native library
(Android/arm64, and Linux/x86-64 for emulators), on a copy of the chosen ROM.
Its signing key is made in `../tools-host/android-keystore` the first time;
later versions must be signed with the same key to install over it.

## Testing

Input scripts (`tests/scripts/*.txt`, format in `frontend/input_script.c`) play
the game with fixed input and a deterministic clock, so a run repeats exactly:

- `new_game.txt`: intro, naming, bedroom, X menu, walking;
- `save.txt`: saves in the bedroom;
- `continue.txt`: the start menu with a save, CONTINUE;
- `menus.txt`: bag, trainer card, options;
- `opening.txt`: the opening's 2D/3D scenes and the title;
- `outside.txt`: walking and running around Twinleaf Town, and `battle.txt`:
  Barry's house, Route 201, Rowan's briefcase and the first battle (both from
  `tests/saves/twinleaf_outside.sav`).

The desktop harness runs one with `--script FILE` (and `--wav FILE` for the
sound). Add `--widescreen
--gpu-compose` to compose as the 3DS does, through the GPU passes run in
software; it prints any frame that falls back to the CPU compositor and why.
The 3DS build runs the same script when `sdmc:/pokeplat_script.txt` exists
(starting from `sdmc:/pokeplat_test_save.sav` if present). It saves the
framebuffers of the requested frames to `sdmc:/pokeplat_dumps/` and then quits.

`tools/geo_check.sh` and `tools/compose_check.sh` snapshot 3D-heavy and
2D-heavy scenes on the harness (CPU and GPU-pass composition) and compare
them with a reference, so changes meant to keep the output can be checked.

Compare the two with these tools:
- `tools/raw_to_png.py` converts the 3DS dumps to PNG;
- `tools/png_diff.py --tol 8` diffs a pair;
- `tools/side_by_side.py` puts two frames next to each other.

The 3DS frames match the harness's `--gpu-compose` frames within the tolerance
(the GPU blends in 8 bits). 3D frames also differ by sub-pixel edge and texel
rounding between the GPU and the software rasterizer. Without `--gpu-compose`,
the harness composes on the CPU (5-bit color math as written for the DS); the
two agree within `--tol 8`.
