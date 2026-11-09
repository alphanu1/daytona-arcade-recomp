# Original course panoramas

Widescreen uses the complete original course sky from the user's imported
ROM. This supports Beginner, Advanced and Expert in Revision A (`daytona`)
and Deluxe '93 (`daytona93`). Selecting a widescreen aspect enables it
automatically; Original remains the default. The launcher also offers 32:9.
No extracted image or replacement artwork is distributed or required.

This follows the design document's Renderer and Enhancements sections.
Scenery selection, opponent selection, draw budgets and HUD placement keep
their existing policies.

## Source and sampling

The game's 512-pixel tilemap is a streaming window into eight 256-pixel
source sections. The complete panorama is 2048 pixels wide. At native height,
16:9 and 32:9 show more of that source without stretching its pixels.

| Course | Game ID | Source table | Source size | Cached indices |
| --- | --- | --- | --- | --- |
| Beginner | 0 | `0x2074240` | 2048 x 392 | 1.53 MiB |
| Advanced | 2 | `0x2081180` | 2048 x 344 | 1.34 MiB |
| Expert | 1 | `0x2074268` | 2048 x 432 | 1.69 MiB |

The tables describe the original character uploads and eight tile maps.
The cache retains 16-bit palette indices and tile category, rather than RGB,
so the existing live palette supplies colour changes and fades. Source
rows begin at tilemap y=48. The unverified fourth source is unsupported.
Revision-specific phase, selector and descriptor addresses are recorded in
`src/runtime/panorama_revision.h`; the source maps and character data are
shared by both revisions.

The full 16-bit camera phase is latched when the game writes its scroll
value and when that value reaches the tile registers. Sampling alongside
the displayed frame avoids reading the next frame's camera at a wrap.
Before activation, the course descriptor, scroll state, visible streamed
map columns and character data must match the source. Loading and stale
tile states retain the original renderer. Checking the course selector
alone is insufficient: it can change before the sky upload is complete.

Only layer-2 source rows use the panorama. Live filler rows, split layers,
window masks, foreground priorities and the native centre are preserved.
Normal software rendering fills only the side margins. The diagnostic
camera sweep uses a separate full-width compositor as a comparison.
The GPU samples the same indices in the existing shared tile shader pass.

One CPU course cache is replaced on a course change. The GPU buffer grows
only as needed and reuses that capacity; uploads are tied to course and
game-instance identity. Native startup allocates no panorama artwork.
Cached artwork is retained across aspect changes. Source validation adds
small CPU records and decoding uses temporary character storage; the table
above measures the persistent index arrays, not total process memory.

The old desktop stretch preference is ignored when loading settings and
omitted on the next save. Capture tools retain explicit panorama and
stretch controls for comparisons.

## Build and check

Follow the normal setup instructions for each ROM set, using separate build
directories. After building the game and capture tools:

```powershell
cmake --build build-daytona --config Release --target test_panorama test_scenery test_app_config m2panoramacheck --parallel 4
ctest --test-dir build-daytona -C Release -R "^(panorama|scenery|app_config|frame_batch|lockstep)$" --output-on-failure
```

Repeat with a build configured for `daytona93`. The `panorama` CTest uses a
synthetic ROM and needs no game assets. It checks source decoding, bounds,
course-cache replacement, loading readiness and sampling boundaries.

The explicit `m2panoramacheck` target needs the user's imported ROM cache,
a cabinet NVRAM snapshot and repository input scripts. Its default mode
checks palette fades, frozen-frame aspect crops, software/GPU agreement at
every widescreen width, and GPU cache reuse across courses. `--play` exercises
the playable app's settings policy, live aspect/renderer changes, native
intervals and game restarts:

```powershell
build-daytona/Release/m2panoramacheck.exe build-daytona/rom_cache/daytona NVRAM_DIR scripts/inputs/race_to_end.txt scripts/inputs/widescreen_advanced.txt scripts/inputs/widescreen_expert.txt
build-daytona/Release/m2panoramacheck.exe build-daytona/rom_cache/daytona NVRAM_DIR --play scripts/inputs/race_to_end.txt scripts/inputs/widescreen_advanced.txt scripts/inputs/widescreen_expert.txt
```

Replace `NVRAM_DIR` with a copy of the cabinet settings used for the replay.
The fixtures keep comparisons in memory and do not save ROM-derived assets.
Aspect crops use the same paused state: independent replays at different widths
can reach different camera phases and are not interchangeable controls.

The ROM-free `frame_batch` and `lockstep` CTests cover the last scheduled
software picture in a catch-up batch and callback ordering, capture lifetime
and bounded allocation. The explicit `m2catchupcheck` target compares drawing
every scheduled picture with suppressing superseded pictures. Both sides run
every guest update; final pixels, CPU/TGP counts, sampled RAM, audio buffers
and cabinet state must match. Run each course with both ROM revisions:

