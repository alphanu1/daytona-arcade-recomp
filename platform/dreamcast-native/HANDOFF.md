# Daytona Dreamcast native: current state

Updated 19 October 2026 (the placed actors lit once per placement key and not when rejected: "19 October: the placed actors' lighting (branch actor-cost)"; Expert's world actors and the Jeffry statue drawn: "19 October: Expert's world actors and
the statue (branch expert-actors)"; the attract cycles the three courses, the next one loaded in the title's frames: "18 October:
the attract over the three courses (branch attract-courses)"; Advanced's eight animated world actors drawn, the course-0
podium compare passing: "18 October:
Advanced's animated world actors, the course-0 podium compare (branch course2-actors)"; the podium of Advanced and
Expert, every course's props, the course-2 actors measured: "18
October: the podium of Advanced and Expert, every course's props (branch course-extras)"; Expert: "18 October: Expert
raced (branch expert)"; front end: "17 October: front-end
flow, course and transmission select"; start here:
"16 October: branch integrate2" under
"Current state" says what landed since the night -- the CONDITION radar and
TIME EXTENSION, the opponent results field and the carried-state finding,
the live-play reject fixes, dust and sparks, music in every set -- how
integrate2 was verified, what waits for the console and what comes next;
"15 October, night: the day end to end" sums up the day before; the
sections after it are the detail:
fence choice, late-frame fixes, rock decal cores: "Night of 14-15 October";
rubber streaks under the car shadows and super-sampled signs, assets-ssfar4e:
"15 October, day"; branch integrate with the slot machine, whole-program LTO
and the recommended set assets-ssfar4eu: "15 October, afternoon"; the plain
catch fence, assets-ssfar4f recommended and the fence sweep off by default:
"15 October, evening"; AICAflow, the soundtrack and its recipe step:
"Sound"). This page is
the working summary; git
history on branch `dreamcast-native` is the record. The root `HANDOFF.md`
is the arcade side's historical archive: search it, do not read it end to
end. `rules.md` at the root holds the project rules.

## Place in the repository (30 October)

The port is `platform/dreamcast-native/`, next to `platform/dreamcast`, and the
default build of the repository is upstream's: `M2_DREAMCAST_NATIVE` (default
OFF) includes `host.cmake`. With it off the CMake targets and tests are those of
upstream main, and the preprocessed `src/runtime` files too.

- `src/runtime` keeps only hooks behind `M2_DC_NATIVE_HOST`, compiled into
  `native_runtime` and `native_refcore`. The ROM paging, word-sized ROM images,
  lazy page tables and GPU-only video of the old bring-up are gone with the
  options that used them: `m2segment --rom-cache-kib`, `--verify-streaming`,
  `--profile-*`, `--stage`, `--export-textures`, `--export-system24`,
  `--check-materials`. HISTORY.md still names them.
- `tools/m2segment.cpp` runs `rt::GameLoop` from images it loads itself, without
  the sound ROMs. `tools/no_sound.cpp` stands in for the 68000 code, which the
  recipe does not generate. `m2import_silent` replaces `m2import --no-sound`.
- `tools/material.h`, `tools/dreamcast_assets.h`, `tools/system24.hpp`,
  `tools/prepare_dreamcast_textures.py`, `tests/pvr_geometry.hpp` and the
  `scripts/fetch_*.sh` moved here. `recipes/inputs/course_{advanced,expert}.txt`
  are the recipe's held-steering menus; upstream's `scripts/inputs` keep theirs.
- `docs/design-notes.md` holds what the port added to the design document,
  `THIRD_PARTY.md` the pins.

The two host tests that had been failing since 18 October were out of date, not
the code: `native_models` (`race_scenery_lumas` takes variable specular since
2380af2) and `native_checker_prune` (`--world-cells` splits checker vertices since
b4fe80b).

Findings that lived in the root handoff:

- The 14-entry window at 0x13f5c (5 back via +0x8c, 14 forward via +0x88, list at
  +0x5c) contains cars, not road sections; pool membership is verified at frames
  240 and 300. The ordered-pair checks and TGP maths remain untouched, and their
  exact collision meaning is not established by that check.
- Do not divide eye-crossing triangles before side clipping: the native stress
  test measures visible precision loss. Keep the independent geometry oracle and
  the magnitude and viewport checks when optimizing. Strict C library NaN and
  min/max calls in a per-triangle SH-4 loop also cost heavily: IEEE finite checks
  plus per-vertex caching cut one diagnostic from 91.57 to 20.66 ms.

## Goal and order of work

A Dreamcast-native Daytona USA in `platform/dreamcast-native/`, the whole game. The PC
code under `src/` and `tools/` is the behavioural reference. Priority:
first the game plays right, then graphics, then sound and disc.

1. Whole-race agreement with the original on course 0 (in progress).
2. A real driven 8-lap finish on hardware, including a top-three finish.
3. HUD: speed, tachometer, position, lap, time.
4. Front-end flow: attract loop, course and transmission select, game over (in progress, branch frontend).
5. Advanced and Expert courses.
6. Remaining rules: difficulty and race-length settings, VMU records.
7. Full scenery and sky back (staged, see "Scenery stages"), then renderer
   work to hold 60 Hz.
8. Sound, then a disc image tested on the real drive.

## Standing rules

- enDjinn owns startup, loop, input, render queues and shutdown.
  START+A+B+X+Y returns to the loader.
- Keep the 17,384 us simulation step. Never change game speed to hit 60 fps.
- No ROM bytes or ROM-derived tables in tracked files. Generated headers and
  packs live in ignored `build/` directories.
- Every gameplay change cites the original code it reproduces. Never move a
  test expectation to get a pass.
- (Superseded 12-14 October: renderer work was redirected toward 60 Hz
  with full scenery; see "Current state".)
- Rendering rules (14 October): 4-bit palette textures only, never
  8-bit palette textures; a 16-bit VQ texture is acceptable only as an
  explicit case-by-case exception. Palette RAM in ARGB1555. 24-bit framebuffer stays
  (16-bit framebuffer ruled out). All renderer math and every memcpy-like copy
  through SH4ZAM; gameplay arithmetic stays exact. Anything on the
  punch-through list that samples no transparent texel goes on the opaque
  list. No mip chains on surfaces seen at grazing angles (road, forest floor)
  or on fine structures (stand glazing mullions). Bilinear filtering on every
  3D texture the arcade filters (the fence stipple and HUD stay point
  sampled). Texture super-sampling only on the far road (4x the
  one-texel-per-pixel distance), the start/finish board and, since 15
  October, the readable signs (3x, as the board). The arcade
  decides what the picture contains; PVR-native techniques decide how, and
  the picture may be better than the arcade where that is cheap.
- Shared console: source enDjinn's `environ.sh` (its `kos-tool` wrapper takes
  a lock in /tmp/dctool-HOST.lock; the holder is in the .who file). Hold
  `dctool_lock` only around ready probe + upload + run, release it as soon as
  the console program has exited (not when an HDMI capture ends), and never
  reset or kill a run someone else started. The host must stay awake during
  console runs (dcload stalls the console while the host sleeps).

## How gameplay is verified now

The main tool is a free-running comparison of a whole race against the
original, not per-routine checks:

```sh
D=$(mktemp -d build/dc-native-race-lockstep.XXXXXX); A=build/dc-native-align-tables.cRiCBG/assets
./build/m2segment build/rom_cache/daytona93 8300 --inputs scripts/inputs/race_to_end.txt --trace-race $D/reference.trace
cc -std=c11 -O2 -Iplatform/dreamcast-native/include -Ibuild/dc-native-race-assets.uhUxz3 \
   -Ibuild/dc-native-announcement-assets.Ho8t4S platform/dreamcast-native/tests/trace_race_native.c \
   platform/dreamcast-native/code/{drive,contact,road,path,visibility}.c -lm -o $D/trace_race_native
$D/trace_race_native $A $D/reference.trace $D/native.trace 5 6000 production
python3 platform/dreamcast-native/tools/compare_race_trace.py $D/reference.trace $D/native.trace --first 5 --search 2 --tolerance 1
```

`production` mode feeds exactly what `code/practice.c` supplies. Fix the first
divergence at its source owner, rerun, repeat.

State on 11 October, late: with the two scripted scenarios (full throttle
without steering, and one with steering, braking and a crash) the host build
has all 40 cars bit-identical to the original on every tick up to the
time-out, except three ticks at scene load where the reference trace samples
mid-pass. Fixed on the way: rolling formation only for cars behind the line
(`c914..c938`); board-default difficulty 1; phase 20/21 entry ticks; guided
mode owner (`77c4`); rolling-start camera and start sequence (`6474`,
`ab78..ad30`); crash camera (`15d18`, table `2864aa8`); detail against the
previous pass's cells; follow heading from body yaw.

Console versus host: car state uses the source-exact divide and length on
the Dreamcast too (SH4ZAM's approximate forms moved the console off the
original within one rolling start). With synthetic full-throttle input the
console matches the host for all 40 cars at every 50-tick checkpoint over
3000 ticks: build with `NATIVE_RACE_CHECKPOINTS=1` and the
`tests/race_controls_wrap.c` wrapper, compare with
`trace_race_native ASSET_DIR - NEW_TRACE 0 3000 gas`. A real controller adds
analogue noise, so console runs with physical input are not repeatable.

Host builds of gameplay code must use `-fno-fast-math -ffp-contract=off`;
without them two grid cars land on the wrong road piece.

Full races: `tests/drive_race_native.c` searches steering inputs with the
native race as simulator and writes a `scripts/inputs` file. Two scripts in
`build/dc-native-race-lockstep6.aFnRK9/` complete all 8 laps in the reference:
2nd place (22-23-24-27-28-25-26-29-30-2) and 9th place (22-23-24-25-26-29).
The native race agrees through the finish, podium timer and cars, record
selection and results timer on the reference ticks.

On hardware (11 October, night): `NATIVE_RACE_SCRIPT=1` replays a script
without a pad and returns to the loader at phase 29. The 8-lap second-place
script (race, finish, podium, results; 12,477 ticks) and the tenth-place
script (11,168 ticks) both match the host on every checkpoint row (260 and
232). Commands and expected files: `build/dc-native-race-lockstep7.hoCZmg/`.
Step 2 of the plan is therefore done for course 0 with automatic
transmission; next are the HUD and the front-end.

The original keeps each car's collision-tracking words (+0x28c..298, +0x27d,
+0x2a4) from the scene that ran before the race; a cold native start zeroes
them. It matters only when a car collides on the very tick it enters the
14-car window, but then the two sides choose different responses (seen at tick
1503 of the 9th-place run). The owner since 30 October is the pool
(`include/frontend_pool.h`, "30 October: games one after another").

Podium, from the reference: entry arms the graph and the cars spawn one tick
later; after cleanup at timer 384 the original shows a static
"Congratulations!" 2D screen with a staff photo, and the same banner sits
over the cinematic before that. Neither overlay is drawn natively yet.

Not yet compared: phases 29/30 in the harness, view changes, time penalties,
the cameras armed through `5af8`/`5b20`. Manual transmission: since 17
October, see "17 October: manual transmission".

## What runs on hardware

Title, cinematic attract, 40-car race with controller input, finish coast,
podium for a top-three finish (phases 27/28, runs the `2865a70[course]` stage
graph through the shared attract scene), results, name entry, back to title.
About 16.7 ms per frame in the reduced view (cars, road, kerbs, barriers).
Music through AICAflow (since 15 October; effects and voices not yet: see
"Sound"). Scenery and sky are back as build stages, see below. Stage 6 (the
default) runs on hardware: see "Current state".

## Current state (16 October)

Defaults (`platform/dreamcast-native/local.cfg.mk`; details in "Build and hardware"):
whole-program LTO in one partition (`NATIVE_LTO=one`, since 15 October),
music through AICAflow (`NATIVE_SOUND=0` builds it silent, AICAflow linked
but not started), stage 6, `NATIVE_FILTER_ALL=1` (every 3D texture bilinear but the catch-fence
stipple), `NATIVE_PT_ALPHA_REF=0x80`, `NATIVE_PAL16=1555`,
`NATIVE_MIP_CLASSES=1`, prebuilt world and car streams,
`NATIVE_SUPERSAMPLE_FAR=1` (default with `NATIVE_FILTER_ALL=1`; `=0` builds
the same image without it, ELF suffix `-nossfar`), and since 15 October (chosen)
`NATIVE_SMALL_CAR_PX=12` (ELF suffix `-smallcar12`; 0 off) and
`NATIVE_GUARD_BAND=2048` (C defaults of practice.c and model_submission.h the
same), and since 15 October, evening `NATIVE_FENCE_SWEEP=0` (C default the
same; `=1` ELF suffix `-fencesweep`). Recommended asset set: `assets-ssfar4f`
of the recipes since 15 October, evening (the chosen set: ssfar4eu with
the plain catch fence as the arcade defines it; "15 October, evening" below),
since 16 October without super-sampling on the infield grass 6924 and the
aprons 6664 ("16 October: the grass is not super-sampled" below).
Before it `assets-ssfar4eu` (ssfar4e + every sign of the sign sheet
super-sampled, no mip chain on a super-sampled sign word, three-level fence;
"15 October, afternoon"), before that `assets-ssfar4e`
(ssfar4cf3r + rubber streaks on or above the road + super-sampled signs, "15
October, day"), before that `assets-ssfar4cf3r` (three-level fence, rock
decal cores, world face groups of 8). The world packs of every set carry the
slot machine reels; every set carries `car-damage.dnb`, the HUD pack with
the CONDITION panel and TIME EXTENSION, the car effects in `car.dnm`/`car.dnt`
and (since integrate2, from `music-handoff`) `music/` with the soundtrack.
The hardware runs of 15/16 October on top of it (radar, oppHr, fxE,
rejectStats) are in "16 October" below. Latest hardware
run of the recipe's default build: the recipe's stats ELF of 6b9fa5e (loaded image
11fa627c...) with assets-ssfar4f (`build/dc-native-fence-hw.fP9gK9/hw/console-ssfar4f.log`):
checkpoints identical, 3 late frames, race registration 7.63 / 16.6 ms mean
/ max, podium 9.60 render and 10.14 registration, race render 9.60 / 14.9 ms
(silent: that set had no `music/`). A fresh recipe run from the ROM and the
music pack on `music-handoff` reproduces every delivered output, 2,174 of
2,174 rows (15 October, night; the run directory was deleted after the
check, disk); on integrate2 the same, 2,174 of 2,174 rows
(`build/dc-native-int2.recipe`, "16 October").
`run_hw.sh` records video only with `VIDEO=1` (the disk filled up).

### 31 October: the last late frames (branch late-frames)

- **Frame trace** (`NATIVE_DIAG_FRAME_TRACE=1`, needs `NATIVE_DIAG_DROP_LOG=1`; `include/frame_trace.h`,
  `tools/frame_trace_report.py`): every frame with updater + registration over 15 ms, and for the run ticks
  `NATIVE_DIAG_FRAME_TRACE_LO..HI` the updater's scopes, each list's time and TA bytes, triangles, headers, the model
  renderer's groups, per world batch and per opponent. The marks read TMU2's counter (one uncached register read), not
  `timer_ns_gettime64`. A build with it moves a few hot entries (a loud warning at the link); keep the window under 100
  frames on Expert, whose heap has little to spare.
- **Why a frame is late.** The CPU is saturated, not one frame heavy: with wait 0 the end of registration moves later
  after the flip by updater + registration - 16.68 ms a frame, and the frame misses when that plus the render passes the
  next flip (`L[k] = max(R[k]-V, L[k-1]+U[k]+R[k]-V)`, late when `L+P > V`; this reproduces 4589, 4598, 4612 from the
  logged U, R, P). A miss gives back one vblank, and the next miss is V / excess frames later: the 10 ticks of Expert's
  3535 and 3545 are 16.7 ms / 1.6 ms a frame, not a period of the game. The frame after a miss runs two ticks (+2.9 ms).
- **Beginner 4582..4619:** three cells (meshes 5, 11, 17) enter the arcade's cell list at ticks 4582-83 with five of mesh
  111: world batches 10 -> 16, TA vertices 6,000 -> 9,200, registration 10.5 -> 13.7 ms. In the worst frame: world prepare
  4.0 ms (points 2.5, marks and LOD 0.8, classify 0.4), world opaque 3.1, punch-through 2.2 (clipped faces 0.8 of those:
  49 faces at 17 us), opponents 2.7 (15 cars, 12 at level 1 at 54 us each), player 1.0, shadow volumes 0.4, slot machine
  glass 0.28. U + R 18.0 ms for 38 frames; one vblank's budget is about 9 ms, so 0.8 ms a frame less was needed.
- **Expert 3524..3550:** the horses and birds. 20 birds join five horses (world batches 14 -> 37), the placed actors take
  setup 3.0 ms (Beginner 0.4) and 2.7 ms of world prepare: 6.2 ms of the 18.3 ms frame. The frame update wrote every
  vertex record through a lerp of three axes: a horse's 155 points are 423 records, 240 us a horse.
- **Fixes, nothing of the picture or the race moves:**
  - `nm_render_world_clock_frame`: each moving point once, then its records; rate 1 (every frame stored) without the lerp.
  - `sq_cache_point_lean`: no near test on the console. A proven point cannot be behind it; host tools abort if one is,
    and `NATIVE_BOUNDS_VERIFY=1` checks every proven point against the five planes on the console: 78.7 million points
    over the three courses' races (36,888 frames: that build runs late), no failure, checkpoints 0 differing.
  - `sq_lod_level`, the shadow rings: a sign test, not GCC's `__unordsf2` call for each far group and ring point.
  - `sq_prepare_source`: consecutive instances in one window share their view, instead of four libm calls an instance.
    `instance_view` itself is pinned by `test_practice_render_split.py`.
  - Slot glass: a reel wholly on the screen is projected without a classification per point, with sign tests (a plain
    `!(a > b)` there is six libgcc calls a point, 576 a frame): 280 -> an estimated 60 us, not measured.
  - Tried and dropped: projecting each shadow ring point once (-0.15 ms): its slow path out of line grows .text past the
    8 KiB step below.
- **Hot layout traps.** GCC's inlining of `practice_prepare` flips with small changes: `records_build` is noinline,
  `sq_prepare`, `sq_prepare_source` and `nm_render_enqueue_objects` always_inline, or the function leaves its slot.
  `tests/test_sq_quads.py` slices `static int sq_prepare(void)` and `test_practice_render_split.py` hashes
  `nm_practice_render_parts`, `nm_practice_render_scenery` and `model_math.h`.
- **KOS's clock jumps 2.5 ms a second.** TMU2 reloads with 12,468,720 counts and `timer_ns_gettime64` reads each as 80 ns:
  0.9975 s, then a jump at the underflow. The 1 Hz spikes of 2.54 ms in `upd`, `reg` and the 19,163 us of "Native displayed
  frames" are that jump, no work (the trace's TMU reads do not show it).
- **Where the statics sit.** The KOS link script aligns what follows .text to 8 KiB, so .rodata, .data and .bss move by
  8 KiB steps with the size of .text, half the O-cache, and a step past it adds 16 KiB of image end (heap residue): the
  heap budget of `stats` then fails (251,304 never taken, budget 262,144). `stats-advanced-finish` and
  `stats-expert-finish` are past that step already, since before this branch, with that figure: their `make check` stops
  there; the recipe does not run it on them, and `play` has 333,224. .text may reach 843,776 bytes at that step; this
  branch ends at 842,052. `stats*` sit at residue 0x2080, `play` and `controller*` at 0x0080. One run of the final code
  padded past the step (residue 0x0080): updater mean -0.14 ms, 1 late frame instead of 2. An earlier candidate at that
  residue, with the shadow rings projected once and `instance_view`'s limits inline (which `test_practice_render_split.py`
  refuses), ran 3.41 / 7.96 ms and 0 late, once; 0.19 ms of its updater is not explained. So the game's ELFs may be on
  the better residue; no run of one over this race exists.
- **Console.** Before (logs gaps2-*): Beginner late 3, race update/registration means 3.91 / 8.27 ms; Expert late 2, 3.46
  / 7.00 ms. Now, checkpoints 0 differing in every run, hot layout at its slots on `stats*`, `play` and `controller`, image
  ends unchanged, `make check` as before (81 PASS, the 5 known FAIL lines): Beginner late 2 (ticks 4593 and 4618, 3.75 /
  8.09 ms), Advanced 0 (2.58 / 6.86), Expert 0 (3.28 / 6.92 ms, flips equal vblanks), attract 0; one commit earlier
  Beginner 2, 2, Expert 0, 0. The V.R. demos ran 1, 0, 1 late against 1, 0, 0, the attract missing 4 vblanks both times.
  Pad monkey schedule 2: 8 games, no reject. HDMI: slot glass, shadows, birds and horses look right (no older video kept to compare).
  `check_stream.py` at step 20: 27 PASS.
  Beginner's two: U + R still about 17.7 ms in 4583..4619; the model asks for about 0.25 ms a frame more.
- **Title stalls now** (`console-gaps2-attract.log`, quiet host): none over 25 ms. Longest title frames 22.3, 20.4, 20.9 ms,
  longest loader slices 22.2, 20.3, 20.7 ms (17, 19, 9 of 435, 473, 354 past their end), longest file call a 32 KB read
  in 6.8 ms. Flips 8,317 over 8,321 vblanks; the V.R. demo's one late frame (28.5 ms) is one of the four.

### 30 October: four gaps closed (branch gaps-1)

- **Eye turn 69b4** (`race_view.h` `nm_race_view_eye_turn`, `race_camera.h`): off course 0, except in VR4, a camera entity
  pitch +28 of -546 or less turns the eye about x by pitch + 546, clamped at -1092, in registers only (+b8, +bc and the
  scenery anchor keep the unturned eye). +28 is half the ground pitch in modes 0 and 2, the full one under the crash
  camera (669c), 0 under the start and pit tracks (667c). View 501560 against the original, rows over 0.001 before ->
  after: Advanced finish 2,847 -> 0 of 13,002, Expert finish 730 -> 0 of 14,256; with VR1..VR4 cycled 425 -> 0 of 5,842
  and 446 -> 0 of 5,356 (VR4 does not turn); time-outs 102 -> 0, 156 -> 0; three crashes on a slope 184, 158, 175 -> 0 of
  192 each, largest 0.64 -> 0.0002. Race views stay within 0.00037 units (word rounding, as on the rows that never
  turn). The turn feeds the detail count, yet every car stayed equal before and after: no race changed. The V.R.
  demos never reach the pitch. `compare_race_trace.py --view-limit U` checks race rows; the course steps use 0.001.
  Crash runs: the finish inputs plus `F-F+70 steer=0x00|0xff`, `accel=0xe0`, then wheel centred, F = 2327 + 1910 on
  Advanced, 2367 + 2970 on Expert.
- **START in the podium and the results.** The tasks read 53e584 bit 4 in every scene: the big bird 22475c, the statue
  22231c, the slot machine 220554 (visible 574 podium rows on Beginner). `props_scene.c` reads the edge on each tick
  that steps the actors, `nm_intro_scene_t.start_edge` carries it to the cinematic's slot machine, practice.c latches
  START in the podium and consumes it in its ticks and the results'. A second find: the result actor takes 53e120 bit 4
  HELD (b680): at 576 or less the countdown drops to 192; `nm_results_t.start_held` feeds `nm_result_clock_step`,
  which was always called with 0. `trace_race_native` runs that step too, and after the race now reads the reference
  rows of logic passes only (`next_logic_row`, as compare_race_trace.py's stalled()). Runs with `start=1` two frames in
  47: Expert finish (frames 16790..18350, 18365..19020): 16,291 rows equal with --podium, test_expert_lockstep 15,596 of
  15,598 + 2 stage cuts (15,243 before, the bird 4.0 off); Expert time-out (8380..9040): 6,308 rows, 6,309 of 6,309 draws;
  Beginner finish (podium ticks n 10580, 10640, 10700, 10760): 12,477 rows, reels and state equal to the original's RAM
  at six ticks (`build/dc-native-game2.work/host/m2segment`, `M2_SNAP_DIR`, `M2_SNAP_B0`; two of the four edges fall
  outside the visible rows and are lost in both). Not stepped: Beginner's slot in the results of a race without a
  podium (no world drawn, nothing reads 5010d0 there). START in the name entry was pressed in the Expert run but the harness
  stops at phase 29.
- **Slow knock 2230ac.** `cone_run.cpp`, `props_run.cpp` and their runs were deleted with their build directories, so
  `m2segment --trace-props NEW_FILE` (with --trace-race, tools/trace_race.hpp) now writes the run format of
  tests/test_props.c; the race trace is byte-identical with it on. `drive_race_native --replay SCRIPT --target
  X,Z,PROFILE,NEAR,SPEED` follows a script to NEAR road profiles before PROFILE, then a controller goes to the point at
  that speed index. `recipes/inputs/inputs-prop-advanced.txt` (id 4, profile 47) and `inputs-prop-expert.txt` (id 14, profile
  555) hit the prop at speed index 54: test_props 10,917 of 10,917 ticks (649 with a moving prop), 32,273 of 32,273 draw
  matrices, 2 of 2 hit sounds, knock starts 2230ac 2/0; every car equal (1,698 and 6,997 rows), 9e00 exact. With the
  Expert finish as a check of the tool: 27,625 ticks, 73,251 draws, 7 sounds. Recipe step `props_slow`.
- **Expert's attract.** Cars, camera, cells and light of every stretch were compared already; draws only on Advanced's
  cinematic. `trace_attract_native --clock F REF --actors C` steps props and actors as practice.c and writes K, P rows for
  course C's cinematic and demo. test_expert_lockstep 2,756 of 2,756 rows (22214c, 22275c, 223bc0, 223ce0, 22430c, 224730,
  224854 drawn; 0.0001 units), test_clock_lockstep on Advanced 2,756 of 2,756, test_props over four loops 11,128 ticks and
  21,149 draws. No game code changed. 5036fc stays 0: the attract's car never enters the statue's zone. On the demo's
  last tick `race.course` is the next course and the actors do not step: nothing they draw is lost there (equal rows).
- **Open.** The keyed pit tracks differ on single rows: 126 of 1,276 rows of the Expert time-out over 0.001, largest 0.20,
  the same before the eye turn (+28 is 0 there); not looked into. In the results the native runs no camera pass, so a
  view left by VR1 or VR2 stands where the original's glides back: nothing is drawn. Whole-recipe runs of the changed
  steps (`props_slow`, `attract_lockstep` with Expert) have not happened; their commands ran by hand.
- **Console.** The benchmarks ran on 30 and 31 October: checkpoints 0 differing, the eye turn's slopes without a late
  frame beyond the usual two. Still by hand, `elf/play` on Expert: hold in the results to skip them, press START in the podium (the
  bird grows, where drawn); on Beginner the slot's reels stop in the podium. Knock a slow prop by hand on Advanced at
  (-298, 165). Expected checkpoint rows do not change: none of this is in them.

### 30 October: games one after another (branch second-game)

A second game after a race, and a game started from the title or the V.R. demo, are bit-exact with the original: one
carried pool from power-on (`include/frontend_pool.h`, owned by `code/practice.c`), which every scene's exit fills and
every scene's start reads, as the original's 44 entities, never cleared. `nm_attract_carry_t` and the cold pool are gone.

- **How the list was made.** A local probe in the board's read and write paths (not committed: per byte of 500000..5fffff,
  was the first access after the select's last iteration a read) and RAM snapshots per phase, for game 1 and game 2 of
  one script, then the same from phase 29 to the next credit against power-on. Words read first and different between
  the two runs are the candidates; each was then followed to its code. Reads that only feed a mask or an overwritten
  value (+0, +30 at a grid reset, which keeps it only without a road hit) carry nothing. A word read first only on some
  paths shows up only where a run takes the path (+260 on Advanced's standing start): for entity 0 the whole struct is
  therefore kept and the entry's own writes applied (`nm_player_carried`), not a list.
- **What is carried.** 5010b0 (`nm_practice_game_over`, then the name mode: phase 29's iteration plus one a frame of
  phase 30). All entities, each scene's exit: +28c..+298, +27d, +2a4 (as before), +244 the neighbour 136e8 avoids
  (`remembered`), +60, +6a (a race's rank: the opponents' first step reads it, 10e10, before the sort pass writes it), and of an entity the next
  grid does not run, +1c, +24, +13e (136e8 measures the distance to it through +244: a smaller grid than the last one).
  Entity 0 across a game: everything e20c and c7ec do not write (`nm_player_carried`): +194/+198, the wheels' surfaces
  +1a8..+1b4 and heights 501100..50110c, +1f0, +22c, +248, +24c, +25c, +260, +264..+26c, +270 and the rest; c7ec writes
  +148 and +215 (done in the start), the player's start c940 zeroes the masks +1ba..+1c0 in phase 2. +b0 is a 32-bit
  word: the race's whole, then the cinematic's low half (caf4: -(profile +2) on every tick of d230). On a rolling start
  the first tick rewrites it, off course 0 e93c filters from it, which is why a first game on Advanced needs it too.
  Globals: 503680 the crash orientation (never reset, every scene's), 5010cc the race clock (stays through the attract;
  15ae4 spins a car only above 1920, so a demo after a game differs from one before it), 5014ba, 501710. After a podium the
  entities of its largest stage (40 on Beginner) hold its tracking words, not the race's; the rank key, lap and sort
  lists stay the race's, and the podium scene starts with 503680 and 5010cc of the race (`nm_frontend_podium_start`).
- **Not carried, on purpose.** The opponents' wheel phases +5a (drawn only). +138, +144 (phase 2 clears +144). +27e/+280/+284 the
  height sample: `sampled_tick` stays "five ticks old" (a sample would have to be exactly 65536 iterations old to count).
  501800.. and 501600.. (drawn), the sound queue, 53e5f0 and 53e600 (interrupt counters). 501884 and the other sound
  words stay in their statics in `code/audio/race_sound.c`, but no lockstep compares a second game's sound yet.
- **The game start on the demo and the title.** The credit's iteration in phase 7 is an ordinary demo tick (2028 -> 20bc,
  before the timer and 2058, so the course stays); then phase 12 (the opponents run, the player's c9c4 takes only e078,
  1210c) and phase 17 (objects off, 1210c once more): `nm_demo_game_start`. Phase 8 at a demo's natural end does the
  same off-pass (`nm_demo_field_off`), and from then to phase 18 no entity word changes: a credit in the title needs no
  game start at all (`NM_CREDIT_TITLE`).
