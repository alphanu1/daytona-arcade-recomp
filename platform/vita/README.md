# Native PS Vita target (experimental)

## ImGui launcher (01.24)

Options also includes **Hold Test button**: enable it, then Start/Resume.
It waits for the game to accept inputs, holds Test for three game seconds,
then clears itself. This request is not saved. Existing Test/Service chords
remain available; Reset Defaults cancels a pending hold.

The GXM build now uses Dear ImGui for its launcher, options and loading screen.
D-pad navigates, Cross activates, Left/Right changes the highlighted setting,
and Circle goes back/resumes. Front touch selects rows, arrow buttons and the
scrollbar. Start+Select still opens the menu during play. The existing clocks,
ROM selector, link configuration, steering curves and display/audio options
retain their saved vita.cfg values. Both ROM executables use the new UI.

ImGui draws through the existing vita2d/GXM context only while the menu is
visible. No SDL3 renderer, extra display-buffer mode or game-renderer switch
was introduced. Wide GPU tiles remain disabled following the earlier hardware
slowdown. Host contract tests do not establish appearance or speed on a Vita.

This is a VitaSDK/SDL2 frontend for the existing native runtime. It is a
native Vita application, **not a PSP/Adrenaline build**. The i960, TGP and
68000 programs still come from the host recompilation pipeline. No
interpreter, replacement game logic, ROM bytes or generated code is added.

The desktop SDL3/SDL_GPU application and root CMake build are unchanged.
The Vita frontend uploads the existing 496x384 software-composited screen
through SDL2's Vita renderer. This does **not** move the Model 2 rasterizer
onto the Vita GPU. Tested on a real Vita (2026-10-06): it plays with no
issues reported. Performance, memory headroom and full-race parity figures
are not recorded yet.

## Multicore GPU builds (`--gpu-fast` and `--gpu-gl`)

Both GPU builds share `main_gpu.cpp` and one multicore flow. The Vita has three
application cores (0-2) and a fourth, system core (3) that a core-unlock plugin can open.

| core (default) | thread | work |
|---|---|---|
| 0 | main | input and menu; the i960 game code; the TGP (run synchronously by the i960, no thread of its own); at vblank start, a copy of the display list RAM (128 KiB) handed to the geometry thread; at vblank end, the System 24 tile update (`Video::screen_update`); the sound hand-off; then the GPU frame: polygon priority sort, materials/textures, vertices, submission and swap |
| 1 | geometry | the display list parse (the MAME geometrizer, unmodified: transforms, clipping, lighting) of the frame the main core just finished |
| 2 | sound worker | the reference sound board of the frame just finished: 68000 program, two MultiPCM, YM3438, resampling into the output queue |
| 2 | 2D worker (vitaGL only) | System 24 tile and palette uploads into the second texture set, layer rectangles (scroll, split screen, windows) |
| any | SDL audio callbacks | mixing the queued samples to the device (reference engine), or the whole native sound engine when it is selected (no sound worker then) |

The GPU (SGX543) draws on its own; core 0 only records and submits.

How one emulated frame N overlaps (each worker is joined before its data is touched again):

* core 0 runs frame N's game logic while core 1 parses frame N-1's display list and
  core 2 runs frame N-1's sound board;
* at vblank start of frame N the display list is copied and core 1 starts parsing it
  (the previous parse, normally finished long ago, becomes the shown list);
