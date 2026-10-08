# Geometry core and per-core profiling (GPU builds: `--gpu-fast` and `--gpu-gl`)

## What changed

The main thread used to run everything except the reference sound board:
i960 game code, synchronous TGP, the geometrizer (the display list parse, the
heaviest single block of a 3D frame), the System 24 video update and the GPU
recording/submission. The parse now runs on its own core; the display stays
on the main core:

```text
core 0  main thread      input, i960 + TGP, buffer RAM snapshot, 2D video update,
                         sound hand-off, GPU recording + submission (vitaGL / vita2d)
core 1  geometry thread  display list parse (unmodified MAME geometrizer)
core 2  sound worker     reference sound board (68000 + SCSP), pinned to core 2
```

Cores: CMake `DAYTONA_VITA_MAIN_CORE` (default 0), `DAYTONA_VITA_GEO_CORE`
(default 1), `DAYTONA_VITA_SOUND_CORE` (default 2) and `DAYTONA_VITA_2D_CORE`
(vitaGL 2D worker, default 2); `-1` leaves a thread unpinned (the scheduler can
then put it next to another one). `scripts/build_vita.py --free-core` (CMake
`DAYTONA_VITA_FREE_CORES`) unpins all of them: they then follow the 4TH CORE
option (`core_policy.h`), so the fourth core can take any of them. Pinned, the
fourth core only takes the SDL audio callbacks.
The sound worker's mask is in the `perf.log` start line (`mask=0x40000` = core 2)
and in `cores:` of every report.

## Files (the geometrizer in src/ is not modified)

| File | Role |
| --- | --- |
| `src/runtime/geo.h` (this directory) | Replaces `src/runtime/geo.h`: the Vita CMake puts `platform/vita/src` **before** `src` on the include path. It includes the original header unchanged, its class renamed `rt::GeoCore`, then declares `rt::Geo`, a wrapper with exactly the interface `M2Board` uses (constructor, `zclip_w`, `parse`, `polys`, `windows`, `set_wide_margin`). |
| `src/runtime/geo.cpp` (this directory) | Replaces `src/runtime/geo.cpp` in the Vita runtime: compiles the original unchanged as `rt::GeoCore`, plus the wrapper and its thread. |
| `core_profile.h` | Per-core accounting and the `perf.log` writer. |
| `main_gpu.cpp` | Shared by both GPU builds: core placement (geometry always pipelined), profiling. |
| `gpu_gl.cpp/.h` | vitaGL: single drawing path (GPU System 24 layers + polygons), sort/texture-build timings, gl.log only in the `--diagnostics` build. |
| `core_policy.h` | Pin masks, fourth-core policy of the unpinned threads. |
| `sound_worker.h` | `set_cpu_mask()`: the sound worker pins itself to its core. |
| `CMakeLists.txt` | The replacement source, include order, core options, LTO. |

`m2_board.cpp`, `video.cpp`, `game_loop.cpp` and the rest of `src/` are compiled
as they are and simply get the wrapper when they say `Geo`. The MAME code is
reused by inclusion, so a change of `geo.cpp`/`geo.h` in `src/` is picked up
automatically; only the interface listed above must stay the same.

## Link-time optimization

`DAYTONA_VITA_LTO` (`scripts/build_vita.py --release`) builds the whole VPK with GCC LTO: the
generated i960/TGP code can then inline the runtime's small memory and bus
accessors (`cpu.cpp`, `m2_board.cpp`), which a normal build calls through
separate object files. The float rules are unchanged (`-fno-fast-math
-ffp-contract=off` also at the link-time code generation). CMake prints
`Vita LTO: enabled`, or a warning and a normal build if the toolchain cannot do
LTO. The link is longer and uses more host memory (the generated code is large),
so the normal build leaves it off. `--release` also turns the diagnostic logs off
(it cannot be combined with `--diagnostics`).

## Why the polygons stay identical (one frame later)

* `parse()` (vblank start) copies buffer RAM (128 KiB, the TGP/i960 display
  list RAM) into a private snapshot and wakes the geometry thread. `GeoCore` is
  built on that snapshot and never reads the live RAM, which the i960/TGP keep
  rewriting on the main core.
* `GeoCore`'s persistent state (polygon RAM, texture/log RAM, matrices, raster
  state) is only touched by the parse while it runs; every other access
  (`polys.size()` = polygon count register, wide margin, mode change,
  destruction) joins it first.
* Two polygon lists are swapped (back = being parsed, front = shown), never
  copied. `M2Board::vblank_end` receives the front list.
* The geometry thread copies the main thread's FPSCR (rounding,
  flush-to-zero, default NaN): the floats are those of the main-core parse.
* A parse failure (`GeoFatal`) is rethrown on the main thread at the next
  join, inside the frontend's normal runtime-error handling.

The GPU frontend has a single mode, no option: the geometrizer always runs
pipelined on core 1. The frame shows the previous parse, which overlapped the
whole next game frame on the geometry core: the main core normally never waits.
Game-visible state is unchanged: the polygon count register still reports the
newest parse. At 30 Hz the list is shown from the frame after its parse.

