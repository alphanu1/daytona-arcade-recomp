# Dreamcast asset recipes

Everything the recommended Dreamcast-native build uses that comes from the
game (asset set, generated headers, the soundtrack in every asset set) is
rebuilt here from your own ROM set and vgmrips' soundtrack pack by one
command. Nothing ROM-derived is in the repository; this directory holds
only parameters: specifications, option lists, texture header words, ROM
addresses, controller inputs and the expected sha256 of every output.

The scripted benchmark races on all three circuits and the attract loop, with their latest hardware results, are
in [FULL_COURSE_BENCHMARKS.md](FULL_COURSE_BENCHMARKS.md).

## How to rebuild from your own ROM

Needs: this repository with `extern/` fetched (`python3 scripts/setup.py` or
`./setup.sh`; the recipe configures its own host build with
`-DM2_DREAMCAST_NATIVE=ON`), CMake and a C++20 compiler, `python3` with numpy and Pillow,
and the KOS/enDjinn Dreamcast toolchain (`pvrtex`; `sh-elf` for the ELFs).
`ENDJINN_ROOT` defaults to `extern/endjinn` (`platform/dreamcast-native/scripts/fetch_endjinn.sh`); SH4ZAM
comes from `platform/dreamcast-native/scripts/fetch_sh4zam.sh` (`extern/sh4zam`).
The ROM set is MAME's `daytona93` (`.zip` or `.7z`); the sound ROMs are not
needed. The music comes from vgmrips' pack (see "Music" below: a browser
download, AICAflow and AICAforge from `platform/dreamcast-native/scripts/fetch_aicaflow.sh`, zlib
headers, `unzip`). From the repository root:

```sh
platform/dreamcast-native/scripts/fetch_endjinn.sh         # once: enDjinn at its pin into extern/endjinn
platform/dreamcast-native/scripts/fetch_sh4zam.sh          # once: SH4ZAM at its pin, built into extern/sh4zam/lib
platform/dreamcast-native/scripts/fetch_aicaflow.sh        # once: AICAflow + AICAforge at their pins into extern/aicaflow
zsh platform/dreamcast-native/recipes/build_assets.sh roms/daytona93.zip build/my-assets \
    --music ~/Downloads/Daytona_USA_\(Sega_Model_2\).zip --elf
```

One of `--music ARCHIVE` and `--no-music` is required (a rerun of an output
directory whose step `music` is stamped needs neither). `--no-music` builds
the sets without `music/` (the game runs silent) and reports their music rows
of `expected.sha256` as one line instead of "not built" each.

About 10 minutes on an M-series Mac (measured 14 October: 9 1/2 minutes
with both variants and `--elf`; host tools from scratch about 2 1/2, the two
scripted-race traces about 2 1/2 in the background, exports 2, packs 4,
ELFs 20 seconds). The output directory (keep it under the ignored `build/`):