```powershell
cmake --build build-daytona --config Release --target m2catchupcheck
build-daytona/Release/m2catchupcheck.exe build-daytona/rom_cache/daytona NVRAM_DIR --race scripts/inputs/race_to_end.txt
```

Repeat with `widescreen_advanced.txt` and `widescreen_expert.txt`. Each replay
uses 5,400 updates, batches of one to four, renderer/aspect changes, all three
draw modes and all four race cameras. Timings measure CPU work without a
window, display waits or an audio device; live fullscreen pacing still needs
manual verification.

For captured comparisons, `scripts/original_panorama_validate.py` accepts
`--set`, `--build-dir`, `--nvram`, `--course` and a fresh `--output` directory
beneath `traces/`. It needs NumPy. Run each course in each revision. It checks
native output, the original centre, software/GPU backgrounds, natural phase
wraps, loading transitions, source scale and HUD composition. It records
commands and hashes, then removes raw captures after comparison. `--bench`
also records repeated runs separately from capture work.

`m2run` and `m2gpushot` expose `--panorama-original`, `--panorama-only` and
`--sky-log FILE` for these checks. `--panorama-only` isolates the complete
back-layer composite; it does not change guest RAM or gameplay.

The shared HLSL is compiled for SPIR-V, DXIL and MSL through
`scripts/build_shaders.py`. Testing on one GPU backend establishes only that
backend's runtime behaviour. These enhancement comparisons are not a new
MAME parity result or a claim of complete road/HUD support at 32:9.


## Local cost observations - 5 November 2026

Initial MSVC Windows Release builds, Intel Core i9-12900HK / Iris Xe,
Direct3D 12. These compare panorama on/off against upstream; they do not
compare with the development executable built using ClangCL. The same
Beginner replay and cabinet snapshot are used for each off/on pair, with two
runs per condition in off/on/on/off order. These are offscreen observations
on this PC, not live presentation or frame-pacing measurements.

Native comparisons use all 6,000 frames in both executables. The upstream
GPU tool does not support `--bench-from`; silently using different windows
produced invalid initial comparisons, which were repeated correctly.

| Native, ms/frame | Upstream game stage | Panorama build game stage | Upstream GPU wait | Panorama build GPU wait |
| --- | --- | --- | --- | --- |
| Revision A | 1.95-1.96 | 1.95-2.02 | 0.58-0.81 | 0.79-0.87 |
| Deluxe '93 | 1.12-1.18 | 1.11-1.17 | 0.83-0.88 | 0.88-0.94 |

GPU wait is a host fence-wait duration, not a GPU execution timestamp.
Native renderer CPU time is approximately 0.20-0.23 ms/frame in these runs.
The native game-stage ranges overlap; the readings do not establish zero
cost in every stage or on other hardware.

At 32:9, both GPU comparisons use the panorama build and measure the last
3,000 frames of a 6,000-frame run, after loading and cache preparation:

| 32:9 GPU path, medians | Panorama off | Panorama on |
| --- | --- | --- |
| Revision A game stage, ms/frame | 2.19 | 2.24 |
| Revision A renderer CPU, ms/frame | 0.26 | 0.26 |
| Deluxe '93 game stage, ms/frame | 2.08 | 2.09 |
| Deluxe '93 renderer CPU, ms/frame | 0.26 | 0.25 |

Peak process working-set medians are 410.1 -> 413.0 MiB in Revision A and
370.1 -> 373.2 MiB in Deluxe '93. These include guest images and driver
allocations; they do not measure total GPU memory. Persistent index-array
sizes are listed separately above.

Software averages include the complete 6,000-frame run and the full 3D
renderer, so their absolute values are not comparable to the GPU race window:

| 32:9 software, ms/frame | Panorama off | Panorama on |
| --- | --- | --- |
| Revision A | 13.12-14.84 | 13.07-13.78 |
| Deluxe '93 | 12.76-14.62 | 12.52-12.93 |

The software ranges overlap and vary more than the small differences between
conditions. They do not establish a speedup or a general frame-rate guarantee.

The subsequent fullscreen regression report used software rendering with edge
HUD placement, which those initial software runs omitted. A matching
6,000-frame Revision A replay with that setting takes 34.87 ms/update in the
original MSVC test build and 18.05 ms/update after rebuilding with ClangCL and
restoring software catch-up coalescing and callback reuse. Execution/sound
counts and final pixels match. This single timing pair measures elapsed
update work, without presentation, and does not separate compiler cost from
the runtime changes.

A separate single-cabinet probe against the development launcher's generated
objects measures 21.41 vs 21.86 ms for the three-update batch ending at frame
960, with identical execution counts, geometry counts and pixels. Both keep
one scheduled picture and run all three guest updates. This checks that case's
cost against the development runtime. Manual fullscreen feedback reports
improved performance, without a recorded frame-time distribution. The
two-cabinet catch-up fixture is a correctness check, and its
absolute timings are not an estimate of the playable app's latency.