* after frame N, core 0 joins the sound worker (frame N-1's packet) and gives it
  frame N's sound packet: one packet in flight at most;
* core 0 then records the picture: the polygons of frame N-1 with, in vitaGL, the 2D
  of frame N-1 (the 2D worker prepares frame N's into the other texture set in
  parallel and is joined at the end of `draw()`); in vita2d, the 2D of frame N.

Load, verified with `--diagnostics` (`perf.log`, one report per 5 s of gameplay, per core:
busy %, ms per frame, worst frame; `core_profile.h`, details in
[GEO_ASYNC.md](GEO_ASYNC.md)). Core 0 is the limit: about 16 ms of the 17.4 ms frame
in a race with vitaGL (estimates in [PERFORMANCE.md](PERFORMANCE.md)). Core 2 comes next
with vitaGL (sound board + 2D worker, about 15 ms); the geometry parse leaves core 1 the
most room. `--free-core` lets the scheduler, and the fourth core, use that room. The CPU
build (`main.cpp`, no `--gpu-*`) keeps everything on one game thread.

* **3D one frame late.** The geometrizer parses frame N while the game runs frame N+1,
  so the polygons shown are the previous frame's. This is fixed, not an option; there is
  no CPU/GPU picture switch any more (the pipelined geometry leaves no exact CPU picture
  to switch to).
  * vitaGL shows the previous frame's 2D layers with them (two sets of layer textures,
    `GpuGlRenderer::S24Slot`, prepared by the 2D worker): the exact picture, one frame
    late.
  * vita2d shows the current frame's 2D (one set of layer textures): the 2D is one frame
    ahead of the 3D. In phase would need two sets there too, and its tiles hold colours,
    not pen numbers: every tile and palette change would be uploaded twice (a palette
    change is a full 8 MB rewrite), measurably slower.
* **Cores.** By default each thread above is pinned to its core. `build_vita.py
  --free-core` (CMake `DAYTONA_VITA_FREE_CORES`) leaves them all unpinned, as the
  earlier GXM build did: they then follow the **4TH CORE** option (`core_policy.h`), so
  with a core-unlock plugin any of them can run on the fourth core. In the default
  pinned build the option opens the fourth core to the SDL audio callbacks only.
  Single cores can also be moved with `DAYTONA_VITA_{MAIN,GEO,SOUND,2D}_CORE`.
* **CPU 500 MHz** and the fourth core are the options described below, in both builds.
* **Emulated sound (reference engine).** `audio.h` + `audio_rate.h`: a 64 ms cushion kept
  by a slight resampling speed change instead of crackles when a frame is late, one short
  silent gap after an underrun, and silent FM not resampled (~1.6 ms per frame saved on
  the sound core). The native engine is unchanged.
* **Widescreen** (aspect, HUD at the edges, stretched backdrop) is drawn by the GXM build
  only for now; the vitaGL build keeps the original 4:3 picture and shows those options
  as GXM-only.