- **Two faults that were not carry.** The start signal object 504900 runs before the player 504e00 (the chain 1838 goes by
  address), so ecc8 reads the rpm it has just revved: it ran after the cars here, wrong on any tick the accelerator moves
  in a standing start. In the one-game traces that moves 8 `pitch` rows on Advanced and on Expert, to the reference's.
  And `input.active.variant` was `ticks & 1` where 503690 is `5010b0 & 1`: same on an even counter base only.
- **Harness.** `tests/game_chain.h`: `trace_race_native --leave` writes the pool and the counter, `trace_attract_native --enter
  S --credit TRACE --leave S2` runs the attract after it (or from power-on) to the original's credit and the game start
  on whichever scene it finds, `trace_race_native --enter S2 --frontend TRACE` the select and the race, so each process
  has one course's packs. A name entry runs no scene there: its length comes from the F rows (with no record the count
  must equal them). `tests/test_games_lockstep.py` chains that from power-on over `recipes/inputs/games-{A..J}.txt`
  (step `games_lockstep`, CTest `native_games_lockstep`).
- **Result.** 10 runs, 18 games: compare_race_trace --require-equal, every field of every car and the timeline (tracking
  words included; `display` aside, as everywhere, and the rows sampled inside a pass at the race's load), and the attract
  before each game, every tick, the demo included: 8 runs of two games: Beginner -> Beginner, Beginner -> Advanced manual,
  Advanced -> Beginner, Expert -> Advanced, credits in the cinematic, ranking, demo and title, and Beginner to the podium
  and a name entry -> Beginner; 2 first games from the title and the demo. The attract after a game with no credit:
  8,274 of 8,274 rows over three loops (courses 2, 1, 0). One-game traces against 9da2e50: fast, manual, attract, front
  end A and B byte-identical; E differs only in `str` of row 0 and Advanced and Expert only in those `pitch` rows, each
  now the reference's value; checkpoint rows identical on all four. `test_frontend_lockstep.py` no longer skips `str`.
- **Not run:** a console, and the step `games_lockstep` inside a whole recipe (a recipe held the lock): its body ran alone
  on the delivered outputs, 13 minutes, 4 traces at a time. make check of the play ELF: 79 PASS + the 5 known FAIL lines;
  heap 349,608 never taken, 703,520 free while racing. Hot layout 160 of 160 needed `nm_hud_tick` marked noinline: the
  unit's other growth (the demo's game start, 4.6 KB of text) inlined it. The probe of the first point:
  `build/dc-native-game2.work/probe.patch` (`M2_PROBE_DIR`, `M2_PROBE_FROM`, `_NEXT`, `_TO`; `M2_SNAP_DIR`).
- **Open.** Time attack is still not native. A second game's sound commands are not compared (trace_race_native's sound
  statics do not cross its processes). A `+244` link to an entity past a smaller grid is carried by reading 136e8 only: in
  runs D and F no link pointed past the grid (D's one link at the race entry: entity 13 to 15).

### 30 October: the stream simulator (branch sim-net)

The simulator had stopped building with SH4ZAM 0.10, drew the road class at 0.5 whatever the camera, and had no frame
table left: the tables and their generator lived in build directories that are gone.

- `platform/dreamcast-native/tests/stream_sim/gen_frames.py OUT.frm ASSETS HEADERS [--course N] [--views TICK:VR,...]
  [--attract TICKS]` writes tables from the game's own code: the frame assembly of `render/scene/practice.c`, sliced
  into `tests/trace_race_native.c`, replays a recipe's race script through the podium (`--script script-manual`: the
  manual car). A script holds no view button;
  `--views` presses them, and the race then leaves the script's course (what is drawn feeds the opponents). Tables are
  ROM-derived: `build/` only. 12,000 ticks at `--step 20` are about 4 MB.
- FRM5 carries each frame's road depth scale, as `practice_road_depth_pick` chose it on every tick. The simulator sets
  it before the frame's lists, the reference expects it, and a frame outside the rule (0.5 for the attract, the podium
  and the keyed cameras; else 0.5 or a preset's h/(h+0.6)) fails. `SIM_ROAD_DEPTH`, `_STEPS`, `_CYCLE`, `_CHECK`:
  `sim_main.c`.
- `check_stream.py` per table: the scale changed in the middle of the run against a world built at it from the start
  and against the queue path, word for word, with every record on the path compared with a build each frame; then the
  record cache with changes falling in meshes' builds (a table that does not manage that fails: it needs some 400
  frames). `--course 1` and `2` take Expert's and Advanced's worlds; Expert runs on its record cache.
- Reference: z is q times one factor, class depth times layer factor, the renderer's form. `(q*layer)*scale` agreed
  only at 0.5. Frame-counter groups (`NM_OBJECT_CLOCK`) are outside its object selection.
- Open, nothing changed for them. `SIM_SH4=bound` and `bound-all` fail on a long attract table: tick 120, Beginner face
  9038, one polygon of a near-clipped wall: its vertex at x -2642, y 2795 lies 0.395 px off in y and 1.5e-4 in z
  (tolerances 1/16 px, 2^-14), a triangle wholly off the screen; the other 1,440 frames hold. The default model passes.
  `SIM_GENERAL_EMITTER=1` on Expert writes the same triangles as production but in 119 of 779 frames some in the opposite
  vertex order (it joins strips production leaves apart next to live faces); the PVR culls nothing, the picture is the
  same, the reference passes both. That emitter draws the inspection views only.
- Not covered: the frame-counter animations, knocked cones and damaged bodies are not in the tables (the damaged
  bodies have `SIM_DAMAGE_CHECK`); the fence sweep and mutants 29 and 30 need a pack with stipple groups; `SIM_SIDE`
  (the PVR work model's sky and HUD) has no generator now.

### 19 October: the placed actors' lighting (branch actor-cost)

- **Why**: the Expert finish run on hardware went from 1 to 7 late frames with Expert's actors, all cause=cpu-registration
  (`build/hw-tools/hw/console-xa-expf.log`: ticks 1720, 1751, 3531..3557, 10141, reg 12.2..14.2 ms, upd 3.4..6.1 ms against
  3.0 mean), registration 6.26 -> 6.58 ms mean; the PLACED groups (statue 840 specular faces, five horses 655, birds) were
  lit per face on every frame they were drawn.
- **What changed** (`include/render/models/clock_light.h`, model_renderer.c nm_render_world_clock_light, practice.c
  practice_clock_place): the lumas of a PLACED group depend on the frame's normals and a key alone, lm = M^T l plus, for a
  group with a specular material, az = (p2, p5, p8) of the placed matrix and the light's view-space z; a placement whose key
  equals the last one lit (the tick a frame repeats, a standing actor) keeps its baked lumas (a frame change invalidates).
  Before lighting, practice_clock_place classifies the instance's bounds exactly as the submission does (model_math.h
  instance_view of the world header, nm_bounds_quick_classify, the planes where it does not decide) and lights nothing of
  an instance rejected whole. The per-face arithmetic is lighting.h's in the old order, the sum ((shown diffuse) + ambient)
  + specular pinned with an empty asm: the render units compile with -ffast-math, under which GCC hoisted ambient +
  specular out of the sides in the first version (a last-bit change of the luma); one-sided faces evaluate the front alone.
  The planned "light once per distinct (normal, material)" was dropped: the statue's 840 faces have 840 distinct normals
  (828 within 1e-3), the horses 127 of 131, the birds 14 of 14 (a dedup table would save nothing).