| Output | What |
| --- | --- |
| `assets-ssfar4f/` | the recommended asset set since 15 October, evening (the chosen default; `ENJ_ROMDIR` at build time, `kos-tool -m` at upload): ssfar4eu with the plain catch fence as the arcade defines it (`specs/ssfar4f.json` = `specs/ssfar4eu.json` without the rule "fence" and, since 16 October, with the rule "verges" holding only 6686): the 277 stipple panels at full checker density at every distance, no three-level fence groups and no quarter-stipple texture; every other file as ssfar4eu's. The ELF default `NATIVE_FENCE_SWEEP=0` goes with it (the sweep acts only on fence groups). Hardware against ssfar4eu, same code, 2 runs each: render 9.61/9.60 vs 9.56/9.56 ms, race registration 7.64/7.64 vs 7.68/7.69, podium 10.15/10.14 vs 10.29/10.29, late frames 3/3 vs 4/4 (those runs are of the set before 16 October). Since 16 October (project decision: the grass is not super-sampled) the infield grass 6924 and the grey concrete aprons 6664 are no longer super-sampled: they stay at their plain level at every distance (bilinear, no mip chain, as every road-surface word under `--mip-skip-surface`); of the rule "verges" only the white track-edge strip 6686 keeps its super-sampled far level; only world.dnm and world.dnt changed. Since 17 October the slot machine's spinning reels (strips 2592, 2720, 2848, 192 faces) are super-sampled beyond 3x their one-texel-per-pixel depth plus their radius (rule "reels", the signs' switch; 52.4 units at focus 280), by instance (`"group": "instance"`: the switch depth in the reels' three materials, the renderer sets the TSP bit on their headers per reel; no face, group or binding added): world.dnt is the one before the rule, world.dnm differs from that one in those three words |
| `assets-ssfar4eu/` | the set before it (three-level fence): ssfar4e + the rest of the sign sheet super-sampled with the same rule (rule "picks" of `specs/ssfar4eu.json`: the slot machine reel frame 2084 next to the casino board, the marquee 3608 with its coplanar layer 3864, the A&R Magical House 3096, the coin 3224, the leader board's lamp digits 3088/3090/3216/3218/3344/3472) and no mip chain on any super-sampled sign word (rules board, signs, picks; the video screens 2584/2588/2840 keep theirs, as in ssfar4e) |
| `assets-ssfar4e/` | the set before that: ssfar4cf3r + the rubber streaks kept on or above the road (`prepare_face_order.py --drape-road-decals`, `tools/road_drape.py`: corners of the 68 streak faces that dip under the road's creases raised, streaks 2 layers above the asphalt instead of 240, so the cars' shadows fall on them) + 16 readable signs super-sampled beyond three times their one-texel-per-pixel distance plus their radius (rule "signs" of `specs/ssfar4e.json`; predicted PVR +0.02 ms a frame) |
| `assets-ssfar4cf3r/` | the set before ssfar4e: ssfar4cf3 + opaque cores over Sonic (2056) and the rock ivy (6424) and their punch-through faces trimmed to the fringe around them (no texel or palette changed; chosen 15 October) |
| `assets-ssfar4cf3/` | the set before it: ssfar4c with the three-level catch fence (dense stipple, quarter stipple, nothing; chosen 14 October after hardware runs) |
| `assets-ssfar4c/` | ssfar4b without mip chains on the forest floor (6428) and stand glazing (3858, 7064); two-level fence |
| `assets-ssfar4b/` | the set before that (far-road super-sampling, gantry board) |
| `assets-ssfar4d/`, `assets-ssfar4bf3/` | with `--variants ssfar4d,ssfar4bf3`: ssfar4c + Sonic rock and rock moss baked into opaque faces (not chosen); ssfar4b + the three-level fence (with the floor and glazing mip chains) |
| `assets-ssfar4cf0/` | with `--variants ssfar4cf0`: a benchmark, not a candidate: ssfar4cf3 with no fence rule, every stipple panel drawn at full density at every distance |
| `assets-*/music/` | in every set (not with `--no-music`): `tNN.afb` and `tNN.afx` of the 45 tracks (the step `music`, `build_music.sh` with its defaults; the files the ELF reads, `include/audio/sound.h`); the full output of `build_music.sh` (with `tNN.afc`, `tNN.afv`, `tracks.tsv`) is in `work/music/` |
| `assets-*/select/` | in every set: the course/transmission select and GENTLEMEN screens (the steps `export_select` and `select_pack`: `m2segment --export-native-select` on `scripts/inputs/race_to_end.txt`, `course_advanced.txt`, `course_expert.txt`, then `prepare_select.py`): `select.dsl` and `tex-N.dt` (PAL4), read by `code/render/ui/select_render.c`; the step checks the native picture against the original's screens (`tests/test_select_render.py`) |
| `assets-*/course2.*` | in every set since 17 October: the Advanced course's physics and visibility packs (`course2.dnc`, `course2.dnr`, `course2-route0..7.dnp`, `course2.dnv`); its world, sky and HUD map are not made yet |
| `headers/` | the generated header directories of the ELF build; `make-vars.txt` lists the make variables (since 17 October `race-start-data` and `podium-data` hold courses 0 and 2, `hud` the Advanced map, `script-advanced` the race script of the Advanced time-out run) |
| `assets-*/course2*` | in every set since 17 October: the Advanced course's packs (`course2.dnc`, `course2.dnr`, `course2-route0..7.dnp`, `course2.dnv`), its world (`course2-world.dnm`, `course2-world.dnt`) and sky (`course2-sky.dns`); since 18 October the frames of its eight frame-counter animations (`course2-actors.dna`, `include/clock_actors.h`) |
| `elf/stats`, `elf/stats-manual`, `elf/controller` | with `--elf`: the default stage 6 ELFs of this checkout (scripted fast race with render statistics and drop log; the same for the manual race, `headers/script-manual`; pad input), `loaded.sha256` |
| `elf/stats-advanced` | with `--elf`, since 17 October: `NATIVE_COURSE=2`, the Advanced time-out run of `headers/script-advanced` with checkpoints (expected rows: `work/script-advanced/expected-checkpoints-advanced.txt`); with its world, sky and map |
| `elf/controller-advanced` | with `--elf`: the controller ELF whose first race is on Advanced (`NATIVE_COURSE=2`; the course is a run-time choice) |
| `assets-*/course1*` | in every set since 18 October: Expert's packs (`course1.dnc`, `course1.dnr`, `course1-route0..7.dnp`, `course1.dnv`), its world (`course1-world.dnm` 2.46 MB, `course1-world.dnt` with the lumas of its live specular faces) and sky (`course1-sky.dns`) |
| `assets-*/music/adpcm/` | in every set since 18 October (not with `--no-music`): `t04` and `t05`, the race pieces of Advanced and Expert, in ADPCM (the step `music_adpcm`, `afx_vgm --adpcm`): their PCM8 bank and flow do not fit the AICA RAM beside the effects' race bank, and `code/audio/sound.c` loads this copy when the PCM8 one does not |
| `elf/stats-expert`, `elf/controller-expert` | with `--elf`, since 18 October: `NATIVE_COURSE=1`, the Expert time-out run of `headers/script-expert` with checkpoints (expected rows: `work/script-expert/expected-checkpoints-expert.txt`), and the controller ELF whose first race is on Expert |
| `elf/stats-advanced-finish`, `elf/stats-expert-finish` | with `--elf`, since 18 October: `NATIVE_COURSE=2` / `1`, the 3rd-place races of `headers/script-advanced-finish` / `script-expert-finish` with checkpoints through the podium and the results (expected rows: `work/script-advanced-finish/expected-checkpoints-advanced-finish.txt`, `work/script-expert-finish/expected-checkpoints-expert-finish.txt`) |
| `elf/play`, `elf/play-heap-trace` | with `--elf`: the game from the attract (every course raced from the select; the unattended attract cycles the courses 0, 2, 1, each next one loaded in the title's frames); the same with `NATIVE_HEAP_TRACE=1` (each named allocation and the heap after each scene load, to compare with `tools/heap_budget.py --session`) |
| `elf/stats-attract` | with `--elf`, since 18 October: the attract from power-on with render statistics and the drop log (`NATIVE_ATTRACT_LOOPS=3`): the cinematics of courses 0, 2 and 1 with their titles, then the run ends; the console's `Native scene load attract`, `Native title` and `DIAG` lines |
| `outputs.sha256` | every output's sha256; the script compares them with `expected.sha256` and prints the differences |

The world packs of every set carry the slot machine's reels (`prepare_world_objects.py`: 64 reel models of 3 faces,
three PLACED instances, kinds SLOT..SLOT+2; since 15 October).

World packs have face groups of 8 faces since 15 October (`--face-group N`, default 8; 32 before): on hardware
race registration -0.40 ms against 32 (16: -0.26, 4: -0.29), the same picture (stream reference passes).

A rerun with the same output directory skips the finished steps
(`stamps/`); delete a stamp to redo a step. `--host-build build` uses an
existing CMake build of this checkout instead of building `host/` (its
`m2segment` must know `--dump-video`), `--check` runs the Dreamcast `check`
target with the ELFs, `--keep` keeps the two large traces.

To build other ELF variants against the outputs, pass the variables of
`make-vars.txt` to `make -C platform/dreamcast-native` (see
`platform/dreamcast-native/HANDOFF.md`, "Build and hardware").

## Disc image

A bootable `.cdi` of the game (the `elf/play` build: attract, credit, select, every course, music and effects) that
runs from a disc without dcload. enDjinn's image target (`base_link.mk`, `$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi`) runs
`mkdcdisc` with the ELF as `1ST_READ.BIN` and the asset set (`ENJ_ROMDIR`) at the disc's root; `make cdi` names it.
From the repository root, after a recipe run (here `build/dc-native-final.recipe`), with mkdcdisc built in
`../mkdcdisc/builddir`:

```
source ../enDJinn/environ.sh
PATH=$PWD/../mkdcdisc/builddir:$PATH make -C platform/dreamcast-native ENDJINN_ROOT=$PWD/../enDJinn \
  $(grep -v '^#' build/dc-native-final.recipe/make-vars.txt) \
  NATIVE_RESIDENT_CAR=1 NATIVE_FRAME_PROFILE=1 NATIVE_HANDOPT=1 RACE_TICK_LIMIT=0 AUTO_START=attract \
  NATIVE_DISC=1 ENJ_BUILDDIR=$PWD/build/dc-native-disc/obj ENJ_BINDIR=$PWD/build/dc-native-disc/bin -j8 cdi
```

The image is `build/dc-native-disc/bin/daytona-native-driving-3d-textured-world-scenery6-filterall-disc-resident-car-handopt-pal1555-smallcar12.cdi`
(about 56 MB; the ELF beside it). It holds ROM-derived data: it stays in `build/`, never committed. `obj/` can go
afterwards.

- `NATIVE_DISC=1` reads the assets from `NATIVE_ASSET_ROOT`, default `/cd/` (a dcload build keeps `/pc/`; its
  binary does not change). The diagnostics that write files to `/pc` (`NATIVE_CAPTURE`, `NATIVE_DIAG_LATE_CAPTURE`,
  `NATIVE_SAMPLE_PROFILE`, `NATIVE_FRAME_COMPARE`) are refused. The console is dropped (KOS would write it to the
  serial port, about 10 ms a long line); `NATIVE_DISC_CONSOLE=1` keeps it (a serial cable, Flycast's serial console).
- Paths: mkdcdisc writes ISO 9660 with Joliet (no Rock Ridge); KOS's `fs_iso9660` takes the Joliet names and compares
  them without case, so `course1-actors.dna` and `music/adpcm/t04.afb` open as they are (no renaming; the longest
  name is 18 characters, the tree two directories deep).
- Reads: a GD-ROM read blocks only the thread that makes it (KOS `cdrom.c`: the DMA's end is a semaphore wait,
  `fs_iso9660.c` holds only its own mutex). On a disc the scene loader's reads run outside its slices
  (`code/scene_loader.c`), and the music's file calls run on a worker thread (`code/audio/sound.c`, "From a disc"):
  a seek of the drive is longer than any idle wait.
- Hardware: a CD-R burned from the `.cdi` (DiscJuggler image, data track in the second session: a console that
  boots MIL-CD/CD-R discs, i.e. not the last revisions) or an optical drive emulator (GDEMU, MODE, USB-GDROM) that
  takes `.cdi`. The image is not padded (`--no-padding`): on a CD-R the data sits at the inner edge, the slowest part.

## Music (AICAflow)

The soundtrack is not in the ROM set: it comes from vgmrips'
[Daytona USA (Sega Model 2)](https://vgmrips.net/packs/pack/daytona-usa-sega-model-2)
pack, 45 VGZ logs of the sound board's two MultiPCMs (no YM3438 writes in any of
them). vgmrips puts a proof-of-work bot check in front of its files, so download
the zip in a browser; the script accepts only the archive with sha256
`ac086fd5...` (see `build_music.sh`). `build_assets.sh --music ARCHIVE`
runs it as its first step (`work/music/`, stamped) and copies the AFB/AFX
pairs into every asset set; on its own, from the repository root:

```sh
platform/dreamcast-native/scripts/fetch_aicaflow.sh        # once: AICAflow + AICAforge into extern/aicaflow
zsh platform/dreamcast-native/recipes/build_music.sh ~/Downloads/Daytona_USA_\(Sega_Model_2\).zip build/dc-native-music
```

About 15 seconds. AICAforge's `afx_vgm` turns each track into `music/tNN.afb`
(its samples, PCM8) and `music/tNN.afx` (the AICA control flow bound to that
bank), with `tNN.afc` seek checkpoints and `tNN.afv` visualisation data;
AICAflow's validator checks every AFX, and `outputs.sha256` is compared with
`music-expected.sha256`. `music/tracks.tsv` lists numbers, titles and sizes.

| | PCM8 (default) | `--adpcm` |
| --- | --- | --- |
| all 45 banks | 21,929,818 bytes | 10,971,718 bytes |
| largest bank (05 Sky High) | 1,576,173 bytes | 788,647 bytes |
| all 45 control flows | 1,179,267 bytes | the same |

AICA RAM is 2 MiB, shared with the firmware, the sound effects and any DSP
delay ring. On hardware AICAflow reports 2,072,768 bytes of AICA RAM for
assets ("Sound: AICAflow up, ..."). `code/audio/sound.c` keeps one recording
resident (a change stops, recycles, frees bank and flow, then loads the next),
so the largest bank plus its flow must fit: the four race pieces (the looping
ones) are 03 The King of Speed 1,561,766 bytes (AFB 1,307,085 + AFX 254,681),
04 Let's Go Away 1,616,233, 05 Sky High 1,818,831 (the largest of all 45) and
06 Pounding Pavement 969,417, so each fits, Sky High with about 250 KB to
spare for effects later. Two at once or a preload of the next piece does
not fit in PCM8; `--adpcm` halves the banks (Sky High 788,647). On the SH4
side a load holds the whole AFX (`malloc`, up to 254,681 bytes for 03) and
AICAflow's 65,568-byte DMA buffer only while it runs; `tools/heap_budget.py`
does not model them (with assets-ssfar4f and the damaged bodies the heap
keeps 3,579,208 bytes never taken at the peak, 3,641,224 free while racing).
`--adpcm` takes about 3 minutes and does not match the expected hashes. The logs give the MultiPCM clock as 8 MHz and `afx_vgm` derives the
sample rate as clock/180 (44,444 Hz); MAME's board now runs them at 10 MHz/224
(44,643 Hz), so the converted music is about 8 cents flat of it.

The ELF plays them from `ASSET_ROOT/music/` (`include/audio/sound.h`); every
set of `build_assets.sh --music` has them. For a set made with `--no-music`
(or an older one), link the output of `build_music.sh` in, e.g. `ln -s
$PWD/build/dc-native-music/music build/my-assets/assets-ssfar4f/music`.
Without it the game is silent ("Sound: cannot load ..." at the first music
start). Which
command starts which recording is `include/audio/music_map.h`;
`platform/dreamcast-native/tools/check_music_map.py ROMS.zip ARCHIVE.zip WORK_DIR`
proves every entry from the ROM set's sound program and the pack (4 seconds).

## Sound effects (AICAflow)

The effects and voices come from the ROM set itself: the sound board's 68000
program (MAME's daytona93 set carries Revision A's, epr-16720/16721) and the
MultiPCM sample ROMs mpr-16491..16494. `build_assets.sh` runs `build_sfx.sh`
as the step `sfx` when ROM is a `.zip` (about 30 s) and copies `sfx/race.afb`
and `sfx/race.dsx` into every asset set; on its own:

```sh
zsh platform/dreamcast-native/recipes/build_sfx.sh roms/daytona93.zip build/dc-native-sfx --music build/dc-native-music/music
```

`tools/prepare_sfx.cpp` plays every effect command the game can send through
the native sequencer (`src/runtime/native_sound_sequencer.*`, the desktop
backend's, compared note by note with the reference board) from the state the
original's boot bytes leave: the sequences of banks 0x20..0x22 (`ae20xx`..
`ae22xx`) and the 128 notes of channel 14 (`9exx`), 443 that sound. Each
becomes a one-chip MultiPCM VGM with its own small sample ROM, so AICAforge's
`afx_vgm` lowers it exactly as it lowers the music. MultiPCM samples always
loop: a sample whose loop is silent is a one-shot and is keyed off where its
loop begins; the kit's percussion notes end at the driver's durations. The
ten engine layers of the driver's `update_engines` (six rpm layers, the
nearest opponent left and right, slip, skid) become controlled flows (`park`:
END becomes PARK) and `engine.tbl` holds their AICA pitch, level and pan words
for every controller value, from the driver's tables with `afx_vgm`'s formulas.
`sfx-banks.txt` chooses what is resident (and PCM8 or ADPCM), `afx_bank
--merge` makes one bank of it, `prepare_sfx pack` the `.dsx` that
`code/audio/sfx.c` loads (directory, velocity gains, engine table, flows).

| bank race | |
| --- | --- |
| content | engine layers (PCM8), the channel-14 kit 9e01..9e2d (ADPCM), THREE TWO ONE GO, crowd, slot machine |
| flows / AFB | 62 flows, 481,486 bytes of samples |
| AICA RAM | 485,984 bytes (32-byte allocations); 25,050 left beside the race recording t03 (1,561,734) |
| SH4 heap | about 17 KB kept (`tools/heap_budget.py` models it) |

The announcer's phrases beyond the countdown: the 50 a race can play (`sfx-banks.txt`, line `voices`;
588 KB of ADPCM samples) do not fit beside the PCM8 race music. Two options, measured on 17 October 2026
(AICA arena 2,072,768 bytes; SH4 heap: the stats ELF's `make check`, budget 3,145,728 free while the scene
runs):