* **Game code options** (both builds): `--fast-inaccuracy` (i960 lockstep checks per
  block + direct work-RAM accesses, TGP counted per block; the fastest) or `--fast-gen`
  (the Dreamcast's `platform/dreamcast/scripts/fast_gen.py` pass, exact), made from the
  host tools into `BUILD_DIR/gen_vita`; `--release` adds link-time optimization and
  drops the logs; `--diagnostics` writes `vita-diag.log`, `perf.log` (per-core report,
  `core_profile.h`) and, with vitaGL, `gl.log`.

```sh
python3 scripts/build_vita.py --gpu-gl --release --fast-inaccuracy        # pinned cores
python3 scripts/build_vita.py --gpu-fast --release --fast-inaccuracy --free-core
```

## Build

### Optional GXM CPU enhancements

The GXM frontend options offer CPU 500 MHz and fourth-core scheduling.
Both require compatible firmware/plugin support; no plugins are installed
by the game. Defaults: CPU 444 MHz / GPU 166 MHz in the vitaGL build (`--gpu-gl`),
CPU 333 MHz / GPU 111 MHz in the vita2d build (`--gpu-fast`), fourth core off (a saved choice wins; RESET DEFAULTS
in the options goes back to them).
The CPU option shows actual frequency, and unsuccessful 500 MHz requests
fall back to requesting 444 MHz. Check your overclock plugin's per-game
profile if the actual frequency differs from the selection.
Fourth-core access is verified through thread affinity readback. Rejected
requests retain ordinary three-core scheduling and show unavailable.
Game and audio threads may use the extra core; this does not split sequential
game logic into additional workers or guarantee higher FPS. (Default GPU builds pin
the game, geometry and sound threads to cores 0-2: there the fourth core takes the
audio callbacks only; build with `--free-core` to let every thread use it.)

Use a homebrew-enabled Vita, a host C++20 toolchain and VitaSDK with its
SDL2 development package. Reference SDK release: 2026.08. Set `VITASDK`
and install the package with that release's package manager:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
vdpm install sdl2
```

Follow the SDK's installation documentation at https://vitasdk.org/ for a
new SDK install. No proprietary SDK or runtime module is required by this
frontend; it uses SDL2's normal Vita renderer, not PVR/PIB. The vitaGL build
(`--gpu-gl`) also needs `python3 scripts/setup_vitagl.py` once: it builds a pinned
vitaGL inside the project (see "vitaGL: pinned build and flags" below).

From the repository root, prepare the host build using your own complete
`daytona93` ROM set in `roms/daytona93.zip` (or `.7z`):

```sh
python3 scripts/setup.py
# After changing the seeds/recompilers, regenerate with the HOST compiler:
python3 scripts/recompile.py
# Build a separate ARM executable and installable package:
python3 scripts/build_vita.py
```

`setup.py` already recompiles when the ROM set is present. The explicit
`recompile.py` command is only necessary after changes or when adding the
ROM set later. The default output is:

```text
build/vita/daytona_vita.vpk
```

A different host build directory is supported:

```sh
python3 scripts/build_vita.py --host-build-dir build-host --build-dir build/vita --jobs 4
```

The equivalent CMake configuration is:

```sh
cmake -S platform/vita -B build/vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDAYTONA_GEN_ROOT="$PWD/build/gen"
cmake --build build/vita --parallel 4
```

Do not configure the host build directory with the Vita toolchain. The
importer and three recompilers run on your computer, not on the Vita. The
Vita configuration fails rather than creating an empty game if any required
generated source group is missing. Keep VPKs, ROM caches and generated C++
local, under the ignored `build/` directory.

## Install and play

### Dual-ROM GXM package and LAN link play

The dual package launches daytona93 by default. In Options, change ROM to
DAYTONA (1994 REVISION A), then choose Start/Reset; switch back the same way.
Switching replaces the native executable, not just the ROM data.
Supply both complete ZIPs yourself:

- `ux0:data/daytona93/daytona93.zip`
- `ux0:data/daytona/daytona.zip`

Settings, EEPROM and backup RAM stay in each set's own directory. Revision A
has its own defaults; CPU/core and other options may need setting again.
An older single-game package reports when the other executable is missing.

Link play requires Revision A on all cabinets and a connected Wi-Fi LAN.
Enable Link Play in its Vita options, set the four next-cabinet IPv4 octets,
listen port and next-cabinet port (default 15112). Reset to apply.
With two cabinets, each one's next address is the other. With more, form a
ring. Desktop uses main's Link Play settings and the same TCP port/protocol.
The pause menu shows the Vita IP, receive/transmit connection and cabinet ID.
No discovery, hostname resolution or internet port forwarding is provided.

Enter test mode with Select+Triangle; Select+Square is service, Cross advances
the menu and Start confirms. Under GAME SYSTEM set one MASTER, the others
SLAVE, and unique CAR NUMBERs. Cabinet type/region and game settings must
match (for example TWIN/JPN); otherwise the game can cancel the link.
Frame sync is optional, off by default. For solo Revision A play, disable
link and configure a single cabinet in test mode.

Build each set's generated sources separately using `scripts/recompile.py
--set daytona --build-dir build/revision-a-host` and the existing daytona93
host build. Then:

```sh
python3 scripts/build_vita.py --set daytona --gpu-fast --host-build-dir build/revision-a-host --build-dir build/vita-revision-a
python3 scripts/build_vita.py --set daytona93 --gpu-fast --host-build-dir build --build-dir build/vita-dual --revision-a-self build/vita-revision-a/eboot.bin
```

The second VPK contains both executables. Actual Vita-to-Vita/Vita-to-desktop
Wi-Fi operation and executable switching require hardware validation.

Install your locally built VPK with VitaShell, then put your **complete
ZIP ROM set** at:

```text
ux0:data/daytona93/daytona93.zip
```

Launch **Daytona Recomp** and choose **START GAME**. ROM CRC and size
validation uses the existing importer. Renaming a different Daytona set
will not make it compatible. The Vita target deliberately omits the 7z
SDK to avoid solid-archive decoding memory spikes. A host `.7z` source can
still be used for recompilation; repack the complete set as `.zip` with
unchanged ROM filenames before copying it to the Vita.

| Action | Vita control |
| --- | --- |
| Steering | Left stick, or D-pad left/right |
| Accelerate / brake | R / L |
| Analogue accelerate / brake | Right stick up / down |
| Shift up / down | D-pad up / down (one shift per press) |
| View 1 / 2 / 3 / 4 | Cross / Circle / Square / Triangle |
| Coin / start | Select / Start |
| Pause menu | Start + Select together |
| Navigate / select / resume | D-pad / Cross / Circle |
| Test switch / service coin | Pause-menu entries |

The menu chord suppresses coin/start while both buttons are held. Pressing
Select significantly before Start can still insert a coin before the chord
exists. Buttons must be released after loading, pausing or resuming so menu
presses do not leak into the game.

The frontend retains the desktop's square-pixel framebuffer aspect ratio,
with side bars on the Vita display. Simulation steps use
`rt::GameLoop::kFrameHz`, not a hard-coded 60 Hz. The Vita frontend now runs
at most one complete simulation frame before presenting it. Fractional host
time is retained at normal speed; overdue whole steps are discarded under
load rather than rendering four complete frames and displaying only the last.
This slows wall-clock progress when the device cannot keep up; it does not
skip guest instructions, increase the guest timestep, or make the simulation
itself four times faster. The generic FrameClock default remains four steps.

See [PERFORMANCE.md](PERFORMANCE.md) for the stage timings now written to
`vita.log`. This is a diagnostic build, not a confirmed full-speed fix.

## Sound and saved data

FM and PCM are resampled independently to the output device rate, mixed,
clamped and played as stereo signed 16-bit audio. The callback only consumes
samples; board execution stays on the main thread. Queues are bounded and
cleared on pause/reset. Mute is saved independently of board state.

EEPROM and backup RAM are saved when changed, every five seconds and on
pause/reset/quit. Writes use a temporary file and retain a `.bak` generation;
loads reject incorrect sizes and try the backup. A sudden power loss can
still lose changes since the last successful save. Use SAVE AND QUIT for a
clean exit. All files are under `ux0:data/daytona93/`:

```text
ioboard_eeprom.bin
backup_ram.bin
mute.bin
vita.log
```

`vita.log` is replaced on each launch. Copy it before reopening the app when
reporting a crash. Missing/incorrect ROM errors and runtime faults are also
shown in the menu. A decoder/runtime fault requires a reset rather than
resuming a possibly inconsistent board state.

## Validation and limitations

Host tests (no ROM or SDK required):

```sh
mkdir -p build
c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/test_vita_controls.cpp -o build/test_vita_controls
build/test_vita_controls
python3 -m unittest discover -s tests -p test_build_vita.py -v
python3 -m unittest discover -s tests -p test_setup_vitagl.py -v
```

ROM-free ARM compile check (requires VitaSDK, SDL2, and fetched SoftFloat/ymfm):

```sh
python3 scripts/setup.py --no-build
python3 scripts/build_vita.py --compile-check
```

The included workflow performs the host tests and this ARM compile check.
**Compile-check mode makes objects only: it does not link generated game
code, produce a VPK, prove floating-point parity on ARM, or test gameplay.**
It intentionally does not upload artifacts or use ROM secrets.

This target still needs a full ROM-generated ARM link and real-device
verification: boot, all courses, manual/automatic gears, sound, sustained
frame time, peak memory, saved settings, and suspend/resume. The Vita
frontend handles SDL background/foreground events and limits post-stall
catch-up, but whether the installed SDL build emits those events during
system suspend must be verified on hardware. No overclock is forced.

SoftFloat keeps the existing 8086-SSE *semantic specialization* (not x86
machine instructions) but uses a project-owned portable platform header
without GCC `__int128`. Its state is main-thread-only in this target. Do
not move board execution onto multiple threads without restoring TLS or
introducing explicitly separate SoftFloat state.

See [HANDOFF.md](HANDOFF.md) for the port's status and
[THIRD_PARTY.md](THIRD_PARTY.md) for SDK dependency references.

## Widescreen, draw distance and cabinet controls

The GXM Options menu now saves Aspect (Original, 16:10, 16:9, 21:9),
HUD (Centred or Screen Edges), and scenery Draw Distance (Shortest through
Furthest). Changes apply on resume; original aspect, centred HUD and default
distance remain the defaults. Widescreen shows additional scenery with the
same focal length, not stretched pixels. On the 960x544 display, 21:9 is
letterboxed vertically. The road window is unchanged by scenery distance.

HUD relocation shares the desktop per-item rules and only activates when
the race HUD is visible. Scenery stays put and crossing banners stay whole.
Recovery 1 restores the earlier rendering paths after the perspective build
lost textured geometry on hardware. Original mode uses GXM tile composition;
wide mode uses CPU tile/HUD composition. Road wobble and widescreen performance
are not fixed by this recovery. Steering curves remain available. The layer arena is12MiB instead of10MiB,
making the three GPU arenas32MiB total.

Hold Select and press Triangle for cabinet Test (enter/confirm).
Hold Select and press Square for cabinet Service (advance/select).
Release between presses. Cross supplies VR1 (menu next) and Start supplies
cabinet Start (menu select), as used by the game's test screens.
Start+Select still opens the frontend pause menu.
Plain Select inserts a coin on release; Test/Service chords do not insert
coins or operate view buttons. The bindings are listed in Options.

Draw distance requires generated code with seeds/daytona93_hooks.txt:
regenerate with scripts/recompile.py before building. Merely linking the
enhancement runtime cannot add a missing hook to old generated code.

## Steering curves

Options → Steering Curve selects Linear (default), Soft (signed square) or
Extra Soft (cubic). Curves apply after the stick deadzone and before inversion;
full lock and D-pad steering remain unchanged. Soft settings give finer control
around centre. The choice is saved as steer_curve=0/1/2 in vita.cfg.


## Wide 2 update

Includes GitHub main through c081a2d, including the stricter condition-panel
overlay detection. Options adds Stretch Tile Background and Skip Launcher,
both off by default and saved in vita.cfg. Background stretching in the Vita
GPU frontend scales only the backdrop to the wide viewport, not the 3D scene
or HUD. Original aspect is unaffected. Unlike desktop's coverage-gated setting,
the Vita option stretches the backdrop whenever widescreen is selected.
Skip Launcher auto-loads the installed ROM on next launch; a load failure
returns to the menu with its error. Start+Select always opens the menu in-game.

The restored polygon/tessellation path is unchanged. Widescreen keeps a native
496x384 CPU tile backdrop and scales it with the existing 2D draw API; no custom
matrix or GPU tile compositor change. This reduces backdrop upload bytes by27%
at16:9. Unchanged foreground pixels reuse HUD grouping and uploads. CPU tile
drawing and wider scene geometry still cost time; real Vita FPS is unverified.

## GPU tiles after main 3044f3b

Current recovery build disables wide GPU tile composition after a hardware
slowdown report, using the CPU wide layers instead. Original-aspect GPU tiles
and GPU 3D stay enabled. The implementation below is retained for profiling.

The Vita branch includes the latest desktop GPU renderer but still uses GXM,
not SDL_GPU's desktop shaders. Background and centred foreground tile layers
are composed on GXM, including widescreen. Like desktop main, moving individual
HUD items to the edges retains a CPU foreground-composition fallback. Tile
decoding/cache updates remain on the CPU. Physical GPU buffering is separate:
double by default, with single and triple available in Options.

Road subdivision additionally checks perspective texture error against the
same reciprocal-depth interpolation used by main. The existing eight-way cap
and pool limits remain; this is not per-pixel perspective-shader parity and
can increase geometry work. Hardware appearance/performance needs testing.

## vitaGL renderer (`--gpu-gl`, experimental)

`scripts/build_vita.py --gpu-gl` builds `main_gpu.cpp` + `gpu_gl.cpp` against vitaGL
instead of libvita2d (`--gpu-fast`). The two flags are mutually exclusive because both
initialise sceGxm. Without either flag the CPU-exact build is produced as before.
Same launcher, options, dual-ROM switching and link play as the GXM build (the ImGui
menu is drawn through vitaGL, `imgui_vita.h`); widescreen stays a GXM feature for now
(see "Multicore GPU builds" above).

* Build requirements: vitaGL and its libraries, built pinned inside the project by
  `python3 scripts/setup_vitagl.py` (next section), and `libshacccg.suprx` installed
  on the console (runtime shader compiler).
* Polygons follow the Model 2 draw priority, as in the CPU reference renderer (higher
  window first, then smaller z sort key, then newest polygon first). The Model 2 has no
  depth buffer; the GPU one only reproduces that order: each polygon gets one depth from
  its rank (`GL_GEQUAL`, never cleared during the frame), so whole polygons are in front
  of or behind each other and intersecting polygons do not cut, as on the arcade board.
  Polygons are grouped by (clip, shader, texture) into one draw call per group.
* System 24 layers are placed by the same depth buffer: foreground before the polygons
  (in front of every polygon), background after them (behind every polygon), so hidden
  pixels are rejected before their shader runs. `k2DLayersByDepth = false` in
  `gpu_gl.cpp` restores the plain painter order (same image). The menu font never uses
  the depth buffer.

### Performance on a Vita (vitaGL build)

Measured on a PS Vita at CPU 444 MHz / GPU 166 MHz (bus 222 MHz), default pinned
cores, `--gpu-gl --fast-inaccuracy`, with the per-core profiler (`perf.log`, written by
`--diagnostics` builds): 41 windows of 5 s, about 3.5 minutes of racing.

| | Result |
| --- | --- |
| Speed | median **57.3 fps** for 57.52 on the arcade (99.6%); 40 of 41 windows at 55 fps or more, 33 at 57 or more; only the first window (start of the race) at 50.8 |
| Polygons | about 1,200 per frame on average, up to 1,700 |
| Core 0 (game + drawing) | 74% busy on average, 95% at worst: the limit |
| Core 1 (geometry) | 53% on average, 71% at worst |
| Core 2 (sound + 2D) | 78% on average, 87% at worst |
| Sound | 8 short gaps in 3.5 minutes (start and a few heavy moments), playback speed 99-100% |

So the game runs at (almost) full speed without overclocking plugins: 444 MHz is the
Vita's highest stock CPU clock. Core 0 has little room left, so a heavy scene can still
drop a frame. The release build (`--release`: link-time optimization, no profiling) is
not profiled, but does the same work with less overhead.

### vitaGL: pinned build and flags

vitaGL, vitaShaRK and the Vita port of math-neon are the work of **Rinnegatamante**
(math-neon originally by Lachlan Tychsen-Smith), SceShaccCgExt of **Bythos**: thank
you! This renderer would not exist without them.

```sh
export VITASDK=/usr/local/vitasdk
python3 scripts/setup_vitagl.py              # once; again after a pin or flag change
python3 scripts/build_vita.py --gpu-gl --release
```

`setup_vitagl.py` clones vitaGL and the libraries it is linked with at fixed commits
and builds them with the VitaSDK toolchain into `extern/vitagl/` (git-ignored, like the
rest of `extern/`). A new vitaGL commit upstream therefore never changes or breaks this
build; moving to one means editing `PINS` in the script and testing on the console.

| Library | Why | Pinned commit |
| --- | --- | --- |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | the OpenGL ES layer over sceGxm | `dca4b9d143290d78ec043131a19be36c78cdc7b5` (2026-10-07) |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | runtime shader compiler used by vitaGL (`libshacccg.suprx`) | `df24065e65098b2d1ac533760109ad4367573f28` (2026-08-22) |
| [math-neon](https://github.com/Rinnegatamante/math-neon) | NEON maths used by vitaGL | `0faab814782c071ff4015527f1ca955ab1ccc470` (2020-07-05) |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | needed by vitaShaRK | `fb0e9d338525b067f3679ab33571323336493cca` (2026-07-29) |

Only the base VitaSDK is used (toolchain, stubs, taiHEN). Nothing is installed into
`$VITASDK`, and a vitaGL already installed there is neither used nor changed: the
script puts its own headers first (`CPATH`) and `platform/vita/CMakeLists.txt` links the
libraries of `extern/vitagl/install` by full path. The configure step prints which
vitaGL it uses (`GPU GL: pinned vitaGL <commit> (release: ...)`), and so does the
summary of `build_vita.py`. Without `extern/vitagl/install` the build falls back to the
VitaSDK's vitaGL with a warning. A rerun with the same pins, flags and toolchain does
nothing; `--force` rebuilds, `--print-flags` shows the make flags.

#### Release flags (default, `--profile release`)

Chosen from what `gpu_gl.cpp` actually does: GLSL shaders only (no fixed-function
pipeline), `GL_TRIANGLES` only, 16-bit indices, no instancing, no framebuffer objects,
no `glTexSubImage2D` (texture memory written in place after `sceGxmFinish`), one VBO
pointing at our own GPU memory (`vglBufferData`), all GL calls on the main thread.

| Flag | Effect for this renderer |
| --- | --- |
| `NO_DEBUG=1` | No argument validation in every GL call (CPU time on the main core, the busiest one). The renderer checks shader compile/link status itself. |
| `HAVE_VERTEX_LAYOUT_CACHE=1` | The attribute layout and patched vertex program are cached per (program, VAO, draw type): a draw with an unchanged layout skips `patch_vertex_program`. Our layout changes once per frame, our draws are many. |
| `HAVE_SHADER_CACHE=1` | The compiled shaders of the 7 programs are cached in `ux0:data/shader_cache/<TITLE_ID>/` (keyed by source hash): only the first boot runs the runtime compiler. |
| `TEXTURES_SPEEDHACK=1` | No texture use tracking in every draw. What it gives up (the copy on `glTexSubImage2D` of a texture still in use) is never needed: the renderer does not call `glTexSubImage2D`. Freed textures are always released by vitaGL's garbage collector, a few frames later. |
| `INDICES_SPEEDHACK=1` | Skips per-stream index source setup. Only breaks instanced draws and 32-bit (`GL_UNSIGNED_INT`) indices, which are not used. |
| `PRIMITIVES_SPEEDHACK=1` | Skips the polygon mode restore after each draw. Only breaks `GL_LINES` / `GL_POINTS`, which are not used. |


#### Debug flags (`--profile debug`)

`LOG_ERRORS=1` (GL errors through `sceClibPrintf`) and `HAVE_SHARK_LOG=1` (shader
compiler messages), with every speed hack off: first thing to try when the vitaGL
build shows a glitch the GXM build does not. Read the output with a log plugin
(PrincessLog, for example). `python3 scripts/setup_vitagl.py` alone goes back to the release flags.

#### Testing the script from scratch on a machine that already has vitaGL

The script never reads the vitaGL of the SDK, but to prove it, run it with a copy of
the SDK that has none, from a fresh clone of the project:

```sh
# Hard-link copy of the SDK: instant, no extra disk space (same file system).
cp -al "$VITASDK" /tmp/vitasdk-nogl            # macOS (APFS): cp -Rc
cd /tmp/vitasdk-nogl/arm-vita-eabi
rm -f include/vitaGL.h include/vitashark.h include/math_neon.h include/shacccg_ext.h \
      lib/libvitaGL.a lib/libvitashark.a lib/libmathneon.a lib/libSceShaccCgExt.a
git clone <this repository> /tmp/daytona-clean && cd /tmp/daytona-clean
VITASDK=/tmp/vitasdk-nogl python3 scripts/setup_vitagl.py
VITASDK=/tmp/vitasdk-nogl python3 scripts/setup.py --no-build
VITASDK=/tmp/vitasdk-nogl python3 scripts/build_vita.py --gpu-gl --compile-check
rm -rf /tmp/vitasdk-nogl /tmp/daytona-clean      # the real SDK is untouched
```

`rm` on the copy only removes its own links. The configure output must say `GPU GL:
pinned vitaGL ...`; with no vitaGL anywhere it would stop with `GPU GL: no vitaGL`.

### How the 2D layers (System 24) are drawn

The HUD, the sky and the other 2D layers come from the System 24 tilemap chip of the
board. The vitaGL renderer does not turn them into RGB images: like the arcade
hardware, it keeps **indexed images** and resolves the colours on the GPU.

* **Layer textures hold colour numbers, not colours.** Each of the 4 layers is a
  512x512 texture (one for the background pass, one for the foreground pass: 8 in
  all). A texel stores the pen number (0-8191) of that pixel, encoded as palette
  texture coordinates: column (`pen % 128`) in red, row (`pen / 128`) in green.
  Alpha is 0 for an empty pixel, which the shader discards (it never reads a colour).
  See `system24_index_texel` in `system24_upload.h`.
* **One palette texture holds every colour.** A 128x64 texture, one texel per pen:
  all 8192 System 24 colours, shared by every layer and both passes
  (`upload_system24_palette`).
* **The shader does the lookup.** For each pixel the `Layer` shader reads the layer
  texture at the scrolled position, gets the pen number, then reads exactly that
  texel of the palette texture (bound on texture unit 2, point sampled: no filtering,
  which would mix unrelated colours). The palette is a lookup table: it is never
  displayed or scaled.

  ```
  layer texture (512x512)          palette texture (128x64)
  texel = pen 1234  ──────────────▶ texel (1234 % 128, 1234 / 128) = colour ──▶ pixel
  ```
* **Fades cost almost nothing.** A fade to black is done by the game itself: it
  rewrites its palette RAM step by step, the tile pixels do not change. The renderer
  only copies the 8192 colours (32 KB) into the palette texture when a colour changed.
  With RGB textures, every fade step meant rewriting all 16384 tiles (8 MB, ~67 ms on
  the Vita): the old stutters during fades.
* **Only changed tiles are rewritten.** Each 8x8 tile has a generation number;
  `upload_system24_layer_indices` rewrites the tiles changed since the last upload,
  row of tiles by row of tiles: changed neighbouring tiles become one run, and each
  texel line of a run is written in one go (NEON, 8 texels per step). A full rewrite
  (scene change) is then long sequential lines, which suits the write-combined GPU
  memory.
* **One frame late, prepared on core 2.** The geometrizer runs pipelined (the 3D shown
  is the previous frame's), so the 2D shown is the previous frame's too. There are two
  sets of layer + palette textures: while a frame draws the set prepared during the
  previous frame, the `daytona_2d` worker thread (core 2, `DAYTONA_VITA_2D_CORE`)
  uploads the current frame's tiles and palette into the other set and computes the
  layer rectangles (scroll, split screen, windows). The main core only emits the quads.
* **Placement by depth.** Foreground layers are drawn before the polygons and
  background layers after them, with fixed depths (see above).

Model 2 polygon textures work on the same principle, with one difference: their texels
are 4-bit indices that the GPU **filters** (blends between neighbours) before the
lookup, as the real board did. Each polygon gets one 128-entry row of a palette texture
(one row per luma table, colour and face brightness), so the filtered in-between values
also have a colour. The rows are built by the CPU the first time a combination appears,
then cached.