- **Identity**: tests/test_clock_light.c (make check with the asset set; the recipe's world steps): every placed group,
  every frame, 64 placements with their own views against the per-face reference (the old loop), 2,763,264 faces equal,
  strict, -ffast-math and ASan builds; the SH4 disassembly of nm_render_world_clock_light before and after (no fmac, the
  fadd operand order as before); the submission's classification is the one practice_clock_place computes, so a skipped
  group has no drawn face; test_clock unchanged (the frame decoding is untouched).
- **Host estimate** (the recipe's clock trace and native trace of the Expert finish run, follow-camera view constants):
  lit faces 178.3 a tick mean, 1,508 worst, before; 115.2 / 1,197 after the frustum test (statue 1,181,880 -> 736,680 face
  evaluations: out of view 38% of its zone); the key saves 0.1% at tick level (birds and horses turn every tick; the 60 Hz
  frame that repeats a tick adds about 4%). At about 0.3..0.45 us a face on the SH4 (60..90 instructions) that is 0.02 ms
  mean and up to 0.45 ms on a statue frame. At the drop log's ticks the trims save little: 1720 and 1751 draw no placed
  group (group 42 alone), 3531..3557 and 10141 draw the horses and birds in view (949 faces, 0..98 of them rejected): the
  lighting is under 0.4 ms of those 12..14 ms frames; the rest is elsewhere (the queue path of about 950 unstripped actor
  faces with the reference side test, and the update's own 3.4..6.1 ms).
- **Hot layout** 161 of 161 (practice_world_instance_rejected is called from the cold practice_clock_place alone); heap
  budgets as before (1,328,552 never taken, 1,648,096 free while racing: the keys are 1.4 KB of statics). ELFs:
  `build/dc-native-actorcost.elf/{stats,stats-expert,stats-expert-finish}/bin` (build.sh there, against
  `build/dc-native-xactors.recipe`; make check: the 5 known FAIL lines).

### 19 October: Expert's world actors and the statue (branch expert-actors)

- **The thirteen handlers of 236940[1]** besides the props (prepare_world_objects.py HANDLERS, include/expert_actors.h):
  frame-counter animations with a fixed placement: 22108c (cell 145: near, nine figures T Ry 238d20.. [(5010b0>>3)&3];
  not near, thirteen T Rz Ry Rx while the player's profile is in 2f8..33e, no phase test; decoded from its TGP command
  words), 22214c (near, T S(0.69,1,0.69) Ry 239548/239748[5010b0&3f] by completed laps 0 / not 0), 222594, 222678,
  22275c x2, 222840 (draw bitmap, T S(s,1,s) Ry 239648[5010b0&3f], s 0.75/1.2/1/1.1; with 22214c colour-cycling signs,
  lumabases 2..65, as Advanced's); static by condition: 223ce0 (288ec28 while the profile is in 208..240, no cell); a
  model by angle: 223bc0 (238a04[(angle(d, 220) * sign >> 6) & 3f], d the distance of the camera entity 501240 from
  223cc8, or 223cd4 with sign -1 while the player faces the wrong way; the 64 lists differ only in u, v); rigid motion:
  22430c x4 (five birds circling, T(p) [T(5,2,0)] Ry((5010b0<<7)+h+k) T(15,0,0), two more in the podium), 224730 (a bird
  T Ry S(scale), the scale grows 0.5 on each START edge up to 100), 224cd0 (two horses in the podium only, z -= 0.2 a
  tick); state machine: 224854 x5 (horses circling an ellipse 25 x 35 until the player is within 8, then running along the
  player's heading at 1.5 times its speed on the ground, re-latched when the player comes near again, 2560 ticks out of
  view then a restart); 223d48 x6 (birds of the time attack, 501a80: never active in this port). The sound manager's
  routine 222264/222288 draws the Jeffry statue 288f614 (840 faces, specular metal) while the podium timer 5010a8 is in
  1016..1211 or the profile in 242..320, counting entries in 5036fc/5036fe (from power-on) and START presses on a standing
  car in its +34: racing only, it turns Ry(+34<<12) (and Rx(8000) for 16..31), upside down on every 64th entry and with a
  reverse finish (501a8c). It sends no sound command, nor does any other actor (their only calls: 1786c, 16e10, 11620).
- **Sizes**: 354 lists, 20,374 distinct faces (the export); every frame of every group as a mesh 63,277 faces (6.45 MB
  at 102 B a face; the birds and horses in model coordinates, their world motion is unbounded); compact:
  course1-actors.dna 247,548 B (48 groups, 13 actors; 841 KB of tables before groups of one list table share them) and
  2,629 base faces in course1-world.dnm (+266,336 B). Per handler (frames x faces a frame): statue 1 x 840 (11 KB of
  tables), horses 32 x 131 (7 instances share 64 KB), birds 32 x 14 (29 instances share 6.9 KB), bearing 64 x 168
  (88 KB, u, v sets), the six signs 64 x 20 (13.8 KB each, 22214c every 2nd frame interpolated, 0.0595 units), figures
  4 x 33/44 (3 KB), 223ce0 1 x 81.
- **Form**: DNA1 gains conditions FAR_PROFILE, LAPS_NONE/SOME, WINDOW (one-frame groups) and ACTOR, PLACED groups (model
  coordinates, every normal stored, the instance PLACED) and an actor table (header flags bit 0); Advanced's file is
  byte-identical. The fixed placements are baked in world coordinates (float32, normals M n unnormalised as lighting.h).
  code/expert_actors.c ticks the actors with state (bearing, birds, big bird, horses, pair, statue: every float
  operation a TGP command's, 43 = nm_force_length(a-c, d-b), 0a = nm_force_angle, 47 = nm_force_cartesian + add, 1d/1e
  sine table, ground nm_contact_query) and gives each draw a frame and a placement program (T, Rx, Ry, Rz, S);
  code/props_scene.c steps them with the props: the race after the pit crew (START edges counted by practice.c), the
  results (practice.c nm_practice_results_tick, the pool runs on), the podium (the tasks are made on its entry tick:
  first draws on the spawn tick). The renderer (practice.c practice_clocks, practice_clock_place) composes view x
  placement (SH4ZAM sine) and model_renderer.c nm_render_world_clock_light gives each face of a PLACED group the binding
  of its luma (diffuse and specular of the race light, a row of 256 bindings per material, every luma from the ambient up
  exported with --extra-keys); clock meshes leave the live specular classes. Two corrections the traces showed: the
  camera entity 501240 is 0, 0 while a keyed camera runs (camera mode 1: the crash camera; the old note said it keeps the
  last follow position; course 0's 223b08 bearing is computed the same way at render time and was not changed), and a
  cinematic scene's profile is its car 0's (clock_scene.h; Advanced's 221f10 reads it only while racing).
- **Checks**: tests/test_clock.c on every frame of the 48 groups (PASS). m2segment --trace-clock-expert logs every draw of
  the actors' code with the TGP's matrix (command 11 read between the game's commands: race trace and screen hash
  identical with and without it) and the view at each actor tick; trace_race_native --clock steps props and actors as
  practice.c and writes the placements; tests/test_expert_lockstep.py compares lists and placements (view inverse times
  the TGP's matrix) every tick: time-out run 6,674 of 6,674 rows, finish run 15,596 of 15,598 rows plus 2 podium stage
  cut rows (the original's player entity is the next stage's car a tick early: 223ce0 and the statue's zone decide one
  row earlier), largest placement difference 0.0002 units. Drawn there: 22108c both branches, 22214c both tables, the
  four sign handlers, 223bc0, 223ce0, the statue (1,407 rows, 5036fc 0 to 4), 22430c with the podium's extra birds, 224730,
  224854 (7,105 rows, never fleeing), 224cd0 (1,212 podium rows). m2segment --check-native-expert-actors runs the original
  ticks (222288, 223be0, 224cec, 22432c, 22475c, every state of 224854) on the reference i960 with the live TGP from random
  task and player words after an Expert scenario: 44,000 cases, 36,533 draws, every task word and list equal, placements
  within 0.00012 units (horses running, re-latching, waiting and restarting; the bird growing; the statue's every turn).
  Sound: the props' hits now go through trace_race_native's sink (9e1d, 9e27 exact on the finish run); Expert's stream
  still differs in 9e29 (20c608, not an actor), the announcer ae20/ae21 and be34/be35: not investigated here.
- **Memory** (heap_budget.py --session 0,2,1,0, the recipe's ELFs): Expert racing 1,680,864 B free (stats-expert;
  2,689,872 before), least over every ELF 1,648,096 (stats-expert-finish), never taken over the session at least
  1,328,552 (budgets 262,144 / 524,288; the Expert load is now the low point): the world pack +266 KB, the DNA +248 KB, the point cache +211 KB (the
  frame bound counts every object's points), emission faces +84 KB, DNT metadata +56 KB. VRAM payload of
  course1-world.dnt 1,462,720 B (849,856 before; the chase copies of the four sign textures 425,984 B); limit 3,571,712.
  Palette banks 58 of 58 (the world's 3 added palettes remapped as on Advanced, weighted error 7.63 against 7.60).
- **Cost** (estimate, not measured): faces of drawn actor groups before frustum culling 203 a tick mean, 1,528 worst
  (finish run), 178 / 1,508 of them lit per placement (the statue's 840 with specular every frame it is in its zone);
  at Expert's queue path of about 100 instructions a face plus about 35 a lit face: about 0.15 ms mean, 1.1 ms worst.
- **Hot layout**: 161 of 161 at their slots (practice_update and the scenery submission untouched; new code out of line).
- **Run from the ROM**: `build/dc-native-xactors.recipe` (--music --elf --check): 2,694 rows identical, 21 changed (every
  set's course1-world.dnm/.dnt, and course2-actors.dna: groups of one list table share it now, 246,288 B against
  253,376, the same frames: test_clock and the clock lockstep pass), 7 added (course1-actors.dna); course 0's packs
  byte-identical; every lockstep step PASS (course1_lockstep and course1_finish with test_expert_lockstep.py, the
  Advanced steps as before), the new step course1_actors (the i960/TGP oracle) was added after the run and PASSes by hand (same command), not yet
  in a recipe run; every ELF's check 76 PASS and the
  5 known FAIL lines; hot layout 161 of 161. Expected checkpoint rows unchanged (no gameplay change).
- **Hardware** (not run here): elf/stats-expert (time-out run: the signs, figures, bearing sign, 223ce0 near the start of
  the lap), elf/stats-expert-finish (statue, birds over cell 199, the five horses circling, the podium's pair and birds),
  elf/stats (course 0, nothing new). Watch: the statue's specular, the birds' shading as they turn, registration
  against 6.26 ms and late frames.
- **Not done**: the attract's Expert stretch (the actors would run through nm_props_intro_step: the statue's 5036fc
  counts the attract car's passes in the original; the native boots with 0); START presses in the podium (the big bird
  and the statue's count ignore them there); the frame bound (point cache) could count objects by cell.
### 18 October (evening): the V.R. demo, the attract's 2D layer, the loader's stretches (branch attract-courses)

- **The V.R. demo is native** (`include/vr_demo.h`, practice.c `practice_demo_start`/`practice_demo_update`: its own
  enDjinn mode, so practice_update and the race's code stay as they are). Phase 6: the cinematic's last stage's cars
  (21/10/10) gridded by entity on the race grid (e20c), the camera reset with the view preset 501710 (637c), the pool
  of the cinematic carried in (the collision words, the rank key +60, 1224c's rank and track lists, the leading lap
  5014ba, the cells), the phase-6 field pass (flags 1<<6) and the camera pass. Phase 7, 834 ticks: 224ef4's counter and
  view ((counter >> 7) & 3), the player c9c4 (the opponents' motion chain with the player's damage prefix, boundary,
  correction, inhibit, road frame, rank key), the opponents cde0 (the race's), the field pass, the race's camera row
  with the view presets; the race's presentation draws it (attract_active 0; no HUD: phases 20..24 only; no sound).
  On its last tick 2058 has already stepped 501460: the opponents' height filter (cd30) and the drawn-car count
  (77d0) read the next course's (`nm_demo_step`'s `course`). Then the title; START/A in it: the select with a cold
  pool (the game start's phases 12/17 on the demo's cars are not native); both triggers + START: the settings.
  The console prints `Native V.R. demo: course C, 834 ticks, F frames, L late (longest us); PVR registration avg/max,
  render avg/max` for each demo.
- **What the scenes carry** (`nm_attract_carry_t`, measured on the attract trace): the cinematic updaters of Beginner
  and Advanced do not write +60, so their cars keep the last demo's rank keys; each cinematic's opponents start with
  the lap byte the course's grid reset gives their entity (`road->grid[].lap`: 255 behind the half of the course);
  5014ba and 501710 go from the demo into the next cinematic and demo. The cinematic's 503690/503692 (variant and
  collision phase) are 5010b0 - ticks - 3 (`phase_clock = object_clock - 3`; 0 in the first loop, so the fourth
  loop's collisions needed it). The "V.R. demo carry" exceptions of test_attract_lockstep.py are gone (no --strict).
- **Lockstep** (`tests/trace_attract_native.c` now runs the demo; `test_attract_lockstep.py` compares its rows in
  the race trace's fields: camera, preset, 5014f7, cells, view matrix, and every car's flags, prof, prev, far, lap,
  done, cnt, rank, key, keyhi, si, spd, x, y, z, yaw, head, path, lat, gear, rpm, acc, brk, want, road, gs, surf,
  mask, the collision words, crel, dmg, crashes, tilt, pitch): four loops (13,000 reference frames: courses 0, 2,
  1, 0 with their demos), 11,032 of 11,032 rows equal; accepted: one demo row sampled mid-pass (Advanced's first demo
  tick: the frame ended after the cars, before the field pass and camera), and the cinematics' accepted rows as
  before. Not compared: body= (the race's body word, alike for every native car) and str= (the demo's player keeps
  the cinematic's steering word; the native cinematic does not keep it).
- **The 2D layer** (`include/attract_2d.h`, `code/render/ui/attract_2d_render.c`, `tools/prepare_attract_2d.py`): tile
  layer A and its line table as the original writes them over the cinematic, the ranking and the demo: ports of the
  writers (18d74, 1a240, 18e2c, 18dec, 18394, 18414, 18478, 18ad0, 18c4c) and of the objects (the footer 5674 with
  the factory credit display "CREDIT 0/3", 5588's blinking INSERT COIN(S), phase 2's AM2 block, the ranking 2266f8
  with the course's first five records from the game save and its line-by-line reveal, the demo's 224e64/224ef4:
  the Japanese text of 53e59a & 1 = 1 and the view boxes). `tests/test_attract_2d.c` against `m2segment --dump-2d`
  (the tile RAM of every 4th frame): 2,718 dumped frames of four loops, every layer A word and every line of the line
  table equal. Drawn as 8x8 quads of one ARGB1555 atlas (1,523 tiles with ink, 256x512, 262 KB of VRAM, resident from
  power-on; direct colour since the 3D scenes use every palette bank; largest channel error 6, checked against the
  original's screens: every ink pixel within 6) on the punch-through list in front of the scene; hidden scanlines
  cut out. Asset `assets-*/attract-2d/attract-2d.dt`, header `headers/attract-2d` (make variable
  NATIVE_ATTRACT_2D_DIR); recipe steps trace_attract_2d (background) and attract_2d.
- **The loader's 2.9 s stretch** (hardware, the first stats-attract run): yield points through the whole render load
  (each step of nm_render_init, the resident header checks, the material UV ranges, the dense luma loop, the world
  attach loops, every per-mesh loop of the stream builds, the material lists, the pack fix-ups of meshpack.c and
  material_pack.c, the track surface, sky, finish, result and label loads); the read cost starts from the hardware's
  fit (735 us a call + 0.101 us a byte); a slice yields once when the next step's estimate would pass its deadline
  (unknown steps: twice the mean stretch, 0.5..4 ms). The report names the longest stretches: `Native scene load
  attract: N us from SITE to SITE without a yield point` (file:line). Not measured on hardware since.
- **SH4ZAM**: KOS's addons got SH4ZAM 2d550a1 at 14:42 today (shz_vec4_t's e[4] became a vector type: geometry.c's
  pointers into it do not compile). These ELFs are built against the release the code was measured with (v0.9.0,
  0fd3a1e: `extern/sh4zam`, its lib/libsh4zam.a) through the new make variable NATIVE_SH4ZAM_ROOT (branch
  gear-gate-default e1f97c2 makes that the default).

### 18 October: the attract over the three courses (branch attract-courses)

- **The original's loop** (`include/attract.h`; `m2segment --trace-frontend` without input, phase handlers 018cc):
  phase 2 (197c: setup, the course's data 39c0, ae1008 and be1700/be1900) -> 3 (1d24: the cinematic of 226360's graph
  2865a60[501460], 1602 iterations) -> 4/5 (the ranking overlay over it, 1 + 320) -> 6/7 (1e5c/2028: the V.R. demo
  race, 1 + 834; at its end 2058 steps the course: 501460 = 20b8[501460] % 4, 0 -> 2 -> 1 -> 0) -> 8 (20d4: be1700,
  be1900) / 9 (2338: the title, 386) / 10 (country JPN: one iteration) -> 2. 3147 iterations a loop on every course:
  course 0 from frame 179, 2 from 3361, 1 from 6541, 0 again from 9723; about 33 lag frames a loop, 3-4 of them at the
  title -> cinematic cut (phase 2's load). A game keeps 501460: the attract after it shows the raced course.
- **The other courses' cinematics**: `prepare_intro.py --podium --courses 0,1,2` now also writes the attract graphs of
  slots 1 and 2 into podium_data.h (`nm_attract_courses[3]`, course 0's is the intro header's `nm_intro_stages`): one
  stage of 10 cars each that re-enters itself at tick 1600 (course 0: 512 + 1088), cameras of 18 and 19 keys, event
  callbacks 2265c4/2265c8 checked to be a bare `ret` in the program ROM. `intro_scene.h` runs the attract on any course
  (`nm_intro_scene_start_attract`); practice.c keeps 501460 of the loop (`practice_attract_course`: 0 at power-on, the
  next one at the cinematic's end, the raced one after a game).
- **Found on the way** (all cinematic-only or presentation): the attract's camera object is the race's (6484 -> 6510),
  so its scene light (7024) follows the player into Advanced's tunnel ranges: `intro_scene.h` steps it (the podium keeps
  none), the range index starts at 0 again at each stage entry (226378 re-arms the camera object), and
  `tunnel_light.h` set the light to the normal one at once without path flag 256 (70a0 -> 72c4; it glided, also in the
  race: presentation only, checkpoints unchanged); a cinematic car's +6d after the line is its lap less one (the
  original's spawns write 255: cars 17/19 of course 0 from 5010b0 509, `intro_math.h`); the windows' tunnel texture
  (6fe0/7008 from the view pass's 6f24) in Advanced's attract as in its race; title start sends be1700/be1900 (phase 8).
- **Verification** (`m2segment --trace-race FILE --trace-attract`: the race trace's rows through phases 2..11, cars in
  3..7; `tests/trace_attract_native.c`; `tests/test_attract_lockstep.py`; recipe step `attract_lockstep`): 3 loops,
  5,769 of 5,769 rows (78,267 car rows) equal in every car field (flags, prof, prev, lap, done, si, spd, x, y, z, yaw,
  head, path, lat, want, road), the camera key tick, the cell bitmaps, the scene light (index exact, components within
  1.2e-7: the TGP's normalise) and the phase and course of each row; view matrix within 0.075 units. Accepted on
  evidence and counted: 4 stage setup rows (the original's script object spawns at the end of its pass, as on the
  podium), 3 script-ahead rows (its speed event's +44 one row early), 40 spawn-row road words, 1 start row's cells (the
  original's bitmaps are the scene before's), 2 mid-pass rows (the frame ended inside 5890). The stretches' timing:
  5010b0 3..1925 (course 0), 3150..5072 (course 2), 6297..8219 (course 1), as the original. Advanced's clock
  animations in its attract: `test_clock_lockstep.py` 1,922 of 1,922 rows.
- **(Superseded the same evening: the demo is native, see above.) Not native: the V.R. demo** (phases 6/7, 834 iterations: counted, the title follows the cinematic at once). It is a
  race of the loop's course (21 cars on Beginner, 10 on the others; e20c's grid from the catalogue 286fac0, the player
  on updater c9c4: driven, not by the pad; opponents cde0; the camera 6510 with the view preset 501710 = (timer >> 7) &
  3: VR1..VR4 every 128 iterations; the 2D object 224e64: the view's name, blinking) and silent (no sound command).
  Its pool carries into the next cinematic: the opponents' spawn cfc0 keeps each entity's lap byte +6e (after course 0's
  demo all 0, after Advanced's entities 1, 2 255 and 3..9 0) and the sort pass its leading lap 5014ba. So on Advanced and
  Expert the native's opponents count their laps from 0 where the original's from the demo's value (positions,
  speeds, camera and cells are equal; Expert's near-neighbour flags 0x2000 differ on its first 7 ticks):
  test_attract_lockstep.py lists these 3,844 rows apart and fails on them with `--strict`. A native demo would make the
  loop 3147 iterations long as the original's (now 2309 + the loads) and give the carry.
- **The load in the title's frames** (`include/scene_loader.h`, `code/scene_loader.c`, outside the LTO unit): the
  attract scene of the next course (its course packs, the cinematic's start, the whole render load: about 6.1 MB of
  files for Beginner, 8.3 MB for Advanced, 8.0 MB for Expert) runs on a thread of its own that only runs inside the
  updater's idle wait for the PVR (`idle_wait.c`, after the sound's reads), in slices: KOS's `fs_read`/`fs_open` are
  wrapped (local.cfg.mk) and cut into pieces sized by a fitted cost (start values: the 18 October hardware log, 1.17 ms
  a call + 7.5 MB/s), and `nm_loader_yield` marks the long computations (each stage of the render load, each texture
  upload, each mesh of the stream build, the steps of the world attach). Palette RAM writes of the job wait for the
  title's release (`nm_palette_ram_defer`/`flush`: the title shows its own banks). Should the load not be done at
  the title's 386th tick, the title goes on (its prompt blinks), counting no iteration, until it is (the original holds
  3-4 frames at its phase 2 load); a credit or the
  settings in the title finish the load first and give it up (its renderer released: the select and the race load
  their own; after the settings the next title loads it again). Estimated reads over /pc: 1.2 s (Beginner), 1.5 s (Advanced and
  Expert) of the title's 6.7 s; the CPU part is not measured. The console prints `Native scene load attract: done in N
  ms; S slices (T ms in them, longest L us, K past their end), R reads of B bytes, O opens; read cost ...` and
  `Native title: F frames, K late (longest L us), H frames past its last tick` for every title: the measurement.
  The boot's first attract and the attract after a game still load at once.
- **Heap**: `heap_budget.py --session` now runs the unattended loop (2, 1, 0) after the boot and the attract on the
  raced course and the next one after each game: on every recipe ELF at least 1,955,240 never taken and 2,292,064 free
  while racing (budgets 262,144 / 524,288); the attract on Advanced has 2,373,984 free (play ELF), the same packs as
  its race. The loader's stack is static (64 KiB of .bss).
- **ELFs** (recipe `build/dc-native-attract.final`, see below): `elf/play` (from the attract), `elf/stats-attract`
  (new: `AUTO_START=attract NATIVE_ATTRACT_LOOPS=3` with render statistics, the drop log and an `attract` segment:
  three cinematics with their titles, then the run ends), `elf/stats` (the fast race) and the others. Run from the ROM
  (`build/dc-native-attract.final`, --music --elf --check): 2,715 rows identical, 1 changed (podium_data.h: the attract
  graphs), every lockstep step PASS (the races, both podiums, attract_lockstep); hot layout 161 of 161 at their slots in
  every ELF (no hot function changed); every ELF's check 77 PASS (with `tests/test_scene_loader.c`: the loader on a
  KOS shim) and the 5 known FAIL lines; every expected checkpoint row unchanged.
- **To watch on hardware**: the cut from each title to Advanced's and Expert's cinematic (no frozen frame: the
  `Native title` line's late and held counts), the `Native scene load attract` lines (time, slices past their end),
  Advanced's tunnel (the cars' light, the windows' texture), and after the loop course 0 again.

### 18 October: Advanced's animated world actors, the course-0 podium compare (branch course2-actors)

- **What the eight handlers do** (`tools/prepare_world_objects.py`, "frame-counter animations", from the code after each
  init entry): each draws list[(5010b0 >> shift) & (frames-1)] of its table in world coordinates (the view matrix
  itself; the 505/606 around the draws are the TGP's matrix push/pop) when its condition holds, read in its tick with
  the pass before's bitmaps: 220304 cell 238b14 or 238b16 in the draw bitmap ((5010b0 & 7f) >> 1); 2209e4 cells 7a/6a
  (draw); 220a7c cells 99/89 (near); 221f10 while 501090 selects phase 22 the player's profile +128 in fa..168
  (unsigned), else its own cell (draw), lists 238eb8[k] + 288bbe0 + 288bbf4; 222030 own cell (near); 222090 own cell
  (near) and the player's completed laps +6d 0..2 (2391b8) or more (2392b8); 222c48 cells 2393c0/c2/c4 (near), 32
  lists; 222cf0 always. They keep no state: `include/clock_actors.h` nm_clock_frame is their whole behaviour, the
  inputs come from the scene (`include/clock_scene.h`, shared by practice.c and the harness): counter_base + ticks - 1
  in the race (+ the results ticks of a race that ends there), counter_base + object_clock + ticks in the podium,
  nothing drawn on the podium's entry tick (the original draws none), the podium's laps as practice_board's.
- **Form** (DNA1, `course2-actors.dna`, 253,376 B; `prepare_world_objects.py clock`): ten groups (221f10's three lists
  are one group, 222090's two tables two alternatives of one selection run), one base mesh each in the world pack
  (instance kind NM_OBJECT_CLOCK = 15, meshpack.h; positions a later frame tells apart are kept apart in the base:
  moved by float32 steps where they coincide); per frame: int16 positions of the points that move (a step is about
  1/32000 of the group's extent: errors 0.00005..0.0027 units), the normals of faces whose side a frame may test, each
  changing face's
  state (texture header and lumas per side, the binding of the world DNT) and the u, v sets. Every frame at full rate
  but 220304, stored every 8th frame and interpolated (largest error 0.067 units; its lumas stay per frame). The
  renderer (`model_renderer.c` nm_render_world_clock_frame) writes the drawn frame into the pack's vertex and face
  tables, the baked bindings and the emission u, v (only when the frame changes); clock meshes stay off the prebuilt
  streams and offline strip runs, take the reference side test, and their mesh and face-group bounds hold every frame.
- **Colour cycling** (222cf0's water, 222030/222090/222c48's lights: lumabases 2..65 are 64 phases of a brightness wave
  that travels along the texture's indices, non-monotonic ramps): no palette bank per phase exists (58 of 58 in use),
  so `prepare_palette_materials.py --chase-variants 16` bakes the wave into 16 copies of each such texture (the 64
  lumabases in runs of 4, a copy per run's mean ramp) that index one palette of 15 brightness levels (levels 42..62),
  a binding per lumabase with its own fitted vertex colour, no mip chain. Error against the arcade's colours: up to
  11/255 in a channel (the DNT6 model error elsewhere is up to 23, mean 10); the wave moves in 16 steps a cycle instead
  of 64 (the 64-frame cycles step every 4 frames, 222c48's every 2). The chase palette took the bank of world palette
  (3,3,1,0), whose 70 faces now use (3,2,1,0) (weighted mean palette error 10.016 against 10.215 before, the new faces
  included). `--extra-keys` makes the bindings of every header and luma a frame shows (1,173). VRAM payload of
  course2-world.dnt 2,349,056 B (983,616 before; 256 texture copies, 1.31 MB); limit 3,571,712.
- **Checks**: `tests/test_clock.c` (recipe step course2_world): every frame of every group as the renderer decodes it
  against the export of the original's lists, in the pack's order: positions within the step (220304 within 0.072),
  u, v, texture headers and lumas exact, normals within 2e-4 (220304 0.0084 interpolated): PASS. The draws:
  `m2segment --trace-clock` logs the handlers' calls of 1786c, `trace_race_native --clock` what practice.c picks, and
  `tests/test_clock_lockstep.py` compares them tick by tick (steps course2_lockstep and course2_finish): time-out run
  5669 of 5669 rows (race and results), finish run 14343 of 14344 rows plus 1 podium stage setup row (the original's
  actors see the new shot's cells a tick earlier, as its pool holds the next stage a tick earlier; the native draws
  there what it draws on its next row). All ten groups drawn (finish run: 221f10 2,701 rows, 222090's high-lap table
  644, 222030's second 1,012).
- **Memory** (heap_budget.py --session 0,2,1,0, the recipe's stats-advanced ELF): Advanced racing 2,406,752 B free
  (3,003,256 before: the DNA1 file 253,376, the world pack +69 KB, its DNT metadata +29 KB, emission faces, point cache,
  stream records of the clock faces (unused) +71 KB, classes and header pool); never taken over the session 2,069,928
  (the Advanced load is now the low point). Over every ELF of the run: at least 2,020,776 never taken and 2,357,600 free
  while racing (stats-expert-finish; budgets 262,144 / 524,288).
- **Cost** (not the stream simulator: its frame generator has no clock instances yet): the drawn groups have 195 faces
  a tick on average and 507 at most before frustum culling (finish run), on the queue path; at Expert's 100
  instructions a face plus the frame writes (8.8k instructions mean, 25k worst) about 0.18 ms mean and 0.48 ms worst.
- **Course-0 podium compare** (task 2): the rows that failed `compare_race_trace --podium` (native 10205, 10206, 10529,
  and the start's 2-4) are frames the board ended inside a logic pass: the timer and cars 0..k had run, the later cars
  and the camera (ctick one behind) not; the native equals the reference for cars up to k and its previous row for the
  rest. Native 10526 is a frame without a pass caught up by a double pass (5010b0 12892, 12892, 12894). The native is
  exact; the tool now accepts such rows only on that evidence and lists them. Course 0 with --podium --skip-rows 0:
  PASS (6 mid-pass rows, 1 caught-up stall; podium camera largest view difference 0.028). The stage setup rows (10849,
  ctick 0 against 323) stay timeline-only as before.
- **Run from the ROM**: `build/dc-native-actors.recipe` (--music --elf --check): 2,694 rows identical, 14 changed
  (every set's course2-world.dnm/.dnt), 7 added (course2-actors.dna); course 0's and 1's packs byte-identical; every
  lockstep step PASS (course2_lockstep and course2_finish with the clock comparison, course 0's race_script now with
  --require-equal --podium); every ELF's check 78 PASS and the 5 known FAIL lines; hot layout 161 of 161 at their slots
  (no hot function moved: practice_clocks is out of line, the renderer's additions are load-time or out of line). The
  expected checkpoint rows of every scripted run are unchanged (no gameplay change).
- **Hardware** (not run here; asset set `assets-ssfar4f` of that run): `elf/stats-advanced` (120 rows of
  `work/script-advanced/expected-checkpoints-advanced.txt`), `elf/stats-advanced-finish` (320 rows, through the podium),
  `elf/stats` (course 0, 259 rows; its frame times as before: course 0 has no animation). On Advanced watch: the sea's
  shimmer (222cf0, always drawn), the chasing lights of 222c48 and the 222030/222090 signs, 220304's slow sway, the
  texture swaps of 2209e4/220a7c, 221f10 while the player is on profiles fa..168; the console lines `Native world clock
  animations: 10 groups, 253376 bytes` and `... 10 of 10 groups, 1373 states, ...`; registration and late frames
  against the run before.
- **Not done**: the stream simulator and its frame generator know no clock instance (the cost above is an estimate);
  the stream records of the clock faces are allocated though unused (71 KB); Expert's actors (222264/222288 and the
  others) and the attract's Advanced stretch.

### 18 October: the podium of Advanced and Expert, every course's props (branch course-extras)

- **Races to the podium** (`recipes/inputs/inputs-advanced-finish.txt`, `inputs-expert-finish.txt`): made by
  `tests/drive_race_native.c`, which now runs any course (`--course N --counter-base B`, the production tick of
  trace_race_native: camera pass, variant bit, collision phase from the counter, wall-lane signal) and has a
  `--pursuit` search (the parameters of a pure-pursuit policy, lookahead and lane, lift and brake thresholds, each
  rollout over 150 ticks; the old fixed-input search could not brake for a corner) and `--launch 0xa8` (a part
  accelerator through the countdown: full accelerator over-revs in phase 20, 0.11 against 0.18 speed 80 ticks after
  GO). Progress counts across the line (off course 0 the grid stands behind it at profile ~987). Advanced: 4 laps,
  3rd at tick 12742; Expert: 2 laps from 30th, 3rd at tick 13996. Commands in the files' first line.
- **Lockstep** (recipe steps `course2_finish`, `course1_finish`; `compare_race_trace.py --require-equal --podium`):
  every car bit-identical on every race row (Advanced 13,128 rows, 20 cars; Expert 14,382 rows, 30 cars), the podium
  cars (prof/x/z/spd/yaw) on 1,597 / 1,596 podium rows, the timers (5010a8, the record notification, clock,
  remaining) through the results to phase 29. Timeline-only rows: the podium entry row and each stage setup row (the
  original's pool already holds the next stage on that tick, the native enters it on the next, the same state from
  then on: also so on course 0) and the results rows (the original's pool keeps the podium cars). The reference's
  frames without a logic pass after the race (the podium and results loads) are dropped before pairing. The podium
  camera: largest view-matrix difference 0.06 (Advanced) and 0.10 (Expert) units, median 0, as on course 0: Expert's
  unrolled camera chain (2865898/930/9c8 into 2865708) is right.
- **Two podium fixes** (both in the original's behaviour, not tuning): the cinematic scene refused every road but
  course 0's, so on hardware the podium of Advanced was rejected at its spawn (`Native podium rejected`); it now runs
  on the race's course (intro_scene.h: a tunnel edge of car 0 leaves the reflection UVs, the slot machine stays course
  0's; the harness takes 2865a70[course] too). And the podium's variant bit and collision phase (5818: 5010b0 & 1, & 3)
  come from the race's frame counter (`phase_clock` = counter base + race ticks + 2 on the first step), not from the
  scene's own ticks: that agreed only when the race's tick count was 2 mod 4, as in the course-0 runs; on Advanced
  cars 16/17 of stage 2 then collided one tick apart (244 podium rows off).
- **Checkpoints**: the rows of `work/script-*-finish/expected-checkpoints-*.txt` include the podium (33 rows a run in
  phases 27/28, the podium cars' hash) and the results. ELFs `elf/stats-advanced-finish`, `elf/stats-expert-finish`.
- **Props of every course** (props.h, props.c, props_scene.c, prepare_intro.py, prepare_world_objects.py): the handlers
  223078, 2230ac, 2230e0 are 223114's cone with other init constants (their lda immediates: rest record 2896d94/da4/db4,
  knock sets 2896de8/e10/e30, hit limit 7.0/2.5/2.5, sound 9e277f/9e007f/9e277f); the tick 2231c0, the manager and the
  sound manager are shared; the manager's argument is the footprint (22382c on course 0, 22385c, 0.9 wider and 0.2
  longer, on the others). Advanced 15 props (3 kinds), Expert 35 (four kinds; its nine 223114 cones too). The
  race-start header holds every course's props over one record pool (4,849 records, 17 tables, 77.6 KB of .rodata;
  course 0's indices unchanged); the props step by the scene's course (race and podium); the world packs get each prop
  as a cone instance (Advanced +3 meshes, 15 instances, 88 faces, +11,456 B; Expert 6 meshes, 35 instances, 136 faces,
  +19,168 B), placed at the rest record's height without prop state (the props' own pose, with the ground query, is
  what is drawn). Expert's sound manager also runs 222264/222288 (an object 288f614 at (-1028, 86.7, 868) under
  conditions, counters 5036fc/fe): not ported, as the other actors.
- **Props verified** against the original game: `props_run.cpp` (cone_run for any course: E holds the tasks up to the
  sound manager, a C record names each scene's course; a waypoint pursuit controller along a driven race's path into
  the props; `build/dc-native-extras.work/props/`) and `tests/test_props.c` for any course (make check with
  `NATIVE_PROPS_COURSE_RUNS=build/dc-native-extras.work/props-runs`): 7 runs (both finishing races with their podiums,
  five steered), 74,740 / 74,740 ticks equal (3,349 with a moving prop), 198,068 / 198,068 draw matrices, 27 / 27 hit
  sounds; knock starts by kind (slow set / fast set): 223114 0/4 (course 0's runs cover its slow set), 223078 3/3,
  2230ac 0/1, 2230e0 6/3. Not reached: 2230ac's slow set (props 4-7 of Advanced, 13-20 of Expert lie off the line). The
  Expert finishing race itself knocks five props (cones 6-9, prop 22): watch them on the console.
- **Course 2's world actors** (task 3, measured, not implemented; `build/dc-native-extras.work/clock/measure*.py`
  on `--clock-actors` exports): the eight handlers' 12 groups, 33,314 faces in 610 frame meshes, 3.40 MB as full meshes
  (102 B a face). What changes per frame: 222cf0 (55 faces, 64 frames) only a face header word (the luma); 2209e4 and
  220a7c (15 faces) a texture word and v by 512 (3 distinct frames of 64); 220304 (192 faces) positions only (264
  unique points; linear interpolation of every 8th frame errs 0.067 units); 221f10 group 0 (42 faces) moves 21 units a
  frame and deforms (no rigid fit, residual 25): every frame; 222c48 (240 faces, 32 frames) positions and luma
  (interpolation errs 0.31 at half rate); 222030/222090 (20 faces, 4 groups) positions, luma, and in one 12 UVs (half
  rate 0.09-0.10). Compact form: one base mesh a group, per frame int16 positions of the unique points (extent 648:
  0.01-unit steps), a byte or two of luma a varying face, UVs where they vary, and a state table for the texture swaps:
  360 KB at every frame, 259 KB with the reduced rates (error <= 0.1 unit). Per frame at most 737 faces and about 700
  interpolated points if every actor were in view (queue path, about 0.4 ms by Expert's 100 instructions a face; one
  or two actors are in view at a time). Advanced races with 3.16 MB free (512 KiB budget): it fits. Done since: "18
  October: Advanced's animated world actors" above.
- **Heap** (check's session 0,2,1,0 on the recipe's ELFs): free while racing course 0 3,069,136 B, Advanced 3,003,256,
  Expert 2,689,872 (before 3,151,056 / 3,164,976 / 2,901,656: the props' 77.6 KB of records, Expert's world objects);
  never taken over the session 2,364,840 (budgets 262,144 / 524,288). Hot layout: 161 of 161 entries at their slots,
  unchanged.
- **Run from the ROM**: `build/dc-native-extras.recipe2` (--music --elf --check): 2,677 rows identical, 29 changed
  (race_start_data.h and every set's course1-/course2-world.dnm/.dnt), 2 added (the two race scripts); course 0's
  packs byte-identical; both finish steps PASS; every ELF's check 78 PASS and the 5 known FAIL lines.
- **Hardware** (not run here): `elf/stats-advanced-finish` (expected rows
  `work/script-advanced-finish/expected-checkpoints-advanced-finish.txt`, 320 rows) and `elf/stats-expert-finish`
  (`work/script-expert-finish/expected-checkpoints-expert-finish.txt`, 345 rows), asset set assets-ssfar4f: every
  CHECKPOINT row equal, through the podium (phases 27/28) and the results; the podium cinematic on both courses with its
  cars and the props (Expert: cones 6-9 and prop 22 knocked over in the race); the props of Advanced and Expert at rest
  on the ground. Also `elf/play` on Advanced/Expert to a top-3 finish if a driver can.
- **Not done**: Expert's sound-manager object 222288 and its other actors; 2230ac's slow knock set against the original (done 30 October);
  the attract's Advanced/Expert stretches (the course-2 world actors: done, see above).

### 18 October: Expert raced (branch expert)

Expert (Seaside Street Galaxy, slot 1) is raced from the Circuit Select (NATIVE_COURSES 7u) and by `NATIVE_COURSE=1`.
- **Gameplay**: no core change was needed. `trace_race_native --course 1` on the 9100-frame time-out run
  (`recipes/inputs/inputs-expert-timeout.txt`: the menu of `course_expert.txt`, steering 0x98; time out at frame 8371,
  results, phase 29 at 9046): all 30 cars equal on 6673 rows (rows 0, 1 skipped as on Advanced). Recipe step
  `course1_lockstep` (fails on any difference), script header `headers/script-expert`, ELF `elf/stats-expert`.
- **Headers** (`prepare_intro.py`): Expert's pit table 234ce8 is the last; its row after the seven waypoints is list
  pointers (only x, z, km/h are read: exported as bits). Its podium graph 2865a70[1] has five stages of 10 cars whose
  cameras 2865898, 2865930, 28659c8 link on into 2865708, which loops on itself: exported without links, the loop
  unrolled past each stage's last event (internal zero-time keys start a shot, as 5cac does after a link). Courses 0, 2
  byte-identical; the shared headers' rows change (they now hold three courses).
- **World** (recipe step `course1_world`): cells of attract frame 6602 under the race light, textures of frame 4000
  of the time-out run; no world object (its 61 actors are cones, props and handlers prepare_world_objects.py does not
  know: listed in `reports/objects.json`). 95 meshes, 23,722 faces, 76,201 vertices; `course1-world.dnm` 2,458,048 B
  (the mesh pack guard is 3 MiB now: every offset and count of the format is 32-bit), `course1-world.dnt` 1,030,944 B,
  144 textures, VRAM 849,856 B, 18 palettes (58 of 58 banks with the cars'). Expert's screen-checker panels shared
  vertex records with other faces: with `--world-cells` they no longer do (`prepare_models.py`).
- **Live specular** (why the race-light export refused): 1,181 faces of 29 materials (glass and metal: mode 1, control
  7, scale 239, diffuse 127, ambient 47) have lighting.h's specular term (2 n.l) n_z - l_z: it reads the view-space z of
  the normal, so it follows the camera's turn (|R| = |l| = 1.094, so it can reach 255 from any level). The export now
  takes every luma it can reach; `prepare_face_order.py --live-specular` bakes those faces without the term and flags
  the pack NM_PACK_LIVE_SPECULAR; the renderer (model_submission.h `sq_live_update`) evaluates the term for the meshes a
  frame draws, one dot a class (855 classes: faces of one material and one normal), and rewrites their baked bindings
  and stream records. Check: stream simulator `SIM_LIVE_CHECK=300` (300 view turns) against lighting.h on the turned
  normal and light: 369,299 of 369,300 side lumas the reference's binding, 1 one step off, 0 wrong; every class changes
  binding; stream records right. Live faces start no offline-strip run (their strip vertices carry a load-time colour).
- **Sky**: 3a48[1], 2048 x 432 (54 tile rows), both export checks pass. **HUD map**: `prepare_hud.py --course-map 1`
  (the Expert HUD trace): 133 map polygons, player and leader marker fits 0.0024 / 0.0027 px with course 0's turn,
  sprite MAP_C1 in the same three banks 61..63 as MAP_C2. **Music**: Sky High (t05) as music_select.h always said; see Sound below.
- **Heap and prebuilt world streams**: Expert's stream records (2,277,312 B of faces + 0.6 MB of lists, strip vertices
  and ranks) do not fit: the load runs out of memory with them (stats-expert image, assets of this branch). A
  per-course switch, `NATIVE_WORLD_STREAM_COURSES` (default 5: Beginner and Advanced; course_assets.h): Expert's world
  batches take the queue path, which submits the same words. With streams the load runs out of memory 76 KB short at the
  stream position lists, with two more stream tables to come (stats-expert image). Session (`heap_budget.py --session
  0,2,1,0`, the stats-expert ELF of the run below): Expert racing 2,901,656 B free (course 0 3,151,056, Advanced
  3,164,976), 2,606,504 never taken over the whole session (the Expert race load is the low point). Keeping the streams
  on Expert would need about 420 KB more (the shortfall plus the 256 KiB budget): a stream table for a budgeted subset
  of meshes (records indexed per mesh instead of by face) is the way, not done.
  Done since (branch expert-streams): the record cache (model_submission.h NM_SQ_STREAM_POOL, make
  NATIVE_WORLD_STREAM_POOL, default 512 KiB): records per mesh in a pool, built in the idle wait for the PVR
  (code/audio/idle_wait.c, sq_stream_idle: 16-face steps, the clock read between them) for the meshes the last frame
  walked, the least recently walked evicted. Static eligibility checks run at load. Hardware, stats-expert-finish:
  registration mean 7.01 -> 6.44 ms, max 16.17 -> 14.76, late frames 7 -> 2, checkpoints 0 of 345 differing; heap
  after the race load 617,936 free (307,200 never taken). stats-attract: 19 -> 8 late (Expert stretch 9 -> 3).
  Offline strip runs no longer take live specular faces on either path (the stream's strip vertex records hold
  load-time colours): Expert's queue path writes about 0.2% more TA vertices for the same triangles. Simulator:
  SIM_STREAM_POOL / SIM_STREAM_BUDGET, SIM_ORACLE_NO_OBJECTS for synthetic frames without world objects.
- **Frame cost of the queue path on Expert** (stream simulator on 600 frames of the Expert time-out run, the SH4
  instruction model of `build/dc-native-cell-streams.SMXQl9/tools/model.py`, frames from a port of the views branch's
  frame generator, `build/dc-native-expert.gen`): world registration 511,390 instructions a frame mean, 980,950 worst,
  against 378,325 / 712,316 with streams (26%, 27%). Calibrated on course 0's hardware A/B of 14 October (streams saved
  203k instructions and 1.29 ms mean): about +0.85 ms mean, +1.7 ms in the worst frame. Expert's frame bound (5x5 cells)
  is 5,300 faces and 17,339 points against course 0's 12,008 and 28,373, so its queue path costs about what course 0's
  stream path does. The live specular update adds one dot per drawn class and two to six stores per drawn live face:
  estimated below 0.2 ms. To measure on hardware: stats-expert's registration against stats-advanced/stats.
- **Heap budgets derived** (coordinator's question; `tools/heap_budget.py` "Budgets"): the old 2 MiB never taken / 3 MiB
  free were a margin set after the 13 October failure. Session replays everything that runs after a load (race-time:
  vertex marks 16,416, enDjinn segments, finish and podium music: the loader's 8,224-byte staging buffer, then the flow
  up to 34,967, freed; scene switches with the CPU packs kept by nm_practice_render_release; name entry; select; the
  VMU writer during the attract load). Race-time needs about 60 KB, the largest block 34,967. Budgets now: 256 KiB never taken over the session, 512 KiB free
  while racing; NATIVE_FRAME_COMPARE adds its 921,600-byte capture; profile and late capture are modelled. check runs
  the first load and a session over every course the set holds. Not yet checked on the console: the session's music and
  scene-switch parts (`elf/play-heap-trace`, NATIVE_HEAP_TRACE=1: compare its `Native heap` lines after each load).
- **Sound**: with the effects' race bank resident (485,984 B of the 2,072,768 B AICA arena) the PCM8 race pieces of
  Advanced (t04, 1,616,201 B bank + flow) and Expert (t05, 1,818,799 B) do not fit (1,586,784 left): before this branch
  both races were silent on the console. `music/adpcm/t04, t05` (recipe step `music_adpcm`): sound.c's loader (spread
  over the idle waits) reads the flow's size before the bank streams (step LOAD_ROOM) and takes the ADPCM copy when
  afx_mem_stats shows no room for the PCM8 bank and flow: Sky High in ADPCM (788,647 B bank). A quality choice for the
  owner; the shutdown report marks such a load "(ADPCM)".
- **Palette banks**: the HUD pack holds the three courses' maps in banks 61..63 (colours 15, 15, 1), as with Advanced
  alone; the podium writes 60..61 on its cinematic's first frame, after the HUD's last frame (phases 20..24), and the
  HUD writes its banks again at the next scene load.
- **Hot layout**: the live specular update is out of line (sq_live_frame): practice_prepare keeps its slot of
  config/hot_layout.tsv (it grew 44 bytes past it with the update inlined).
- **Run from the ROM**: `build/dc-native-expert.recipe` (on 7776581 + this branch; the steps after select_pack and the
  ELFs again after the rebase from c7d6f80, whose tools for the earlier steps are the same): 2706 rows identical, 127
  added, 10 changed (the shared headers and HUD sheet-1 above); course1_lockstep PASS (6673 rows, 30 cars); every ELF's
  check 77 PASS and the 5 known FAIL lines, heap first load and session PASS. The hot layout was derived again on the
  stats build (nm_hud_tight +6 bytes for MAP_C1, practice_prepare +28: practice_update 0x20 later, estimate 2.6 samples
  a frame as before). Hardware: elf/stats-expert (expected rows work/script-expert/expected-checkpoints-expert.txt, 141
  rows), elf/controller-expert, elf/play, elf/play-heap-trace; asset set assets-ssfar4f.
- **Not done**: Expert's props and actors (cones 223114 without a rest pose, 223078/2230ac/2230e0, eleven handlers);
  a finishing Expert race with podium against a reference (the podium graph is exported, not compared); the attract's
  Expert stretch (the attract shows course 0).

### 17 October: other courses, first slice (branch courses)

Plan: `build/dc-native-courses.PLAN.md` (not committed): every course-0 assumption with file:line, what
Advanced and Expert need, reference traces, memory, ordered steps.

Facts established:
- The course byte 501460 is the ROM table slot, not the menu position: **0 Beginner, 1 Expert, 2
  Advanced** (laps 3950[slot] 8/2/4/8, cars 3980[slot] 40/30/20/8; `course_advanced.txt` gives slot 2,
  `course_expert.txt` slot 1). Files and tool arguments use the slot: Advanced is `course2.*`.
- The attract cycles the courses: slot 0 from frame 180, slot 2 from 3362, slot 1 from 6542, slot 0 from 9724.
- Off course 0 the race starts standing: 231e68[course] puts the start stage 501850 at -1, so c8b0/c91c
  give every car its normal updater (no rolling formation), ab78 returns at once (`c != 0`), and phase 20
  lasts 321 ticks: 3458 decrements 5010a8 from 320 (33b8) and selects 21 when it turns negative. The start
  signal object 501248 (c010, disabled at once when 5010ec == 8) revs the engine with the accelerator
  (c130), calls 62b4 at 5010a8 == 192 (the start camera ends 128 ticks in) and sets 501a02 = 26 at 0.
  `include/race_start.h` nm_race_start_signal.
- The guided-mode shift of 5014f7 is 77d0[course] = 2, 1, 2, 0 (only Expert differs from course 0).
- The slot machine 2204fc is a course-0 actor; it no longer runs on other courses.
- The reference's first phase-20 row differs per menu path: FIRST_ROW 5 in the course-0 runs, 4 in the
  Advanced run (three loading rows); 5010b0 on that tick is 2116 there (2368 on course 0; both 0 mod 4).
- The program reads 501460 in 56 places; the plan lists them with their native status.

Verified (host): `trace_race_native --course 2` on an 8000-frame Advanced reference
(`recipes/inputs/inputs-advanced-timeout.txt`, steering 0x98 to the time-out): 5669 ticks from phase 20
to the results phase 26, all 20 cars equal in prof/lap/done/rank/x/z/spd/yaw on every row but the first
two (the last loading row and a row the reference samples mid-pass); the start signal's rpm, camera,
cells and guided mode equal on every row. Course 0: the fast race's race script and checkpoints are
byte-identical to the recipe's. The recipe step `course2_lockstep` repeats the Advanced check and fails on
any difference (`compare_race_trace.py --require-equal`).

Hardware (17 October): the world-less Advanced stats ELF matched all 120 expected checkpoint rows, 20 cars,
standing start; the course-0 ELF of the branch identical with unchanged frame time.

Second slice (17 October, rebased on dreamcast-native 1a6b422):
- **The course is a run-time choice.** `nm_practice_course_select(slot)` (practice.h, for the front end;
  `NATIVE_COURSE` only sets the boot value, ELF suffix `-courseN`). Each scene start loads its course's
  packs (practice_course_load: the race's course, the attract's 0) after releasing the scene's render
  packs, so they take the heap top again; the scene loads that course's world, world materials and sky
  (course_assets.h: `courseN-world.dnm/.dnt`, `courseN-sky.dns`; course 0 keeps `world.*`, `sky.dns`).
  One race-start header holds every course (`prepare_intro.py --race-start --courses 0,2`:
  `nm_race_course_starts`), one podium header too (`--podium --courses 0,2`: `nm_podium_courses`,
  2865a70[course]; course 2's stage 2 lists its events unsorted, as 2264f4 allows). Records: the game
  save's table of the race's course (23b544[course]: table N for slot N). Music, events, announcements,
  reflection, guided mode, HUD map: by course.
- **Advanced world** (recipe step `course2_world`): the cells of frame 3422 under the race light (58
  meshes, 15,301 faces), the world objects prepare_world_objects knows (4 meshes, the pit crews; the
  props 223078/2230ac/2230e0 and the handlers 220304, 2209e4, 220a7c, 221f10, 222030, 222090, 222c48,
  222cf0 are not drawn yet), textures from the Advanced race's video at frame 4000, resource numbers
  by texture word (`key_pack.py cells EXPORT auto`), no level-of-detail rules, road surface by
  `--surface-ground` (recipes/course2-surface.txt, no audited list), mip chains, strips:
  `course2-world.dnm` 1,502,112 B, `.dnt` 1,061,152 B (VRAM 978,816 B).
- **Sky**: export_sky.hpp reads 3a48[course] and the section height (43 tile rows on Advanced); both its
  checks pass (512 headings against the tile updater and the layer pixels). DNS1 byte 16 = content rows
  (0: 392), so course 0's sky.dns is unchanged.
- **HUD map**: `prepare_hud.py --course-map 2 TRACE` (trace_hud2: the Advanced run): 98 map polygons,
  player/leader marker fits 0.0033/0.0034 px with course 0's turn J; sprite MAP_C2 in a third palette
  bank (HUD banks 61..63, palette_banks.h). The HUD tracer counts the markers from the end of the overlay.
- **Heap**: the world point cache and face queues are sized to one frame (5x5 cells + objects:
  nm_render_world_capacity); heap_budget.py models it and `--course`; check runs it for course 2 too.
  Advanced race: 3,470,680 B free while racing, 3,305,544 never taken (budgets 3,145,728 / 2,097,152);
  course 0: 3,413,504 free (3,307,696 before).
- Lockstep: Advanced time-out all 20 cars equal on 5668 rows; fast and manual race scripts PASS; the four
  views runs give the same native traces as 4d321ba's harness (ptimer now equals the reference's).

Third slice (17 October evening, on dreamcast-native 78c1e0e with the front end):
- **Front end**: the select's course goes through nm_select_course_raced (NATIVE_COURSES, default 5u now:
  Beginner and Advanced; Expert races Beginner with the console note) and nm_practice_course_select into
  practice_race_course; the attract loads course 0, the race entry the chosen course's packs. The front-end
  harness still runs its attract on the loaded packs: course 0 only (trace_race_native --frontend).
  tests/test_frontend_lockstep.py PASS (selects A-E, races A/B/E every field of every car).
- **Scene light** (include/tunnel_light.h): 7024 from the camera object 6510 (pointer 501240): +60..+68 the
  world light, +84 the range index (7030: one step a tick by the player's profile against the enter/exit tables,
  backwards while bit 28); 709c: path flag 256 and a profile in [175, 232): normalize(car - light[k]) where the
  mask 0x2aaaaa has bit k, else 0; the light glides by 2323aa[speed >> 6] a component. Tables shared by courses
  0, 2, 3 (race_start_data.h nm_race_course_starts[c].tunnel). trace_race.hpp/trace_race_native print light=;
  Advanced time-out run: 5449 rows equal, 220 within 1.8e-7 (the TGP's normalise), the loading row aside. Cars
  are lit by it; the baked scenery keeps the normal light (the original relights the tunnel walls).
- **Windows' tunnel texture** (6fe0/7008): h0 |= 0x3c0, h2 = 0x0c0c (64x64 at sheet 0 384,512 of course 2's
  texture RAM, a dark mottled texture); the car pack carries 209 tunnel bindings (tools/tunnel_windows.py, recipe
  step export_video2 before car_packs; 55 palettes as before, VRAM +2 KB); nm_render_car_material_header swaps the
  window materials (shared 1c8/241: material 0; the player's 13b: 354) on TUNNEL/RESTORE. Not modelled: 6f60's
  restore of the shared windows when the player's +88 car leaves.
- **The eight world actors of Advanced** are frame-counter animations (list[(5010b0 >> shift) & mask] in world
  coordinates; tools/prepare_world_objects.py HANDLERS): 220304 (64 by (c&7f)>>1, cells 238b14/16), 2209e4 and
  220a7c (cells 7a/6a, 99/89), 221f10 (racing: player profile fa..168; plus lists 288bbe0, 288bbf4), 222030,
  222090 (table by completed laps 0..2 / more), 222c48 (32 lists, three cells), 222cf0 (always). --clock-actors
  builds them for measurement: 33,402 faces in 610 meshes; most frames deform (no rigid fit), three only change
  texture. Full meshes ~3.4 MB, per-frame positions ~1.2 MB: beyond the ~300 KB the course leaves. Needs a new
  representation (vertex animation at reduced frame rate, texture animation by binding swap) before porting.
  The props 223078/2230ac/2230e0 (as 223114: rest heights from a reference) are not drawn either.
- **Expert measured** (cells of attract frame 6602 without the race light: its export refuses, variable
  specular or no supplied normals): 95 meshes, 23,722 faces, 76,179 vertices, cells pack 2,356,960 B (over the
  2 MiB pack format limit, NM_PACK_MAX_BYTES); packs course1.dnc 598 KB, .dnr 172 KB, routes 8 x 28 KB. Heap model
  (Advanced's world materials standing in): as built 1,058,096 B free while racing; without prebuilt world
  streams 3,658,680 B (passes; stream faces alone are 2,277,312 B); without car streams only 1,107,960. So:
  a per-course switch off the world streams (CPU cost to measure), plus the pack limit and the static light.

Not done: the tunnel effects of Advanced (6fe0: 19dc8 rewrites the player's window headers, word 0 |=
0x3c0 and word 2 = 0x0c0c, a course-2 texture at sheet 0 (384,512) that car.dnt does not hold; 709c: the
car light in a tunnel is a per-range point light, tables at +0x78/+0x7c/+0x80 of its object), the
unknown world actors above, an attract on other courses, Expert (pit waypoints; memory), the course
switch's heap on hardware (watch the nm_heap_report lines after each scene load: a block left above the
course packs would keep their old place as a hole).

### 18 October: fixed placement of the hot code and data (branch layout, NATIVE_LAYOUT=1 by default)

Registration moved by 0.1-0.3 ms between builds with unrelated changes (LTO re-emits the whole program). Now the
link places the race's hot set at fixed addresses, from `config/hot_layout.tsv` (`tools/hot_layout.py`, docstring:
why and how): KOS startup, empty space, a hot data block at 0x8c013600 (71 small statics of the renderer and
updater, 12.7 KB), 37 registration functions (order minimising a direct-mapped miss estimate from the
crewopt sample profile, per inner loop and per object), 21 per-frame enDjinn/KOS/libc functions, 32 updater
functions, each in a fixed slot; then the rest; the image end (heap start) padded to residue 0x1a54 mod 16 KiB
(33ac4dd's). Matching by base name (`.text.NAME .text.NAME.*`) survives clones and `.lto_priv.N`.
- Hardware (scripted fast race, registration; console-lay-*.log): 78c1e0e 7.72 ms, late 9; layout with the data
  block at 0x100 7.45, late 6; the same plus a 4.4 KB function 7.44, late 5; the whole .text content 0x3500
  higher (block at residue 0x3600, code shifted with it: same I-cache relations, .rodata/.data/.bss 16 KiB higher,
  heap and stack in place) 7.34, late 4. Render 9.38-9.39 and checkpoints identical in all. The last is the
  default since commit "Hot layout: the data block at 0x3600". The first guess (the registration's stack frames,
  residues about 14-16 KiB, against the hot statics) is refuted: at 0x3600 the block's head sits there. Why
  0x3600 wins is open (an O-cache relation of the block and the literal pools with the heap or .bss): the PMU ELFs
  of both placements answer it on hardware (build/dc-native-layout.elf).
- Static invariance (`build/dc-native-layout.elf/report-invariance.txt`): the layout build of 78c1e0e, the same
  plus a 4.4 KB function in code/audio/sound.c, the layout on 33ac4dd (before the front end and sfx), the PMU and
  the sample-profile builds: all 161 entries at the same cache index in all five, sizes equal except
  practice_update/practice_prepare (their source changed), practice_shadow_volumes' code (its source changed, same
  size) and the PMU build's sq_submit_object (its counters; `@pad` keeps the rest in place). The hot region of
  the 4.4 KB-dummy build is the same bytes but 1066 literal-pool words pointing at cold code or data.
- Checks: every link prints "PASS: hot layout" or a LAYOUT banner listing what moved; make check fails on it
  (76 PASS now: the 75 + this one; the 5 known FAIL lines). After a deliberate change of a hot function, rerun
  `tools/hot_layout.py derive` (command in its docstring) and commit the config. `NATIVE_LAYOUT=0`: the linker's
  own order (suffix -nolayout); `NATIVE_HOT_ORDER=1` (pin_layout.py A/Bs) turns it off.
- Cost: the empty 13.5 KB before the block and up to 16 KB for the end padding (stats ELF: 3,202,128 free while
  racing against 3,236,304 without the layout).

### 17 October: front-end flow, course and transmission select (branch frontend)

Plan and findings: `build/dc-native-frontend.PLAN.md` (phase handlers, the select object, what reaches the race);
work files `build/dc-native-frontend.out/` (`build_m2segment.sh`, `build_harness.sh`, `build_elf.sh`, reference
traces `ref/`, select art `sel/`), asset set `build/dc-native-frontend.assets` (sfx3 + `select/`).

- **The original's flow.** Phase bytes 5010a4/5010a0, handlers 018cc. Attract loop: 2 (setup, music) -> 3
  cinematic (1602 iterations) -> 4/5 ranking overlay over the same stage graph (1+320) -> 6/7 V.R. demo race (1+834;
  player and 20 opponents gridded, the player on the opponents' updater chain, views every 128 iterations) -> 8/9
  title (1+386) -> 10 -> 2, courses 0, 2, 1 (20b8); 3147 iterations a loop. Coin mode starts the game on the credit
  itself (1108 -> 2818), in phases 3 (not its first iteration), 5, 7, 9: phase 12 (game start, ae1001) -> 17 (every
  attract object off, falls into 18's substate 0) -> 18 (the select object 501218, updater 225234) -> 19 race entry.
  Game over: 25/26 (673 iterations) -> 29 -> 2 (no record) or 30 (name entry) -> 2; 501460 keeps the raced course.
- **The select** (`include/select.h`): course slot from (501050+50105c)>>1 (<= 0, <= 108, else: slots 0/1/2 =
  courses 0/2/1), transmission > 30 = manual, the accelerator's high edge 501068 confirms (the transmission only once
  its 640-iteration timer is <= 624), timeouts confirm, START held at the transmission confirm is time attack
  (501a80; not native: the pad never holds it). 17 states, then the GENTLEMEN screen; the longest select is 1593
  iterations (the 1600 safety timer never fires). Lag frames (31 at the course load) add no iteration.
- **What the race inherits** (`include/frontend.h`): the pool's collision tracking words of all 40 entities
  (+28c/+290/+294/+298/+27d/+2a4) as left by the credit's iteration, phase 12 (opponents step with flags 1<<12,
  the player frozen, 1210c) and phase 17 (1210c over the switched-off field); the leading lap 5014ba (the
  cinematic's laps count from 0, the original's from 255: -1 offset); the frame counter 5010b0 (race counter base:
  slot machine parity and the collision phase 5010b0 & 3; first attract after power-on: cinematic tick + 3); and the
  first tick's drawn-car count 5014f7 (race view, the previous scene's cell bitmap). The native cinematic equals
  the original's for every car word on every iteration up to the credit (measured to frame 1290).
- **Verification.** `m2segment --trace-frontend` (per main-loop iteration, sampled at the vblank wait 12a8) of
  `recipes/inputs/frontend-{A..E}.txt` (credits at frames 1290/390/690/1090/1690, all courses, both
  transmissions, timeouts, confirms, an ignored early edge): `tests/trace_frontend_native.c` equal on every select
  iteration (5 runs, 644-1790 iterations); `trace_race_native --frontend` + `tests/test_frontend_lockstep.py`: the
  races of A, B, E equal on every row in every field of all 40 cars, tracking words included (2614, 2580, 2326
  rows; the player's +b0 on rows 0-4 aside: the cinematic's steering word, rewritten by the first tick). Existing
  damage and slot lockstep unchanged (default counter base 2368).
- **Native flow** (no build flag): power-on -> phase 2 (music) -> cinematic 1923 ticks -> title 386 ticks (the
  demo's 836 iterations counted) -> phase 2. START/A in the cinematic = the credit; in the title = credit with a
  cold pool. `code/gameplay/select_mode.c` (-fno-lto): game start, select ticks, voices, race entry with the
  carried state; name entry/game over -> phase 2; both triggers + START in the attract opens the settings.
  Records go to the raced course's table.
- **Pad (proposal):** stick or D-pad left/right step the highlighted choice (fed as the wheel 0/64/127 and 0/127),
  START, A or the right trigger confirm (one accelerator edge; a press outside the two choosing states is dropped).
- **Courses:** `NATIVE_COURSES` (bit per course, default 1 = course 0) lists the courses whose data the build has;
  the select shows and plays all three as the original; a course without data races course 0 (console line).
  `nm_select_course_raced` is the hook for the courses branch.
- **Select art** (`m2segment --export-native-select`, `tools/prepare_select.py`, recipe steps export_select and
  select_pack, `assets-*/select/`): layers 2/0 and the panels/digits/arrows from their authored blocks (exact
  PAL4), maps, rings and the small map from the screen (maps quantized to 16 colours); 84 textures, 858 KB of texels,
  34 banks, loaded at the select's start. `code/render/ui/select_render.c` draws by state; wipes, slide-in and
  palette fades are cut (same tick counts). `tests/test_select_render.py`: the host build of the renderer against
  the original's three screens, every pixel equal but the maps and the transmission screen's two cars.
- **Not done:** the transmission screen's two turning cars (3D, car pack); the ranking overlay (phase 5); time attack;
  courses 1/2 data. (The second game, the counter after a game and a credit in the demo or title: "30 October: games
  one after another".)
- make check of the play ELF: 75 PASS + the 5 known FAIL lines; heap 3,002,536 never taken / 3,290,960 free while
  racing. Not run on hardware.
- Code placement (branch fe-merge): the unpinned LTO build moved 23 of 32 registration functions against
  33ac4dd (hardware: registration 7.56 against 7.43 ms). The front end's gameplay now runs inside the LTO unit
  at load time only (no race function gets a caller from the -fno-lto select mode), the attract's ticks are
  outlined from practice_update (now 424 bytes under the baseline's, so it keeps its pinned place). Pinned A/B
  (`build/dc-native-femerge.elf/pin_pair.sh fast|play check`, tools/pin_layout.py from 33ac4dd's map, the select
  objects as --extra): all 32 registration functions at the baseline's I-cache index and size, 22 of 24 updater
  functions (practice_update itself shorter, nm_intro_scene_step attract-only).

### 17 October: operator settings and VMU records (branch rules)

Plan and findings: `build/dc-native-rules.PLAN.md`; work files `build/dc-native-rules.work/` (EEPROM dirs,
reference traces), host build `build/dc-native-rules.host/`, ELFs `build/dc-native-rules.elf/build.sh`.

- **Settings in the original.** The I/O board's 128-byte EEPROM; 228230 copies it to backup RAM 1d00000, 227cf0
  checks "SEGA", game id 8240, layout 0001 and a CRC-16/XMODEM (table 23c1d0) over +0a..+7f, falls back to the copy
  at 1d00080, then to the factory image 23c150, and publishes 53e59c = +20 DIFFICULTY (EASY, **NORMAL**, HARD,
  HARDEST), 53e5fe = +19 ADVERTISE SOUND (**ON**, OFF), 53e59a = +1b COUNTRY (USA, **JPN**, EXPORT), 53e5d0 = +1a
  CABINET (**DELUXE**, UPLIGHT), 53e110 credits. That is every item of GAME ASSIGNMENTS (229640..). **No race-length
  setting exists in daytona93**: the laps are table 03950[course] (8, 2, 4); no link settings either.
- Difficulty is the only one the race reads: the start time (3860: 2870304[course][d]) and every award (13574,
  135a4). Advertise sound: 1ca8 at phase 2 sends ae1008 or be1a10 (fade). Country: texts, voices, mph (b1a4);
  cabinet: colour translation at boot (0a84); both left at the factory values (presentation the native art does not
  cover).
- Native: `include/settings.h` (values and defaults of the original), live in `code/save/game_save.c`; practice.c
  reads the difficulty at the race start (scripted races stay NORMAL); the title fades with ADVERTISE SOUND OFF.
- Harness: `m2segment --nvram DIR` / `--save-nvram DIR` (tools/common/nvram.h), `tools/settings_eeprom.py PROGRAM
  NEW_DIR --difficulty hard ...` writes the factory EEPROM with the fields set and the CRC sealed (`--show` decodes).
- Lockstep (`tests/test_difficulty_lockstep.py`, `trace_race_native --difficulty`): fast race easy/hard/hardest
  10203/10203 rows each (remaining at the finish 982/362/297), race_to_end time-outs easy/normal/hardest
  5444/5640/5401 rows, the ninth-place script on hardest (carry mode) 10789/10789 rows, a time-out in lap 8.
- **Records.** Three backup-RAM tables of 24 slots of 24 bytes; 226a5c picks them by the course byte 501460 & 3:
  **0 Beginner -> 1d00360, 1 Expert -> 1d005a0, 2 Advanced (and 3) -> 1d007e0** (23b544). Defaults 2265cc at boot
  when the signature at 1d00200 is missing: time base 2266d0[t] = 0x2800, 0x3c00, 0x3c00 + 320 per rank, the view
  word left erased. The selected slot per course (*1d00a20/a24/a2c). Time-attack bests 1d00b60..b9f (lap and race,
  automatic and manual; 3750/aa0c, only with 501a80 set; reset 226654 for courses 0-2: lap 1280/7680/3840, race
  10240/15360/15360 for courses 0/1/2). `tests/check_save_reference.c` against `m2segment --save-nvram`: after the
  boot every table, slot and time-attack word equals the native defaults; after the fast race the original's
  tables equal the defaults with its record inserted (slot 0, 9252).
- Save: `include/save_data.h` (payload "DNRS" v1, 1812 bytes, CRC-32; VMS file DAYTONA93NAT, 5 blocks, our own
  icon, header CRC as vmu_pkg_build), `save_store.c` (load/write through card operations: no VMU, empty, full,
  damaged, truncated, faults; a card that already holds a save gets the session's new records merged in, so a VMU
  put in after the boot or swapped keeps its own records), `game_save.c` (live state; KOS vmufs; the write runs on
  a KOS thread, requested by name_mode.c before the title and by the settings screen; boot load in nm_practice_run
  before the title). `tests/test_game_save.c` (make check) runs it all against a fake VMU.
- Settings screen (proposal until an options menu exists): at the title hold both triggers and press START.
  DIFFICULTY, ADVERTISE SOUND, CLEAR RECORDS (A twice), EXIT, the VMU's state; START/EXIT saves, B cancels.
  `tests/test_settings_mode.py`.
- Not done: COUNTRY/CABINET (other 2D art and colour tables), time attack itself, the course select passing its
  course to the records (practice.c PRACTICE_COURSE = 0). Not verified on hardware yet.

### 17 October: manual transmission (branch gears)

The original: 501464 is the transmission (0 automatic, 1 manual), written by
the transmission select 2256fc; in the scripts, the wheel right during frames
2010-2390 of the race_to_end menu selects manual on course 0 (1450-2350, as in
race_steer_right, also changes the course). The shifter is three bits of the
input byte 1c00012, inverted into 53e120 bits 20-22 every frame by 1c020 (no
delay: script frame F is trace frame F's `gearsel`); gears 1-4 are the values
5, 6, 1, 2, neutral 7, and table 234abc keeps the gear in neutral. The manual
gear rule 11e80, the manual torque curve (ed44), crash tables and the 1210c
field were already native (gears.h, drive.h, damage.h, race_field.h). New:
the transmission reaches the grid body variant e5ac (cars 0-4 start on
288eb4c), the player's tick and the results field (code/practice.c,
tests/trace_race_native.c `--transmission manual`, which feeds the trace's
shifter like the wheel).

Pad (a proposal, `PRACTICE_SHIFT_*` in code/practice.c, include/shifter.h): A
one gear up, X one gear down, relative to the car's gear, the lever in neutral
otherwise; Y is the view change (branch views). Choosing manual until a select
screen exists: `NATIVE_TRANSMISSION=manual` (ELF suffix `-manual`), and
holding X at the START that begins a race picks the other transmission
(read in nm_practice_restart_from_title; an AUTO_START boot may start before the pad is read).

Lockstep: recipes/inputs/inputs-manual.txt (drive_race_native --manual: 8 laps,
3rd place, 20 shifts 4-3-4) on the board against production `--transmission
manual`: tests/test_manual_lockstep.py PASS, player and timeline equal on
10154/10154 race rows, the 39 opponents too (tracking words aside). The
recipe makes the trace, runs the test and writes headers/script-manual and
work/script/expected-checkpoints-manual.txt; `--elf` adds elf/stats-manual.
Manual script headers add `NM_RACE_SCRIPT_MANUAL` and the shifter in edges
bits 3-5; automatic headers are byte-identical (the fast race's header and
checkpoints unchanged).

Colours: 7358/7374 draw 286fab0/286fab8[501464], so the manual car is body
960dae and shell 961cb0 (same vertices and faces as 95ff9d/961bbf, other
texture headers); its damaged bodies are car-damage.dnb's variants written in
its face order (nm_damage_body_face_map; stream_sim `SIM_DAMAGE_CHECK=manual`).
The crew's manual twins (1ad3c, set + 64) are in the car pack now (12 models,
car.dnm +23 KB, no new texture). The HUD gear indicator b0c0 is native since
18 October (branch gear-hud, below). Not ported: the manual hoods (no hood view), the
best-time tables per transmission (3778/aa38, only with 501a80 nonzero).
Not verified on hardware yet.

### 18 October: the gear indicator b0c0 (branch gear-hud)

The arcade has one, manual races only (b0c8: 501464), and it is the icon of the 2001 Dreamcast version:
on a change of the gear +148 against +14c (written only here) the HUD object's +6 counts 31 ticks of
lever animation, frame counter>>1 of the row 23253a[old][new] of the blocks 23263c[0..7] (3x3 tiles at
10004c4: the H-gate with the knob in gear 1..4, the middle, the left and right neutrals, and a blank block
on the last tick), then "(n)" (2872a0a, digit gear+1 by 18698 in colour 0x84, 2872a1a) over "SHIFT"
(28729de at 10005c2). The 16 rows follow a rule (old gear, its column's neutral, through the middle and
the other neutral when the columns differ, new gear): include/hud.h nm_hud_gear_frame, checked against
all 256 bytes by prepare_hud.py, which exports the blocks and digits as sprites (14; no new palette
bank: they are placed after every other sprite, a bank may now span sheets, and bank 0's lever sprites go
on a fourth sheet, 256x32; sheets 0 and 1 are byte-identical, bank 2 (63) gains the digits' yellow).
Cost (check_hud_tight over the native manual race): 136.8 -> 141.6 rectangles and 274 -> 283 TA records a
frame on average, peak 180 -> 185 of 288, plus a header where the sheet changes; nm_hud_gear and
nm_hud_gear_place are out of line (cold), nm_hud_place grew 108 bytes; config/hot_layout.tsv derived again
(the data block grew 100 bytes: the hot code starts 0x60 later). Checked against the original's manual race (m2segment --trace-hud
of recipes/inputs/inputs-manual.txt; its S9..S11 rows hold the cells): check_hud --manual on the
reference variables and trace_race_native --transmission manual --hud both agree on 10148 frames (lever
on 526, digit on 9612), 2 frames one tick stale; check_hud.py's previews show the indicator's pixels
equal to the arcade's but where the needle (a frame late, as everywhere) crosses it. Options, off by
default: NATIVE_HUD_GEAR_GATE=1 keeps the H-gate in place of "(n) SHIFT" (as the 2001 version shows it),
NATIVE_HUD_GEAR_AUTO=1 runs the indicator in automatic races too. The digital km/h of the 2001 version is
the arcade's own speed readout (b1a4), drawn already. +14c is kept across races in the original (a race
that starts in another gear than the last ended in animates once); native resets it to the entry gear.
The other mismatches check_hud.py reports on the manual trace (cells after the finish from frame 14340,
the player's map marker from 12056) are there without the indicator too.

Gear digit with the gate (branch gear-digit, chosen 19 October, the default):
`NATIVE_HUD_GEAR_DIGIT=right` (default with the gate; 0: the gate alone, suffix -gateonly) adds the "(n)" digit
(GEAR_DIGIT_1..4, no new art or bank) to the right of the H-gate, centred on it, from the shift on (the lever still
animates): cell top left 297,76 (NM_HUD_GEAR_DIGIT_AT_X/Y = lever +25,+4; make NATIVE_HUD_GEAR_DIGIT_AT_X/Y moves
it, suffix -gearat<X>x<Y>), columns 297..304 and rows 76..90 against the gate's art 274..293 x 73..92; clear of the
km/h (to 263), the arc's "10" (from 310) and "TIME EXTENSION!!" (rows 96..111), so it is never hidden. Draw
order: nm_hud_place places the needle before the indicator now, so the needle (sheet 2) lies under the digit
(sheet 2, later) and the gate (sheet 3); its inner end (radius 121) crosses the digit's cell from about 8200 rpm
to the stop, behind it. The earlier beside (low right, hidden under the extension message) and over (digit on
the gate, the digits' sheet painted last, NM_HUD_PAINT_SHEET) proposals are gone; the original's "(n) SHIFT"
(NATIVE_HUD_GEAR_GATE=0) now also draws over the needle where they meet, as the arcade's tiles do. Cost over the
manual race (check_hud_tight PASS, 0 pixels differ): 138.8 -> 139.8 rectangles and 278 -> 280 TA records a
frame, peak 183 of 288; nm_hud_place in its slot, nm_hud_gear_place cold. Manual lockstep unchanged (hash
6da929c5). Pictures: tools/hud_gear_preview.py HUD_PACK OUT (host; variants digit, gate, original; gears 1-4, a
shift, the extension message, <variant>-needle.png with the needle from 8000 rpm to the stop).

### 17 October: the views VR1..VR4 (branch views)

Plan and findings: `build/dc-native-views.PLAN.md`; work files
`build/dc-native-views.work/`. The scripted views race passed on the
console (135/135 checkpoint rows, before the rebase onto the manual
transmission).

- The arcade: the held VR buttons (`53e120` bits 5-7, 16; not with START
  held) pick the view `501710` in mode 0/2 during phases 6/7/19-22
  (`8874`); every race starts in VR3 (`33d4`). Presets `1d00220` (copy of
  `2320b6`): VR1 driver's view (mode 2, eye 1.05 up 2.15 ahead, focus 240,
  car not drawn), VR2 hood (mode 2, eye 0.2 ahead, focus 250, the hood
  231eb0), VR3 behind (mode 0, the old follow view), VR4 high behind (mode
  0, focus 260). The mode switches at once; eye, pitch, focus and window
  centre `501730` glide 1/8 a tick (`8814`). Mode 2 takes the drawn yaw
  `+4e` and an extra Ry(-`+d6`); VR1 pulls the eye back while spinning
  (`6938`). The finish sets mode 0 (`43bc`); the pit cameras are per view.
- **The view changes the race**: the view matrix feeds the detail levels and
  the drawn-car count the opponents read, and the eye the scenery anchor.
  The fast race with view changes crashes and times out in the original.
- Native: `include/race_view.h`, `include/race_camera.h` (the camera pass
  shared by practice.c and trace_race_native), presets and per-view pit
  tracks in `race_start_data.h`, `vr=`/`preset=` in `--trace-race`.
  Lockstep against the original with views (damage run E + 8 presses, fast
  race + 17 presses, pit runs in VR1 and VR4): all 40 cars equal on every
  race row except the known three start rows; camera mode and view counters
  equal on every row. Without the data the same replay diverges at row 721.
  test_damage_render.py's native check on the views run: 3,763/3,763 frames
  with the original's part lists (driver's view: shadow only; hood) and
  matrices within 1e-4 / 0.0015.
- Car pack: the clean hoods of both cars (94fff9, manual 9506a8:
  `append_player_parts.py --hoods`); the automatic car's damaged hoods are
  position/normal variants of the clean hood's mesh in `car-hoods.dnb`
  (`prepare_damage_bodies.py --hoods`, 21 KB), written into it as the
  damaged bodies are (the manual hood in its face order); stream_sim
  `SIM_DAMAGE_CHECK=hoods|hoods-manual`: refreshed tables equal rebuilt ones.
- Heap: the world's dense material/luma table (271 KB) is freed once the
  world is attached (load only). make check on build/dc-native-combo.assets
  with the hood car pack: 3,352,144 free while the scene runs (budget
  3,145,728; combo as delivered 3,181,184; the hoods cost 92 KB).
- Pad (proposal): D-pad up/right/down/left = VR1..VR4 held, Y = next view,
  START held keeps the view; A/X shift; START+B toggles the inspector
  (B alone: session reset).
- Render cost (stream simulator, fill.sim's calibrated model, fast race drawn
  from each view): VR1 8.64 ms mean / 12.49 max, VR2 10.07 / 13.55, VR3
  10.33 / 13.81, VR4 10.31 / 13.67. The close views cost less (no player car,
  no cars behind).
- Open: the radar zoom chord (START + VR2/3/4) is not wired; course 1/2's
  pitch-dependent eye turn (`69b4`) is not ported (done 30 October); the manual car's hoods.

### 16 October: the grass is not super-sampled (branch grass-no-ss)

Decision: the infield grass is not super-sampled, and neither are the grey
concrete aprons. `recipes/specs/ssfar4f.json` rule "verges" now holds only
the white track-edge strip 6686; the infield grass 6924 and the aprons 6664
stay at their plain level at every distance (bilinear, no mip chain: both
are road-surface words, `mip_skipped_surface_words` of the mip DNT report).
Every other super-sampled word is unchanged (asphalt 7168/6656/7680, lines
3588/3590/3591/6232, finish 6168, streaks 6680, edge strip 6686, gantry board
and the signs). Cause (`build/dc-native-fill.REPORT.md`): the worst race-E
frames (3164-3184, 2351) were 6924's super-sampled far level over 85-142k
pixels, 60-70% magnified, because a `{"run": 32, "radius": 1000}` group
with a far centre also switched its near faces. Only assets-ssfar4f
world.dnm/world.dnt change (1201 level-of-detail groups instead of 1290;
console "Native level of detail: 1201 groups"). Fill model (main fit, tr
smoke): race E mean -0.26 ms, p99 13.19 -> 12.11, max 15.16 -> 13.03; E
3164-3182 -3.3 to -4.5 ms, 2351 -2.8 ms; fast race mean -0.08 ms. The fast
race spikes at 5054 and 9722 (and their pop) are not grass: there a big
asphalt 7168 group flips to its super-sampled level (96-101k pixels, 75%
magnified); unchanged by this, the super-sampling guard of the fill report
would take them. 6664 alone is negligible (at most 64 super-sampled pixels
a frame). Host evidence and the hardware runs to make:
`build/dc-native-grass.README.txt`.

### 16 October: branch integrate2 (radar, opponents, live-play rejects, dust and sparks, music)

Branches (all local, nothing pushed): `dreamcast-native` 93775e0 holds
`next` (913fa26: car damage, rule 9148) plus, in order, the CONDITION radar
and TIME EXTENSION (45cc254, 87c386b), the opponent results field (d2a68b0,
96c1917, b28c84b) and the live-play reject fixes (93775e0). `integrate2`
(worktree `build/dc-native-int2.wt`) = 93775e0 + `fx-dust-sparks` (c04aa62,
22c1730) + `music-handoff` (03cca02, 44f0175) + this page; it fast-forwards
`dreamcast-native`. One conflict: `tests/trace_race_native.c` option
parsing (`--effects` beside `--start-lap`/`--poke-speed`; the code is then
byte-identical to `live-reject-show`, the demo branch that merged the same
four branches); `recipes/expected.sha256` only in its header comments. Run
commands and pass conditions: `build/dc-native-int2.README.txt`.

What landed since the night (each with its own README in `build/`):
- CONDITION panel 12c98..13208 and "TIME EXTENSION!!" ae98 (branch
  `hud-radar`, `build/dc-native-radar.README.txt`): the panel's player and
  opponent icons (every car within 42 units, zoom 0.005, the player's
  heading up) and the message blinking 4 on / 4 off for 128 ticks after a
  lap's time award, both in the HUD pack (28 recipe rows: `hud.dp`,
  `sheet-0.dt`, `sheet-1.dt` of every set, `hud_data.h`). check_hud.py
  against the arcade's HUD traces: native fast race PASS (10,198 panel
  frames, 33,300 icons, worst 0.0036 px), damage run E PASS (6,139 icons);
  run A matches up to frame 4336, where car 16's carried-state divergence
  (next item) moves two icons (78 frames). Hardware run `radar`
  (`build/dc-native-fence-hw.fP9gK9/hw/console-radar.log`): checkpoints
  identical, 3 late frames, HUD rectangles peak 182 of 288. Not there: the
  START + VR2/3/4 zoom chord (no VR buttons), the lap time message ad34.
- Opponent results field 4110/1210c (branch `opponent-fix`,
  `build/dc-native-opp.README.txt`): at phase 25 the handler 4110 clears
  flags bit 31 of every opponent (41b4..41dc: cde0 stops), while the
  interaction object 1210c keeps running in phases 25/26 (sort, timers,
  passing, collision pass 13f08, event tail 19038). Ported as
  `nm_race_scene_results_field` (race_scene.h), driven from
  `nm_practice_results_tick`. `tests/test_damage_lockstep.py` now asserts
  the opponents too: C, E, F, H 4436/4436 rows, G 4435 (its row 3628 is a
  mid-tick reference sample), A rows 0-1751. Run H's checkpoints change in
  15 results rows (cars 1/2 parked in contact); fast and tenth files
  unchanged. Hardware run `oppHr`: checkpoints identical to
  `build/dc-native-opp.elf/script-H/expected-checkpoints.txt`, 1 late frame.
- The carried-state finding (same README, cause 2; reported, not ported):
  the attract and select phases run 13f08 on the same entity pool every
  frame, and the race entry resets neither the collision tracking words
  (+28c/+290, +294/+298, +2a4, +27d) nor 5014ba, the leading lap of the
  sort pass 1224c (-1 in the recordings, START at frame 1290; a cold native
  start has 0). Run A's contact of cars 16/20 at native row 1752 consumes
  car 16's stale travel. `trace_race_native` carry mode plus `--start-lap
  -1` makes all six damage runs equal in every player, timeline and
  opponent field on every row (6,747,156 opponent fields). A faithful port
  needs the attract -> select -> race persistence of the pool, which the
  native front-end does not have (title -> race; no select screens).
- Live-play rejects (branch `live-reject`, `build/dc-native-reject.README.txt`;
  the first live-play run stopped with "Native practice rejected
  tick 5671"). A host fuzz of the production tick (`tests/fuzz_race_native.c`)
  rejected 66 of 8,000 abusive races on b28c84b: cvtri overflow 51 (the yaw
  step f9d0..fdf4 divides by a turn radius near 2^127; fbd0's cvtri now
  saturates as PRM 11-50, NaN gives INT32_MIN), a negative speed index 3
  (after a side contact; ef08/10464/10474 read the ROM words before the drag
  table and at the 16-bit wrap: three of them are erased 0xffffffff = NaN,
  now carried with the reference's NaN rules), a finish on profile 194/195
  12 (10bcc reads the route's 8 guard records; the cursor now reads them).
  After the fix: 8,000 races, 71.7 M ticks, 0 rejects; new ctests
  `native_race_fuzz`, `native_race_fuzz_finish`,
  `native_negative_speed_lockstep` (the original with the player's speed
  poked at its block entry, `M2_POKE_ROW`/`M2_POKE_SPEED`, equal on all
  1,617 rows). A reject now prints its step, stage, car and the player's
  state. Hardware run `rejectStats` (stats ELF of live-reject): checkpoints
  identical, 6 late frames, race registration 7.43 ms mean. Open from the
  fuzz: carry race 128165 leaves lockstep at native row 1942 (the original
  starts the crash camera there, the native does not; pre-existing).
- Car effects, dust and sparks (branch `fx-dust-sparks`,
  `build/dc-native-fx.README.txt`; spec
  `build/dc-native-effects.11xv92/spec-effects.md` sections 2, 3, 7, 8):
  the rule of 9088 in `include/car_effects.h`, dust 231f06[1..31] and sparks
  232006[9..24] drawn punch-through after the player's parts (47 models, 134
  faces, 10 PAL4 textures = 20,480 bytes of VRAM; car.dnm/car.dnt of every
  set and the race header: 19 recipe rows). ctest `native_effects_render`:
  every 9088 call of the effects logs reproduced, native races E/C/fast
  against the display lists on every race frame. Hardware run `fxE` (damage
  run E): checkpoints identical, 3 late frames, 3,544 punch-through draws,
  race render 8.39 ms mean against 8.24 for the same run without effects
  (`damageE`); dust and sparks were checked by eye on hardware and look
  right. Tyre smoke is computed but not submitted (translucent, below).
- Music in every asset set (`music-handoff`, above in "15 October, night"):
  `build_assets.sh --music ARCHIVE` or `--no-music` (one is required), 810
  music rows in `expected.sha256`.

Verification of integrate2 (host; no console use):
- Recipe from `roms/daytona93.zip` and the music pack with this tree
  (`build/dc-native-int2.recipe`, `--music ... --elf --check --variants
  ssfar4d,ssfar4bf3`, 1,430 s): 2,174 of 2,174 rows identical, so the
  merged `expected.sha256` needed no row change. Against `dreamcast-native`
  93775e0's file: car.dnm and car.dnt of the nine sets and
  `headers/race-header/intro_drive_data.h` changed (19, the effects), 810
  music rows added; the 28 HUD rows were already there (radar). Bulky
  `work/export` subdirectories deleted after the check (disk).
- ctest `-R native` (`build/dc-native-int2.cmake`, log
  `build/dc-native-int2.ctest.log`): 40 of 44 pass, 4 skipped as before
  (split_cutouts, opaque_cores, lod_groups, late_capture), none failed;
  `native_slot_lockstep` with the fast race hash 6368fbab, damage
  render/lockstep, effects render, fuzz, negative speed, wrong way,
  contact clear, slot machine included.
- HUD (`build/dc-native-int2.out/verify_hud.sh` -> `verify_hud.log`, the
  radar branch's checks with this tree's HUD pack): fast and E PASS, A
  the known 78 frames, tight geometry 0 pixels differ; the native .hud rows
  of fast, E and A byte-identical to the radar branch's.
- SH4 simulator (`tools/sh4_gameplay_sim.py lto`, `build/dc-native-int2.sim.log`):
  fast and tenth checkpoints IDENTICAL (fast about 3.990e9 instructions,
  335,180 a tick over ticks 2..4000; text 224,012 bytes).
- Stream oracle (`tests/stream_sim/check_stream.py` with
  `build/dc-native-int2.recipe/assets-ssfar4f`; the effects frame table of
  run E `build/dc-native-fx.gen/E4.frm`, the fast race and the attract of
  `build/dc-native-ssfar4f.gen`): 0 mismatches at stages 3, 5 and 6 and
  with the damaged bodies (`build/dc-native-int2.out/check-stream.txt`).
- The recipe's stats ELF (loaded c75398ec...): `make check` 71 PASS + the
  5 known FAIL lines; heap with assets-ssfar4f 3,489,224 never taken at the
  peak, 3,546,280 free while racing (budgets 2,097,152 / 3,145,728).

Waiting for the console (`build/dc-native-int2.README.txt`): the stats
fast race (`int2fast`), the stats damage run E (`int2E`, dust, sparks and
damage; expected checkpoints byte-identical to the car-damage run's) and a
playable ELF with music (`int2play`), all with
`build/dc-native-int2.recipe/assets-ssfar4f`.

Next, in priority order:
- Tyre smoke and spin smoke, the translucent half of the effects (spec
  `build/dc-native-effects.11xv92/spec-effects.md` sections 5 and 7; plan in
  `build/dc-native-fx.README.txt`): the rule is done (`nm_car_effects` gives
  the smoke draws, `translucent=1`); to do: the spin smoke step of task 9e2c
  once per gameplay tick (8 particles, bit-exact on 29,448 logged ticks),
  export of 231f86 and the four 232206 lists (35 models, 206 faces), a
  translucent class in DNT6 (it refuses checker headers now), and a third
  model list on `PVR_LIST_TR_POLY` pre-sorted back to front on the SH4, with
  the stream simulator taught the TR list. Spec cost +0.66 ms mean, 3.2 ms
  worst PVR in effect frames.
- Pit stop (spec `build/dc-native-pit.xjs2cE/spec-pitstop.md`, five new
  reference runs in that directory): the entry on row 152 of the pit lane,
  which the native already decides (`route_transition`) and drops; the
  kinematic pit drive dc18/dd14 toward seven waypoints, counter 501a84 (640,
  repair `+208 &= ~0x10f` and the clean body at S+512, release at S+640),
  the remaining time frozen, opponents hidden and the guided-mode byte 8,
  all 64 world actors suspended while the crew works, the keyed pit camera
  tracks, the six-figure crew, "PIT WORK!" ab2c. The native equals the
  original up to the decision tick and diverges the tick after. About 6-8
  days, gameplay about 3.
- AICAflow PR #12 (firmware ABI 8: status by the AICA's main-CPU interrupt)
  is merged upstream (origin main f448121). The Daytona pin bump (both pins
  in `platform/dreamcast-native/scripts/fetch_aicaflow.sh`, music rows if AICAforge encodes
  differently, `PLAYING_POLL` dropped in `code/audio/sound.c`) was then in
  progress on branch `aicaflow-abi8` (`build/dc-native-afx8.wt`);
  integrate2 still pins c6b116dd.
- Then the rest of the list under "15 October, night" (podium screens,
  cones, hood/VR views, asynchronous track loading, `NATIVE_HOT_ORDER`
  under LTO) and the carried-state front-end question above.


### 15 October, night: the day end to end

Branches (all local, nothing pushed): `dreamcast-native` 0f0eeb5 holds the
last code the console has run (the recipe ELF of 6b9fa5e with ssfar4f; 0f0eeb5
adds only the handoff). Local `next`
(913fa26) = 0f0eeb5 + `car-damage` (85c92b9, 7c54d75) + `rule-9148`
(d068229), host-verified, waiting for the console; `music-handoff`
(worktree `build/dc-native-music.wt`) = `next` + the music recipe step and
this page. Three branches were in progress on top of `next` in their own
worktrees, nothing committed when this was written: `opponent-fix`
(`build/dc-native-opp.wt`), `hud-radar` (`build/dc-native-radar.wt`),
`fx-dust-sparks` (`build/dc-native-fx.wt`). The recipes' default ELFs of
`music-handoff` (= `next` for everything compiled): stats loaded
be1327b6..., controller 2146f339... (`make check` 71 PASS + the 5 known
FAIL lines; heap with assets-ssfar4f 3,579,208 never taken at the peak,
3,641,224 free while racing).

Hardware tools (`build/dc-native-fence-hw.fP9gK9/`): `run_hw.sh RUN ELF
ASSETS` from the repository root (paths relative: it prefixes `$PWD`) takes
the console lock, probes the loader, holds off host sleep (`caffeinate`),
uploads with `-m ASSETS -x ELF`, releases the lock when the program exits;
`VIDEO=1` records the HDMI capture, the avfoundation device chosen by
name (device indices move; about 500 MB a run, off by default since the
disk filled up). Outputs `hw/console-RUN.log`,
`hw/video-RUN.mp4`, `hw/run-RUN.out`. `python3 summary.py RUN...` prints per
run the checkpoint rows that differ from lockstep7's fast-race file
(`ckdiff`; a run with another script compares with its own expected file,
e.g. `diff` as in the READMEs), the late frames of the drop log and render /
registration mean/max for race and podium; `clips.py` makes side-by-side
clips. With the grabber's audio: `build/dc-native-sound.ZcjVhQ/run_hw_sound.sh`
(audio by device name, RMS per second; used on 15 October) and
`build/dc-native-sound-demand.elf/run_hw_audio.sh` (run_hw.sh plus
the capture's audio device; not yet run). Each branch's run is described
in a `build/dc-native-*.README*.txt` (ELF, set, commands, pass condition);
copies of the day's READMEs: `build/dc-native-ssfar4f.recipe/notes/`.

Hardware results of the day, all the scripted fast race (stats ELF), 12,477
ticks, checkpoints identical to the reference in every run listed (late =
drop log; render and registration = race mean, ms; podium = registration):

| run | ELF | set | late | render | registration | podium |
| --- | --- | --- | --- | --- | --- | --- |
| base1/base2 | no LTO, no AICAflow | ssfar4cf3r | 4/4 | 9.51/9.51 | 7.73/7.73 | 10.19/10.19 |
| lto1/lto2 | LTO, no AICAflow | ssfar4cf3r | 1/1 | 9.51/9.51 | 7.56/7.57 | 9.92/9.91 |
| headcf3r | no LTO, AICAflow (7b235b8) | ssfar4cf3r | 12 | 9.51 | 7.89 | 10.46 |
| sndA/sndA2 | the same, AICAflow started (no music/ in the set) | ssfar4cf3r | 10/12 | 9.51/9.51 | 7.89/7.89 | 10.46/10.46 |
| sndB/sndB2 | the same, `NATIVE_SOUND=0` | ssfar4cf3r | 9/9 | 9.51/9.50 | 7.87/7.87 | 10.39/10.40 |
| signsE/signsU | no LTO, AICAflow | ssfar4e/ssfar4eu | 10/12 | 9.59/9.55 | 7.91/7.95 | 10.51/10.52 |
| integrate-eu | LTO, AICAflow (integrate) | ssfar4eu | 4 | 9.56 | 7.66 | 10.24 |
| musicA/musicB | LTO, music on / `NATIVE_SOUND=0` | ssfar4eu + music/ | 7/4 | 9.56/9.56 | 7.65/7.65 | 10.24/10.24 |
| nossfar | LTO, `NATIVE_SUPERSAMPLE_FAR=0` | ssfar4eu | 3 | 9.03 | 7.65 | 10.14 |
| fence-A1/A2 | LTO, three-level fence + sweep | ssfar4eu | 4/4 | 9.56/9.56 | 7.68/7.69 | 10.29/10.29 |
| fence-B1/B2 | LTO, plain fence | ssfar4euf0 | 3/3 | 9.61/9.60 | 7.64/7.64 | 10.15/10.14 |
| ssfar4f | the recipe's stats ELF of 6b9fa5e (loaded 11fa627c...) | ssfar4f | 3 | 9.60 | 7.63 | 10.14 |

- LTO is the default (`NATIVE_LTO=one`, e85eb85/b4d273a): every unit
  `-flto -ffat-lto-objects`, the link `-flto-partition=one`; the gameplay
  units keep `-fno-fast-math -ffp-contract=off` per function. Race
  registration 7.73 -> 7.56 ms (-2.2%), podium 10.19 -> 9.92, late frames
  4 -> 1, checkpoints identical. `NATIVE_LTO=0` (suffix `-nolto`) loads the
  previous default image (bf2cfb83...). `check_build.sh` knows LTO's names
  (71 PASS + the 5 known FAIL lines).
- The layout lesson: linking AICAflow without LTO cost registration 0.14 to
  0.16 ms (7.73 -> 7.87 with the sound never started, 7.89 started) and
  podium 0.20 to 0.27 ms, and 5 to 8 more late frames, although nothing of
  AICAflow runs in the frame with `NATIVE_SOUND=0`: the sound runtime itself
  (AICAflow started, no recording) costs 0.02 ms (sndA against sndB,
  within the noise: 0 ms), the rest is
  where the link put the hot code on the 8 KB direct-mapped I-cache (see the
  update profile: capacity-bound, ~18.7 KB of hot lines). LTO recovers it
  and more: with AICAflow and LTO 7.66 (integrate-eu) against 7.95 without
  LTO on the same set (signsU). Rule: any change of what is linked (a
  library, a new unit) needs a hardware A/B of registration, even when the
  new code does not run; LTO is what makes the layout stable enough.
- Music A/B with the soundtrack playing (sound-demand ELFs, `build/dc-native-sound-demand.README.txt`):
  registration 7.654 vs 7.653 ms in the race, 10.241 vs 10.242 on the
  podium, render 9.559 vs 9.561: music costs the frame nothing. The three
  extra late frames of A are the three track loads that land inside a frame
  (drop log cause `load`): finish (BREAK DOWN) 441 ms / 27 vblanks, podium
  (ROLLING STARTED) 1,046 ms / 62, results (GAME OVER) 520 ms / 30; the race
  piece (03, 1,420 ms) loads in the race-start pause. Audio captured on
  hardware earlier (`build/dc-native-sound.ZcjVhQ/hw/`): attract 50 of 53 s
  louder than -50 dBFS, the stats run plays 03, 07, 11, 15 in order.
- Slot machine 2204fc: done and run on hardware (slotB: three START presses
  stop the reels on "7 7 7", +448 ticks; the console printed `remaining=882`,
  `start-edges=3`, commands 82 / hash 3a1ace0a as the host, checkpoints
  identical; pictures `hw/slot-*.png`, `hw/slotz-*.png`).
- Signs: ssfar4e (16 signs super-sampled beyond 3x their texel distance)
  and ssfar4eu (+ the rest of the sign sheet, no mip chain on a
  super-sampled sign word) run on hardware within 0.04 ms of each other;
  the far-road and sign super-sampling together cost 0.53 ms of render
  (nossfar 9.03 vs 9.56). Then the fence trick was dropped: ssfar4f
  (the plain catch fence as the arcade defines it) is the recommended set:
  render +0.05 ms, registration -0.04/-0.05 ms, podium -0.14 ms, one late
  frame fewer against the three-level fence with sweep; the confirming run
  of the recipe's own ELF (ssfar4f) lands on the plain-fence numbers.
- Wrong way a818: done and run on hardware (wrongwayE: the damage run E
  script, 94 checkpoint rows identical to its own expected file, 3 late
  frames; the blinking "逆走": `hw/ww-*.png`, `hw/wrongway-sheet.png`).
- Car damage (spec `build/dc-native-damage.IXyMGs/spec-damage.md`; branch
  `car-damage`, `build/dc-native-cardamage.README.txt`): the player's 16
  damaged bodies as position/normal variants of the clean body
  (`car-damage.dnb`, 186,080 bytes, in every set; 16 more meshes in car.dnm
  were rejected: +1.15 MB heap), wheel wobble, view rules; render check
  against the arcade's display lists on every race frame of logs E/V; stream
  oracle 0 mismatches with the wrecked body. Contact clear 9148 (branch
  `rule-9148`, `build/dc-native-r9148.README.txt`): +98/+99 cleared while the
  spin angle is outside +-0x1000; damage run C now equal to the original on
  all 4,439 rows (it diverged at row 3581). Both merged into `next`,
  host-verified, waiting for the console: runs damageE and r9148C (commands
  in the two READMEs; damageE needs a set with `car-damage.dnb`, e.g.
  `build/dc-native-cardamage.recipeF/assets-ssfar4f`).
- Effects (spec `build/dc-native-effects.11xv92/spec-effects.md`): five
  effects, four drawn: dust (9500/95c4), tyre smoke (97f8/9888) and sparks
  (9148) of the player's car, and the spin smoke (task 9e2c) of the last car
  to start a spin (the player only until the first opponent spin); skid
  marks build trails but are never drawn. No effect consumes a random
  number; dust, smoke and sparks are functions of fields the port already
  holds bit-exact (34,036 / 34,036 logged draw calls reproduced), the spin
  smoke has 8 particles of its own (bit-exact on 29,448 ticks). Plan
  (section 7): the 9148 clear first (done, `rule-9148`); export the 82
  effect lists (`tools/export_course.hpp`, `prepare_intro.py`, recipe
  `car_packs`: car.dnm/car.dnt rows change); dust and sparks punch-through,
  smoke and spin smoke translucent at alpha 0.5 on a pre-sorted list (the
  model renderer has no translucent list yet); spin smoke stepped once per
  gameplay tick in the race presentation. Measured price 0.66 ms mean, 1.9
  ms p99, 3.2 ms worst render in effect frames; about 5-6 days. Branch
  `fx-dust-sparks` has started on it.
- AICAflow (Daytona is a consumer; repository dfchil/AICAflow): PR #10
  (command queue tail read back only when the queue looks full; no ABI
  change) is merged, origin main is now f7ac62b6, ahead of our pin
  c6b116dd; PR #12 (draft, open then; merged since as f448121, "16 October"): the firmware reports status changes by
  the AICA's main-CPU interrupt, firmware ABI 8, no polling fallback;
  `afx_update` touches no AICA register until news has arrived, new
  `afx_instance_refresh()`; not yet on hardware (needs DKR attract runs,
  Daytona with `afx_update` every frame, and a check that Flycast delivers
  the interrupt, without which `afx_init` fails there). Daytona follow-up
  once #12 is merged: move both pins in `platform/dreamcast-native/scripts/fetch_aicaflow.sh` together
  (a new AICAforge may encode differently: rerun `build_music.sh` and
  update `music-expected.sha256` and the music rows of `expected.sha256`),
  drop the 16-frame polling in `code/audio/sound.c` (`PLAYING_POLL`: call
  `afx_update` every frame) and check `tests/test_sound.c`, rebuild (the
  Makefile always reruns AICAflow's library make, 5ed2153, so firmware and
  library move together), then a hardware A/B as musicA/musicB plus the
  natural end and restart of a looping piece.
- Music in every asset set (this branch): `build_assets.sh --music ARCHIVE`
  runs `build_music.sh` as its first step and copies the 45 AFB/AFX pairs
  into `music/` of every set, so every run of a recipe set plays music
  (`--no-music` for silent sets). AICA RAM: one recording is resident, the
  largest (05 Sky High, 1,818,831 bytes of bank and flow) fits the 2,072,768
  bytes AICAflow reports with about 250 KB to spare; the four race pieces
  fit one at a time, never two (recipes README, "Music").

Open as of that night, roughly in priority order (gameplay first; the
radar, TIME EXTENSION, opponent and effects items have since landed, see
"16 October"):
- Opponent divergences: the damage lockstep compares the player's row only
  (`tests/test_damage_lockstep.py`); the opponents of the damage runs are
  what branch `opponent-fix` is working on.
- Condition radar (CONDITION panel 12c98..13208, spec-damage.md section 5:
  a proximity display, not damage; zoom by START + VR2/3/4): branch
  `hud-radar`.
- TIME EXTENSION ae98 ("TIME EXTENSION!!", `5010d4 := 128` at a lap award,
  the native `award.display`; spec-damage.md section 6): missing.
- Effects (above): branch `fx-dust-sparks`.
- Pit stop (spec-damage.md section 3: entry dc18, `501a84 := 640`, "PIT
  WORK!" ab2c, the crew handlers, the body repair): missing; the native
  computes the entry condition (`route_transition`) and nothing consumes it.
- Podium: the "Congratulations!" screen with the staff photo and the banner
  over the cinematic; the blank ticks before results.
- Cones knocked over (`build/dc-native-world-anim.LwFeBn/spec-cones.md`,
  render only, bit-exact in 6 driven scenarios; each cone needs its own
  PLACED instance).
- Hood and VR views: done on branch `views` (17 October, below); the VR
  buttons also choose the race music (choice 0 now: KING OF SPEED).
- Asynchronous track loading: the finish, podium and results loads stall
  0.4 to 1 s inside a frame (3 late frames a race, above). Two pieces do not
  fit AICA RAM in PCM8, so a preload needs ADPCM banks (`build_music.sh
  --adpcm`, other hashes) or a load that streams in the background.
- `NATIVE_HOT_ORDER` under LTO: the hot-order section file names functions
  of a build without LTO, so the option goes with `NATIVE_LTO=0`; an order
  for LTO's names (or none) is still to be measured.

Specs of the remaining actors, each verified against the arcade's
display-list logs: `build/dc-native-world-anim.LwFeBn/spec-simple.md`
(die, symbol wheel, video screen, pit crews), `spec-leaderboard.md`,
`spec-board.md` (gantry board), `spec-slot.md` (done), `spec-cones.md`;
`build/dc-native-damage.IXyMGs/spec-damage.md` (damage, wrong way, the
CONDITION panel, the other HUD owners, pit stop);
`build/dc-native-effects.11xv92/spec-effects.md` (effects, the 9148 rule).

### 15 October, evening: plain catch fence, assets-ssfar4f recommended, fence sweep off

The catch-fence render trick is dropped: the fence is drawn as the
arcade defines it. The arcade's fence is no texture but 277 quads under one
material (header 0x8000 0 0 0x9b80: Model 2 screen-checker mode, flat dark
colour on every other pixel, at every distance). Every set keeps the minimum
the PVR needs to show that half coverage (`prepare_face_order.py
--stipple-panels`: punch-through quads with a 128x128 PAL4 checkerboard of
the panel's colour and holes, every mip level again a checkerboard, point
sampled: `NATIVE_STIPPLE_FILTER=0`). What went: the fence rule of
`lod_groups.py` (dense stipple -> quarter stipple at 0.5 texel/pixel +
radius -> nothing at 0.12 + radius; 277 three-level groups and the
quarter-stipple texture) and with it the fence sweep, which acts only on
those groups.
- Recipes: `assets-ssfar4f` (`specs/ssfar4f.json` = `specs/ssfar4eu.json`
  without the rule "fence"; built always, `ENJ_ROMDIR` of `make-vars.txt`).
  Only its world.dnm/world.dnt differ from ssfar4eu's: 1290 level-of-detail
  groups (0/1290/0) instead of 1567 (0/1290/277), 7106 faces in ranges, 155
  texture resources instead of 156; world.dnm 1,418,208 bytes (-15,136),
  world.dnt 1,009,344 (-11,008). Byte for byte the world packs of the
  benchmark variant ssfar4euf0 that ran on hardware
  (`build/dc-native-fenceplain.README.txt`). ssfar4eu stays built as the set
  before it.
- `NATIVE_FENCE_SWEEP` default 0 (local.cfg.mk and the C default of
  model_submission.h); `=1` (suffix `-fencesweep`) remains for the older sets
  with a fence rule and loads the previous default image byte for byte
  (8b56b728...). The simulator follows the C default; `check_stream.py
  --fence-sweep` and `build_sim.py ... -DNM_FENCE_SWEEP=1` (mutants 29-31)
  check the option.
- Hardware A/B on the current code (interleaved, 2 runs each, checkpoints
  identical in all), plain fence (no sweep) vs three-level fence + sweep:

  | | plain fence | three-level + sweep |
  | --- | --- | --- |
  | render, race mean (ms) | 9.61 / 9.60 | 9.56 / 9.56 |
  | registration, race mean (ms) | 7.64 / 7.64 | 7.68 / 7.69 |
  | registration, podium (ms) | 10.15 / 10.14 | 10.29 / 10.29 |
  | late frames | 3 / 3 | 4 / 4 |

  The plain fence costs the PVR about 0.05 ms (as on the older base:
  ssfar4cf0 9.61 vs ssfar4cf3 9.56) and saves the SH4 0.04-0.05 ms in the
  race and 0.14 ms on the podium (277 fewer level-of-detail groups walked,
  no sweep); one late frame fewer. The arcade's picture, no switch or
  thinning to see, at no cost to the frame.
- Host evidence (build/dc-native-fenceplain.README2.txt): recipe from the
  ROM 1355 of 1355 rows (1209 unchanged, 146 new ssfar4f rows); `make check`
  of the default stats ELF (LTO) 71 PASS + the 5 known FAIL; heap with
  ssfar4f 3,759,880 never taken at the peak, 3,829,120 free while racing;
  pack checks PASS; stream reference 0 mismatches on the every-tick fast
  (11416) and attract (1801) frames with ssfar4f, sweep off and on (the old
  tick-1395 sweep mismatch is gone with the fence groups); slot, wrong-way
  and damage lockstep ctests PASS, press-free hash 6368fbab. Console:
  "Native level of detail: 1290 groups (0/1290/0 ...)", no "Native fence
  sweep" line.

### 15 October, afternoon: branch integrate, assets-ssfar4eu recommended

Branch `integrate` (local, not pushed; worktree `build/dc-native-integrate.wt`)
= dreamcast-native 7b235b8 + slot-machine (fast-forward) + lto-one (merge;
`NATIVE_LTO=one` default) + signs-union (merge): `tools/lod_groups.py` keys
far materials by the whole material record (before, 15 super-sampled far
faces of ssfar4e took another material's light: 2064 x3, 2070 x6, 3594 x6),
and variant ssfar4eu (`recipes/specs/ssfar4eu.json`, rule "picks": 2084,
3608+3864, 3096, 3224, the leader board's digit pages
3088/3090/3216/3218/3344/3472; mip chains off on every super-sampled sign
word but the video screens 2584/2588/2840). ssfar4eu is now the recommended
set (`ENJ_ROMDIR` of `make-vars.txt`), built always; ssfar4e stays built as
the set before it. `tests/check_build.sh`: the AICAflow check asks the
LTO-aware `linked`/`elf_symbol` (it was skipped under LTO: 70 instead of 71
PASS).

Host evidence (`build/dc-native-integrate.README.txt`): recipe from the ROM
1209 of 1209 rows identical (`build/dc-native-integrate.recipe`); `make
check` of the default stats ELF (LTO) 71 PASS and the 5 known FAIL lines;
slot unit and lockstep tests PASS (remaining equal on every row of A..E,
press-free hash 6368fbab); `test_lod_groups.py` PASS; stream reference 0
mismatches with the fence sweep off on frames regenerated with ssfar4eu
(fast 11416, attract 1801; byte-identical to the slot machine's frames),
fence sweep on: the known tick-1395 mismatch only; no super-sampled far face
of ssfar4eu (1530) or ssfar4e (1426) carries another material's light; heap
with assets-ssfar4eu 3,690,152 never taken at the peak, 3,746,616 free while
racing. Console: "Native level of detail: 1567 groups (0/1290/277 ...)"
(ssfar4e: 1463). Next: the hardware run (README above).

### Night of 14-15 October

Hardware runs, all the scripted fast race (stats ELF), checkpoints identical
to the reference in every run; tools in `build/dc-native-fence-hw.fP9gK9/`
(`run_hw.sh RUN ELF ASSETS`: lock, ready probe, HDMI video, upload, unlock;
it holds off host sleep with `caffeinate`, since one run stalled when the host
slept; `summary.py RUN...`; `clips.py` side-by-side clips), runs and videos in
its `hw/`. Late frames from the drop log; render/registration = race mean.

| run | what | late | render | registration |
| --- | --- | --- | --- | --- |
| 4c | HEAD 9ac963b, ssfar4c (2-level fence + sweep) | 11 | 9.54 | 8.29 |
| 4cf3 | same ELF, ssfar4cf3 (3-level fence) | 14 | 9.56 | 8.39 |
| 4cf0 | same ELF, naive fence (no fence rule) | 19 | 9.61 | 8.34 |
| sc12 | `NATIVE_SMALL_CAR_PX=12`, ssfar4cf3 | 6 | 9.51 | 7.97 |
| g2048 | `NATIVE_GUARD_BAND=2048` | 13 | 9.56 | 8.35 |
| lodmesh | per-chunk LOD decision (rejected, below) | 15 | 9.56 | 8.52 |
| sc12g2048 | both options | 5 | 9.51 | 7.93 |
| 4cf3r | HEAD ELF, ssfar4cf3r (rock decal cores) | 17 | 9.58 | 8.44 |

- Fence (superseded 15 October, evening: the plain fence, ssfar4f, sweep
  off): the three-level fence was chosen after seeing it; the sweep
  (f68f40f) runs on hardware (self-check PASS). The recommended set is now
  ssfar4cf3 (recipes, commit 90a2881). The fence costs little in any form:
  the naive fence adds about 0.05 ms render; the late frames are the same
  places in every run (tree-lined straights at ticks ~1153, ~2295-2416,
  ~4587-4624, podium entry 10202), the fence only tips a few more over.
- Late frames: three fixes tried and then evaluated; (1) and (2)
  are the defaults since 15 October: (1) `NATIVE_SMALL_CAR_PX` (8d6946a): an
  opponent under N rows drawn with the level-1 body, the arcade's level
  untouched; N=12 saves 0.42 ms registration on hardware (model 0.57; N=8 0.45,
  N=6 0.36); pictures: cars of 8-12 rows lose their livery detail
  (`build/dc-native-late-fix.ozTvWz/pics/cmp-4588.png`). (2)
  `NATIVE_GUARD_BAND` (8b14c27): 2048 draws correctly at the ticks with the
  most otherwise clipped triangles, saves 0.04 ms (model 0.13 in late ticks).
  (3) LOD decided per chunk of 8 groups when the chunk's depth range allows:
  levels and stream byte-identical in the simulator, model -0.05 ms, but
  hardware +0.13 ms registration (layout/cache effect, not modelled):
  rejected, patch `build/dc-native-late-fix.ozTvWz/rejected/lodmesh.patch`.
  With (1)+(2) five late frames remain: four at ticks 4590-4612 and the
  podium entry (registration 14-15 ms).
- Rock decals: baking (ssfar4d / ssfar4d-own2) is not recommended: with no
  free bank, the world palettes get worse (texels off by more than 16: 11.6% ->
  15.1% shared, 19.1% own2). The alternative taken is variant ssfar4cf3r
  (7e4f39f; the recommended set since 15 October): Sonic
  (2056) stays punch-through on its opaque rock, solid parts as opaque cores,
  punch-through trimmed to the fringe; the ivy (6424) the same with a hull; no
  texel or palette changed. Simulator: equal to the
  reference, pictures equal but for 0..6 edge pixels; PVR model -4,400
  punch-through pixels a frame where the decals show (25% of frames), up to
  -0.6 ms at tick 8290. Hardware: no measurable render change on average,
  registration +0.05 ms (adopted for the GPU headroom).
- Instrumented simulator on HEAD: `build/dc-native-late-fix.ozTvWz/`
  (`patch_sim_head.py`, the forest analysis's patch tolerant of HEAD;
  `compare.py RUNS BASE VARIANT...` the SH4 model per tick, with
  `clipcost.json`).

15 October, early morning (plan: (1) profile the gameplay update, then (2)
smaller face groups):
- (1) Sample profile of the default build on hardware
  (`build/dc-native-update-profile.YnFqsd/`: `report.txt` whole run,
  `report-race-updater.txt` race-phase updater only, made by filtering the
  sample words' section and phase bits): race updater 3.03 ms a frame,
  registration 8.91 (that build has the 1.92 MB sample buffer). About a
  quarter of the updater is render preparation (HUD enqueue 0.20 ms, bounds
  planes prepare/classify 0.21, car parts, scenery, objects); the rest is the
  game: opponent movement (nm_race_movement_step 14.5%), guided motion 11%,
  force math ~10%, contact ~7%, copies ~6%, finite checks ~4%. PMU on
  hardware (`NATIVE_DIAG_PMU=1`, current tree, race updater per frame):
  602k cycles for 251k instructions, I-cache stalls 181k, D-cache stalls 161k.
  The race updater executes ~600 hot code lines (18.7 KB) and ~1,900 colder
  ones every frame: capacity, not placement, on the 8 KB direct-mapped I-cache
  (no link order can fit it). Gameplay code variants on hardware, all
  checkpoints identical: -O3 (current) 602k cycles, noinline 612k, lto 623k,
  -Os 671k: none helps. What would: less code per opponent (restructuring
  the per-opponent path, bit-exact under the lockstep harness; not started)
  and, on the render side, `nm_bounds_planes_prepare/classify` with SH4ZAM and
  one finite check on the results (~0.1-0.15 ms, not started).
- (2) World face groups of 8 (recipes `--face-group`, default 8): simulator
  reference passes on every frame at 32/16/8/4; SH4 model -0.39/-0.64/-0.88
  ms race mean for 16/8/4; hardware (default ELF, assets-ssfar4cf3r):

  | faces per group | late | race registration | podium registration |
  | --- | --- | --- | --- |
  | 32 | 8 | 7.97 | 10.49 |
  | 16 | 7 | 7.71 | 10.14 |
  | 8 | 4 | 7.57 | 9.99 |
  | 4 | 3 | 7.68 | 10.15 |

  8 is the default; the four world.dnm sha256 in expected.sha256 changed.
- The render statistics' worst frame showed 96 and 105 ms in two of these
  runs with no stray event and no late frame in the drop log: the same
  statistics artefact as below, not tied to rnddone_stray.

15 October, morning: animated world objects (97c94cb; host-verified, NOT yet
run on hardware: the console and the HDMI capture were unreachable). Specs
from the original's handlers, each verified against the display-list logs,
in `build/dc-native-world-anim.LwFeBn/`: spec-simple.md (die 220be4, symbol
wheel 220b14, video screen 22024c, pit crews 220d44), spec-leaderboard.md
(221dd8: the car numbers of positions 1-5, not a time display; nothing near
the gantry shows a time), spec-board.md (gantry board rule, podium GOAL until
the podium car passes the line), spec-slot.md (slot machine 2204fc),
spec-cones.md (cone knock-over). Native state against every logged draw:
`verify_native.py` and `verify_board.py` (all agree); FRM3 frame tables from
`gen/gen_frames.py` (the generator copy with FRM3, live race state in podium
frames). Still missing, in order:
- Slot machine 2204fc: DONE on branch slot-machine (host-verified, not yet
  run on hardware). Gameplay: include/slot_machine.h (220534), race scene
  member + nm_race_scene_slot (5010b0 = 2368 + race tick), START latched
  per gameplay tick into force_recovery (practice.c start_pending; script
  edges bit2), ae2003/ae2002 counted and hashed with the race commands
  after the events (not yet handed to nm_sound_command, which the other
  race commands now reach; no recording plays them); remaining equal to the
  original on every row of A..E (tests/test_slot_lockstep.py, inputs in
  recipes/inputs/inputs-slot-*.txt; ctest native_slot_lockstep needs the
  NATIVE_SLOT_TEST_* paths). Render: kinds SLOT..SLOT+2 (meshpack.h), 64
  reel meshes with Ry baked and three PLACED instances; the attract and
  podium run their own visual machine (nm_intro_scene_t). Every logged
  reel draw agrees (race 834, podium 574, attract 611 frames). The glass
  face in front of the reels (screen-checker) is left out. The world packs
  of every recipe set carry the reels, assets-ssfar4e and the recommended
  assets-ssfar4eu included (the road drape skips world-object meshes;
  expected.sha256 updated). Hardware run: build/dc-native-slot.README.txt.
- Cones knocked over (render only; spec-cones.md: trajectory tables, the
  player car's footprint, two ground re-queries a tick; bit-exact against
  the original in 6 driven scenarios). Each cone then needs its own PLACED
  instance.
- Car effects (tyre smoke, dust, sparks: player's car only), the podium's
  "Congratulations!" screen and banner, the blank ticks before results.
- Audio work was in progress in parallel (code/audio/,
  include/audio/, tools/m2segment/main.cpp, uncommitted, not part of this
  work; since committed: b2c81b9, 1e21ada).

Open from that night:
- (Closed 15 October, evening: the fence sweep is off by default and the
  recommended set has no fence groups. With `NATIVE_FENCE_SWEEP=1` on the
  older sets the known mismatch remains: tick 1395 of the every-tick fast
  frames, a cut vertex's u 1.4e-4 off, an ill-conditioned cut of an edge
  nearly parallel to the plane; the fix would belong in the reference.)
- Render statistics: the worst-frame list shows 34 ms and 119 ms at race tick
  9817 in two runs that also report `rnddone_stray 2`; the drop log has no
  late frame there: a statistics artefact, cause not found.
- The repository root holds an untracked file `1` (per-tick statistics
  columns, probably a mistyped redirect): left in place, not deleted.

### 15 October, day: rubber streaks under the car shadows, super-sampled signs (ssfar4e)

Host-verified only (commits 00cf154, 0a479b7; work, commands and pictures in
`build/dc-native-decal-ss.1MjJoQ/`, `commands.txt`).

- Why 240 layers: prepare_face_order.py gave every road decal (scenery
  classes: a later-stage road-up material over road faces, here the rubber
  streaks 6680 and one yellow stripe 6232) `ROAD_DECAL_LAYER` = 240, one
  global lift for the worst case. The streaks are a mesh of their own: their
  corners float 0..0.106 units above the road triangles the PVR draws (median
  0.05; up is +y, the supplied road normals point down), but 68 of the 501
  long streak quads chord under the road's creases on crests and banked
  curves, by up to 0.177 units; 240 layers (x1.234 written depth) covered
  those dips for every streak. Side effects: streaks drawn through crests up
  to 23% nearer, and the cars' shadow prisms (one unit up and down, road
  depth scale) caught only 16% of the shadow on streaks within 12 units of
  the camera, nothing beyond (simulator, fast race).
- Fix (`prepare_face_order.py --drape-road-decals`, `tools/road_drape.py`):
  the corners of a dipping decal face rise by its dip (271 corners, median
  0.05, at most 0.177; x, z, u, v and every other corner as the arcade has
  them), and road decals lift 1 + the road surface's largest layer = 2
  layers; decal and non-decal faces are not layered against each other.
  Simulator: shadow landed on 99.4% / 98% / 94% / 85% of streak fragments at
  view depth <12 / 12-24 / 24-50 / 50-100 units (before 16% / 1% / 0% / 0%);
  streaks behind the road surface 12 of 4.7 million fragments; streaks drawn
  over road that is truly more than 1% nearer 17 pixels in the whole fast
  race (before 168,150); "decal wins over its coplanar base" unchanged within
  0.003% (the 9 more pixels
  of "base wins although a higher layer lies in the same plane" are cars
  standing on streaks); SH4 model unchanged. Rejected alternatives, kept as
  options: snapping every corner onto the road and cutting the streaks along
  the road creases (exact, but 501 -> 1,039 faces, +0.10 ms SH4), extra
  layers per dipping face (shadow lost again on those faces).
  Layers per road-surface material, before -> after: streaks 6680 240 -> 2,
  yellow stripe 6232 241 -> 2, finish line 6168 1, asphalt 6656 0/1,
  asphalt 7168 / 7680, lane and edge lines 3588/3590/3591, kerb 2988, aprons
  6924/6664 0 (unchanged: their layers come from real coplanar overlaps).
- Signs (rule "signs" of `recipes/specs/ssfar4e.json`, as the gantry board:
  per face the super-sampled far level beyond 3x the one-texel-per-pixel depth
  plus the face's radius; every base board + lettering stack shares its
  switch, checked on all 69 stacked polygons). Picture: `out/signs-sheet.png`.
  Costs from the simulator's fill with the full-road calibration (29.8 ns a
  super-sampled sample, punch-through charged every covered sample):

  | sign | words | faces | texels/unit | switch, units (cars) | far px/frame (fast) | PVR ms fast/tenth/attract |
  | --- | --- | --- | --- | --- | --- | --- |
  | garage banner, green | 2064+2066 | 18 | 24-32 | 29-37 (6.1-7.7) | 65 | .0021/.0021/.0034 |
  | banner "sea gull" | 2068+2070 | 16 | 24-32 | 29-38 (6.1-7.7) | 132 | .0048/.0042/.0013 |
  | banner, script | 3592+3594 | 20 | 24-32 | 29-37 (6.1-7.6) | 95 | .0034/.0034/.0039 |
  | banner, violet | 3596+3598 | 8 | 32 | 29 (6.1) | 18 | .0007/.0006/.0018 |
  | banner "Armadillo" | 3080+3082 | 8 | 32 | 29 (6.1) | 10 | .0005/.0003/.0007 |
  | banner, blue | 3084+3086 | 8 | 32 | 29 (6.1) | 20 | .0008/.0006/.0017 |
  | pit-wall boards | 2072-2075 | 60 | 32 | 29 (5.9) | 47 | .0023/.0013/.0026 |
  | blue board | 2076+2332 | 2 | 17 | 49 (10.2) | 4 | .0001/.0002/.0000 |
  | logo board | 2078+2206 | 4 | 35 | 24 (5.0) | 7 | .0002/.0002/.0021 |
  | casino board "777" | 2082 | 8 | 21 | 39 (8.0) | 35 | .0011/.0011/.0000 |
  | "Pit Road" arrow | 2334 | 1 | 9 | 93 (19.2) | 22 | .0007/.0007/.0001 |
  | rock lettering "RUBAX High Quality OIL" | 2570-2574 | 9 | 28-31 | 30-32 (6.2-6.7) | 134 | .0040/.0042/.0047 |
  | DICE | 2844+2846 | 2 | 9 | 101 (20.7) | 9 | .0011/.0013/.0000 |
  | SMS | 3227 | 1 | 8 | 103 (21.2) | 12 | .0004/.0003/.0001 |
  | rank tags 1ST..5TH | 3349, 3413, 3477 | 5 | 21 | 39 (7.9) | 15 | .0004/.0004/.0000 |
  | video screen (E.B.B) | 2584, 2588, 2840 | 4 | 12 | 76 (15.6) | 29 | .0008/.0008/.0008 |

  Total predicted PVR +0.023 / 0.022 / 0.023 ms a frame (fast / tenth /
  attract), far below the 0.3 ms limit; SH4 model +0.027 ms (32 more
  level-of-detail groups walked a frame, 1,289 -> 1,463 groups). Left out:
  words never visible in the three runs (A&R horse 3096, horse sign 3608,
  number wheels 3352/3100, deck 3092) and the cargo crates' stencils (3986,
  50 faces). Stack bases and plain boards (2072) are included so a stack
  switches as one. Heap: 3,981,720 bytes free while racing (ssfar4cf3r
  4,080,736); simulator reference 0 mismatches at stages 5/6 x
  fast/tenth/attract (FRM3 frames); only world.dnm/world.dnt differ from
  assets-ssfar4cf3r; recipes from the ROM: 771 outputs identical. Default
  ELFs of the recipes against assets-ssfar4e: stats loaded 67deeee3...,
  controller f4756d8d... (`recipe/elf/`).
- Next: a hardware run of the stats ELF with assets-ssfar4e (late frames,
  render), then pictures of the streak shadows and of the sign switch.

Recommended asset set (`ENJ_ROMDIR` at build time, `-m` at upload):
ssfar4f = ssfar4eu with the plain catch fence as the arcade defines it (no
fence rule; "15 October, evening"), and since 16 October with the infield
grass 6924 and the aprons 6664 out of the rule "verges" (not super-sampled). ssfar4eu = ssfar4e with every sign of the sign sheet super-sampled and no
mip chain on a super-sampled sign word ("15 October, afternoon"). ssfar4e =
ssfar4cf3r with the streaks on or above the road and the
super-sampled signs (above). ssfar4cf3r = ssfar4cf3 with the rock decal cores (above). ssfar4cf3
= ssfar4c with the three-level catch fence (dense stipple, quarter stipple,
nothing), made by the recipes below (`assets-ssfar4cf3`; its world.dnm is
byte for byte the hardware-run `build/dc-native-fence-sweep.PrBsVt/assets-ssfar4bf3`'s).
ssfar4c = ssfar4b without mip chains on the forest floor (6428) and the stand
glazing (3858, 7064) (historical copy
`build/dc-native-ss-far.eSWVWs/assets-ssfar4c`). ssfar4b
(`build/dc-native-ss-far.eSWVWs/assets-ssfar4b`, world.dnm sha256 12e3defd...,
world.dnt bd087f6a...) is `build/dc-native-mipmaps.ds44Xc/assets-mip-select`
with a world pack whose road (asphalt, lane and edge lines, finish line,
rubber streaks, verges; not the kerbs) switches to a super-sampled far level
at four times the depth where one texel is one pixel tall (spec
`grazing_texel_pixels` 0.25, camera height 1.85; runs of up to 32 faces of
one mesh, bounds group and material), plus the start/finish gantry board
beyond about 65 units. Console line: "Native level of detail: 1056 groups".
Asset sets are ROM-derived: they live in `build/` and are never committed.
Rebuild from a ROM set, everything the recommended build uses (asset sets
ssfar4f, ssfar4eu, ssfar4e, ssfar4cf3r, ssfar4cf3, ssfar4c and ssfar4b, each
with the soundtrack in `music/`, every generated header, optionally
the default ELFs), byte for byte the delivered files, from the repository
and vgmrips' music archive (see "Sound"):

```sh
platform/dreamcast-native/scripts/fetch_aicaflow.sh   # once
zsh platform/dreamcast-native/recipes/build_assets.sh roms/daytona93.zip build/my-assets \
    --music ../aicaflow-foundation-lab/Daytona_USA_\(Sega_Model_2\).zip --elf [--variants ssfar4cf0,ssfar4d,ssfar4bf3]
```

(`--no-music` instead of `--music` for silent sets.)

About 10 minutes; `platform/dreamcast-native/recipes/README.md` has the dependency
graph, the options and what each recipe file is. The output directory holds
`assets-ssfar4f/` (the recommended set, `ENJ_ROMDIR` of `make-vars.txt`), `assets-ssfar4eu/`, `assets-ssfar4e/`, `assets-ssfar4cf3r/`,
`assets-ssfar4cf3/`, `assets-ssfar4c/`, `assets-ssfar4b/`, with `--variants`
the benchmark `assets-ssfar4cf0/`
(naive fence), `assets-ssfar4d/` (rock decals baked, not recommended) and
`assets-ssfar4bf3/` (ssfar4b + the three-level fence),
`headers/` (all generated header directories), `make-vars.txt` (the make
variables for them),
`elf/{stats,controller}` with `--elf`, and `outputs.sha256`, compared with
`recipes/expected.sha256` at the end. Change a recipe there (specs, option
lists in `build_assets.sh`), not in `build/` scripts, and update
`expected.sha256` when the delivered outputs change on purpose.
The historical work directories remain the evidence: ssfar4b/4c's packs
`build/dc-native-ss-far.eSWVWs/` (`commands.txt`), ssfar4d
`build/dc-native-pt-to-op.fUm2UP/`, ssfar4bf3
`build/dc-native-fence-sweep.PrBsVt/`, HUD pack
`build/dc-native-hud-lean.cUMRe7/pack`. `m2segment --dump-video DIR` (new)
writes the video tables the palette packer reads.

Measured on hardware (stage 6, defaults, scripted fast race, 12,939
vblanks): with far-road super-sampling about 10 late frames, PVR render
9.5 / 17.1 ms race mean / max, SH4 registration about 8.3 ms mean; without
super-sampling 3 late frames. Readings per asset set:
`build/dc-native-ss-far.eSWVWs/hw/results.txt` (ssfar1/2/05/3/4) and, for
the data-cache steps, `build/dc-native-dcache.CdFQJX/hw/results.txt`.
ELFs of the committed tree (assets-ssfar4b, the HUD pack above), with the
verification behind them: `build/dc-native-consolidate.5a9jum/`
(`commands.txt`, `RUNLIST.txt`).

Kept from the data-cache work (`build/dc-native-dcache.CdFQJX/`): PMU group
hooks (`NATIVE_DIAG_PMU=1` prints "PMU-GROUP phase=P group=G ..." per model
renderer loop; `pmu_groups.py` summarises logs), 32-byte alignment of the hot
tables, SH4ZAM block copies (`include/nm_mem.h`: `NM_COPY` for gameplay
values of 64 bytes or more, renderer copies). Hardware: neutral to slightly
positive. The committed tree with `NATIVE_SUPERSAMPLE_FAR=0` loads that work's
"mem" stats image byte for byte (loaded d3c197aceb17ac62).

Tried and rejected:
- Prefetches in the hot loops (data-cache step 4): registration 8.16 ->
  8.46 ms mean, 13.1 -> 14.7 ms max; without the corner-point prefetches
  8.26 / 13.1. Removed entirely.
- Strip rule sets beyond `exact` (`prepare_restrip.py` diagonal, unwrap,
  unwrap-diagonal): they change pixels (simulator PVR model) for a small
  vertex saving; `exact` stays.
- Super-sampling the whole road-surface class (`NATIVE_SUPERSAMPLE_SURFACE=1`,
  about +3.5 ms PVR): measurement option only. Of the far-road switch
  distances four times the base was chosen (ssfar4); nearer switches
  cost more (whole-run render means ssfar05 10.4, ssfar1 9.8, ssfar2 9.2,
  ssfar3 8.8, ssfar4 8.6 ms).
- The first tight HUD (d238013, cores plus punch-through pieces; hardware
  mixed): replaced by the lean HUD (ce6e04b, the pack above).
- 16-bit framebuffer: ruled out (`NATIVE_DIAG_FB565` is a
  measurement option only).

Latest hardware run of the default build (s-final, assets-ssfar4b,
`build/dc-native-consolidate.5a9jum/hw/console-final.log`): 12 late frames of
12,938; race PVR render 9.5 / 15.0 ms mean / max, SH4 registration 8.4 /
16.0 ms; podium registration 10.5 / 17.9 ms. The GPU never exceeds the frame;
the late frames are SH4.

Where the late frames are (late-frame capture, 14 October:
`build/dc-native-late-captures.8sO2Bo/`, pictures in `out-run1/contact.png`;
analysis `build/dc-native-forest-sh4.Gk0uzj/results.txt` and
`pics/contact-forest.png`): all genuinely late race frames are on long
straights lined with trees (ticks about 1155-1178, 2295-2409, 4586-4631) plus
the podium entry (10202). Registration there is 13.1-15.2 ms and the on-time
frames next to them are as heavy: these stretches sit at the limit. Not the
near-level trees as such (denser forest frames elsewhere model at 10.4-12.6 ms)
but four things together: distant opponents (15-25 drawn, mostly 2-34 px
tall; car cost follows submitted vertices, about 165 instructions per TA
vertex, not screen size), tree LOD groups and tree triangles clipped at the
1024 px guard band (70% guard-band only: trees by the road over 1000 px tall),
the long sightline (more cells, the punch-through rail, side tests), and the
LOD walk (about 140 instructions per group, 345 groups). Ranked options
(estimated savings in those frames; not implemented at the time; 1-2
later became defaults): (1) opponents under 12 px at the lowest body level 0.6 ms, under
6 px as a two-triangle sprite 1.2 ms in total (render-only; the drawn-car count
for the opponent logic stays computed the arcade's way); (2) guard band 2048 px
0.3 ms (needs a hardware picture check of large coordinates); (3) tree switch
at 100 px 0.26 ms (popping visible at 100 px on hardware; keep 50); (4) cheaper LOD
walk about 0.2 ms. Frames with two simulation ticks (update 6 ms) need more.
Measurement tools: `NATIVE_DIAG_DROP_LOG=1` (v2, events tied to their scene;
`tools/drop_log_report.py LOG`), `NATIVE_DIAG_LATE_CAPTURE=N[_FULL]`
(`tools/late_capture_report.py`; each copy from VRAM costs about 133 ms, the
frames it delays are tagged `capture` and excluded), `NATIVE_DIAG_PMU=1`.

Decided or answered the night of 14-15 October (see "Night of 14-15
October" above): the three-level fence (ssfar4cf3; since 15 October,
evening the plain fence of ssfar4f); rock decals not baked
(ssfar4d, ssfar4d-own2 degrade the world palettes), cores instead
(ssfar4cf3r, recommended since 15 October); late-frame options (1) and (2)
are defaults.

Open:
- (Closed 15 October, day: rubber streaks lifted 2 layers instead of 240, the
  dipping ones raised onto the road; the shadows fall on them, see "15
  October, day". Hardware run of ssfar4e still to do.)
- Barrier and garage banners are base board + punch-through lettering stacks;
  only baking takes them off the punch-through list, and that needs palette
  banks (none free) or a VQ path. Since ssfar4e both layers are super-sampled
  beyond 3x their texel distance.
- The late-frame capture build is host-verified.
- Picture checks not yet written down: the seam where super-sampling starts
  (moves with the car, hysteresis 1.1), rubber streaks (punch-through tested
  after filtering on the four-sample mean) and their shadows, the signs'
  switch to the super-sampled level, lamp shimmer on the gantry board.
- SH4 registration (about 8.3 ms mean, 13 ms max) is the other half of the
  frame budget; the PMU group hooks say which renderer loop misses.
- The sample-profile measurement build (`NATIVE_SAMPLE_PROFILE=1`) fails the
  running-heap budget at stage 6 with assets-ssfar4b (2,809,864 bytes free
  while the scene runs, budget 3 MiB), as it did with assets-ssfar1; the
  default build passes (4,675,336 bytes never taken, 4,729,872 free).
- Effects (smoke), the podium's "Congratulations!" overlay, disc: not
  started (see the sections below). Sound: music through AICAflow in every
  ELF, heard on hardware since 15 October ("Sound").

## Sound: AICAflow and the soundtrack (15 October)

Music plays in every game ELF and has run on hardware (audio captured; the
music A/B in "15 October, night": no frame cost, three late frames from the
in-frame loads); effects and voices do not exist yet.

Setup and assets:
- `platform/dreamcast-native/scripts/fetch_aicaflow.sh` puts AICAflow `f448121` (origin main, 16
  October; firmware ABI 8) into the ignored `extern/aicaflow`, and through
  AICAflow's own pin AICAforge `e77de852` (origin main; its SDK pin
  `53c464db`). Before 16 October the pin was `c6b116dd` (ABI 7); the move
  changed no asset format: `build_music.sh` gives the same 181 files.
- `platform/dreamcast-native/recipes/build_music.sh ARCHIVE OUTDIR` turns vgmrips'
  Daytona USA pack into 45 AFB/AFX pairs (plus AFC/AFV) with `afx_vgm`,
  validates every AFX and compares with `recipes/music-expected.sha256`
  (15 s, byte-identical runs). The archive (sha256 `ac086fd5...`) is a browser
  download: vgmrips is behind an Anubis proof-of-work check. A copy is in
  `../aicaflow-foundation-lab`. The ELF reads `ASSET_ROOT/music/tNN.afb` and
  `tNN.afx`. Since 15 October, night `build_assets.sh --music ARCHIVE` runs
  it as its first step (`work/music/`, stamped, 16 s) and copies the 45
  pairs into `music/` of every asset set it builds; `--no-music` builds
  silent sets; one of the two is required. `expected.sha256` holds the
  music rows of every set (hashes only).
- The logs hold MultiPCM writes only (no YM3438), and only the music chip
  (the board's second MultiPCM, all effects, is not in them). 8 MHz/180
  versus MAME's 10 MHz/224: about 8 cents flat.

Which command plays what (`include/audio/music_map.h`, proven):
- The original's commands: `build/dc-native-sound-commands.BTQx3Q`
  (`m2segment --trace-sound`, new; attract, the fast race and VR-button
  variants; static enqueue sites). Every music start is `ae10xx`; bank 0x10
  is all playlists on the music track, banks 0x20..0x22 effects and voices.
- `tools/check_music_map.py ROMS.zip ARCHIVE.zip WORK` (4 s): the native
  sequencer with Revision A's sound program (`epr-16720/16721`, in your own
  daytona93 set; daytona93's own `epr-16489/16490` is missing) plays each
  ae1000..ae102f; its note-ons match exactly one recording each (multiset
  1.000, two at 0.999; first 400 in order 1.000; runners-up < 0.52). 45
  commands, 45 recordings; ae1000/0f/1f play nothing; loop flags equal the
  logs' loop offsets (the four race pieces). A swapped map entry fails it.
- When (`include/audio/music_select.h`): title start = original phase 2
  (ae1008 advertisement); title -> race = phases 12..19 (ae1001, be1a10,
  race table[choice]); phase 23 (finish table, after the handler's commands);
  podium phase 27 (podium table + ae211c); results (ae1002, b00700.. on the
  no-podium path, already intents); name entry (ae1005, be1a10, initials
  table, already intents). Choice: VR1 0, VR2 2, VR3 1, VR4 3, else the
  course; the native race has no VR buttons, so 0 (KING OF SPEED, BREAK DOWN,
  ROLLING STARTED).

Runtime (`code/audio/sound.c`, `include/audio/sound.h`):
- KOS sound is wrapped away (`--wrap=snd_init,snd_shutdown`; enDjinn calls
  `snd_init`): KOS's driver and firmware are no longer in the ELF. AICAflow's
  library is built in its checkout, its firmware embedded with `#embed`.
  +31,436 bytes (11,756 firmware); heap budget passes (4,569,800 never taken).
- AICAflow ABI 8: the ARM7 reports status changes by interrupt
  (`ASIC_EVT_SPU_IRQ`, AICAflow's own KOS threaded handler: the port must not
  hook it); AICAflow recycles instances that ended and reports each to the
  end callback from its event thread. No polling: `afx_update` and
  `afx_instance_recycle` no longer exist. `sound.c`'s callback only queues
  the report (a lock-free single-producer ring, as DKR does);
  `nm_sound_update` takes it (repeat or next recording), so a frame that only
  plays makes no AICAflow call and no G2 access; a fade sets the gain each
  frame. `afx_init` returning `-AFX_TIMEOUT` (no boot report by interrupt,
  ~100 ms) leaves the game silent with one `Sound:` line.
- One recording resident; a change stops, waits until AICAflow has freed it,
  loads (synchronous file read), starts. `nm_sound_settle` does that inside the
  title, race and name loading pauses. The finish (BREAK DOWN 409 KB), podium
  (ROLLING STARTED 979 KB) and GAME OVER (505 KB) loads land in a frame:
  measured over `/pc` 441, 1,046 and 520 ms (musicA; one late frame each).
  AICA RAM for assets on hardware: 2,072,768 bytes; the largest piece (05
  Sky High, bank + flow 1,818,831) fits alone, two pieces never.
- `afx_update` runs every frame only while a stop, recycle or start is
  pending, every 16th frame while a recording just plays (`PLAYING_POLL`,
  1e21ada; with AICAflow PR #12's interrupt, merged upstream as f448121, it
  can go: the pin bump is in progress, "16 October"); a fade sets the gain
  every frame. Only music starts without a recording are printed (at most
  8); effects and voices are counted. `make NATIVE_SOUND=0`: AICAflow linked
  but not started (ELF suffix `-nosound`), for A/B runs.
- Fade `be1a<n>`: linear gain ramp over 32 n / 868 s (the original's 32 steps).
  Looping pieces restart from their start (repeats the intro). Not reproduced:
  `b70700` alone (choice 2 without START mutes channel 7 of LET'S GO AWAY).
- `make NATIVE_SOUND_TEST=NN`: recording NN from boot, game commands ignored.
- Host checks in `make check`: `tests/test_sound.c` (fake AICAflow ABI 8:
  boot timeout, start, repeat on an end reported from another thread, no call
  while just playing, error end, fade, stops, reports racing the updater, a
  full report queue, missing files, settle, shutdown, map and tables,
  ASan/UBSan); title, inspect-pause and results tests assert the calls.

Hardware runs (15 October): `build/dc-native-sound.ZcjVhQ/run_hw_sound.sh
RUN ELF ASSETS` records the grabber's audio (by device name) and prints RMS
per second; its `hw/` holds the attract run (title with the advertisement,
one attract: 50 of 53 s louder than -50 dBFS) and the stats run (tracks 03,
07, 11, 15 loaded in order, the wav files). The A/B with and without sound
(musicA/musicB, sndA/sndB) is in "15 October, night".

Open: effects and voices (second MultiPCM; no VGM source: needs the sound
program and PCM ROMs); preloading or ADPCM so the finish/podium/results loads
leave the frames; loop points; VR buttons; the disc path for `music/`; the
AICAflow PR #12 follow-up (merged upstream as f448121; pins, no 16-frame
polling, hardware check: in progress on branch `aicaflow-abi8`, "16 October"). The music itself is not ROM-derived in the recipe: it
comes from the vgmrips logs; rendering it from the ROM set's sound program
and sample ROMs (the native sequencer of `check_music_map.py` plays the
notes, not the MultiPCM register stream) would make the archive
unnecessary.

## Scenery stages (12 October, host-verified only)

The world is on the cars' palette pipeline: `world.dnt` is DNT6 (PAL4 textures
stored once, gain-bucket palettes shared with the cars in banks 2..59: cars
54, world 19 of which 15 are car palettes, 58 of 58 in use), `world.dnm` is
DNM6 with two new header flags (`meshpack.h`): static light baked per face
(front luma in `face.reserved`, back luma of two-sided faces in
`face.padding`; the arcade's world lumas are constants, checked against its
display list) and a scenery class per material (stage, road-surface depth
class). The renderer evaluates no world light and uploads nothing per frame.

`make NATIVE_SCENERY_STAGE=n` (`include/render/scene/track_surface.h`), one
asset directory for all: 0 reduced view, 1 + sky, 2 + opaque walls, stands and
signs, 3 + ground and the grandstand crowd (default 12-14 October: about
1 % of frames late on hardware), 4 + cutouts, 5 + the catch fence (the arcade's
screen-checker panels), 6 + world objects (the default since 14 October, see
"Build and hardware").

State of 12 October, evening (host-verified; hardware has seen none of it):
`build/dc-native-road-filter.H1hcIW/` (`commands.txt`; packs, simulator with
fill counts, ELFs). Earlier packs, frame tables and the first simulator:
`build/dc-native-world-pal4.JmhovB/`.
- Road surface class always point sampled; everything else follows
  `NATIVE_BILINEAR`. The default world pack has no mip chains (`assets-nomip`);
  with a mipmapped pack (`assets-mip`) opaque scenery uses the multiplier 0.25
  (`NM_WORLD_MIP_BIAS`), `NATIVE_MIP_BIAS` overrides. A PAL4 mip chain cannot
  be ignored per polygon: the texture word addresses the 1x1 level and the
  top level lies 4 bytes off the 8-byte address grid, so "no mipmaps" is a
  pack choice.
- No polygon goes to the translucent list. The 277 screen-checker panels are
  a catch fence seen against crowd and sky 50 units and more behind it: none
  has a backing face to bake a shade into. `prepare_face_order.py
  --stipple-panels` turns them into punch-through faces; the stipple texture
  (`prepare_palette_materials.py`) is a checkerboard at every mip level.
- `prepare_face_order.py --flatten-stacks --flatten-limit 2 --composites`:
  the 70 grandstand polygons were four coplanar faces each (seats and three
  punch-through crowd layers); now one opaque face with a 16-colour composite
  texture and palette. 210 faces left the pack. 14 smaller stacks (signs) are
  reported as candidates: each needs a palette bank and all 58 are in use.
- Heap: see "Memory" below (the per-binding header/UV cache no longer exists).

World objects: see "World objects (stage 6)" below. Adjacent world quads do not share UVs, so only 89 offline strips
exist: unwrapping UV periods offline would cut the world's TA vertices by
about a third.

Not verified: a driven race to the finish, any real win, the podium's 2D
overlay, and what the original shows in the ~386 blank ticks before results.

## Renderer after the simplification of 13 October (host-verified only)

- One path: direct store-queue submission of resident DNT6 packs with baked
  world light. Gone: the unbaked world light, normal cones (the group table
  stays in the pack for its bounds), the legacy non-flat strip path, the
  translucent checker list, far-distance rejection, texture streaming and the
  VQ/zlib bank caches in `materials.c`, the eager demand path, and the build
  switches that selected them. A car or world pack that is not DNT6, or a
  world mesh pack without baked light, is refused at load with a message.
  `material_pack.c` still reads DNT1..5: the packers' intermediate packs
  (DNT4 is the input of `prepare_palette_materials.py`) and the transfer
  bench use it.
- The console no longer runs a startup stream oracle. The reference renderer
  is host code, `platform/dreamcast-native/tests/reference_stream.h` (scalar
  transforms, the arcade light, binary-search bindings, the uncached clipper;
  no table or cache of the renderer). `tests/stream_sim/` is the submission
  simulator; with `SIM_ORACLE=1` it compares every submitted triangle of every
  frame with the reference:
  `python3 platform/dreamcast-native/tests/stream_sim/check_stream.py [--course N] ASSETS FRAMES.frm`
  (stages 3, 5 and 6; `tests/stream_sim/gen_frames.py` writes the frame
  tables, see "30 October: the stream simulator"; ctest
  `native_stream_reference` when the cache variables
  `NATIVE_STREAM_TEST_ASSETS` / `NATIVE_STREAM_TEST_FRAMES` are set). Run it
  after every renderer change. The console keeps the SH4 self-checks (FTRV
  matrix and lighting, the assembly vertex writer, header compile parity).
- Statistics (`draw_rear`, strip counts and the like) are host hooks
  (`SQ_COUNT`); the console counts nothing per face or vertex.
- `make NATIVE_ROAD_FILTER=1`: bilinear on the road-surface class too (default
  0, point sampled; `NATIVE_FILTER_ALL=1`, the default since 14 October,
  filters it whatever this says). `make NATIVE_STAGE_TRACE=<race tick>`: diagnostic that
  prints every step of the updater and of the list callbacks from that tick
  until 40 updates into the podium scene.
- Open (13 October): on hardware the stage 3 scripted fast race froze once on
  the last race frame before the podium (`hw/console-s3.log`); the previous
  commit's build passed. The host does not reproduce it: every tick of the
  script, race into podium in one process, is clean in the simulator with the
  reference renderer, in the scene assembly and in the gameplay harness under
  ASan/UBSan. Bisect and trace ELFs are prepared (commands.txt, section 7).
- Commands, digests and ELFs: `build/dc-native-simplify.k44mdZ/commands.txt`.

## Opaque cutout cores (13 October, host-verified only)

Hardware said the trees' whole GPU cost is the punch-through mechanism, so
what is opaque in a cutout texture can now be drawn as opaque polygons:
`tools/opaque_cores.py` designs, per texture, texel fills (holes become an
entry of the texture's own palette; still PAL4, no new bank, no extra VRAM)
and polygons in texture space; `prepare_face_order.py --opaque-cores SPEC`
and `prepare_palette_materials.py --texture-fill SPEC` apply them. Kinds:
`overlay` (punch-through face kept, opaque core a layer above it),
`outline` (face removed, fitted polygon: trees with a polygonal silhouette),
`rects` (face removed, bars and posts as rectangles); plus `unwrap`/`merge`
for materials that stay punch-through. No renderer change: the pieces use
the existing split material and `NM_SCENERY_POINT`.

- What the cutouts are: 3600/2600 pine billboards; 3584/3612 tree line;
  3924 trunks of the big pines (crossed quads, one 9-texel bar); 6684 the
  three-beam rail (39 of 91 faces are reduced-view material); 6680 road
  streaks (dithered decals on the road, 108 faces a frame: they stay
  punch-through, and only 21 more strips form because most shared edges are
  not the pair a quad's fan diagonal lets a strip cross); 6424 ivy on the
  rock wall, stacked on an opaque face.
- Simulator model, stage 5 fast script (control tsp 121k, tripass 4,616):
  trees core only -36% / -41% with +159 world triangles a frame; with the
  whole face kept under the core only -15% / -9%: the fringe is the cost.
  Rails and trunks opaque: -11% / -12%, +144 triangles (the rail needs five
  rectangles a section). Triangle count cannot stay at the current level.
- `tests/stream_sim/check_cutout_sources.py` (also `check_stream.py
  --source`) checks every changed face against its source face from the
  shipped packs: texels covered, colours, plane, layer. The simulator can
  write colour pictures (`SIM_COLOR`).
- Kind `bands` (the requested shape for the pines: opaque body between two inner
  lines, a punch-through strip per side out to the branch tips, end caps,
  trunk rectangle) keeps the ragged outline but costs 12 triangles and 24
  TA vertices a tree at 2 points per line (model: tsp -26%, tripass -1%,
  +580 world triangles a frame); 3 and 4 points exceed the renderer's
  6,144-triangle frame cap. There is no static level of detail: every tree
  face comes within 67 units of the camera somewhere. Tree faces have one
  baked luma for both sides, so the renderer already makes no side test for
  them (`sq_baked_binding`); the simulator prints both counts.
- The punch-through chunk is 512 by default (`NATIVE_PT_CHUNK`, register
  write documented in `code/main.c`).
- Asset sets, ELFs and the numbered run list for hardware:
  `build/dc-native-opaque-cutouts.OokCA8/RUNLIST.txt`.

## Registration time: emitter and prepare rework (13 October, host-verified only)

SH4 registration was the limiter (stage 5: 10.1 ms of the frame). The
submitted stream is unchanged, word for word, in 18 simulator configurations
(assets-nomip, assets-tb2, assets-trees-core5; stages 3 and 5; fast, tenth,
attract), each with the reference check; nothing has run on hardware.

- Production emitter `sq_emit_batches` -> `sq_emit_faces_op/_pt`
  (`model_submission.h`): reads a 32-byte record per face built at load
  (`nm_sq_face_t`: corner offsets, u,v, strip-run length), proves the TA budget
  once per batch (`sq_faces_fit`), keeps the store-queue address and the
  counts in registers. Clipped faces, strip runs and window-reflection faces
  leave the loop through small helpers. `sq_emit_range` /
  `sq_emit_batch_general` remain for the inspection view, fixtures, scenes
  without a world and batches near the TA limit; `SIM_GENERAL_EMITTER=1`
  compares the two.
- `sq_prepare_batch` is four functions (`sq_classify_groups`, `sq_walk_baked`,
  `sq_walk_car` / `sq_walk_lit`, `sq_cache_points_wide` / `_checked`).
- One cached point per position: faces mark position representatives (byte
  marks on the heap), nothing is copied; u,v come from the face record. A
  scene that rewrites vertex UVs must call `nm_render_uv_dynamic` for the
  range (`practice.c` does, before each reflection update).
- Cars walk 32-byte hot face records (`nm_sq_lit_face_t`); a light class
  entry keeps the binding.
- `NM_DRAW_TRIANGLES` is now the byte budget's vertex count (14,336): the TA
  guard (448 KiB of the 512 KiB vertex buffer) is the only limit.
- Y in the frozen view (X): punch-through polygons are drawn through an 8x8
  ARGB1555 checkerboard, opaque ones solid.
- Scripts, patches per step, ELFs per step and the results:
  `build/dc-native-emit.JHtXeB/` (`commands.txt`).

## Level of detail for cutouts (13 October, host-verified only)

- Format: DNM6 flag `NM_PACK_LOD_GROUPS` (`meshpack.h`; count in the flag word's
  high half, per-mesh first/count in `mesh.padding[4]`, a table of 32-byte
  `nm_lod_group_t` behind the strips): one source polygon as up to three
  consecutive face ranges, a centre and two switch depths at reference focus
  280. A range may be empty (draws nothing). `check_native_meshpack` validates
  it; the stage selection (`track_surface.c`) rewrites the ranges.
- Renderer: `sq_walk_baked` walks runs of faces; per group one bounds test, one
  depth of the centre, a state byte (hysteresis 1.1), then only the chosen
  range. A pack without the flag takes the old path: the 18 simulator
  configurations are word for word HEAD's. X then Y tints groups by level.
- Reference: the simulator passes each group's drawn level; the reference
  checks it against its own record and the switch depths and renders that
  range only. Mutants 6..8 of `build_sim.py` are the level-of-detail ones.
- Packer: `tools/lod_groups.py`; `prepare_face_order.py --lod-groups LOD.json`
  and the same file to `prepare_palette_materials.py --texture-fill`. A shape
  whose texel fill differs gets a second PAL4 copy of the texture under the
  same palette (header word 1 bits 8..9). `tests/test_lod_groups.py`.
- Fence: posts, beams and supports are separate opaque reduced-view geometry
  (material 6875), so the far level of a stipple panel is simply nothing.
- Sets L1..L7, ELFs, numbers and the run list: `build/dc-native-lod.70M0gR/RUNLIST.txt`.

## Rich world packs on the fast paths (13 October, host-verified only)

- Cause of the 3 ms that every richer tree set cost on the SH4: the emission
  face table addressed a corner as a 16-bit byte offset (index * 32), so a
  pack with one mesh above 2,048 vertices got no table
  (`sq_emit_faces_prepare`: "Native emission faces: ... general emitter" in
  the console log) and its whole source went through `sq_emit_batch_general`
  and the per-vertex loop `sq_cache_points_checked`. th1, th2, te1, tl1, L3,
  L6 and L7 have 2,050 to 2,230 vertices in their densest cell; nomip, tb2,
  L1, L2, L4, L5 stay below. The TA proof (`sq_faces_fit`) never failed in any
  set (peak 388 KiB of the 448 KiB budget).
- Now a corner is index*8-32768 in a signed word (`sq_corner_of`,
  `sq_corner_point`): 8,192 vertices a mesh, same instruction count per
  corner. The simulator prints the path of every batch (`SIM_PATHS=FILE`,
  "paths per frame" in the summary).
- Also in the production loop: a queued face whose triangles all lie beyond
  one plane is dropped on its four status words (was: loop exit, general face
  function, new proof, re-entry; 47 a frame with nomip, 115 with th2); after a
  clipped face the rest is proven in the loop; a strip run starts only at an
  intact face and writes each vertex from an 8-byte record
  (`nm_sq_strip_corner_t`), 41 instructions a vertex instead of 55 to 100.
- Stream identical to HEAD dd2c89d in 36 simulator configurations; nothing has
  run on hardware. ELFs, scripts, results: `build/dc-native-fastpath.MdhHxq/`
  (`commands.txt`).

## World objects (stage 6, 13 October, host-verified only)

- What they are: not cell data but actors. Program table `236940[course]` has
  256 pointers (one per scenery cell) to lists of 24-byte records (x, y, z,
  heading, id, handler, argument); `2002c` makes a task of each. A handler
  draws 16-byte object lists (`1786c`) when the bit of its own cell (`16e10`
  from its position) is set in the draw bitmap `501520` or the near bitmap
  `501540`, as they stood BEFORE this tick's scenery pass (the previous
  pass's cells, like the car detail pass). Course 0: 242 models, 3,446 faces.
- `make NATIVE_SCENERY_STAGE=6` with a pack that has them (DNM6 flag
  `NM_PACK_WORLD_OBJECTS`, object word in `instance.padding[0]`, material
  stage 5; `meshpack.h`). Drawn: the two sign-tower bodies, the ten cones at
  rest, the gantry board (START/GOAL/CHECK from race state) and the stand's
  roof glazing (64 models by the bearing of the camera entity `501240`).
  Tools: `m2segment --export-native-objects`, `tools/prepare_world_objects.py`,
  `prepare_models.py --world-objects`, `prepare_palette_materials.py
  --add-mesh-keys`. Frame tables for such a pack are FRM2; the reference
  (`tests/reference_stream.h`, `nm_reference_world_objects`) checks the
  selection.
- Not drawn (need runtime state; owners and addresses in
  `platform/dreamcast-native/tools/prepare_world_objects.py`, `HANDLERS`): pit crews,
  slot-machine reels (start button, time bonus: gameplay), leader board,
  the two spinning sign tops, the video board, cone knock-over, tyre smoke,
  dust and sparks. Open: in the podium the arcade shows GOAL for its first
  ticks, the native board CHECK.
- Packs, frames, ELFs, pictures: `build/dc-native-world-objects.ZINSuG/`
  (`commands.txt`).

## Memory (13 October, host-verified only)

- What happened: the stage 6 build ran out of memory while loading
  (`build/dc-native-world-objects.ZINSuG/hw/console-s6.log`). The heap is the
  16 MiB minus the image (1.05 MB at `0x8c010000`) minus the 64 KiB KOS keeps
  below `0x8d000000`: 15.6 MB. The load held the cars' header/UV cache
  (3.6 MB) until its end, and built the cars' 1.8 MB emission records twice:
  KOS's `memalign` does not fit a block into the hole of the same block just
  freed (it asks for 56 bytes more), so the second one grew the heap by its
  full size. Stage 5 sets already ended within 20 to 780 KiB of the limit
  (`Practice heap headroom ... uncommitted=`); stage 6 was 250 KiB over.
- Now (`include/heap.h` states the rules): no header/UV cache on the console
  (`draw_emit_value` compiles each header once, straight into a pool); the
  emission table is 8 bytes a binding (colour, class address) plus one
  32-byte class per distinct header/crop/depth (`nm_sq_class_t`,
  `sq_emit_records_build`); every table is allocated once. The face loop
  loads one word more a face (the class address). The sample profile's
  1.92 MB buffer is allocated before any asset (`nm_sample_reserve`).
- `platform/dreamcast-native/tools/heap_budget.py ASSET_DIR --stage N [--profile]
  [--elf FILE] [--table]` replays a load through a model of KOS's allocator
  and prints the heap map; `--layout head` is the old order, and
  `--hardware-check` compares that with six console logs (heap end within
  3 KiB, the stage 6 failure included). Stage 6 objects set: 8.4 MB taken at
  the peak, 7.2 MB never taken (5.3 MB with the profile buffer); before,
  the load needed 15.8 MB. The Dreamcast `check` target runs it for the
  ELF's own assets and fails below 2 MiB at the peak or 3 MiB while the
  scene runs; ctest `native_heap_budget` (cache variables
  `NATIVE_HEAP_TEST_ASSETS`, `NATIVE_HEAP_TEST_STAGE`) does the same on the host.
  A change of the allocation order in the code must be made in the tool's
  `load_tree` too.
- On the console: an allocation that fails says `FAIL: out of memory: OWNER
  needs N bytes ...` with the heap's state; `Native heap ...` lines after each
  scene load give top, high-water mark and free memory in the tool's terms;
  `make NATIVE_HEAP_TRACE=1` prints every named allocation.
- Largest blocks left, should memory get short again: car point cache
  1.13 MB (sized for all 196 car part instances), car.dnt bindings and
  colours 1.19 MB (the renderer reads them only at load, but the lookup
  functions stay public), car.dnm 1.15 MB, world point cache 0.87 MB.
- ELFs, scripts, heap maps: `build/dc-native-memory.7p4eRz/` (`commands.txt`).

## PVR render time: what the picture consists of (13 October, host-verified only)

- The console's `Practice PVR n=596 ... render avg/max` line covers updater
  intervals 5..600 only: the first ten seconds, the rolling start. Every render
  time quoted so far is that window. `make NATIVE_DIAG_RENDER_STATS=1` adds the
  same reading for the rest of the race and the podium, a histogram and the
  worst frames with their race tick (`DIAG PVR render` lines after the run).
- Measurement builds (`local.cfg.mk`, each names the ELF, none changes the
  default build, whose loaded image is byte for byte commit 88f990b's):
  `NATIVE_DIAG_NO_HUD`, `_NO_SHADOWS=1|volumes|receivers`, `_NO_SKY`,
  `_NO_CARS`, `_NO_WORLD`, `_LISTS=3|2` (unused lists get no object pointer
  blocks; `pvr_list_begin/finish` wrapped at link time because enDjinn opens
  all five and KOS waits for exactly the enabled ones), `_FB565` (measurement
  only: the product keeps 24 bits), `_PAL1555`, `_OPB=16|32`,
  `_ROAD_DEPTH=1.0`, `_FILL_PROBE=pt|tr|presort|add` (two layers of a 256x256
  quad: the price of effects before any exists), beside the older
  `NATIVE_DIAG_CAR_OPAQUE` and `NATIVE_BILINEAR=0`.
- The simulator's PVR work model (`SIM_PVR`) now holds the sky quads, the HUD
  and the shadow volumes (`SIM_SIDE`, `SIM_HUD`; the frame generator writes a
  side file with sky heading/scroll and the HUD quads) and counts, per class,
  opaque tile lines, visible pixels by filter and visible triangle-tile
  entries. Its start-window means explain 38 earlier hardware runs to
  0.12..0.17 ms: render ms = 7.2 + 28.4 ns x pixels textured for the
  punch-through alpha test + 216 ns x punch-through triangle passes.
- What the model says of the base picture (stage 3): the HUD is the largest
  punch-through item (42k to 49k textured pixels a frame, 58 to 71% of all,
  two thirds of them transparent: predicted 1.2 to 1.7 ms); the sky quads are
  36 to 40% of the opaque list's tile lines; shadow volumes add 14 to 25% to
  them; 115k pixels are bilinear PAL4 from an ARGB8888 palette, which KOS's
  pvr_set_pal_format (dc/pvr/pvr_pal.h) warns filters at about half speed. The road-surface class is written
  at half its 1/z, i.e. BEHIND its true depth: it hides nothing.
- Effects (not implemented): in the reference logs only the player's car has
  them (none in the attract or podium log; in the race log they follow the
  player's parts). In that race 21% of frames have one; a smoke frame is
  12 triangles covering 131k pixel samples (max 372k, half the screen, up to 7
  layers), 120k of them textured for the alpha test: +3 to 4 ms as
  punch-through, +8 to 12 ms in the worst frame. What a blended pixel costs,
  pre-sorted or auto-sorted, only the probe runs can say.
- Run list (12 ranked, each with prediction and reading), ELFs, model tables,
  the effect estimate: `build/dc-native-gpu-split.ScloIy/RUNLIST.txt`
  (`commands.txt`, `reports/`).

## HUD geometry (13 October, host-verified only)

- What it was: 59 punch-through quads a frame on average (87 at the end of a
  race), drawn without a depth test; 42k to 50k pixels textured for the alpha
  test, two thirds transparent. The tachometer (183x39 texels, 83% empty),
  the condition frame (80x88, 90%) and the map (55x100, 77%) were 32k of them.
  Hardware: 1.3 to 1.4 ms of render time.
- Now (`include/render/ui/hud_draw.h`, "Tight geometry"; `tools/prepare_hud.py`,
  `tight_pieces`, `--min-gain` 32 texels a rectangle): each sprite is opaque
  cores (rectangles without a transparent texel, on the opaque list) and
  punch-through pieces for the rest, which may run under a core. The table is
  in `hud_data.h`; the sheets and the palette are byte for byte the old ones,
  so asset directories stay valid, but an old `hud_data.h` no longer compiles.
  The loader checks the table against the sheet texels.
- Paint order is depth: rank r at 256+2r, cores one nearer, GEQUAL with write,
  in front of the 3D picture (1/z up to about 100). No HUD header has a shadow
  bit. Frames with the finish actor (phases 23, 24) and `NATIVE_HUD_DEBUG_TEXT`
  builds draw the same rectangles "flat": all punch-through, in paint order,
  no depth test, as before, because those overlays are drawn that way too and
  would paint over the cores.
- Rows: 480/384 = 5/4 puts every fifth output row centre exactly on a texel
  row boundary; the whole quads left that row to the PVR's rounding. Sprites on
  whole arcade pixels are now drawn 1/8 output pixel higher, which makes the
  boundary rows the ones a sampler rounding down takes and keeps cores from
  sampling beyond their edge. Columns: 640/496 leaves 1/62 pixel at least. Not
  seen on hardware yet: look at the HUD's horizontal edges first.
- The markers turn, so they stay one punch-through quad each; the needle is
  one opaque quad. At most 288 rectangles a frame (36 KiB of the 64 KiB TA
  reserve; peak 264): beyond that the sprites that gain least per rectangle
  return to one quad (never happens in the two scripted races).
- `tests/check_hud_tight.c` (ctest `native_hud_tight`, cache variables
  `NATIVE_HUD_TEST_PACK`, `NATIVE_HUD_TEST_ROWS`): every HUD row of both
  scripted races drawn as whole quads, by depth and flat, compared at every
  pixel. The simulator reads either kind of side file (`SIM_HUD_FLAT=1`).
- Model, stages 3 and 6 alike: HUD pixels textured for the alpha test 41.9k ->
  8.3k (start window), 48.9k -> 12.7k (race); predicted -0.9 to -1.0 ms of the
  1.2 to 1.5 ms the HUD cost; flat frames -0.5 ms. SH4: 236 -> about 820
  vertices a frame (peak 1,056), copied as finished 32-byte records.
- ELFs, scripts, model tables: `build/dc-native-hud-tight.OB03Ms/` (`commands.txt`).

## Prebuilt world streams (14 October, host-verified only)

- `NM_SQ_STREAM` (default; `make NATIVE_SQ_STREAM=0` builds the queue path
  alone, ELF suffix `-nostream`, for A/B): a world batch has no face walk and
  no queue. Prepare: group classes and levels of detail as before, then the
  positions of every face of the drawn groups/ranges are marked from position
  lists built at load and transformed; positions only proven faces hold take a
  lean form (FTRV, near test, FSRRA, x/y/q only: 47 SH4 instructions instead of
  about 90; 78% of world positions at stage 6). Each list callback walks the
  list's 96-byte face records (`nm_sq_stream_face_t`: colour, pooled header,
  depth factor with layer and road class, corner words, u/v as written, side
  constants; a mesh's records are in list order, `sq_stream_rank` maps a
  level-of-detail stretch to its records) and writes each drawn face straight
  from its record (`sq_stream_put_face_sh4`, 23 instructions a vertex). Faces of
  groups not proven inside the guard view, strip runs, clipped faces and
  guarded mode go through helpers with the queue path's exact rules.
- Back faces stay a CPU decision (plane constants, as before). Measured
  alternative (simulator probe): PVR winding culling would disagree with the
  arcade's side on about 1.9 triangles a frame at stage 6 (holes and wrong back
  faces: supplied normals vs geometry), add ~655 TA vertices a frame, and save
  no instructions; KOS also leaves FPU_CULL_VAL unprogrammed.
- The stream is the queue path's word for word: 105 simulator configurations
  identical to HEAD d238013 with and without the streams (`SIM_NO_STREAM=1`),
  references pass; stream mutants 9..14 in `build_sim.py` (layer factor, rear
  binding, LOD range, corrupted prebuilt u/v, corner, strip vertex) are caught.
  Console self-checks: the face writer and the lean transform against their C
  forms before the first frame.
- The road class's scale changes with the camera (`sq_road_depth_set`): the
  records of the road faces are rewritten, a mesh the record cache is building
  starts again. `check_stream.py` holds that to a world built at the scale from
  the start, word for word (mutants 41..44).
- Model (SH4 instructions, world only, assets-objL12): about 29% fewer on
  average, 28-36% in the worst frames; stage 6 fast 694k -> 491k a frame.
  Memory: +1.27 MB at stage 6 (5.4 MB never taken; 3.5 MB with the profile).
- Everything, including the hardware run list: `build/dc-native-cell-streams.SMXQl9/commands.txt`.

## Strips (14 October, host-verified only)

- What strips can still give is small. Per frame (simulator, assets-objL12,
  stage 6 fast race) the world writes 4,154 TA vertices for 2,194 triangles
  and the cars 1,738 for 883; the default new strips save 37 world and 68
  car vertices (3.5 KiB of 193 KiB TA input) and, with the assembly run
  writer, about 14,000 SH4 instructions of the ~420,000 the emitters spend
  (stage 3: 6,300 of 270,000); stage 6: 7,700 of the 14,000 are the run
  writer alone. The limits: adjacent world faces rarely share u,v; most
  drawn world faces are in tiny LOD ranges; a run breaks at every culled or
  undrawn face. Static potential (every face drawn) is larger: world -9.1%
  TA vertices exact, -15.8% with diagonals and unwrapped u,v (the world
  objects' roof glazing: 64 models of which one is drawn). Tables:
  `build/dc-native-strips2.31FI1U/out/{static,model}.txt`.
- `tools/prepare_restrip.py IN.dnm IN.dnt OUT.dnm` (post-pass on the pack the
  console loads; module text has the rules): reorders faces inside each
  material/layer/LOD range and chains them; strips may now take either
  winding (all headers have culling off; meshpack.h, validator). Default
  rule set `exact` draws exactly the source's triangles (simulator "set"
  digest equal in every frame). `diagonal` / `unwrap` / `unwrap-diagonal`
  change pixels in the simulator's single-precision PVR model
  (`SIM_PIXEL_FACES`): measurement options only.
- Renderer: a run may start on any triangle of an offline strip
  (`SQ_RUN_EVEN_ONLY=1` restores the old rule; with it the simulator equals
  HEAD in all 105 configurations); stream runs are written by
  `sq_stream_put_run_sh4` (22.75 instructions a vertex in its loop, the face
  writer has 23) from 16-byte strip vertex records that carry the colour;
  console self-check `sq_stream_run_check`. The car path is unchanged: a car
  stream path would save ~45,000 instructions a frame at the world stream's
  costs, light and side tests not counted (out/model.txt).
- New asset set `build/dc-native-strips2.31FI1U/assets-strips` (objL12 with
  world.dnm and car.dnm restripped; heap 5.10 MB never taken, 3.18 MB with the
  profile). ELFs, run list and every command: that directory's `commands.txt`.

## Car streams (14 October, host-verified only)

- `NM_SQ_CAR_STREAM` (default; `make NATIVE_SQ_CAR_STREAM=0` builds the queue path for the cars, ELF suffix
  `-nocarstream`; `NATIVE_SQ_STREAM=0` turns both stream paths off): a car part whose bounds lie inside the guard
  view and nearer than the sky (20.2 of 20.4 car batches a frame) has no face walk, no queue and no vertex marks.
  Prepare: every distinct position of the part, lean x/y/q transform (`sq_car_batch_prepare`). Each list callback
  (`sq_car_list`): a decide pass (`sq_car_decide`: side from the plane constants, one-sided rears dropped, the light
  class entry of the side shown, evaluated on its first use in the callback and resolved once to binding, colour
  and emission class) lists the drawn faces; a write pass (`sq_car_write`) writes them in `sq_emit_face_loop`'s
  order with a new assembly face writer (`sq_car_put_face_sh4`, integer u, v converted as the queue path does,
  31 instructions a vertex); strip runs, faces lit on their own (light dot near zero, undecided side) and window
  reflections (u, v read from the vertex table when drawn) go through helpers with the queue path's rules.
  Other parts (straddling the near plane or the guard band, no plane constants, inspection views, the one
  screen-checker mesh) take the queue path unchanged.
- Records: the queue path's own per-face tables (`nm_sq_lit_face_t`, `nm_sq_face_t`). Records with u, v already
  scaled (the world's form) would cost 0.4 to 1.3 MB; a stage 6 load with the sample profile had 158 KB above the
  3 MB running budget. New tables: 44,910 bytes (`Native car streams: 189 meshes, 13457 faces ...`).
- Stream identical word for word to HEAD 9d644c8 in 114 simulator configurations (the 105 plus 9 on assets-strips),
  each with car and world streams, without car streams (`SIM_NO_CAR_STREAM=1`) and without world streams; every
  reference passes; also with every car list in guarded mode (`SIM_CAR_GUARDED=1`). Mutants 18..22 (class colour,
  culled side, position list, list split, layer factor) are caught; the simulator now also fails when a list
  callback submits a header of another list. Console self-check: "Native SQ car stream face writer PASS"; the same
  check passes in the GDB SH simulator (`sh-elf-run`).
- SH4 instruction model (cars only, measured paths): fast race 357,911 -> 284,830 a frame (-20%), tenth -21%,
  attract -19%; the light class evaluations (371 a frame) stay the largest item. Heap: +41 KB.
- Everything, ELFs and the hardware run list: `build/dc-native-car-stream.7CRbEe/` (`commands.txt`, `RUNLIST.txt`).

## Build and hardware

Every ROM-derived input comes from one command (see "Current state" and
`platform/dreamcast-native/recipes/README.md`); `--elf` also builds the two default
ELFs. Any other ELF, from the repo root, against those outputs:

```sh
A=build/my-assets                                      # build_assets.sh's OUTDIR
source extern/endjinn/environ.sh                      # platform/dreamcast-native/scripts/fetch_endjinn.sh; ENDJINN_ROOT defaults to it
make -C platform/dreamcast-native $(grep -v '^#' $A/make-vars.txt) \
  NATIVE_RESIDENT_CAR=1 NATIVE_FRAME_PROFILE=1 NATIVE_HANDOPT=1 RACE_TICK_LIMIT=0 AUTO_START=1 \
  ENJ_BUILDDIR=$PWD/<new>/obj ENJ_BINDIR=$PWD/<new>/bin -j8 check
# scripted fast race: + NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$A/headers/script-fast NATIVE_RACE_CHECKPOINTS=1
#   (the stats ELF adds NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1)
```

These are the variables of `build/dc-native-consolidate.5a9jum/build_elf.sh`;
built from commit bbf1c2f's tree against the recipe outputs, both default
ELFs load the same images as the delivered ones there. `make-vars.txt` sets
`RACE_ASSET_DIR`, `NATIVE_PODIUM_DATA_DIR`, `NATIVE_RACE_START_DATA_DIR`,
`NATIVE_HUD_DIR`, `NAME_ASSET_DIR`, `NATIVE_RESULT_PAL4_DIR`,
`NATIVE_RESULT_DATA_DIR`, `NATIVE_RESULT_LABEL_DIR`, `NATIVE_RECORD_DATA_DIR`,
`NATIVE_INTENT_DATA_DIR`, `TITLE_SCENE_DIR` and `ENJ_ROMDIR` (assets-ssfar4f).

Defaults since 14 October (project decisions; `platform/dreamcast-native/local.cfg.mk`,
recipe and ELFs in `build/dc-native-filter-defaults.a8dgha/`):
- Scenery stage 6 (`NATIVE_SCENERY_STAGE`, also the C default of
  `track_surface.h`, the `check` target's heap budget and `heap_budget.py`).
- `NATIVE_FILTER_ALL=1`: every 3D texture bilinear (road, punch-through
  cutouts, cutout cores, sky); the HUD and 2D screens stay point sampled.
  The catch-fence stipple (and, in the sets with a fence rule up to
  ssfar4eu, its sparser level-of-detail stipples) stays point sampled
  (`NATIVE_STIPPLE_FILTER=1` filters it: no fence); with ssfar4f the plain
  stipple is drawn at every distance.
- `NATIVE_PT_ALPHA_REF=0x80` (with `NATIVE_FILTER_ALL=1`; `code/main.c`
  writes PT_ALPHA_REF once after `pvr_init`): at the reset value 0xFF a
  filtered cutout loses half a texel of silhouette. `none` leaves the
  register alone (the default of a build without `NATIVE_FILTER_ALL`).
- `NATIVE_PAL16=1555`: ARGB1555 palette RAM (`NATIVE_PAL16=8888` as before).
- `NATIVE_MIP_CLASSES=1`: mip D adjust per class, for textures that have a
  chain: road-surface class and grandstand composites
  `NATIVE_MIP_BIAS_SURFACE` (default 2 = 0.5), the stipple 1.0, everything
  else `NATIVE_MIP_BIAS` (default 4 = 1.0). `NATIVE_MIP_CLASSES=0` is the
  rule before (opaque scenery 0.25, cars and punch-through 1.0,
  `NATIVE_MIP_BIAS` = every opaque polygon).
- Mipmaps come with the asset set, chosen by `ENJ_ROMDIR` at build time (the
  `check` target's heap budget reads it) and by `-m` at upload:
  the recipes' `assets-ssfar4f` is the recommended set (see "Current
  state"; `build/dc-native-ss-far.eSWVWs/assets-ssfar4b` is
  `assets-mip-select` plus the far-road super-sampled levels), `build/dc-native-mipmaps.ds44Xc/assets-mip-select` the same
  without them (chains on trees, signs, objects, cutouts; none on the
  road-surface class or the grandstands),
  `build/dc-native-strips2.31FI1U/assets-strips` the set without mipmaps.
- `NATIVE_FENCE_SWEEP=0` (since 15 October, evening; C default the same):
  no catch-fence sweep. `=1` (ELF suffix `-fencesweep`) clips fence panels at
  depth planes between their levels; it acts only on the fence groups of the
  sets with a fence rule (ssfar4cf3 .. ssfar4eu), ssfar4f has none.
  `tests/stream_sim/check_stream.py --fence-sweep` checks that build's stream.
- `NATIVE_SUPERSAMPLE_FAR=1` (default with `NATIVE_FILTER_ALL=1`): TSP
  super-sample bit on headers marked 0x1000 in word 1 (`tools/lod_groups.py`
  "supersample" rules: the far level of a level-of-detail group); a pack
  without such headers draws as before. `=0`: ELF suffix `-nossfar`.
- Measurement option `NATIVE_SUPERSAMPLE_SURFACE=1`: TSP super-sample bit on
  the whole road-surface class (about 3-4x the texture cost of those pixels).
- Measurement option `NATIVE_DIAG_PMU=1`: SH4 PMU counters per phase and per
  model renderer loop ("PMU-GROUP" lines).
- `NATIVE_LTO=one` (since 15 October): whole-program LTO in one partition;
  `NATIVE_LTO=0` (suffix `-nolto`) builds without it.
- Default since 18 October: `NATIVE_LAYOUT=1` (fixed hot placement, above).
- Not defaults (options): `NATIVE_HOT_ORDER=1` (its section file names a
  build without LTO: use it with `NATIVE_LTO=0`), `NATIVE_GAMEPLAY_OPT=lto`
  (turns whole-program LTO off), `NATIVE_SOUND=0` (silent, AICAflow not
  started, suffix `-nosound`).
- The configuration before 14 October: `NATIVE_SCENERY_STAGE=3
  NATIVE_FILTER_ALL=0 NATIVE_PAL16=8888 NATIVE_MIP_CLASSES=0` (loads the same
  image as commit 41ed653's default build). The ELF name carries every
  option that is on (`-scenery6-filterall-...-pal1555`).

Use fresh object and bin directories when flags change. `NATIVE_PODIUM_TEST=1`
jumps into the podium after 300 race ticks: a presentation smoke test only.

Source `$ENDJINN_ROOT/environ.sh`, check `kos-tool -t CONSOLE_IP:31313
--ready`, then upload with `-m` assets and `-x` ELF (or
`build/dc-native-fence-hw.fP9gK9/run_hw.sh RUN ELF ASSETS`, which does all
of it: "15 October, night"). Runs can be checked unattended: record the HDMI
capture with `ffmpeg -f avfoundation -framerate 60 -video_size 1920x1080
-pixel_format uyvy422 -i "DEVICE:none"`, choosing the device by name, not by
index (indices move), and end a run you started with `reset_dctool_wait` from
`environ.sh`. Only reset your own run, after checking no other `kos-tool` is
running.

## Known source facts worth not re-deriving

- Frame order: `01758` runs the phase handler (`01898`, table at `018cc`),
  then `01838` runs the task table `22f1f0` in row order.
- Phase 27 sets timer 1600 and falls into phase 28; cleanup at timer 384,
  phase 25 when negative. Cinematic tasks run for 1215 ticks.
- The cinematic scene's phase flag is bit 3 in attract and bit 28 in the
  podium; bit 28 selects the third crash directory and suppresses crash sound
  and camera-restore branches.
- A car spawned on the last road profile reads the record after the table for
  one tick; for course 0 those bytes are zero and are exported as such.

## 30 October: the presentation list (branch gaps-2)

Four of six done, on the console on 31 October (below). The pack changed: `hud_data.h` and `hud/` go together, an older
asset set stops at "FAIL HUD startup". The `expected.sha256` rows came from the `hud` step's command by hand (a disk cache
around `tight_pieces`, which gives what the search gives); a whole recipe then gave 2737 identical, 0 different.

- **Rolling start overlay** (`include/hud.h` nm_hud_overlay, a pure state of 501850, 5010b0, 501054, 501854). `ROLL_*`:
  the banner and its car run left 4 pixels a tick through the 1024-pixel ring of the tile layer pair, ACCEL ON! or AUTO
  DRIVE by the pedal, 3 2 1, GO!. The banner's stipple is not art: a translucent panel of 11 rectangles, 25,476 arcade
  pixels (+0.1 ms by the CONDITION panel's measurement, against 1.2 ms for the stipple as punch-through), under
  NATIVE_HUD_RADAR_PANEL. The map stays bright under the panel; in the original the stipple covers it.
  Not reproduced: a pedal crossing 128 inside the 15 on-ticks leaves column 43 of ACCEL ON! until the next erase.
- **Lap message** ad34: `MSG_*`. ae24.. (" PIT WORK" on +204) never runs: e45c's zero is the program's only store to +204.
- **Chord** 129d0: `race.held`, nm_hud_map_apart scales the map and the marker places about the leader matrix's constant;
  markers keep their size (6e-8 / +38 under +38). Pad: START + right, down, left the zoom; START + up + right, up + left,
  up alone the map's size (a D-pad has no VR1 + VR3). `nm_hud_quad_fixed` false no longer means opaque: that was only true
  of sprites of one core, and the scaled map was the first other one.
- **"2" marker** 12b34: 288b5f0, 14 polygons, from `--second-marker 1 TRACE` (new step `trace_hud_lead`: the Expert
  finish leads from frame 11483; the fast race never leads). `MARKER_TOP`: a copy of the player's marker on its sheet.
- **Found on the way, fixed:** every marker on Advanced and Expert was wrong: headings ignored the map model's angle
  234d98 (270 and 174.37 degrees: backwards on Expert) and Expert's depth (size 0.818); up to 17 px. `nm_hud_course_maps`
  now has turn and size; prepare_hud checks the fitted angle against the ROM's.
- **Hot layout.** Sprites from NM_HUD_TIGHT_EARLY on have their own table (`nm_hud_tight` keeps its slot) and are placed
  out of line at the end of nm_hud_place. Do not pass `&list` to a function early in nm_hud_place: the escape drops the
  constant `corners` and costs 5 KB (15,692 bytes against a slot of 10,912). `practice_hud`'s slot is 0x2c0, full. HUD
  banks are 60..63 (the lap font's colours fit nowhere; 60 is free in phases 20..24). VRAM 71,680 -> 139,264 bytes.
- **Budget by the pack's cost model:** rolling start, banner fully on the screen with the hint: +0.8 ms a frame (0.25 of it
  SH4, 98 rectangles), 0.93 with the panel; GO! +0.15; lap message +0.13 (64 ticks a lap). Peak 202 of 288 rectangles.
  The Beginner's late frames (ticks 4589..4615) are no rolling start, but a lap message over them would add to them.

Checks, all against the original: `m2segment ROM 13300 --inputs inputs-finish-fast.txt --trace-hud DIR 250`, `check_hud`
(`tests/check_hud.c`, rules on the reference's variables), `trace_race_native ... --hud ROWS`, `tools/check_hud.py DIR
PACK ROWS [--course N] [--preview-dir]`, `check_hud_tight PACK ROWS`. Fast race 10,200 frames: hint drawn 178, digits 180,
GO! 32, banner 560 with its scroll word, LAP TIME 320, BEST LAP 128; 18 + 2 reference frames by pixel, nothing differs in
the overlay. A run with the six chords (5,500 frames): +34 bit for bit, +52, +38, markers within 0.0165 px. Expert finish
10,306 frames, Advanced 8,447: "2" on 1,516 and 218, markers within 0.0136 px. Tight rectangles 0 pixels different. No
gameplay field moved: nm_race_scene_t has `accel` at its end, for the hint only.

- **Standing-start lamps not done** (501248; needs new models). Camera-space 3D: from 5010a8 = 320 the object falls from
  (0, 6.5, 15.2) to (0, 1.5, 5.2) ahead of the eye, turning +0x800 a tick about its axis (c234); c6a4 draws, at S 1.3, the
  three-lamp bar 288cbf8 (166 polygons) turned about x by +38, and under 192 the board 231ee0[n] below it at y -0.88
  (288cba8..be4, 20 polygons each, digit by n = ((5010a8 - 15) >> 6) + 1). +38 runs 1024 a tick to 231ed0[n]: 71b, c71b,
  871b, 471b for n = 3..0, the bar's grey, red, yellow, green faces. 64 ticks after 0 the object breaks into four falling
  pieces (c2c0, c4b4 with the same two models), gone at 192. The way in is the car pack (`append_player_parts.py`, like
  the crew): car.dnm and car.dnt of every set, a new draw in camera space, and a whole recipe for the world palettes
  that share car.dnt. Its sounds: c594 (9e29xx, ae216e, ae216f).
- **Transmission cars not done** (25eb4). T(23af44[i]) Ry(+5a), 286fab0[i] (359 polygons), 2890550, 286fab8[i], wheels
  7384, the unchosen car under the dim light 502210. The bodies are in the car pack, but gameplay/select_mode.c has no 3D
  scene, and the select's 34 palette banks sit on the cars' 2..59: it needs a camera and light pass, and the car's
  palettes moved or the select's.
- **Open, not on the list:** the tile layers shake on an impact (0x100a000 words 8, 0, -4, 0, 4, -2, 2; b30c..; 42 frames
  of the fast race, the whole HUD a few pixels sideways): not native. A manual car's map marker is 288b62c (13210[1]),
  the export has only the automatic's 288b618. `hud_render.c`'s comment says the CONDITION panel is off by default;
  local.cfg.mk has it on.
- **Console, 31 October (9762da6):** checkpoints 0 differing of 259, 320 and 345; late 3, 0, 2, attract 0 of 8,265; the
  late frames are the old ticks (Beginner 4589..4612, Expert 3535, 3545), none in the rolling start. VRAM 1.28 MB free at
  worst. Pad monkey, schedule 2: 8 games, 3 sites of credit, all courses, 0 DROP. On HDMI: AUTO DRIVE, the banner and its
  panel through the screen's edges, 3 2 1, GO!, BEST LAP with TIME EXTENSION!!, Expert's markers along the course, the
  "2" under the player's. Not looked at on hardware: ACCEL ON! with the pedal up, START + D-pad (needs a pad), the map
  sizes held over a lap.
- **Disc (branch disc-image, merged).** `kos-tool -i` of an ISO with `/pc` mapped to an empty directory: the same 8 games,
  every asset from `/cd/`, the loader's reads outside its slices and the music's worker under slower reads (about 3.3
  times dcload). Without NATIVE_DISC the ELF is byte for byte the recipe's. Not proven: a GD-ROM's seeks, the build
  without the console (dbgio null), IP.BIN and 1ST_READ: a burned CD-R or an ODE.