| | streamed (`--voices stream`, `NATIVE_SFX_VOICE_STREAM=1`) | resident (`--voices resident`, music in ADPCM) |
| --- | --- | --- |
| race music | t03 PCM8, 1,561,734 bytes | t03 ADPCM (`build_music.sh --adpcm`), 908,544 bytes; the whole soundtrack in ADPCM (t01, t06, t11 in PCM8 no longer fit beside the bank) |
| AICA RAM | race bank 485,984 + the playing phrase (at most 23,904): 1,146 left | race bank with the phrases 1,106,208: 58,016 left beside t03 |
| SH4 heap free | 3,264,736 (directory, the 24 KB phrase buffer, an 8 KiB worker stack) | 3,306,872 (as without phrases: 3,307,536) |
| G2 / CPU | per phrase: the read (from /cd a worker thread below the game's priority; from dcload's /pc, whose calls hold the CPU, the main thread in its idle wait for the PVR, one chunk a frame sized to the shortest recent wait), then the samples by chunked G2 DMA over a few updates, never waited for | none in the race (1.1 MB more at start-up) |
| timing | a phrase starts once read and uploaded (the log reports its start latency in updates); one at a time, two waiting at most (the original can overlap two) | as the original |
| quality | voices ADPCM, music PCM8 | voices ADPCM, music ADPCM (4-bit: noise on cymbals and quiet passages) |