The vitaGL renderer shows the 2D layers (System 24: HUD, sky tilemap) one frame
late too, so 2D and 3D are in phase: the picture is the exact one, one frame
(17.4 ms) late. (The vita2d renderer keeps one set of layer textures and shows
the current frame's 2D, one frame ahead of the 3D: two sets of its colour tiles
would upload every tile and palette change twice.) vitaGL keeps two sets of 2D
textures; while a frame shows the set
prepared during the previous frame, a worker thread on core 2 (the sound core,
`DAYTONA_VITA_2D_CORE`) uploads this frame's changed tiles and palette into the
other set and computes its layer rectangles. The main core only emits the quads
and waits for the worker at the end of `draw()` (`2D: wait for the core 2
worker` in `perf.log`; the job itself is `2D worker` under CORE 2).

If the geometry thread cannot be created the main core is used (2D then one
frame behind the 3D), and the line `geometry: ... thread=0 result=<error>` is
written to `perf.log` and to `vita-diag.log`. If the 2D worker cannot be
created, its job runs on the main core and `GPU25 2D worker: thread
unavailable` is written to `vita-diag.log`. Old `geo_mode` keys in `vita.cfg`
are ignored.

## perf.log (per-core time accounting)

All log files exist only in diagnostic builds: `python3 scripts/build_vita.py
--gpu-gl --diagnostics` (CMake `DAYTONA_VITA_DIAGNOSTICS=ON`). That flag
enables `perf.log`, `gl.log`, the periodic lines of `vita-diag.log` and the
profiling clocks; a normal or `--release` build writes nothing except faults to
`vita-diag.log`.

`ux0:data/<set>/perf.log` is rewritten at each launch. A background thread writes one report
every 5 s of **gameplay** (menus excluded, a window restarts after a pause).
Copy the file before relaunching.

For every core: share of the core's wall time (`core%`), average per shown
frame (`ms/frame`), worst frame (`max ms`), frames with that work (`hits`):

```text
==== perf 3: 5.01 s gameplay | geo=ASYNC_PIPELINED | renderer=VITAGL | CPU 444 MHz GPU 222 MHz BUS 222 MHz ====
frames: emulated 52.63 fps (target 57.52), shown 52.63 fps, worst loop 19.00 ms, board frame budget 17.38 ms
cores: main=0 geometry=1 (thread) sound=2 (thread)
section                                    core%  ms/frame    max ms   hits
CORE 0 main: busy 93.7% (17.80 ms per shown frame)
  i960+TGP game logic (to vblank)           40.6      7.71      8.49    300
  ...
CORE 1 geometry: busy 39.2%
  display list parse                        39.2      7.46      8.47    300
  parses 300, avg 7.46 ms, worst ... | polys avg 2400 ... | start->done avg ...
  main core blocked on 2 of 300 joins (worst wait 0.90 ms), polygon count reads 0, failures 0
CORE 2 (sound + 2D worker): busy 22.6%
  ...
work per emulated frame: i960 110000 instr, TGP 300000 instr (both inside the i960+TGP lines), polygons 2400
PRIORITY main core: 1) i960+TGP game logic 40.6% (7.71 ms) 2) polygon recording 18.4% (3.50 ms) ...
csv,3,5.010,ASYNC_PIPELINED,52.63,52.63,0.060,7.707,...
```

(Illustrative numbers, not a device measurement.)

Main-core sections, in loop order (with `idle / unmeasured`, 100 %):

| Section | Content |
| --- | --- |
| input + menu + frame clock | pad, chord, frame clock |
| i960+TGP game logic | game code up to its wait-for-vblank loop (split from the vblank handler by the geometrizer's own timestamps; at 30 Hz, frames without a parse keep the handler here) |
| geometry on main core | snapshot + wake-up of the geometry thread (the whole parse if the geometry thread is unavailable); the snapshot is shown nested |
| i960+TGP vblank handler | the vblank interrupt handler |
| 2D video update | `Video::screen_update` (System 24 tile cache) |
| board other | probes/scheduler between those stages; `(nested) WAIT for the geometry core` is shown under it |
| sound sync / dispatch | joining the sound worker, dispatching the next packet (or native send) |
| renderer prepare_frame | texture cache resets |
| frame begin | GPU fence wait (vitaGL) + clear |
| 2D texture uploads / 2D layer recording | System 24 textures and quads |
| polygon priority sort | Model 2 draw order |
| polygon recording | vertices, materials, batches (vitaGL: texture builds shown nested) |
| frame end | GL/GXM submission + swap, including vsync waits |
| logging | diagnostic formatting/enqueue |

The `csv` line repeats every section's ms/frame; its column names are written
once at the top of the file, so runs can be compared in a spreadsheet.

Reading it:

* `WAIT for the geometry core` high: the parse is longer than a whole frame of
  the main core: `CORE 1` is the limit.
* `i960+TGP game logic` dominant: recompiled guest code; `work per emulated
  frame` gives the instruction volumes. (TGP time alone is not separated: that
  would need a change in `src/runtime/m2_tgp_board.cpp`.)
* `polygon priority sort` / `polygon recording`: renderer CPU work, the next
  candidates to move to the geometry core.
* `frame end` large while `idle` is small: GPU-bound or vsync-limited; see the
  `swap` line of `gl.log` (vitaGL).

## Status

Built with VitaSDK and played on a console (both GPU builds, October 2026). The
geometrizer wrapper was also run on the host against stubbed kernel calls (60 Hz
and 30 Hz, both modes bit-identical to the original parse, ThreadSanitizer
clean); that harness is not shipped.