`build_sfx.sh` fails when the largest streamed phrase does not fit beside the race bank and t03, and when the
race bank leaves less than 4 KiB. `sfx-expected.sha256` holds the default (`--voices stream`) outputs' hashes.

The game's side: `include/race_sound.h` reproduces the commands the original
derives from gameplay state (engine rpm `be17`, slip `be19`, the start voices,
the lap award's cues, the resets); `tests/trace_race_native.c --sound` writes
the native race's commands and `tools/compare_race_sound.py` compares them with
the original's UART bytes (`m2segment --trace-sound`): the step `race_script`
requires engine, slip, announcer and the kit's event-tail notes to match the
fast race in order.

## Files here

| File | Used for |
| --- | --- |
| `build_assets.sh` | the driver |
| `build_music.sh` | the soundtrack: vgmrips pack to AICAflow AFB/AFX (see Music above); `build_assets.sh --music` runs it |
| `build_sfx.sh`, `sfx-banks.txt`, `sfx-expected.sha256` | the sound effects and announcer phrases: ROM set to AICAflow banks and `voices.pak` (see Sound effects above); `build_assets.sh` runs it |
| `key_pack.py` | binding-key DNT4 packs (world, cars): the bindings `prepare_palette_materials.py` and `prepare_face_order.py --materials` read, without texture data |
| `world-dnt4-resources.txt` | per world material header the texture resource number of the historical world DNT4 (face order ranks) |
| `world-object-models.txt` | model addresses drawn by the arcade outside the cells and car lists (`prepare_world_objects.py lists --extra-models`) |
| `opaque-cores/th2.json`, `far1.json` | tree shapes for `tools/opaque_cores.py` (near and far level of the trees) |
| `opaque-cores/rock-decals.json` | ssfar4cf3r's overlay cores for Sonic (fringe in pieces) and the rock ivy (fringe hull), no texel filled |
| `specs/L12.json`, `specs/ssfar4b.json`, `specs/ssfar4bf3.json`, `specs/ssfar4b-fence-plain.json`, `specs/ssfar4bf3-rock.json`, `specs/ssfar4e.json`, `specs/ssfar4eu.json`, `specs/ssfar4f.json` | level-of-detail rules (`tools/lod_groups.py`): trees, fence; ssfar4b adds the far-road super-sampling and the gantry board; ssfar4bf3 has the three-level fence of L15 (ssfar4cf3's); ssfar4b-fence-plain is ssfar4b without the fence rule (ssfar4cf0's); ssfar4bf3-rock is ssfar4bf3 with the rock decals' cores added to its `cores` list (ssfar4cf3r's); ssfar4e is ssfar4bf3-rock with the rule "signs" (banners, pit-wall boards, the other lettering faces: per face, the super-sampled far level beyond 3x the depth of one texel per pixel plus the face's radius, as the gantry board); ssfar4eu is ssfar4e with the rule "picks" (the same switch; ssfar4eu's); ssfar4f is ssfar4eu without the rule "fence" (ssfar4f's: the plain fence) and with the rule "verges" reduced to 6686 (16 October: the infield grass 6924 and the aprons 6664 not super-sampled) and the rule "reels" (17 October: the slot machine reels, the signs' switch) |
| `decals/rock-shared.json` | ssfar4d's baked composites (`prepare_face_order.py --bake-decals`): rock with Sonic, rock with moss, one palette bank |
| `inputs/inputs-finish-fast.txt` | controller script of the second-place 8-lap race (made by `tests/drive_race_native.c`) |
| `inputs/inputs-expert-timeout.txt` | Expert (slot 1): the menu of `recipes/inputs/course_expert.txt` (steering 0xe0 held through the accelerator confirm), then full accelerator and steering 0x98 to the time-out (frame 8371) and the results (9100 frames; the reference of the step `course1_lockstep`) |
| `inputs/inputs-advanced-finish.txt` | Advanced: the same menu, then a race searched by `tests/drive_race_native.c --pursuit --launch 0xa8 --course 2 --counter-base 2116` (first frame 2331): 4 laps to 3rd place, the podium and the results (17800 frames; the reference of the step `course2_finish`) |
| `inputs/inputs-expert-finish.txt` | Expert: the same menu, then a race searched by `tests/drive_race_native.c --pursuit --launch 0xa8 --course 1 --counter-base 2156` (first frame 2372): 2 laps to 3rd place, the podium and the results (19100 frames; the reference of the step `course1_finish`) |
| `course1-surface.txt`, `course2-surface.txt` | the reduced view's road-surface headers of Expert and Advanced (none audited: `--surface-ground`) |
| `inputs/inputs-advanced-timeout.txt` | Advanced (slot 2): the menu of `recipes/inputs/course_advanced.txt` (steering 0xa0 held through the accelerator confirm), then full accelerator and steering 0x98 to the time-out and the results (8000 frames; the reference of the step `course2_lockstep`) |
| `inputs/inputs-damage-{A,C,E,F,G,H}.txt` | the fast race's script to frame 3149, then steering/braking into the walls and against the traffic to frame 7600 (time up in lap 1): the original's runs of `tests/test_damage_lockstep.py` (`m2segment ROM 7600 --inputs ... --trace-race reference-damage-X.trace`) |
| `inputs/frontend-{A,B,C,D,E}.txt` | credits at different points of the cinematic attract and different course/transmission choices, timeouts and confirms: the original's runs of `tests/test_frontend_lockstep.py` (`m2segment ROM N --inputs ... --trace-frontend frontend-X.trace --trace-race race-X.trace`; A, B, E with a race) |
| `inputs/inputs-prop-{advanced,expert}.txt` | the finishing race's inputs to the road before a prop of kind 2230ac, then `tests/drive_race_native.c --target` into it at speed index 54: its slow knock set, which no race reaches (the step `props_slow`: `m2segment ROM N --inputs ... --trace-race R --trace-props RUN.bin`, `tests/test_props.c`) |
| `inputs/games-{A..J}.txt` | a game started in the title or the V.R. demo, and two games on the same or another course, with the credit in the cinematic, the ranking overlay, the demo or the title, one after a podium and a name entry: the original's runs of `tests/test_games_lockstep.py` (`m2segment ROM N --inputs ... --trace-frontend frontend-X.trace --trace-race race-X.trace --trace-attract`) |
| `expected.sha256` | sha256 of the asset sets and headers as delivered on 14 October 2026 (assets-ssfar4cf3 added that night, assets-ssfar4cf3r, assets-ssfar4e, assets-ssfar4eu and assets-ssfar4f on 15 October; car-damage.dnb and the music/ rows of every set that night) |
| `music-expected.sha256` | sha256 of the default outputs of `build_music.sh` (AICAflow c6b116dd and f448121, AICAforge e77de852) |

## Dependency graph

`M` = `m2segment ROM FRAMES`, `I` = `--inputs inputs/inputs-finish-fast.txt`,
`T` = `platform/dreamcast-native/tools`. Every step runs a committed tool.

```
ROM .zip --m2import_silent--> rom/daytona93/*.bin (R)
R/program.bin, tgp_program.bin --m2recomp, m2tgprecomp--> host/gen --CMake--> m2segment, check_native_*

Exports (m2segment):
  M 240 --export-native-cells --race-scenery-lights      -> cells240
  M 300 --export-native-cells --race-scenery-lights      -> cells300
  M 528 --export-native-race-cars                        -> race-cars
  M 4080 I --export-native-player-car                    -> player4080
  M 4080 I --export-native-models --dump-video           -> models4080, video (palram, colorxlat, lumaram, tex0, tex1)
  M 240 --export-native-{name,result,finish}-models      -> name-models, result-models, finish-models
  M 240 --export-{result,name}-label-colors              -> result-colours.txt, name-colours.txt
  M 240 --export-native-sky                              -> sky/panorama.tga
  M 3120 --export-native-2d                              -> title
  M 300 --export-native-objects lists.txt                -> objects300
  M 13300 I --trace-hud 250                              -> hud-trace
  M 17500 I --trace-race                                 -> race.trace

ROM-only packs and headers:
  T/prepare_drive R                      -> drive.dnd
  T/prepare_contact R 0                  -> course0.dnc
  T/prepare_road R 0                     -> course0.dnr
  T/prepare_path R 0 0..7                -> course0-route0..7.dnp
  T/prepare_visibility R 0 cells240      -> scenery.dnv
  T/prepare_intro R --race-cars          -> intro_drive_data.h (40 cars)
  T/prepare_intro R --podium | --race-start            -> podium_data.h, race_start_data.h (with the pit tables and
                                                          the crew's motions, model sets and hierarchy)
  T/prepare_race_records program.bin --name-output     -> race_record_data.h, name_entry_data.h
  T/prepare_race_intents program.bin                   -> race_intent_data.h
  T/prepare_result_actor program.bin                   -> result_actor_data.h

Cars:
  T/prepare_models race-cars --race-cars R --share-positions --align-tables      -> car models (DNM2)
  T/prepare_indexed_materials race-cars                                           -> DNT5
  key_pack.py dnt5                                                                -> car keys (DNT4)
  T/append_player_parts (models, keys) player4080 models4080 R video intro_drive_data.h --effects race-cars --crew race-cars
                                                       -> car.dnm + car effects (dust, sparks, smoke, spin smoke: 82 models, include/car_effects.h)
                                                          + the pit crew (26 models, include/pit_crew.h)
                                                          + glass/shadow, keys + glass, race-header/intro_drive_data.h
  T/prepare_palette_materials keys video                                          -> car.dnt (DNT6)    [asset set]
  T/prepare_face_order --intro-header --materials car.dnt --match-decal-diagonals --strip-scope material -> sorted car.dnm
  T/prepare_restrip sorted car.dnt --car-levels race-header --keep-sources 0x8d9e42 0x8d7ab7 -> car.dnm [asset set]
  T/prepare_damage_bodies race-cars R car.dnm   -> car-damage.dnb [asset set] (the player's 16 damaged bodies as
                                                   position/normal variants of the clean body's mesh)
  T/prepare_damage_bodies race-cars R car.dnm --hoods -> car-hoods.dnb [asset set] (the hood view's three damaged
                                                   hoods 231eb0[0][1..3] and the clean one as variants of its mesh)

World:
  T/prepare_models cells240 --world-cells --face-group-size 32 --align-tables --share-positions --collapse-flat-cutouts
                                                                                  -> world-cells.dnm
  key_pack.py cells cells240 world-dnt4-resources.txt                             -> world keys (DNT4)
  T/opaque_cores opaque-cores/th2.json | far1.json video world-cells.dnm          -> specs/near-th2.json, far-far1.json
  specs/L12.json, specs/ssfar4b.json (reference near-th2/far-far1)
  T/prepare_world_objects lists R --extra-models world-object-models.txt          -> lists.txt
  T/prepare_world_objects sources R objects300 cells300                           -> sources
  T/prepare_models sources --world-cells --world-objects (same options as above)  -> world-src.dnm
  T/prepare_face_order world-src.dnm --materials keys (options of build_assets.sh)            -> world-ssfar4b-ref.dnm
  T/prepare_face_order ... --split-video video --lod-groups ssfar4b.json                      -> world-ssfar4b.dnm
  T/prepare_palette_materials keys video --mesh -ref --split-mesh --share car.dnt ... --texture-fill ssfar4b.json
                                                       -> world-ssfar4b.dnt; with the mip options -> world.dnt [asset set]
                                                       (+ --mip-skip-textures 6428,3858,7064 -> ssfar4c world.dnt)
  T/prepare_restrip world-ssfar4b.dnm world-ssfar4b.dnt --rules exact             -> world.dnm [ssfar4b, ssfar4c]
  ssfar4bf3: the same with specs/ssfar4bf3.json; ssfar4cf3: its packs with ssfar4c's --mip-skip-textures
  ssfar4cf0: the same with specs/ssfar4b-fence-plain.json
  ssfar4cf3r: T/opaque_cores opaque-cores/rock-decals.json -> specs/near-rock.json; the same with specs/ssfar4bf3-rock.json
  ssfar4e: ssfar4cf3r's with specs/ssfar4e.json and --drape-road-decals (both face orders)
  ssfar4eu: the same with specs/ssfar4eu.json; mip DNT also without mip chains on the sign words of the
           rules board, signs and picks (not the video screens 2584, 2588, 2840)
  ssfar4f: ssfar4eu's with specs/ssfar4f.json (no fence rule, verges 6686 only, reels; packs world-ssfar4bf*, then world-ssfar4f-mip.dnt)
  ssfar4d: face order (both meshes) + --bake-decals decals/rock-shared.json, mip DNT with --mip-skip-textures,
           strips made with the mip DNT

Name, result, finish, sky, title, HUD, script:
  T/prepare_models name-models --name-models R --name-header --share-positions    -> name.dnm, name_data.h
  T/prepare_result_labels R --name-scene --colors name-colours.txt                -> name-labels/, labels/name_label_data.h
  T/prepare_result_labels R --colors result-colours.txt                           -> labels/, result_label_data.h
  T/prepare_result_pal4 result-models                                             -> result/, result_pal4_data.h
  T/prepare_models result-models --result-models R                                -> result.dnm
  T/prepare_models finish-models --finish-models R                                -> finish.dnm
  tools/prepare_dreamcast_textures.py finish-models/materials --format pal4     -> finish/
  T/prepare_sky sky/panorama.tga                                                  -> sky.dns
  T/prepare_scene title --title-program program.bin                               -> native_scene.inc + textures
  T/prepare_hud R hud-trace                                                       -> hud_data.h, hud/
  cc tests/trace_race_native.c code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c (intro, intent,
     race-start, record, podium headers)
  trace_race_native packs race.trace ... production --script-header               -> race_script_data.h
                                                       (edges bit2: the trace's START edge `recover`)

Advanced (course slot 2; the course byte 501460 is the slot of every per-course ROM table: 0 Beginner,
1 Expert, 2 Advanced; files of another course carry the slot, include/course_assets.h):
  T/prepare_contact R 2, T/prepare_road R 2, T/prepare_path R 2 0..7  -> course2.dnc, course2.dnr, course2-route0..7.dnp
  M 3422 --export-native-cells (the attract shows slot 2 from frame 3362)  -> cells2-3422 (deleted after use)
  T/prepare_visibility R 2 cells2-3422                                 -> course2.dnv
  T/prepare_intro R --race-start --courses 0,2 / --podium --courses 0,2  -> race-start-data, podium-data (both courses)
  M 3422 --export-native-cells --race-scenery-lights, M 4000 --dump-video, objects of 236940[2], key_pack auto,
  face order (no level-of-detail rules, --surface-ground), DNT6 with mip chains, strips -> course2-world.dnm/.dnt
      (the eight frame-counter animations: a base mesh a group; DNT6 --extra-keys sources/clock_keys.txt, the
      headers and lumas of every frame, and --chase-variants 16, the colour-cycling ramps baked in 16 texture copies)
  T/prepare_world_objects clock R objects2 sources course2-world.dnm  -> course2-actors.dna
      (tests/test_clock.c: every frame of every group as the renderer decodes it against the export's lists)
  M 3422 --export-native-sky, T/prepare_sky                            -> course2-sky.dns
  M 8000 --trace-hud, T/prepare_hud --course-map 2 --first-bank 60 --max-banks 4 -> hud (MAP_C2)
  M 8000 inputs/inputs-advanced-timeout.txt --trace-race --trace-clock -> race-advanced.trace, clock-advanced.txt
  trace_race_native --course 2 --counter-base auto --clock, compare_race_trace --require-equal --skip-rows 0,1,
  tests/test_clock_lockstep.py                                         -> script-advanced/race_script_data.h
                                                       (the step fails unless all 20 cars agree on every row and
                                                       every animation draws what the original's does, every tick)

Expert (course slot 1; 18 October):
  T/prepare_contact R 1, T/prepare_road R 1, T/prepare_path R 1 0..7  -> course1.dnc, course1.dnr, course1-route0..7.dnp
  M 6602 --export-native-cells (the attract shows slot 1 from frame 6542), T/prepare_visibility R 1 -> course1.dnv
  T/prepare_intro --race-start / --podium --courses 0,1,2              -> race-start-data, podium-data (three courses)
  M 6602 --export-native-cells --race-scenery-lights (variable specular: every reachable luma), M 4000 --dump-video,
  objects of 236940[1] (the props as cones; Expert's world actors, include/expert_actors.h: a base mesh a group,
  PLACED groups in model coordinates), key_pack auto, face order --live-specular --surface-ground, DNT6 with mip
  chains, the live faces' lumas, --extra-keys sources/clock_keys.txt (every luma a placed face can show) and
  --chase-variants 16 (the colour-cycling signs), strips              -> course1-world.dnm/.dnt
  T/prepare_world_objects clock R objects1 sources course1-world.dnm --course 1 -> course1-actors.dna (with actors;
      tests/test_clock.c)
  M 3000 inputs/inputs-expert-timeout.txt --check-native-expert-actors drive.dnd course1.dnc (the actors' original
      ticks on the i960 and TGP against code/expert_actors.c: random task and player words, every state)
  M 6602 --export-native-sky, T/prepare_sky                            -> course1-sky.dns
  M 9100 --trace-hud, T/prepare_hud --course-map 1                     -> hud (MAP_C1)
  M 13000 inputs-expert-finish.txt --trace-hud (the player leads from frame 11483), T/prepare_hud --second-marker 1
                                                                       -> hud (MARKER_SECOND, MARKER_TOP)
  M 9100 inputs/inputs-expert-timeout.txt --trace-race --trace-clock --trace-clock-expert -> race-expert.trace,
      clock-expert.txt (the actors' draws with the TGP's matrices)
  trace_race_native --course 1 --counter-base auto --clock, compare_race_trace --require-equal --skip-rows 0,1,
  tests/test_expert_lockstep.py                                        -> script-expert/race_script_data.h
                                                       (the step fails unless all 30 cars agree on every row and
                                                       every actor draws the original's lists with its matrices)
  vgmrips pack 04, 05: afx_vgm --adpcm                                 -> music/adpcm/t04, t05

Races to the podium on Advanced and Expert (18 October):
  M 17800 inputs/inputs-advanced-finish.txt --trace-race --trace-clock -> race-advanced-finish.trace, clock-advanced-finish.txt
  M 19100 inputs/inputs-expert-finish.txt --trace-race --trace-clock --trace-clock-expert -> race-expert-finish.trace,
      clock-expert-finish.txt
  trace_race_native --course N --counter-base auto (race, podium 2865a70[N], results), compare_race_trace
      --require-equal --podium --skip-rows 0,1                         -> script-advanced-finish/, script-expert-finish/
                                                       (fails unless every race row, every podium car and the
                                                       timers through the results agree; checkpoints with the podium;
                                                       Advanced: test_clock_lockstep.py, Expert:
                                                       test_expert_lockstep.py, through race and podium)

The attract loop over the three courses (18 October):
  M 10000 (no input) --trace-race --trace-attract --trace-clock        -> attract.trace, clock-attract.txt (deleted after use)
  T/prepare_intro --podium --courses 0,1,2                             -> podium_data.h also holds the attract graphs
                                                                          2865a60[1], [2] (nm_attract_courses)
  tests/trace_attract_native.c (three loops: 0, 2, 1), test_attract_lockstep.py, test_clock_lockstep.py
                                                       (fails on any difference in a car, the camera key tick,
                                                       the cells, the scene light or the view beyond 0.25 units but
                                                       what the V.R. demo before leaves: not native, listed apart)

Games one after another (30 October; the step games_lockstep, no output):
  M N inputs/games-X.txt --trace-frontend --trace-race --trace-attract -> frontend-X.trace, race-X.trace (deleted after use)
  tests/test_games_lockstep.py: for each game of a run, from power-on, trace_attract_native (--enter, --credit, --leave),
  trace_race_native (--enter, --frontend, --leave), compare_race_trace --require-equal, every field of every car and
  test_attract_lockstep.py                             (fails on any difference; tests/game_chain.h)
```

## Where the historical intermediates differ

All final outputs are byte for byte the delivered ones (`expected.sha256`),
and the default ELFs built from commit bbf1c2f's tree against them load the
same images as the delivered ones (`build/dc-native-consolidate.5a9jum`,
stats 7bfee34e..., controller 8c932b09...).
Some intermediates of the original work were made differently and are not
reproduced as such:

- The world and car DNT4 packs were made by one-off VQ and palette fitting
  scripts in `build/` (`dc-native-world-bases`, `dc-native-modulated`,
  `dc-native-car-anchors`, never committed). Their texture data is not used
  by anything downstream; `key_pack.py` writes the bindings they had. The
  world resource numbers are kept in `world-dnt4-resources.txt`.
- The video dump came from a patched `m2segment` (`dc-native-car-render`,
  frame 5700 of the fast race); `m2segment --dump-video` at frame 4080 of the
  same race writes the same five tables.
- `world-object-models.txt` was read from display-list logs of the reference
  (`dc-native-world-objects` inv.py); it is kept as the list of addresses.
- The cell exports of the current `m2segment` differ from the historical ones
  in comment lines and one manifest key only; the packs made from them are
  identical.
