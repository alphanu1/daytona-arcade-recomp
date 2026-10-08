# Handoff

## Scenery selection and polygon budget PR branch (2026-10-08)

Branch `scenery-budget` starts from upstream/main `82201ed`, separate from
the staged integration merge. Widescreen at the existing 16:10, 16:9 and
21:9 choices now appends nearby scenery cells omitted by the game's narrow
directional selection while retaining course exclusions and original order.
The default polygon allowance scales with view width and retains the higher
draw-distance allowance; the launcher offers Automatic and a fixed Custom
allowance. Native Default remains at 5,000. The per-board scenery state
prevents linked cabinets sharing a width or custom allowance.

The headless replay tools can exercise 32:9 scenery and budget behavior, while
the launcher remains capped at 21:9. The source commit's larger 32:9 capture
matrix is held for a later panorama PR because the sky boundary needs work.
The upstream frame pacing and mobile input loop were retained while adding
the budget call. The 32:9 headless replay is a development route, not a
launcher choice: a Deluxe '93 frame visibly shows the remaining vertical
sky-art boundary. The panorama PR should address that presentation and then
expose the ratio. Refresh this branch against upstream/main before opening
the PR if upstream has advanced.

Release tests in the isolated checkout: `scenery` and `app_config` CTests
pass. Fresh Deluxe '93 generated code built `m2run.exe` and `daytona.exe`.
Across 600 attract frames, native keeps the 5,000 budget and does not change
the original list; 32:9 uses 13,771 and expands the list on 416 frames. A
3,600-frame Deluxe '93 race uses 9,033 at 21:9 and 13,771 at 32:9, expanding
the list on 2,013 frames in both runs with no budget rejections. A 32:9
Custom 5,000 run retains the expanded list and fixes the allowance at 5,000.

Fresh Revision A generated code built `m2run.exe` and `daytona.exe`,
including all four new cost and budget-observer hooks. In a 3,600-frame
advanced-course run, native keeps 5,000 with no list change; 32:9 uses
13,771 and expands the list on
2,041 frames, raising observed peak object cost from 4,710 to 7,517. The
particular sampled frame still matches pixel-for-pixel, so it is not evidence
of a visible correction there. The original `race_to_end` missing-building
fixture does show the correction at frame 3,600: new selection keeps 25
cells versus 10 with old selection, and the two 32:9 captures differ in
20,483 of 524,544 RGB pixels. Both runs use the same 13,771 budget and have
matching CPU/TGP instruction totals and sound command count, isolating cell
selection as the cause. The local `roms/daytona93.zip` is a split set, so it
cannot be imported alone (it lacks the shared `mpr-16528.10`). The merged
`roms/daytona.zip` contains the complete Deluxe '93 set: the PR branch's
Deluxe '93 importer accepted it, and all ten imported images match the cache
used for recompilation byte-for-byte by SHA-256. Testers can select that
merged archive in the Deluxe '93 launcher. All caches, captures and NVRAM
remain ignored local files. Deluxe '93 `m2gpushot.exe` also built and completed a
3,600-frame Direct3D 12 headless 32:9 race capture: the output is 1366x384,
the allowance is 13,771 and the list expands on 2,013 frames, matching the
software replay's selection metrics. This is a GPU smoke test, not a proof of
software/GPU pixel identity or physical device behavior.

## Mobile upstream rebase and GPU startup diagnostics (2026-10-07)

Rebased mobile onto original alphanu1 upstream/main 0435a46; recovery branch
backup/mobile-before-gpu-20261007. Main/Vita/PSP refs are not modified.
The Xiaomi Mali-G715 report exits normally from SDL_main after GPU creation
fails, not an established native crash. API version alone cannot establish
support; no device feature dump or physical phone is available here.

Shared SDL_GPU creation now uses properties to opt out of unused shader clip
distances, indirect-draw first-instance and anisotropy. Audited game shaders,
samplers and pinned ImGui backend for usage. Depth clamping remains required:
the current pipelines disable depth clipping, so merely disabling that device
feature would be invalid. No shader fork or external dependency edits.
fail() now snapshots the SDL error, logs it and opens an SDL native message
box; dialog failure is logged too. Startup lists compiled GPU backends.
These frontend changes follow the design's Platform layer section.

Mock regression tests cover requested features, property/device failures,
cleanup and error-message preservation even when logging changes SDL's error.
Host app objects and mobile main syntax checks pass; GPU startup, pacing and
TestHold tests pass. Xiaomi runtime compatibility and dialog appearance are
not hardware-verified. Android build initially required matching ANDROID_HOME
and ANDROID_SDK_ROOT; mismatched paths are a local configuration issue.
Android assembleDebug succeeds with both SDK variables pointing to
/home/boucy/Android/Sdk. The arm64 APK passes ZIP integrity and apksigner
verification, contains libmain.so/libSDL3.so, and contains no ROM archives.
Touch-control and mobile ROM-path regression tests also pass.

## Original upstream and platform test-menu integration (2026-10-06)

Merged alphanu1/daytona-arcade-recomp main 1877da9 into the fork, not merely
the fork's origin/main. Recovery branch: backup/main-before-alphanu1-20261006.
Upstream adds the shared TestHold helper and launcher action. Mobile keeps
that shared action with wrapped help text on small displays; Vita exposes
the same one-shot action in its ImGui options, retains physical Test/Service
bindings and does not persist the request. Both ROM versions wait until
frame 240, hold Test for 173 game frames and release, per upstream tests.
Resetting an armed Vita game restarts the full hold; Reset Defaults cancels it.
Inputs and Platform layer sections govern this frontend-only integration.

All 15 targeted tests (13 Vita, pacing and TestHold) pass, as do standalone
touch controls and 13 Android-path plus two desktop ROM-file cases. Desktop
app objects build and mobile launcher/main syntax checks pass with M2_MOBILE.
Vita main_gpu.cpp cross-compiles for both the 1993 and Revision A targets.
Full ROM cross-rebuilding was stopped deliberately in favor of targeted
frontend compilation; no new VPK/APK/IPA or physical device validation is
claimed for this update. Vita and mobile branches are advanced to the
integrated main; PSP untouched.

## Vita and mobile integration into main (2026-10-06)

Rebased psvita-native-frontend and mobile onto fetched origin/main 91560cf.
Mobile's touch controls and iOS file picker were already ancestors of main;
its rebase therefore fast-forwarded without replaying commits. Vita replayed
13 commits, retaining the ImGui launcher, both ROM revisions and platform
settings. The only conflict was this handoff: both upstream licence notes
and platform history were retained. No implementation conflicts occurred.
Recovery refs: backup/vita-before-main-merge-20261006 and
backup/mobile-before-main-merge-20261006. PSP is untouched.

This merge integrates the rebased Vita work into main; mobile is already
included. Publishing uses an explicit lease on the old GitHub Vita tip,
with main and mobile updated by fast-forward in the same atomic push.
Device builds and hardware runtime testing are not repeated by this merge.

Validation: desktop app objects and the selected test targets build; all
13 Vita tests plus app_pacing pass. Standalone mobile touch controls and
Android/desktop ROM-file tests pass. The broad all-target build stopped at
test_fp with unresolved SoftFloat helper symbols in this empty-build-type
configuration; no floating-point implementation changes were made. Mobile
tests are standalone programs/scripts, not named CMake build targets.

## Vita main rebase and ImGui launcher (2026-10-06)

Rebased psvita-native-frontend onto fetched origin/main 50d6638. Recovery
branch backup/vita-before-imgui-20261006 retains the pre-rebase history.
Main and PSP branches are not modified by this work; no Vita push performed.

Replaced the GXM frontend's bitmap menu with pinned upstream Dear ImGui,
rendered through the existing vita2d context. All 28 Vita options, dual-ROM
launching, link settings, physical controls and saved settings remain.
Front touch operates rows, adjustment buttons and the scrolling settings
panel; D-pad selection scrolls into view. UI work runs only in the launcher,
pause menu and loading screen. No additional gameplay display buffering.
The adapter supports the uniform-tint primitives used here, not arbitrary
per-vertex colour gradients. Geometry fringe AA is disabled; font AA remains.

Rebase integration preserves the Vita native-sized backdrop stretch path,
widescreen CPU tile fallback and foreground caches, while adapting the HUD
polygon helper to upstream panel detection. Non-Vita external-renderer
margin handling is retained. Host desktop app compiles; all 13 Vita host
tests pass, including real ImGui draw generation against a mocked backend,
pool exhaustion and GPU shutdown synchronization. These checks do not prove
physical Vita rendering, touch behaviour or gameplay performance.
The Vita ImGui target disables its unused desktop shell-opening handler,
which otherwise links unavailable execvp/waitpid functions from VitaSDK.

Both VitaSDK GPU builds pass. Final dual-ROM package:
build/vita-enhancements/daytona_vita.vpk (01.24, Daytona Recomp ImGui).
ZIP integrity and ARM ELF checks pass; the bundled daytona.self SHA256
matches build/vita-revision-a/eboot.bin. Archive contains executables and
licenses only, no ROM archives. Physical Vita installation remains untested.


## Mobile touch controls and latest main (2026-10-06)

## Vita Revision A link and dual-ROM launcher (2026-10-03)

Rebased psvita-native-frontend with --rebase-merges onto fetched origin/main
9ad266b (three-computer link validation). Recovery branch:
backup/vita-before-link-20261003. Merge conflicts combined enhance and
comm_board in Vita runtime; preserved Vita wide margins and foreground
cache invalidation while accepting upstream scene detection. Main and PSP
branches are unchanged; nothing pushed.

User supplied roms/daytona.zip alongside daytona93.zip. Revision A import
passes CRC validation; its i960/TGP/sound code is separately generated under
build/revision-a-host, never committed. GXM build selects DAYTONA_VITA_ROMSET;
the daytona93 VPK can bundle Revision A's SELF as app0:daytona.self.
Options ROM selector + Start/Reset replaces the process using LoadExec.
Each executable uses its own ux0:data/<set>/<set>.zip, vita.cfg, EEPROM and
backup RAM; existing 1993 saves are not repurposed as Revision A saves.

Vita SceNet IPv4 transport implements the shared TCP ring protocol with
nonblocking connect/accept/read/write, partial-send retention, a 64 KiB
bounded queue, reconnect delay and two-second no-progress timeout. Network
modules/heap are owned only where initialized here. Options expose enable,
four next-IP octets, local/next ports and optional frame sync (off by default).
Reset applies changes; pause status shows local IP, RX/TX and cabinet ID.
1993 link requests are rejected explicitly; Revision A is required on peers.
Master/slave, unique car numbers and matching cabinet/region settings remain
the game's test-menu choices. Native audio now tolerates unsupported effects
as upstream does (linked vibrato omitted), while invalid data still faults.

The host integration uses the real Vita transport through a narrow POSIX
SceNet shim against desktop TcpLink: numbering and data both directions,
37-byte partial sends, stalled-peer loss, reconnect and queue overflow pass.
This is not evidence of Vita Wi-Fi, executable switching or a hardware race.
Those remain the next device checks. CPU500/core options, road renderer,
wide CPU tiles and removal of the buffer selector remain preserved.
Address/undefined sanitizer execution of the socket integration also passes.
Host suite after rebase: 24 tests pass, two optional Lua tests skip.
Both ARM release executables and SELF/VPK builds pass. Final dual package:
build/daytona-vita-dual-rom-link.vpk. Archive checks pass; bundled eboot.bin
and daytona.self match their respective build outputs. The archive contains
only executables, SFO and license documents, no ZIPs or extracted ROM assets.
Build directories: build/vita-enhancements (1993, existing generated input),
build/vita-revision-a (fresh Revision A generation). Both were incrementally
rebuilt after final frontend edits, then bundled in that order.

## Remove configurable Vita display buffering (2026-10-02)

User reports 30 FPS and requests removal of single/double/triple settings.
Removed the menu/config field, physical ring adapter and its obsolete tests.
Old gpu_buffers keys are ignored and disappear on the next settings save.
The same pinned libvita2d now builds without source injection, retaining
upstream's normal three-surface queue and display synchronization. This
removes custom buffering, not the framebuffer storage needed for scanout.

Inspection found no explicit 30 FPS cap: FrameClock retains fractional time
at native 57.524 Hz; the display callback waits one vblank, not two.
Do not claim the reported slowdown is proven caused by buffer count, or
force game logic to 60 Hz. Audio pacing, CPU/core options, renderer and
wide tile fallback are unchanged. Hardware speed remains unverified.
Package: build/daytona-vita-standard-presentation.vpk.
Validation: release cross-build and archive check pass; ELF has upstream
vita2d_swap_buffers and no custom daytona_vita2d symbols. Host CTest:
21 passes and two optional Lua skips. New one-step GXM clock regression
produces 5752 game steps over 6000 simulated 60 Hz display ticks (100 s).
Deleted adapter/test sources remain recoverable in Git history.

## Optional Vita CPU clock and fourth core (2026-10-02)

GXM options now include CPU 500 MHz and a persisted fourth_core switch.
Defaults remain 333 MHz and fourth core off. CPU requests are read back;
rejected/ineffective 500 MHz requests fall back to a 444 MHz request, with
actual frequency shown in options. An overclock plugin/profile may override
application requests; selecting 500 alone is not proof it is active.

Main probes USER_ALL | SYSTEM affinity and falls back to USER_ALL on failure.
Reference sound jobs and both audio callback implementations adopt the shared
mask once per change, with callback caches reset when reopening devices.
No kernel patch/dependency is installed. A working core-unlock plugin is
required. Existing threads can use the additional core; no game-loop
parallelization or audio clock/pacing change is introduced.

Host fallback tests and audio worker/lifecycle tests pass; complete CTest
suite: 22 passes, two optional Lua skips. Vita release cross-build and VPK
archive validation pass. Package: build/daytona-vita-500mhz-fourth-core.vpk.
Actual plugin acceptance, stability and performance require hardware testing.
Wide CPU tile fallback, GPU roads and double-buffer default are unchanged.
Main and PSP remain untouched; no push.

## Wide tile recovery after hardware slowdown (2026-10-02)

User reports moving tile composition onto GXM made the game slower. Disable
the explicit wide GPU tile capability in the Vita frontend: widescreen now
uses the existing CPU backdrop/foreground fallback. Original-aspect GPU tiles,
GPU 3D, the road subdivision change, audio, clocks, physical double-buffer
default and main3044f3b remain unchanged. No additional performance hypothesis
is presented as established; the earlier CPU-only timing excluded GPU cost.

This isolates wide tile composition from the road change, which also can add
vertex work. If this build remains slow, measure/subtract that separately;
do not advertise moving work to GXM as automatically faster. Hardware result
for this comparison build remains pending. Main and PSP untouched; no push.
Package:build/daytona-vita-wide-tile-recovery.vpk.
Validation: VitaSDK build and21 CTest passes (2 optional Lua skips). The
fallback composition and mode transitions are covered by renderer tests.

## Rebase onto GitHub main and GPU tile/road follow-up (2026-10-02)

Fetched origin/main3044f3b and rebased psvita-native-frontend with merge history
retained. Backup:backup/vita-before-main-20261002 at20becae. A preliminary flat
rebase was aborted before continuing with rebase-merges to preserve Vita merge
content. Resolved Video API conflicts by keeping desktop snapshots/instance
tracking and preserving Vita widescreen, plus the renamed HUD copy destination
parameter. Main and PSP branches are untouched; no remote push. The obsolete
Vita frame-skip helper/docs from an old merge were not reinstated; replacement
docs describe the physical GPU buffers (double default).

New main renders tiles on GPU except for per-item HUD relocation, which still
uses CPU foreground composition. Vita now matches that split: GXM composes
centred foreground as well as backdrop, skips CPU pixel composition/uploads,
and retains dirty state for transitions back to the relocated HUD fallback.
CPU tile decode/cache work remains. Main's SDL_GPU shaders are not Vita GXM
binaries; they are retained for desktop, not falsely advertised as a direct
Vita shader port. Audio timing, game cadence and double buffering unchanged.

Main's per-fragment reciprocal-depth UV correction confirms the difference
from Vita's affine tessellation. Add an edge-midpoint texture-error criterion
alongside the existing depth/span minimum subdivision. A synthetic shallow
road triangle goes from46.545 to0.925texels midpoint error, at subdivision8
instead of1. Initial test wrongly expected0.5 at that cap; corrected to check
the measured sub-texel result. The0.5target is not a guaranteed bound, especially
at the eight-way cap or when pool pressure lowers subdivision. This can add
vertex work. No homogeneous matrix or untested shader replacement is used.

Tests exercise scroll-dirty GPU foreground skipping, fallback transitions,
HUD placement, source UV/stretch separation and25440 tessellation cases.
Hardware road appearance, GPU timing and overall speed remain unverified.

Validation: full host build,21 CTest passes with2 optional Lua skips, and
ASan/UBSan renderer contracts pass. The first dirty-transition test omitted
tile_memory_w under write tracking; it now uses the real board notification.
Desktop SDL_GPU Vulkan smoke tests complete120 and1200frames at16:9; the latter
with HUD-edge option enabled. This is not Vita rendering or full-race parity.
VitaSDK build and archive checks pass. Package:
build/daytona-vita-main-gpu-tiles.vpk
SHA256:50dde138308c20680cf6b5a4f9a8ba92abc5c84cb4aa9fb8a4104c90e5a3728b.
Frontend options, controls, audio and physical buffer adapter compare unchanged
against the pre-rebase backup. Source tree is ready for device testing; no push.

## Widescreen GPU backdrop optimization (2026-10-01)

Hardware feedback: all physical buffer choices still slower in widescreen.
Buffer count was not a cure for the extra rendering work. The wide path still
CPU-composed scrolling backgrounds, unlike original aspect's GXM tiles.

Add an explicit GPU-background capability enabled only by Vita's GPU frontend.
Wide screen_update skips CPU background composition/copies; GXM uses the
existing System24 tile upload and composition around unchanged 3D polygons.
Only background destination x changes for stretch, never UVs or 3D projection.
CPU foreground and per-item HUD relocation remain intact. Non-stretched side
margins clear to palette pen0 as in the original GPU path. The old CPU backdrop
path remains when capability is off; normal desktop/PSP behavior is unchanged.
Double GPU buffers remain default; no frame skipping, audio or clock changes.

A synthetic host test (1000 scrolling updates,margin93,empty geometry,constant
character pixels,changing hscroll) measured526.78ms before and238.81ms after
for CPU screen_update, about55percent less. Commands/source are under ignored
build/vita-wide-bench-new.cpp and /tmp/vita-wide-bench.cpp. This excludes GPU
cost and does not establish hardware FPS, visual fidelity or a full-speed game.
Prior homogeneous-WVP failure is not reintroduced: no draw_polygons changes,
shader changes, custom matrix, or HUD algorithm changes.

Host renderer contracts cover margins59/93/200, centred/stretch coordinates,
unchanged UVs and HUD pixels, no CPU backdrop generation, and one CPU layer
draw instead of two. Full host build and21 CTest passes (2 optional Lua skips),
ASan/UBSan renderer contracts, VitaSDK build and VPK archive checks pass.
Package:build/daytona-vita-wide-gpu-background.vpk
SHA256:400df3856a523876c0686cd47a8aac79c7c555b3b0e3d247760fb8e5ed36cb1e.
Hardware rendering and audio checks remain necessary. Main/PSP untouched;
no remote push.


## Vita physical GPU display buffers (2026-10-01)

User requested actual Vita GPU buffering separate from main's software draw
mode, with double as default. Options now persists gpu_buffers1..3; default
and Reset Defaults select2. Old draw_mode remains ignored. No frame skipping,
clock catch-up, audio backend, or polygon-renderer changes are reintroduced.

The pinned public MIT libvita2d source is built privately in the build tree.
display_buffers.inc changes the active GXM surface ring, not board cadence.
Double/triple retain normal display-queue sync; single finishes GPU work and
directly presents the sole surface without queuing identical old/new sync
objects. It may tear during rendering and is not promised faster. Switching
drains GPU and display work, copies the latest frame to surface0 when needed,
waits for scanout, then resets front/back indices. All three allocations stay
alive to allow switching; this does not reclaim framebuffer RAM.

Source archive hash verified. Initial build caught upstream JPEG ceil missing
math.h with the current compiler; target-only forced include resolves it.
All seven pinned shaders' extracted binaries match the installed package
exactly (object metadata hashes differ). No shader compiler used.
The design and dependency records now describe physical buffers rather than
the withdrawn frame-skip options; upstream MIT license is packaged.

Validation: adapter tests cover all mode transitions, VSync on/off, invalid
counts, uninitialized/in-scene/system-app rejection and GPU-before-display
ordering. ASan/UBSan pass;21 CTest passes,2 optional Lua skips. VitaSDK cross-build,
linked buffer-control symbols and VPK archive checks pass. Package:
build/daytona-vita-gpu-buffers.vpk. Hardware switching,
tearing, frame pacing and audio remain unverified. Main/PSP branches untouched.


## Vita pre-draw-mode pacing recovery (2026-10-01)

Hardware feedback after9790e57: game and audio still lag since draw modes.
The synthetic debt test did not establish the cause of native audio stutter.
Back out the Vita draw-mode UI, config loading and presentation gate, and the
follow-up retained-debt clock. main_gpu.cpp is identical to6a6fdc8 apart from
the PACING RECOVERY menu title; controls and input tests match that baseline.
This restores the previous VSync-paced presentation on every frontend loop,
not just when a board frame is due. Saved draw_mode values are ignored and
removed on the next settings save; other preferences are preserved.
Shared main frame-skip support remains merged but defaults to zero and is
not enabled by Vita. Native audio remains device-clocked per the design
document Audio section; neither audio backend nor GPU geometry was changed.
Do not re-propose frame skipping as physical single/double buffering.

Validation: source comparison to6a6fdc8,20 CTest passes,2 optional Lua skips,
VitaSDK cross-build and VPK archive validation. Installable recovery package:
build/daytona-vita-pacing-recovery.vpk. Hardware playback remains unverified;
this is a controlled rollback, not a claim of a measured hardware speedup.
Main and PSP branches untouched. No remote push.


## Vita audio pacing follow-up (2026-10-01)

Hardware report: audio stutters with draw-mode selection. A deterministic
37-second alternating 35 ms / 2 ms host-timing test exposes the one-step
clock discarding whole-frame debt:1114 board/audio steps instead of2128.
Vita GPU frontend now retains at most four frames of debt, still executing
only one board frame per loop. Cheap skipped frames can recover that debt;
no multi-step live-geometry submission or renderer changes are introduced.
Pause/reset clears debt; sustained overload remains bounded. Other frontends
keep the existing FrameClock policy. Audio dispatch stays outside draw gating.

The design document Audio section requires device-clocked native playback.
That backend already has it and is unchanged. This finding explains reference
sample starvation and late main-board commands, not proven native callback
underruns. Do not claim this reproduces or resolves every hardware crackle;
if native music still stutters, capture callback timing and active engine on
the device next rather than increasing latency or changing audio fidelity.

Validation: regression recovers2128 steps, bounded overload/reset tests pass;
20 CTest passes,2 optional Lua skips; public VitaSDK cross-build passes.
Package:build/daytona-vita-audio-pacing.vpk. Hardware listening remains pending.
Main and PSP branches unchanged; no remote push.


## Vita draw modes from updated main (2026-10-01)

Merged origin/main931504e05f4cd471a207c78b275f48e6ce77cc4c into the Vita branch.
Upstream calls frame-skip presets Double Buffered / Single Buffered / Every
Third Frame. They mean draw every1/2/3board frames, NOT physical buffer counts.
The earlier uncommitted buffer-library drafts were moved to ignored
build/paused-buffer-drafts; they are not compiled or shipped. The installed
libvita2d, framebuffer ring and VSync remain unchanged.

Vita Options exposes those three modes, persists draw_mode0..2 (default0),
clamps loaded values and applies on resume. GPU submission/clear/swap is gated
by the same pre-step board frame counter used by M2Board::vblank_end.
No stale live geometry is redrawn on skipped frames: retain the last displayed
frame instead. The clock is capped to one board step per frontend iteration.
Board execution, input and reference/native audio dispatch stay outside the
presentation gate. Menu drawing remains available; zero-step iterations avoid
duplicate scene submission and retain the existing idle delay. SIM FPS label
makes clear the counter measures simulation, not presentation cadence.
The polygon renderer and perspective vertices are unchanged from Wide2.

Validation:20CTest passes,2optional Lua skips. Draw cadence tests sweep invalid
and valid modes over600board frames (600/300/200draws). Public VitaSDK build
and VPK archive checks pass. Three6000frame race_basic headless replays show
identical196665345i960,223429779TGP,12050interrupts,3636sound-command bytes,
and7843660868000instructions. Host times35.18/19.45/14.24seconds are not Vita
FPS. Last-picture hashes differ as expected because skipped modes retain an
earlier frame:ad67233983ea8808 /04b9cd7b8fe2cdda /0504a052c927e9ff.

Artifact: build/daytona-vita-draw-modes.vpk
SHA256c02ffa1702a682062c991cbf569dabafcbe8bd22d055540254fd6281fc8833b1.
Menu title DRAW MODES. LoggingOFF. No hardware result yet.
Main and PSP branches untouched; only the local Vita branch is updated.
Next: compare simulation pace, audio and displayed motion across presets on
real Vita. Lower draw frequency can reduce render cost but also makes motion
less smooth; do not advertise these presets as actual buffer-count changes.


## Wide2: merge GitHub main and Vita options (2026-10-01)

Rebased mobile again onto origin/main 10c85cb before completing touch input.
Kept upstream pacing.cpp and Pacer alongside mobile controller startup, iOS
Files import and screen fixes. Backup: backup/mobile-before-main-touch-20261006;
the touch-work stash remains as an additional recovery copy. No branch pushed.

Shared mobile-only touch overlay: analogue horizontal steering, gas/brake,
sequential gears, four views, coin/start and launcher menu. Raw SDL fingers
capture controls independently (device and finger IDs), not ImGui's single
mouse pointer. Short taps survive until a game frame; focus loss, cancellation,
menu and safe-area changes release input. Existing physical controls merge
with touch through Controls; desktop receives zero touch input by default.
This follows the design's Platform layer & build boundary; runtime/shaders
and upstream pacing remain unchanged. See mobile handoff for validation.

## Mobile rebase onto remote main (2026-10-06)

Rebased mobile onto origin/main 6817bff, preserving Android document imports,
iOS Files import, screen sizing and unsigned packaging. CMake conflict
resolution keeps upstream link/force-feedback sources and Windows socket
libraries alongside mobile SDL targets. Generated-source paths combine
M2_GEN_ROOT with upstream M2_ROMSET. Mobile startup gamepad enumeration now
uses the upstream Devices owner. Initialize the link address before Android's
stale-URI early return. The previous mobile tip is retained at
backup/mobile-before-main-20261006. Main and the other platform branches are
unchanged. Existing IPA files predate this rebase and must not be presented as
rebuilt from it.
Validation: desktop daytona_app compilation passes; 13 Android-path import
tests and 2 desktop-path tests pass with the host SDL shim. Remote main is an
ancestor of the rebased tip. iOS/Android device builds are not rerun here.

## iOS launcher and Files picker (2026-10-06)

Replaced the unsupported SDL iOS dialog with a UIKit document import delegate,
keeping a separate private copy for each selection before the existing ROM
validation. Enabled modern full-screen launch sizing, safe-area placement,
logical-point ImGui styling, text wrapping, a visible scrollbar and blank-space
drag scrolling. Picker errors are shown beside the ROM field. Changes follow
the design's thin platform layer; runtime and shaders are unchanged.

Device Release IPA and simulator build succeed. The simulator cannot provide
visual verification: SDL_CreateGPUDevice reports that its device does not meet
SDL_GPU Metal hardware requirements; the resulting black screen is not UI
validation. Native Files selection and layout need another physical-device
test. See platform/mobile/HANDOFF.md. Do not claim successful picker import or
screen fit based on compilation alone.

## Mobile iOS packaging (2026-10-06)

The iOS build script supports `UNSIGNED=1` for AltStore: disable Xcode
signing, stage the device app under `Payload/daytona.app`, and produce
`build/ios/Daytona-unsigned.ipa`. The bundle template now explicitly supplies
CFBundleExecutable and the APPL package type. Custom build and generated-source
paths are normalized before packaging; argument handling remains compatible
with macOS Bash 3.2. No ROM archives or generated sources are committed.
This follows the design document's Platform layer & build and ROM handling
sections: the shared runtime/renderer is unchanged. See the mobile handoff
for build validation and device-test status.

## Recovery 1 after hardware regression (2026-10-01)

IMG_2856 shows the c3ec891 package losing most textured scene geometry: road
and car surfaces missing, while tiles and some solid/checker geometry remain.
This overrides the host-test success recorded below. The exact GXM failure
mechanism has not been isolated; the host uniform/projection model did not
execute the installed shader and was insufficient to validate the change.

Restored gpu_fast.cpp, perspective_vertices.h, video.cpp/h and corresponding
renderer tests byte-for-byte to1ca8cb3 (git diff --exit-code verified).
Both homogeneous-WVP and wide GPU-tile changes are withdrawn, rather than
shipping another unverified matrix guess. Steering curves, their controls
tests, saved settings and existing Test/Service bindings remain.
Menu identifies DAYTONA RECOMP - RECOVERY 1. Main and PSP untouched.

Public VitaSDK cross-build and VPK archive checks pass. Input tests pass;
restored renderer ASan/UBSan contracts pass, including painter order, checker
parity, wrap/mirror addressing, allocation ownership and shutdown.
Artifact: build/daytona-vita-recovery-1.vpk
SHA256825368c373f9c52dd00329ff16fc9d9e08aed68c0dfa614efe3eed584693e0a5.
Logging remains off. No hardware recovery result has been observed yet.

Road wobble and wide-mode cost remain as in the earlier working renderer.
Next: verify Recovery 1 on hardware before attempting isolated optimisations.
Do not redistribute daytona-vita-perspective-curves.vpk as a working fix;
its host math checks and replay hash did not demonstrate real GXM correctness.


## Vita perspective, widescreen cost and steering curves (2026-10-01)

Work remains on psvita-native-frontend; main and PSP branches untouched.
The Rendering/Enhancements design governs: presentation changes only, native
board timing unchanged, original aspect and linear steering remain defaults.

Road texture warp: previous affine subdivision used1/4/16/64 times as many
triangles depending on depth/span, then reduced precision under pool pressure.
Replaced it with homogeneous positions and the existing libvita2d WVP shader:
clip.w=depth, clip.z=0.5*depth, ordinary UVs. This gives perspective interpolation
with three vertices per source triangle. Painter order, stencil clips, tint,
solid/checker paths and fence ownership are preserved. Matrix changes are scoped
to textured batches and restored on all exits. No custom shader/compiler.
The exported libvita2d matrix is an internal ABI dependency documented with
upstream source revision and installed archive hash in platform/vita/THIRD_PARTY.md.
Recheck it when updating that library. Physical GPU output is not yet verified.

Widescreen had forced every tile layer through CPU composition and full uploads.
GXM now composes background tiles at the selected aspect, and foreground tiles
when centred. Sky margin colour is sampled through the same tile rectangles,
preserving split/window/scroll semantics. Active edge HUD alone uses shared CPU
grouping; unchanged source pixels reuse the grouped layer/upload even when
background scroll dirties drawing state. Pixel comparison, not a hash; extra
HUD source snapshot is about0.74MiB. Further distance still adds geometry.

Options adds persistent Linear/Soft/Extra Soft steering (linear, signed square,
cubic) after deadzone and before inversion. D-pad and full lock are unchanged.
Existing Test/Service mappings remain. Diagnostic logging staysOFF.

Validation:
- Host build and20CTest passes;2optional Lua tests skipped.
-5151 perspective sample comparisons and matrix projection/depth checks.
- Actual renderer submits3vertices for a depth100:1, wide triangle; no subdivision.
- All256stick positions across curves, monotonicity, endpoints/invert/D-pad tests.
- Final standalone renderer contracts pass, including eight sky comparisons
  (four split modes x normal/line scroll) against CPU top-left composition,
  unchanged-HUD upload reuse and matrix restoration.
- ASan/UBSan renderer contracts pass including sky comparison; final later test
  fixture enlarges the long-depth triangle, with the ordinary contracts rerun.
-6000frame16:9/edge-HUD/default-distance host replay:196665345i960,
  223429779TGP,3636sound-command bytes, hash9047513777edfaae, unchanged.
  42.27seconds on this host is not a Vita performance measurement.
- Public VitaSDK cross-build and VPK archive validation pass. Package:
  build/daytona-vita-perspective-curves.vpk
  SHA25666f209118967cb3c0beb55453d54a0f4dfb5b88c1bfda1b09bd683e2656aee87.
  Previous package retained at
  build/vita-enhancements/daytona_vita-before-perspective.vpk.

What not to re-propose: more affine subdivision to hide road warp costs both CPU
and vertex pool and remains approximate. Do not restore blanket CPU wide mode.
The former extreme-UV test rejected a finite polygon only because CPU u*q
overflowed; homogeneous submission keeps it finite, and its expectation was
updated. Host Ninja reported a truncated log and rebuilt fully on a subsequent
invocation; builds were sequential in each directory and completed successfully.
Next: confirm road lines, HUD/sky/clip transitions and frame pacing on real Vita.
No device FPS, crash-free-runtime or visually-fixed claim from these host tests.


## Vita GXM presentation options and cabinet binds (2026-10-01)

Switched to psvita-native-frontend and fast-forwarded to origin/bc02bb0 first.
No PSP files or commits were brought across; main and PSP branches untouched.
Upstream already contained desktop widescreen, per-item race-HUD gating and
draw-distance hooks, but the GXM frontend had none of their options connected.

Added persistent Aspect0..3, HUD edges and Draw Distance-2..2 settings, live
application on resume, scrolling16-row options and unchanged defaults.
Original GXM System24 fast path remains. Wide modes use shared CPU tile/HUD
composition, widened geometry clip planes and matching GXM projection/clip/HUD
offsets. Layer textures reserve896x384 for up to21:9;12MiB layer arena raises
totalGPU reservation30->32MiB. Wider/further options may cost FPS; no physical
performance claim.21:9 fits with vertical letterboxing.

Select+Triangle maps Test and Select+Square Service, consuming coin/view inputs.
Plain Select coin now triggers on release so staggered chords do not insert
coins. Menu latch suppresses a coin on resume; Start+Select remains frontend menu.
Controls regression covers both chords, staggered press and release.

Found existing generated C++ lacked hook_draw_list. Regenerated privately into
build/vita-enhancements-input/gen with seeds/daytona93_hooks.txt; onlychunk012
differs. Vita build links enhance.cpp and uses that generated tree. No generated
game code/assets are committed.

Validation complete: full host build,20CTest passes (2optional Lua skipped),
11build-helper tests and final GPU lifetime/layout/HUD ASan/UBSan pass.
Default and Furthest6000frame host16:9/HUD-edge replays complete:
default196665345i960 instructions, hash9047513777edfaae; furthest202533889,
hashc45f82273dcf08c6. Both223429779TGP instructions and3636sound-command bytes.
These are host replay results, not Vita speed or physical visual validation.

The first cross-build used old hookless generated code and was discarded.
An interrupted retry initially overlapped; both owned build trees' processes
were stopped, then a single build was resumed and completed. A host replay
link first missed the ymfm include path; corrected before the recorded runs.
The build helper now rejects GPU packages with a missing generated hook.

VPK: build/vita-enhancements/daytona_vita.vpk, archive integrity verified.
SHA256:95523616c6226f77293c8f71122f483aa1bbb9bc9973cc2e017483d9ba14da42.
ELF contains hook_draw_list and shared hud_polygon_offset; frontend objects
are newer than the final settings/control/source edits. Logging remains off.
Evidence: build/vita-enhancements-{package,final-tests,asan-final,
race-default,race-furthest}.log. Physical Vita testing remains required,
especially FPS at higher scenery levels and HUD appearance.
Changes are local on psvita-native-frontend; no push to main or PSP.

## Current state

**Force feedback: centring driver override, Linux (issue #16).** A PS2
Driving Force on Linux feels crashes but no centring on bends: the kernel's
Logitech driver offers, as far as known (hid-lg4ff from memory, not checked
here), constant force and autocentre but no spring, so the board's centring
spring is never played (autocentre is off: the game centres the wheel).
rt::DriveBoard::motor(x): the force the board's own closed loop drives for a
wheel at x (springs towards the centre over 8% beyond a 2%/6% dead zone,
uncentring away, the constant force as is, resistance nothing). Controls tab,
Linux only: "Centring driver override" (ffb_centring_override, off); on, the
constant force carries motor(wheel) from the game's own steer value
(io().inputs.steer) and the spring effect carries nothing. main passes it on
Linux only. Also: test_app_controls counted every joystick, so a real wheel
plugged in failed it (#16); it counts on top of what is there. Tests pass;
not tried on a wheel without a spring.

**Hide mouse cursor in game (issue #18).** Launcher, Game tab, next to
Fullscreen: "Hide mouse cursor in game" (hide_cursor, saved, off by default).
One rule in the main loop, every pass: hidden when the option is on, a game
exists and the launcher is closed; shown otherwise, so Esc (or a fault that
opens the launcher) shows it and Resume or Start hides it again, without
touching each place in_launcher changes. While hidden, ImGui's
NoMouseCursorChange is set: its SDL backend shows the cursor on every frame it
draws, and mobile draws its touch controls during play. Tried in the desktop
app (2026-10-07): hidden in play, shown on Esc, hidden again on Resume.

**Hold Test button (the test menu without F2).** rt::TestHold
(src/runtime/test_hold.h): holds the cabinet's Test switch (in0 0x04, active
low) for 173 frames (3 s), from game frame 240 at the earliest, then reports
it is done. Launcher, Game tab, next to Start: "Hold Test button"
(Config::hold_test, not saved; it clears itself when the hold ends); F2 and
any Test binding still work. For mobile, which has no F2 key. Measured first
(m2run, screen hash at frame 2,400): the game wants a fresh press from about
frame 200 after power-on; a press already down then is ignored even if held
to frame 287; a 3 s hold from frame 200, 240, 600 or 1,200 gives the same
screen as a 10-frame press at 1,200 (the test menu; the menu's
Test-selects-an-item does not fire). Revision A the same. test_test_hold. Tried in the desktop app
(2026-10-06): the test menu opens and the box clears.
Left for the Vita frontends' developer: GPU25 has no Test item; the plain
frontend's TEST SWITCH presses for one frame only, which can be missed; both
can use rt::TestHold. Dreamcast has no way into the test menu yet.

**Licence: BSD-3-Clause (LICENSE).** "Copyright (c) 2026, Ben Templeman and
contributors". The same licence as the MAME code transplanted into the
runtime; every linked component is compatible (BSD, zlib, MIT, public
domain). rules.md rule 9, THIRD_PARTY.md and the README say so; the open
decision is gone. GPL code (the MiSTer core, Supermodel) stays read-only.
Docs also brought up to date: the mobile notes folded into Current state,
PR #8's CI, branches now on main, #6 (Android and iOS, with touch, merged),
and the README's platform table: Android, iOS and PS Vita tested on devices
and working (2026-10-06), Dreamcast in Flycast.

**Force feedback: confirmed, enhancements documented (issue #9).** The
reporter confirmed on a Simagic Alpha Mini: no shaking, centring works, no
cut-outs. Documented in docs/issues.md, not started: a safeguard against
force feedback cutting out (SDL's DirectInput update reacquires a lost device
and retries, but its effects stay stopped: restart any effect
SDL_GetHapticEffectStatus reports stopped, about once a second; a theory from
SDL's code, seen in another recomp, not in Daytona), and settings asked for
after FFBPlugin: strength up to 200%, centring spring, resistance, a damper,
minimum force, with defaults that keep today's force feedback.

**Android and iOS (PR #14, platform/mobile).** The desktop runtime and its
SDL3/SDL_GPU renderer packaged for Android (arm64, SDLActivity, the game as
libmain.so) and iOS, from game code generated on a desktop (M2_GEN_ROOT). A
touch overlay on mobile only: analogue horizontal steering, gas and brake,
sequential gears, the four views, coin and start, and the launcher menu.
Fingers are tracked by device and finger ID (not ImGui's one pointer), short
taps last until a game frame, and focus loss, cancellation, the menu and
safe-area changes release them; touch merges with physical controls through
Controls, and the desktop gets no touch input. Android reads the ROM document
through SDL; iOS imports a private copy of each Files selection (a UIKit
document picker), the launcher fits the safe area and scrolls. UNSIGNED=1
platform/mobile/ios/build.sh makes build/ios/Daytona-unsigned.ipa for AltStore.
Runtime and shaders unchanged (the design's Platform layer & build). Checked:
Android assembleDebug and iOS device Release builds, plist lint, the unsigned
IPA; the ROM import and touch unit tests. Tested on devices (2026-10-06):
Android, iOS and PS Vita all play with no issues reported. Not recorded yet:
performance figures. (The iOS simulator cannot be used: it stops at
SDL_CreateGPUDevice, no SDL_GPU Metal there.) Found on an Android device: a
build that saved the picker's content:// URI in the config, reopened at the
next start, crashed on opening; a saved content:// is now cleared and the
archive asked for again (a Browse result is staged into app-private storage
first). If it still crashes at start, take a logcat before changing the import
again. Details: platform/mobile/HANDOFF.md.

**Windows setup: pin Clang discovery to VS 2022.** Both setup scripts now
select the documented VS 2022 toolchain when newer versions are installed
alongside it. CMake receives the matching generator and installation path;
mismatched cached configuration is cleared, preserving generated sources.

Checked: three setup regression tests pass. Revision A built with VS 2022
and Clang 19.1.5; 12 CTest tests passed, two optional Lua tests skipped.
CI now runs the setup tests and builds on branch pushes. Remote CI passed
(Build and Vita compile check; merged as PR #8).

**Frame pacing and fullscreen mode (issue #7).** src/app/pacing.h: Pacing
(Clock, Display, Vrr) chosen from the display's refresh and three settings,
all off (pace_smooth, pace_sync_display, pace_vrr), and Pacer, the per-pass
frame count. Clock is the old loop moved over unchanged (test: identical
frame counts over 20,000 random passes). Display: a frame every N refreshes
(Smooth: a multiple of 57.52 Hz within 1%; Sync: the first division of the
refresh at or under 61 Hz, if 56 or over: 60 on 60/120/180/240 Hz, nothing on
144/165), capped by the wall clock (vsync forced off) and restarted after a
stall. Vrr: one frame per pass, SDL_DelayPrecise to a 1/57.52 s deadline,
vsync kept (not IMMEDIATE as first planned: VRR with vsync and a frame cap
does not tear). Reference audio's stream ratio is scaled by game_hz/57.52 so
Sync to display does not overrun the 240 ms queue; native audio keeps its
device clock. The refresh is read every pass, so pacing follows the window
between displays (seen: 60 and 144 Hz). Fullscreen mode (fullscreen_mode,
"WxH@Hz"): the display's modes in the launcher, applied with
SDL_GetClosestFullscreenDisplayMode/SDL_SetWindowFullscreenMode. Measured
over 15 s in the game: default 57.9, Sync on 60 Hz 59.8, VRR 57.53 frames/s.
Design doc's open question on pacing answered. Not tried on a VRR display;
not listened to with Sync on.

**Legacy Logitech wheel support (issue #4).** The original Driving Force
(046d:c294) is listed but sends nothing through SDL 3.4.16's HIDAPI lg4ff
driver, which reads c294 reports only when they are exactly 27 bytes; the
reporter confirmed it works with SDL_JOYSTICK_HIDAPI_LG4FF=0. A setting, not
a change for everyone (SDL's driver serves the wheels it was written for):
Controls tab, "Legacy Logitech wheel support (Restart Required)",
legacy_logitech_wheels (on by default on Linux, whose kernel driver gives
Logitech wheels force feedback; off on macOS, where SDL's lg4ff haptics are
the only force feedback for them). On, main sets SDL_HINT_JOYSTICK_HIDAPI_LG4FF to
"0" before SDL_Init (SDL reads it when it finds devices; a change while
running is not guaranteed to hand the wheel back to evdev) and logs it.
Hidden on Windows, where SDL leaves lg4ff off already (hid.dll). The wheel's
GUID differs between the drivers, so it is bound again after switching.
Checked: a throwaway --profile with the setting on logs it, runs and saves it;
all CTest tests pass. Not tried with a Logitech wheel here.

**Audio tab: music and effects volumes (issue #10).** Launcher: a new Audio
tab with Volume, Mute, Music, Effects (music_volume, effects_volume: 1 and 1
by default) and the Native audio switch, moved from the Game tab. Reference
audio: at each MultiPCM key-on, SoundBoard::voice_channel reads the owning
channel from the driver's voice pools in 68000 RAM (0xf01500 and 0xf01618, 28
records of 10 bytes; byte 0 nonzero in use, bit 3 the chip; byte 1 the slot
code; byte 3 the channel; byte 6 0xff on the engine layers' reserved records,
skipped: they carry chip 0's bit with chip 1's fixed slot codes, which gave
1,597 false double matches in a race before they were skipped). Music is
channels 0-9 and 15 (NativeSoundSequencer::music_channel, the driver's "stop
music" set); a key-on with no record (the engine's fixed slots 18-27 on chip
1) is an effect. MultiPcm sums music and effect slots apart and scales them
before its 16-bit clamp; at 1 and 1 the sums are added as before. Native
audio: VoiceEvent::music, NativeSampleMixer::set_volumes per voice, set from
the audio callback through atomics.
Checked: class against the native sequencer's channel, note for note
(scratch program on the oracle's pairing): daytona93 race 4,899/4,899, attract
2,274/2,274; daytona race 4,892/4,892, attract 2,095/2,095; one voice record
per key-on. race_basic reference WAV byte-identical before and after (20 MB);
screen hash ad67233983ea8808 and instruction counts unchanged. Music alone +
effects alone = the full mix except 0.097% of samples, where a chip clips in
the full mix. Race RMS: effects 0.195, music 0.059 (10 dB). New tests:
multipcm_volumes, native_sample_mixer's volumes case; all CTest tests pass.
Not listened to on this machine.

**Force feedback: Daytona's drive board commands (issue #9).** The command
meanings taken from Supermodel's later drive boards were wrong for Daytona:
the game's centring spring (0x39-0x3C, on all race) was played as a 60 ms
sine, so wheels shook constantly with no centre pull, and pads buzzed. Checked
against the drive board's own program, EPR-16488A: disassembled in MAME
0.289, then run with Lua taps on its I/O (commands fed on port $27, wheel
positions on its ADC at $80, motor power and direction read from $46).
rt::DriveBoard now follows it: motor on/off (0x0-), one effect at a time with
strength in the low 3 bits (0x2- resistance, 0x30-37 centring spring, 0x38-3F
the same with a dead zone, 0x4- uncentring, 0x5-/0x6- constant force), no
force for 0x1- and the x8-xF halves, queries (0x8- up) changing nothing; no
vibration (the board has none). app::ForceFeedback: spring (negative for
uncentring), friction and constant force; pads rumble only for the constant
force and uncentring; a wheel whose haptics fail to open gets nothing instead
of SDL's DirectInput rumble (a sine through the wheel) and is retried every
3 s; failed effect updates are retried; pausing keeps the board's state
(ffb.reset() on a new game). Launcher: "Log force feedback" (ffb_log, off).
Checked: test_app_controls and all CTest tests pass. Not checked on a real
wheel; which of 0x5-/0x6- is left on a cabinet is not known (Invert force).
Details and the command table: docs/issues.md.

**Open issues reviewed (docs/issues.md).** Each open GitHub issue checked
against the code: #4 (Driving Force on Linux: SDL 3.4.16's lg4ff driver drops
input reports that are not 27 bytes; ask for a run with
SDL_JOYSTICK_HIDAPI_LG4FF=0), #7 (frame pacing; four display settings
planned, all off, so the default stays native speed, 57.52 frames/s), #9
(force feedback; the drive board's commands checked against its program),
#10 (reference audio's mix matches MAME; native audio's master gain 1.95 and
missing sample envelopes) and #6.

**Dreamcast port (in progress; on main since PR #11).** Now about 32 frames/s in
Flycast (reported 2026-10-06; the work behind it is not on main yet, so the
measurements below stop at the older figures). platform/dreamcast: a
KallistiOS build run by platform/dreamcast/build_dreamcast.py after the
desktop build, from the `daytona` (Revision A) set only, like platform/vita.
Runtime changes are behind M2_DC_MEMORY (ROM read through a page cache the
frontend supplies, texture and frame buffer RAM supplied, a two-level page
table, no GPU-layer copies, lazy rasterizer buffers, reused Lockstep
callback slots, geo_test's cursor moved on without its loop), M2_DC_SPIN_SKIP
(the game's frame-wait loop at 0x1394 skipped by whole passes to the next
lockstep event: 79% of the i960's instructions in a race) and M2_DC_SPEED
(the same output, faster: tile rows with nothing to draw skipped, geometrizer
object data parsed only for frames that are shown, frame skip 3 allowed, tile
layers composed in the PVR's 16-bit formats);
the desktop defines none of them. Checked after the changes: build-daytona m2run
race_basic, screen hash 9427a612c5cb7511, 909,312,001 i960 and 195,261,176
TGP instructions, as before. In Flycast a whole recorded race (race_basic) runs,
drawn by the PVR with textures and both tile layers, in lockstep with the
desktop at all 100 checkpoints; 25 frames/s with every 2nd frame drawn, the default, with sound (175.9 s for the race; 29 frames/s, 207 s, with every 4th; 1,414 s at the start); with the controller it runs until Flycast is closed. The geometrizer's transforms by the SH-4's ftrv (M2_DC_NATIVE_GEO, picture only; --no-native-geo turns it off): race 240.8 -> 237.0 s in Flycast, lockstep exact. Tile layers composed again only on the lines that changed (M2_DC_SPEED, Video::compose16; dcmemcheck's layer hashes identical): 237.0 -> 228.5 s. The race's scenery layer (pixmap layer 2) drawn by the PVR from a video RAM texture at its scroll instead of composed every frame (M2_DC_SPEED; dcmemcheck rebuilds and compares it): 228.5 -> 218.2 s. The geometrizer's ROM reads in blocks (GeoPtr::read, M2_DC_SPEED; dcmemcheck identical): 218.2 -> 212.8 s. Two remembered ROM pages per region and an FPU finiteness test in the renderer: 212.8 -> 205.3 s. The i960's and the TGP's instruction counts kept in registers by fast_gen.py (dcfastcheck identical every frame): 205.3 -> 195.1 s. Renderer radix sort and colour cache: 195.1 -> 193.4 s. No library memcpy/memset for kept polygons and register frames (M2_DC_SPEED): 193.4 -> 187.5 s. Tile block compare without memcmp: 187.5 -> 186.5 s. Culled polygons skip their texture reads and lighting (M2_DC_SPEED): 186.5 -> 180.1 s. Two-level entry switch and binary chunk search in the i960 code's dispatch (fast_gen.py): 180.1 -> 176.9 s. The runtime reads count + Lockstep::pending during a call, so the generated code stores its count once instead of adding it (M2_DC_SPEED): 176.6 -> 175.9 s, 0.21 MB of RAM back. The play build prints no TRACE lines and reports every 1,800 frames (the serial port is slow on a console). Found for the desktop too, not changed
there: Lockstep::calls_ never shrinks (a callback every 1,024 instructions;
estimated 270 KB a second on the desktop). KOS 2.2.1 in extern/kos-dc
(git-ignored): DreamSDK R4's installed KOS master stops every C++ program
using libstdc++'s exceptions at startup. Details, measurements and failures:
platform/dreamcast/HANDOFF.md.

**Vita build: the ROM set.** The 1994 set's M2_ROMSET broke the Vita compile
check (its CMake builds the runtime itself, without the define);
platform/vita/CMakeLists.txt defines M2_ROMSET="daytona93", and
rom_import.h falls back to daytona93 when a build does not say. Checked on
the link-play branch's CI: Vita compile check passes (with link play's
comm_board added to the Vita runtime there).

**Link play (on main).** Revision A's communication board
(837-10537), from MAME's m2comm simulation: src/runtime/comm_board.{h,cpp}
(the protocol: shared RAM set-up at cn_w, the master's 0xff/0xfe numbering
tokens round the ring, every frame each cabinet's 0xe00-byte block from
shared 0x2000 to the next, landing at 0x21c0, and the master's 0xfc vsync;
frame sync optional, off by default as in MAME). M2Board::set_link attaches
it to a LinkTransport; with none (the default, and daytona93) the comm
registers stay plain, so nothing changes. src/app/link_socket.cpp: TCP, this
cabinet listens, connects to the next (retried until it answers), non-
blocking reads, whole-frame sends with a 2 s limit, POSIX or Winsock. App:
launcher Game tab "Link play" (on/off, port, next host:port, frame sync,
status line); daytona --profile NAME for a second cabinet on one computer;
m2run --link-listen/--link-next/--link-sync and --save-nvram. Master and
slave are the game's own settings (test mode > GAME SYSTEM: LINK ID, CAR
NUMBER; factory: MASTER, car 1, twin). Checked: tests/test_comm_board.cpp
(two boards in memory: numbering, data both ways, frames in pieces, loss);
two headless m2run cabinets on localhost (master: factory EEPROM; slave: one
set to LINK ID SLAVE, CAR NUMBER 2 through test mode by script): the
linked attract (通信システム 2人まで対戦できます), both through the linked
course and transmission select, a race with POSITION /2 on both, red car
(car 1) and blue car (car 2); "cabinet 1 of 2" / "2 of 2"; the link lost
when the other cabinet quits. Not checked: two computers, Wi-Fi, frame
sync, more than three cabinets.
First two-computer try (Mac 192.168.1.52 slave, port 15112, next
192.168.1.2:15113; the other the master): NETWORK CHECKING, because the
other computer was not listening on 15113 (connection refused: its port was
still the default 15112). The launcher's waiting status now says which half
of the ring is missing (next cabinet reached or not; a cabinet connected to
this port or not), and the help says every computer can use the same port.
Then linked on two computers once the cabinets' settings matched (the game
cancels the link, "CANCELLED", when they differ: one was DELUXE/USA, the
other TWIN/JPN). Native audio then stopped the game: linked play's music
turns on the MultiPCM LFO (MIDI controller 0x01, value 1, channels 0 and 5;
found with the new m2run --native-audio-check on a headless linked pair,
frames ~1,510), which the native mixer does not have, and the app treated
any unsupported effect as a fault. Now it plays without the vibrato and
says so once in the log; invalid data and callback failures still stop it.
Open: the LFO in the native mixer (the driver's controller 0x01 handler to
MultiPCM registers 6/7, MAME's multipcm LFO).
Tested by the user: three computers linked (Windows, macOS and Linux), all
played well.

**The seed_scan seeds checked against MAME (daytona93).** A local MAME
build (scripts/build_mame.sh) and scripts/m2_check.sh on every scenario: the
11 in scripts/inputs and two new ones, attract_long (9,000 frames of
attract) and race_to_end (race_basic played on to 20,000 frames: out of
time, game over and the screens after). All match MAME in every check:
native i960 code (every device event and interrupt), the native
geometrizer, the CPU 3D layer, the composed screen, the recompiled TGP and
the sound 68000. race_to_end alone: 2.16 billion i960 instructions, 19,997
frames, 20.2 million polygons, 597 million TGP and 261 million 68000
instructions identical. Coverage (MAME's indirect-branch log against the
seed list): the 11 old scenarios reach all 334 harvest seeds and none of the
248 seed_scan seeds (they are the harvest's own scenarios); attract_long and
race_to_end reach 81 of them (10 of 19 game modes, 13 of 29 task states, 3 of
85 jump-table entries, 5 of 16 lda tables, 48 of 92 ROM-record handlers, 2 of
7 data-ROM pointers), now checked. The other 167 need states no scenario
reaches yet (other endings, name entry, link play). Found on the way:
- An Apple silicon MAME differed from ours in the geometrizer's last bit
  (attract frame 173): clang fuses a*b+c into FMA on arm64 by default;
  build_mame.sh now builds it with -ffp-contract=off, like our code.
- build_mame.sh picks a Python whose XML parser loads (Homebrew's Python
  3.14 here had a pyexpat built against a newer libexpat: "No parsers
  found"); setup.sh --build-mame on macOS installs sdl3 and pkgconf (MAME's
  macOS front end is SDL 3, found through pkg-config), not sdl2.
- The first m2_check run of race_to_end stopped at 6,897 frames (cause not
  found: MAME's output goes to /dev/null there); run again by hand, MAME
  recorded all 19,997 frames and every check matched.

**Setup builds the 1994 set too.** scripts/setup.py builds every set it
finds in roms/: daytona93 into build/ (as before), daytona (Revision A) into
build-daytona/ (configured as the main build, with -DM2_ROMSET), and prints
how to start each. The rejection and no-ROM messages name both sets.
getting-started.md and the README describe both, including Revision A's
first run (a single cabinet set in test mode). Checked: setup.py from a
configured tree with both sets in roms/: both games built, tests pass.

**Widescreen: scene or 2D screen, by window and coverage.** The margins'
fill (stretched or sky behind a 3D scene, each row's edge colours on a 2D
screen) was chosen by "the 3D covers half the screen". Attract and race
camera shots that look at a lot of sky cover 36-49% (measured: daytona93
attract frames 720-780, 6720-6860, 7020-7140, 8320-8460; Revision A the
same kind), so those frames smeared their sky's edge colours sideways and
then snapped to stretched when the camera moved. Video::scene(): one window
and at least 15% coverage. Measured over attract and race_basic on both
sets: daytona93's car and circuit select draw their 3D in 2-3 windows,
Revision A's select screens have no 3D, titles none; every one-window frame
with 15-49% is a scene. Checked: daytona93 attract frame 760 now stretched,
circuit select still edge colours (16:10, stretch on).

**Wheels, pedals and force feedback.** Controls gained a third binding
column, Wheel / joystick: every SDL joystick is opened (app::Devices), and
an action binds to an axis, button or hat direction of a device by GUID, so a
wheel, pedals and a shifter can be separate devices. An axis is calibrated
when bound: the capture records where it rested and how far it was moved
before being let go (pedals resting at either end or short of full range; a
900-degree wheel's chosen lock). Saved as `<action>.joy=` lines, with
`joy_deadzone`. Force feedback: the game writes the drive board's command to
I/O board dual-port RAM byte 0x11 (found by logging its writes: 44 in a
race, every type), which IoBoard queues; rt::DriveBoard decodes them (the
meanings below were wrong for Daytona and are replaced: see "Force feedback:
Daytona's drive board commands" above. Then: the
command set of Sega's later drive boards, as Supermodel documents it:
0x1- centring, 0x2- friction, 0x3- vibration, 0x5-/0x6- pull right/left, 0xc-
reset; 0x0-/0x4- sequences and 0x7- not modelled) and app::ForceFeedback
plays them on the steering device: SDL haptics (spring, friction, sine,
constant force on the steering axis; only changed levels sent) or rumble.
Launcher: Force feedback strength (70% default, Off) and Invert force; m2run
prints the commands by type. Checked: tests/test_app_controls.cpp with SDL
virtual joysticks (a wheel and pedals resting at +32767; config round trip;
ADC values; a wheel paddle; unplugging; drive command decoding; rumble and
its scaling and stop). Not checked: a real wheel or force feedback (none here; user
testing): the launcher marks the wheel column, its dead zone and force
feedback Experimental; the direction of the pull may need Invert force.

**The 1994 set (daytona, Revision A) builds and runs, for comparison.**
CMake `M2_ROMSET` (daytona93, the default, or daytona), one set per build
directory: `scripts/recompile.py --set daytona --build-dir build-daytona`
with the set at roms/daytona.7z. The importer has both sets' MAME tables
(Revision A: other program, sound program, two main data ROMs mirrored from
0x800000, two polygon and two texture ROMs; the TGP program is the same
2,024 words, at main_data 0x800020). Saves are per set (pref folder
daytona-recomp/<set>). Seeds: seeds/daytona.txt, the daytona93 seeds carried
over by code (new scripts/seed_map.py: 469 of 582, 175 of them by
alignment where the revision inserted code), then seed_scan.py --set
daytona (239 more; 38,001 instructions reachable). Revision A does some
float maths on the i960's FPU instead of the TGP: m2recomp gained addr, subr
and modi (MAME's semantics; register forms only; daytona93's generated code
unchanged). Hooks: seeds/daytona_hooks.txt, draw distance at 0x17508
(daytona93's draw-list routine moved by 0x490: the same masked instructions,
RAM and boot-time budget); checked: -2 and +2 change the frames as on
daytona93. Its default settings are a linked twin cabinet, which waits on the
link at the settings screen: set a single cabinet in test mode (F2) once;
m2run and m2gpushot `--nvram DIR` load the app's saved EEPROM and backup RAM
(tools/common/nvram.h). Checked with that: 12,000 attract frames, and every
input script (races on all three courses, steering, test mode screens)
runs with no missing code; race_basic plays through circuit select,
transmission select, the rolling start and the race, with drive board
commands. Native audio works on Revision A too: its sound program has the
native sequencer's tables from 0x505a on 0x48 bytes later, their pointers
with them (sequence banks and engine sound tables unmoved); the sequencer
detects the layout by those tables' first pointers. scripts/
test_native_sound_oracle.sh takes the set from the ROM folder's name and
M2_NVRAM=DIR: Revision A race, 6,000 frames: 4,892 of 4,893 notes the same
as the reference audio (the extra one at the run's last instant), the same
two pitch differences as daytona93, RMS ratio 1.02, no invalid data.
Why: the arcade's wheels sit inside the wheel arches with a gap; ours (and
MAME's) poke out over the wings. The car code and data are the same in both
revisions (wheel table at 0x234af4 / 0x230d54: ±0.525, 0.32, 1.4125/-1.4;
body and wheel models on shared polygon ROMs), and Revision A here frames
the arcade footage's bridge shot (frames 8680-8780) almost exactly and still
shows the tyres over the wings: not a revision difference. In attract the
car is moved by the course-following routine (0xca40), so body roll is 0
and pitch small; the full physics (0xf31c) does not run. Open: what on the
board differs (the geometrizer port, the TGP).

**Super sampling (hardware renderer).** Launcher > Game > Super sampling:
Off, 2x, 3x, 4x (`supersampling=` in launcher.ini; m2gpushot
`--scale N`). GpuRenderer::render draws into a target `scale` times the
frame: vertices stay in original pixels (the viewport scales them), clip
rectangles and the depth buffer scale, the tile shaders map target pixels
back to original ones (tiledata[2]; tile pixels repeated), and textures
take a finer mip level, log2(scale) levels (texlod + 128 log2(scale); the
rasterizer picks levels from z, not screen size). The checker pattern is
per target pixel. main draws it into its own texture with mip levels and
shows the level nearest the window's size, so a frame bigger than the
window is averaged down (supersampling). 1x: byte-identical to before (104
race frames, 16:9). race_basic 16:9 headless (Metal): 455 / 389 / 295 /
265 frames/s at 1x / 2x / 3x / 4x. Checked: a 3x race frame (no cracks,
HUD in place) and the app at 3x (700 frames).

**Tilemaps, step 2: drawn on the GPU (hardware renderer).** m2.hlsl
ps_tiles_back / ps_tiles_front compose the System 24 layers per pixel with
Video::draw's rules (window masks, per-line scroll, the split modes, the
back pass's opaque 3 and 2), then the widescreen margins as fill_margins
(edge colours, the sky's colour, or stretched). Inputs: the decoded pixmaps
(a u16 per pixel, pen and category; uploaded by the span of tile rows
changed since the last upload, by the tile generations; never with
SDL's cycle flag, which would drop the rows not sent), and a per-frame
snapshot Video takes at screen_update of tile RAM 0x4000-0x6fff and the
4,096 pens. Video's external-3D `cpu_layers` is now `desktop`: the CPU only
estimates the 3D coverage (margin fill) and, with the HUD at the edges in a
race, still draws the front layers and moves the HUD blobs (uploaded as a
texture then). Checked against the previous build (CPU-drawn layers):
byte-identical frames, 0 pixels differ, over race_basic at 4:3, 16:9,
16:9 stretched and 21:9 with the HUD at the edges, the advanced and expert
courses, test mode and test drive (622 frames; the races use split modes
and per-line scroll). race_basic 4:3: 400 -> 526 frames/s; game 1.82 ->
1.03 ms, renderer CPU 0.53 -> 0.33 ms, GPU 0.16 -> 0.54 ms. Tested by the
user on macOS (Metal), Windows and Linux.

**Tilemaps, step 1: decode only changed tiles (both renderers).** Measured
first (m2gpushot --bench now reads Video's own timers): of the hardware
frame's 2.2 ms game time, the CPU tilemaps took 1.27 ms: decoding the four
512x512 layers 0.55 ms (all 16,384 tiles, every frame), drawing them with the
scroll/split/mask rules 0.63 ms, composing 0.09 ms. Video::decode_layers
(desktop; the Vita path keeps its own cache) re-decodes only tiles whose
value or character changed, comparing character RAM (256-byte pages, then
32-byte characters) only on frames the game wrote it. Same pixmaps: race,
time attack, test mode and attract screen hashes unchanged. Decoding 0.55 ->
0.02 ms; hardware 343 -> 409 frames/s, software race 189 -> 204. Next: the
layers drawn on the GPU (0.65 ms drawing + 0.09 composing).

**Vulkan application name (MangoHud showed "SDL").** SDL's Vulkan backend
hard-codes VkApplicationInfo: no application name, engine "SDLGPU", which
overlays such as MangoHud show instead of the API; SDL has no property to
change it. `patches/sdl3/0001-vulkan-application-name.patch` (applied by
setup.py's new apply_patches, shared with MAME's patches; already-applied
patches are skipped; fetch forces the checkout if a patched file would block
a new pin) reports SDL_SetAppMetadata's name ("Daytona USA", set by main)
and no engine name. Checked: setup re-applies it after a revert and skips it
when present; macOS builds and runs. The MangoHud result itself is untested
here (no MangoHud on macOS).

**Windows: the game's messages.** daytona is a WIN32 (GUI) program, so on
Windows its output went nowhere and a command window returned at once; a
tester could not see which renderer ran. It now attaches to the parent
console when there is one, else writes daytona.log in the pref folder, and
prints `daytona: renderer hardware (GPU)` / `software (CPU)` whenever the
active renderer changes. Checked on macOS (both lines); the Windows branch is
compiled by CI only.

**Hardware renderer speed (`m2gpushot --bench`).** race_basic, 6,000 frames,
headless on this Mac (Metal): software 189 frames/s (5.3 ms a frame) at 4:3
and 188 at 16:9; hardware 324 (3.1 ms) and 347. Hardware frame at 4:3: game
2.20 ms (logic, geometrizer, CPU tilemap layers), renderer on the CPU 0.72 ms
(vertices, uploads, a 4 MB texture RAM compare, the colour table), waiting
for the GPU 0.17 ms. From the draw-mode figures (every third frame drawn:
442 frames/s), logic is about 0.75 ms, the CPU tilemap layers about 1.45 ms
and the CPU 3D rasterizer about 3 ms a frame. Next costs, in order: tilemaps
on the GPU; texture RAM tracked by writes instead of compared.
Done: M2Board::tex_write counts writes (VideoMem::tex_generation) and the
renderer uploads texture RAM only when the count moves; the 4 MB compare and
shadow copy are gone. Renderer CPU 0.72 -> 0.54 ms, 324 -> 346 frames/s at
4:3; GPU frames byte-identical to before (8 of 8 sampled).

**Hardware renderer, widescreen and a mip-level fix.** Hardware mode keeps
widescreen: Video's external-3D mode with CPU layers no longer drops the
margin (only the Vita path does); both layers are width() wide, the
backdrop's margins filled as in software mode with the 3D coverage taken
from Raster::coverage_estimate (the polygons on an 8x8-pixel grid; no CPU 3D
layer exists here) and the front layer's HUD moved to the edges; the GPU
projects with the margin, widens full-width windows into it, and moves the
condition panel's quads by Video::gpu_hud_shift (same box and z as the
software path). Fix found while comparing: the rasterizer's max mip level is
30 - countl_zero(min(w, h)) = log2(min) - 1; stage 2 used log2(min), so a
fading circuit-select map (texlod -321, mml 1132) took level 7 not 6 and
came out coloured instead of grey. Measured after (race_basic, Metal,
m2gpushot vs m2run, every 650 frames to 5200): 89.7-100% identical,
94.4-100% within 8 levels; the rest are rounding on high-contrast textures
(road lines, rock), where a one-step texel coordinate difference flips the
blend. 16:9 with HUD at the edges: race frames 94.8-98.3% identical.

**Hardware renderer, stage 2 (textures).** ps_poly is a port of the
rasterizer's draw_tex_span and fetch_bilinear_texel in integer arithmetic:
the 4-bit sheets with their 2048x1024-as-1024x2048 mapping, bilinear 8-bit
blending (LERP), wrap/mirror/edge rules, mip levels by fast_log2 (the same
128-entry table) and texlod, the microtexture, the translucency flag and
test, the luma RAM and the colour translation. Data: three read-only storage
buffers (texture RAM, both sheets, uploaded only when it changes; luma RAM;
colour translation with the rasterizer's gamma applied on the CPU); per
polygon texture state as flat integers decoded as render_one does; 1/z, u/z,
v/z interpolated noperspective. Measured (race_basic, Metal, m2gpushot vs
m2run): frame 1500 99.9% of pixels identical; race frames 3000 and 4500
95.5% and 95.1% identical, 98.4% and 98.8% within 8 levels. The differences
are scattered over textured surfaces (far road, rock face), not edges:
float differences flipping mip-level and texel thresholds. The rasterizer
accumulates 1/z, u/z, v/z per pixel along each span; the GPU evaluates each
pixel's directly, and Metal compiles with fast math. Exactness is stage 4.
build_shaders.py pulls the x86-64 Ubuntu image explicitly (a cached arm64
one failed with "exec format error").

**Hardware renderer, stage 1 (geometry).** Launcher > Game > Renderer:
Software (exact; default) or Hardware (Experimental). `src/app/gpu/`:
`m2.hlsl` (one source) -> `scripts/build_shaders.py` (DXC v1.9.2609 to
SPIR-V and DXIL, SPIRV-Cross to MSL; Docker when the tools are not
installed) -> `shaders_gen.h` (committed; builds need no shader tools).
GpuRenderer draws the 3D in the rasterizer's order (window, then z, newest
first), projected as model2_3d_project, each polygon a fan from vertex 0,
clipped to its window by scissor; a depth buffer with depth = draw order
and LESS reproduces "first polygon to fill a pixel wins" (the rasterizer's
fill buffer). Colour: the solid renderer's palette/luma/gamma; textures are
stage 2 (the Vita GPU path is a reference only: a tester saw small road
geometry/orientation errors there). Video's external-3D mode gained
`cpu_layers` (the CPU still draws the tilemap layers for it; the Vita path
does not). No widescreen in hardware mode yet. `m2gpushot` renders frames
offscreen through it for comparison with m2run's (tools/common/
input_script.h shared). Checked on Metal: the game runs (710 frames in
15 s), and a race frame's geometry, HUD and backdrop line up with the
software renderer's. Not yet run on Vulkan or Direct3D 12.

**Draw mode (frame skip).** Measured first: Daytona runs the board in 60 Hz
mode and the geometrizer starts a new frame every vblank (3,000 of 3,000
race frames drew a new 3D picture), i.e. double buffered. Launcher > Game >
Draw mode: Double buffered (every frame, the default), Single buffered
(every 2nd), Every third frame (every 3rd); `m2run --frame-skip 0|1|2`.
M2Board::vblank_end skips screen_update on the frames between (3D raster,
tilemaps, composition), keeping the last picture; the geometrizer still
parses every frame (the game reads its polygon count). Measured race_basic:
identical i960 (196,665,345), TGP (223,429,779) instruction, interrupt
(12,050) and sound byte (3,636) counts in all three; 190, 332, 442 frames/s
headless on this Mac. Default hash unchanged.

**Skip launcher.** Launcher > Game > "Skip launcher" (saved): start-up goes
straight into the game, as `--autostart` does, when the ROM set checks out;
otherwise the launcher shows with the reason. Esc still opens it. Checked:
with it set, `daytona` started the game at once (377 frames in 8 s).

**"Stretch tile background" stretches, it does not extend.** In a race, with
the option on, the backdrop as drawn for the 496 columns is scaled across
the whole width (linear blend per row); the tester wanted no repeat at all.
Video::draw_ext (drawing the tiles past the screen edge) is removed. Earlier:

**Widescreen sky: plain by default, "Stretch tile background" to extend.**
A tester still saw a seam in the margins with the tile backdrop drawn out.
Measured: the race sky is tilemap layer 2, one layer in normal scroll mode
(not a split pair: the split-mode alternation added to draw_ext changed 0
pixels there), and its hscroll sweeps the whole 0..511 range over a lap
(152 values), so the original 4:3 screen passes the picture's join too;
only ~79% of rows match across it. So the margins default to the sky's
plain colour behind 3D, and launcher > Enhancements > "Stretch tile
background (Experimental)" (`m2run --stretch-backdrop`) draws the tiles out.
2D screens keep each row's edge colours either way. Split pairs in draw_ext
now alternate A/B every 512 columns (a 1024-pixel panorama), as the
hardware's layout implies; not exercised by Daytona's race sky.

**Launcher labels.** Options that need it say so: "Graphics API (Restart
Required)"; "Native audio (Experimental, Reset Required)" (it applies when a
game starts or is reset, not on an app restart); "HUD at the screen edges
(Experimental)". Everything else applies straight away.

**Draw distance (enhancement, off by default).** Launcher slider (Shortest,
Shorter, Default, Further, Furthest = -2..+2), `m2run --draw-distance N`.
Found by tracing, not by guessing: the geometrizer's master z clip is unused
(0xff); object commands in the display list are written by the TGP, which
reads model lists from its own ROM (only 8 of ~3,000 i960 FIFO words per
frame match model addresses). The i960 picks what to draw:
- Scenery by course cell: a 16x16 grid (cell = x + 16 y; offsets table
  0x17136 = dx + 16 dy). 0x16f74..0x17070 takes the 5x5 around the car's cell
  (r8) in nearest-first order (0x17104; 0x1711d on two courses), kept where
  two visibility masks allow (r13 from 0x171f8, r9 from 0x1727c), into the
  draw list (count 0x5016c0, cells 0x5016c1.., 63 bytes before 0x501700) and
  a near list of the 10 nearest (0x501600) plus bitmaps 0x501500/20/40.
  Measured over a race: 8-13 cells listed, always reaching radius 2.
- 0x17828 draws every object of each listed cell (table 0x501420) until a
  per-frame polygon budget runs out: 0x5010e8 accumulates each object's cost,
  0x5010f4 is the limit (5000, set once at 0x1210), checked at 0x17a78 and
  0x1786c. Extra cells appended to the list drew nothing until the budget
  was raised: it, not distance, is what stopped them.
- The road: a 14-section window (0x13f5c: 5 back via +0x8c, 14 forward via
  +0x88, list at +0x5c of its struct; ordered pair checks via table 0x13ec4
  and TGP maths at 0x14180). Also game logic (car/section interaction);
  not changed.
Mechanism: `m2recomp --hooks FILE` ("ADDRESS name": the generated code calls
rt::hook_<name>(c) before that instruction), `seeds/daytona93_hooks.txt`,
`src/runtime/enhance.{h,cpp}`. hook_draw_list at 0x17078: -1 keeps the
game's list within one cell, -2 the car's cell only; +1 lists all 5x5, +2
all 7x7 (inside the grid), and raises the budget to 10000 / 15000 (restored
to 5000 back at default). Only the draw list and the budget change; the near
list and bitmaps the game logic reads do not. Measured (race_basic, m2run on
this Mac): -2 242 frames/s, -1 198, default 193, +1 190, +2 185; default's
screen hash unchanged (ad67233983ea8808). +2 adds visible scenery (frame
2600: 1,560 pixels, a tree line behind the billboard); most frames differ
by tens of pixels, because the road, not the scenery, is the horizon.

**Setup's "ROM set rejected" was shown for any recompile failure.** A
Windows tester got it with a zip that imports on macOS and Linux. The
reader is portable C++ (binary I/O, fixed-width fields, its own inflate
and CRC), so the likelier cause is a later step failing on Windows (the
game's generated code has never been compiled there) under the wrong
message. `recompile.py` now exits 3 only when `m2import` rejects the set;
any other failure gets "ROM accepted, the build failed, see the errors
above". The tester's next run confirmed it: the ROM was accepted and the
generated code compiled (`m2run`, `m2native` built); the final build step
still failed. Likely cause, not yet confirmed on Windows: `daytona` is a
WIN32 (GUI) executable and `main.cpp` did not include `SDL3/SDL_main.h`,
so nothing provided `WinMain`. Now included (no effect on macOS/Linux).
The tester's next output showed the real failure: MSVC building SDL itself,
`yuv_rgb_internal.h` C2099 "initializer is not a constant". Our directory-
wide `/fp:strict` reached SDL, and under it MSVC will not fold C float
constants in static initializers. SDL now gets an empty COMPILE_OPTIONS
(ours keep /fp:strict). CI never built SDL (it was only added with
generated game code); SDL, ImGui and the app objects (`daytona_app`) now
build whenever extern/sdl3 exists, so CI compiles them on every OS; only
linking `daytona` still needs a ROM set. CI then compiled SDL, ImGui and
`daytona_app` under MSVC and clang-cl (including the yuv_rgb file that
failed). The tester's PC had built with MSVC: setup.ps1's Visual Studio
Installer `modify` ran unelevated and, it seems, failed quietly. setup.ps1
now runs it elevated, checks the Clang toolset with vswhere afterwards,
stops with instructions if it is missing, and sets M2_COMPILER=clang
(`--msvc` for MSVC). Untested on a real PC; CI only checks it parses
(GitHub's Windows runners have no winget). README's Setup and
docs/getting-started.md now cover Clang on Windows and `--msvc`, updating,
a clean rebuild, the "build failed" message, and the widescreen options.

**Widescreen (enhancement, off by default).** Launcher > Enhancements >
Widescreen: Original (4:3), 16:10 (614x384), 16:9 (682x384), 21:9
(896x384); `m2run --aspect 16:9` for headless dumps. More of the scene at
the sides, same focal length, nothing stretched: `Geo::set_wide_margin`
opens a full-width viewport's left/right clip planes by the margin,
`Raster` draws into a wider layer (stride >= 496 + 2 x margin) with the
clip widened for full-width viewports, `Video` composes at `width()` with
the tilemaps centred and each back-layer row carried into the margins (the
sky's colour, not the backdrop pen). With it off every path is the old one:
all 11 scenarios give their previous screen hashes. With 16:9 all 11 run to
the end. Sampled 21:9 race frames show no obvious edge pop-in yet; not
checked frame by frame.
"HUD at the screen edges" (with widescreen; `m2run --hud-edges`): two
groups in 496-wide coordinates move out by the margin: lap and lap times
(x < 125, y < 130) left; position, condition panel and course map
(x >= 352, y < 300) right. Decided per item: the front layers' pixels are
grouped into blobs (pixels within 4 of each other join) and a blob moves
only if it lies wholly inside a group, so a banner crossing a group stays
whole and centred. The first version decided per area (a band at the cut
had to be empty): during the rolling start the banner's letters passed the
cut, the decision flipped frame to frame and the HUD jumped back and forth.
Measured after, frames 2585-3200 every 5th: the course map is at the edge
in all but the first (HUD not yet drawn). "40TH/40" reaches x 367. The
condition panel's box and car are polygons in the main 3D window at sort z
0x600 (scenery there is above 18000); overlay polygons (z <= 0x0fff) inside
a group move with it, but only while the race HUD is on screen: a tester
saw the car's door come off in an attract close-up, because the car's own
near polygons (and the ranking screen's 2D markers) sat in the HUD's
right-hand area and were moved. Now nothing moves unless the frame has the
condition panel's box (one checker-shaded overlay polygon, texheader
0x8000, sort z <= 0x0fff, ~77x82 at x 385..462, y 67..149;
Raster::race_hud_visible). Measured after: 240 attract frames at 16:9
identical with the option on and off; the race HUD still moves, and the
map stays at the edge through the rolling start. A tester still saw 3D
moved in play: the rule was any polygon at sort z <= 0x0fff inside the right
group. Now only the condition panel's own quads move: polygons at exactly
the box's z inside the box's outline (Raster::find_race_hud records both).
Measured: race at 16:10, option on vs off, 0 of 120 frames differ outside
the HUD areas.
Side margins: in a 3D scene (the 3D layer covers >= 50% of the screen;
measured races 69-100%, select screens ~23%) the back layers are drawn
margin to margin by Video::draw_ext, draw()'s rules pixel by pixel for any
screen column: scroll, per-line scroll, the split modes that put layers
L and L^1 side by side, priority, window masks. Checked: its visible columns
equal draw()'s on every frame of a race (M2_CHECK_DRAW_EXT=1 prints any
difference; none). Earlier tries, all wrong in play: one plain sky colour;
each row's edge carried out (smeared the clouds); copying columns mod 512
(a tester saw the backdrop duplicated with a seam: a column off the screen
can belong to the other layer of a split pair); a "joins up across the
wrap" test to tell sky from menus (failed on the race sky, median 79% of
rows). On 2D screens (car, circuit select) each row carries its own edge
colours: their art covers only the 496 columns, and drawing further shows
leftover tiles as stripes. Before that the margins were the sky's plain colour (the back
layers' top-left pixel); carrying each row's edge out smeared the sky
picture's clouds and mountains. Off: all scenario hashes unchanged; 21:9
with it on: all scenarios run to the end. rules.md now lets enhancements change game logic.
Measured for draw distance: Daytona leaves the master z clip at 0xff
(0 polygons culled by distance over a 6,000-frame race); backface 4.2M,
behind the camera 1.1M, off-screen 1.4M. The limit is in the game's code.

**Windows: Clang by default; MSVC fixed.** MSVC failed on SoftFloat: the
CMake used SoftFloat's `build/Linux-x86_64-GCC/platform.h` everywhere, whose
`opts-GCC.h` needs `__int128`, `__builtin_clz` and GNU inline (C4235, C4013).
MSVC and clang-cl now get `cmake/softfloat-portable/platform.h` (no
`INLINE_LEVEL`, no builtins, no 128-bit type) and `__declspec(thread)` /
`thread_local` for its globals; `-DM2_SOFTFLOAT_PORTABLE=ON` forces that
header anywhere. Measured here with it forced: `test_fp` 0 mismatches,
race_basic, time_attack and course_expert screen hashes identical to the
GCC-header build. `setup.ps1` installs (or adds to an existing Visual
Studio) the Clang tools; `setup.py` builds with the ClangCL toolset when
present, MSVC otherwise (`M2_COMPILER=clang|msvc` forces one), and
reconfigures a build directory set up for the other.
`.github/workflows/build.yml` builds and tests both, without a ROM set,
alongside macOS and Linux (GCC, Clang).
Not yet run on a Windows PC here; a tester reports Clang + Ninja builds.
First CI run: SoftFloat compiled under both; both then stopped on
`tests/test_vita_gpu_memory.cpp`, `alignas(262144)` (C2345; clang-cl: 8192
bytes at most on Windows). That buffer is now aligned at run time, and the
nine Vita host tests are opt-in (`-DM2_VITA_TESTS=ON`, 21 tests) instead of
part of every desktop build (12 tests).

**PS Vita frontend merged (PR #3, `c3007c6`).** Desktop unchanged by it,
measured: all 11 scripted scenarios and 3,000 frames of attract give the
same instruction counts and screen hashes as `398eb4b`; 21/21 tests pass.
Native audio is opt-in (launcher checkbox). Its notes live in
`platform/vita/` (HANDOFF, THIRD_PARTY for SDL2); the Vita build itself is
untested here (needs VitaSDK).

**Setup for players.** `docs/getting-started.md` walks from nothing to
playing on each OS, with troubleshooting for what went wrong in practice:
`./setup` for `./setup.sh`, the ROM set under another name (setup silently
built tools only), a different Daytona set (`daytona`, program ROMs
`epr-16722a`/`16723a`: `m2import: missing epr-16530a.12`), Start disabled.
`scripts/setup.py` now says so itself: with no `roms/daytona93.*` it names
any archives in `roms/`; a rejected set gets an explanation instead of a
traceback; on success it prints the command that starts the game. README's
Setup links to the guide.

**macOS (Apple clang, arm64, Metal) builds and plays.** First run on a Mac
with a real `daytona93` set turned up:

- `scripts/recompile.py` built only the check tools after generating code,
  never `daytona` or `m2run`, so a fresh `./setup.sh` left no game. It now
  builds everything.
- The launcher saved the ROM path as typed (`roms/daytona93.7z`), so the
  check failed and Start stayed disabled when run from another directory.
  It now stores the absolute path. A saved GPU choice the host lacks
  (Vulkan on a Mac without MoltenVK) exited at `SDL_CreateGPUDevice`; it now
  falls back to automatic.
- The seeds were short. The windowed game stopped at `0x1d8c`, then longer
  runs at `0x2266f8`, `0x5788`, `0x225028`, `0x223078`, `0x2265c4`: code the
  game reaches only through pointers in states the harvest never visited.
  `scripts/seed_scan.py` finds them statically (see Findings) and added 248
  seeds (334 -> 582): 24,504 -> 34,497 reachable instructions, recompile
  still all native.
- Time attack stopped in the geometrizer: texture-point/header reads and a
  polygon-RAM walk run one past the end of their memory. `GeoPtr`/`GeoPtr16`
  now wrap (see Findings).

Measured after, `m2run`: attract 40,000 frames (11.6 min of game time),
all 11 scripted scenarios in `scripts/inputs/` (races, courses, time attack,
test menus) run to the end, all native; race_basic screen hash unchanged
(`ad67233983ea8808`) by the wrap. `daytona` on Metal: 60 s, 3,396 frames,
no stop. **Not verified against MAME**: the newly seeded code has not been
lockstepped (it only runs in states the scenarios reach, and the harvest
did not), and the wrap has no MAME counterpart to compare with.

**Sound.** The Model 1 sound board runs natively: its 68000 program
(`epr-16489`/`16490`) is statically recompiled (`src/m68k` decoder,
`tools/m2sndrecomp`, runtime context `src/runtime/snd_cpu.h`), with the
YM3438 (ymfm, BSD-3) and both MultiPCMs (MAME's, transplanted into
`src/runtime/multipcm.cpp`) as native code on `snd::SoundBoard`
(`src/runtime/sound_board.cpp`). All 1,916 reachable instructions decode
exactly as MAME's 68000 disassembler prints them; the driver has no
indirect jumps. Lockstep against MAME's 68000 (`tools/m2sndcheck`, MAME
patch 0003, `M2TRACE_SNDLOG`): attract 15.7M instructions and race 77.9M
instructions, 3,648 interrupts, 5.16M device accesses, all identical.
No clock: the driver polls YM timer B (868 Hz tick) in its main loop and
takes the UART's RxRDY on IPL 2, so time is counted in 68000 instructions
(752,000 per second, MAME's rate on this program) and events land on it:
command bytes one line-time apart (31.25 kbit/s), YM timer expiries in
exact YM clocks. `GameLoop` advances the board one frame per video frame
and hands it that frame's UART bytes. `daytona` plays it through two SDL
audio streams (YM at 55.6 kHz, MultiPCMs at 44.6 kHz) with a small speed
trim holding 60 ms of queue; volume and mute in the launcher. `m2run --wav
FILE` writes it headless. Against MAME's own audio (`-wavwrite`) for 26 s
of attract: the same 48 command bytes, per-second loudness within a few
percent, a constant ~125 ms offset (when the i960 sends the first
commands), no tempo drift. Race: 3,636 bytes natively vs MAME's 3,648.
Checked here with SDL's disk audio driver (no sound card in the
container): continuous output from the windowed game.

**Launcher.** `daytona` opens a Dear ImGui launcher in its window
(`src/app/launcher.cpp`): ROM browse (SDL3's native file dialog: Windows,
macOS; xdg-desktop-portal or zenity on Linux; typed path as fallback) with
per-file verification, graphics API and fullscreen; a Controls tab binding
every arcade control to a key and a gamepad button or axis half (press to
bind), analogue triggers for the pedals and a stick for steering, live
meters, dead zone, invert. Settings save to `launcher.ini` in the SDL pref
path as they change. The ROM set is loaded natively from the zip
(`src/runtime/zip.cpp`, own inflate; `rom_import.cpp`, the importer's table):
images byte-identical to `scripts/m2import.py`'s, in 0.8 s. 7z ROM sets too (`src/runtime/archive.cpp`: the
7-Zip LZMA SDK's public-domain decoder; solid LZMA2, LZMA, PPMd checked
byte-identical to the zip import, 0.9 s for 7-Zip's default). The build's
importer is now the same C++ code (`tools/m2import`, used by
`scripts/recompile.py`), so a .7z-only user can build. Esc in game
returns to the launcher (Resume, Reset, Quit). Verified here under Xvfb on
Vulkan: verification, the Controls tab, a key rebind saved to the ini, Start
from the zip, pause, and the zenity file dialog. Resolution and upscaling
options are to come.

**The game is playable in a window.** `daytona` (`src/app/main.cpp`): SDL
3.4.16 (built from source, static) with SDL_GPU: Vulkan or Direct3D 12 on
Windows, Vulkan on Linux, Metal on macOS (`--gpu` to choose). Each composed
frame is uploaded to a GPU texture and blitted, letterboxed 4:3, onto the
swapchain; the game advances at the board's 57.52 frames/s and is presented
at the display's rate. Keyboard and gamepad map to the I/O board; settings
EEPROM and backup RAM persist in the SDL pref path. Verified here on Vulkan
(Mesa lavapipe under Xvfb): the attract demo in the window. Direct3D 12 and
Metal not run yet (no Windows or Mac here). The 3D layer is still drawn by
the CPU reference rasterizer; the GPU rasterizer is a later step.
`rt::GameLoop` (`src/runtime/game_loop.cpp`) holds the frame pacing that
`m2run` and `daytona` share.

**The game runs on its own.** `m2run` runs the recompiled game on the
native board runtime (`src/runtime/m2_board.cpp`) with no trace and no
MAME: boot, the settings screen, then the attract demo in full 3D.
Headless for now (frames dumped every N; `scripts/rgb2png.py`); 600 frames
in 5.7 s including the CPU rasterizer (106 frames/s), 23 M i960 and 12 M TGP
instructions, all native.

How it runs, with no clock:
- Frame pacing is the game's: vblank starts when the game waits in its
  wait-for-vblank loops (0x12b0-0x12bb, 0x12f0-0x12ff: spinning on the
  frame counter at 0x00500000); a CPU-bound frame (the boot texture upload
  at 0x1388) gets vblank after one frame's worth of work (110k
  instructions, as MAME measures 25 MHz / 57.52 Hz). Vblank ends when the
  handler has returned to the wait loop. A windowed build waits for vsync
  there; frame rate is the only limit.
- The TGP runs before the i960 reads buffer RAM: the game sends the TGP a
  command, writes -1 to the mailbox at 0x0091fff0 and spins until the TGP
  writes 0 (0x1166c). Without this the game waits forever.
- I/O board: the dual-port RAM mailbox protocol, native (command 1: latch
  inputs into bytes 0-10; 3: load the 128-byte settings EEPROM into
  0x100-0x17f; 2: store it). Inputs take the scripts/inputs format
  (`m2run ... --inputs FILE`).
- Sound UART: TxRDY is immediate, so the IRQ3 handler drains its queue at
  once; the sound board receives the bytes at the line rate (Sound, above).

With scripted inputs (`--inputs scripts/inputs/race_basic.txt`) the whole
game flow runs standalone: coin-up, Circuit Select, car select, the race's
rolling start (3,000 frames in 23 s headless: 80 M i960 and 62 M TGP
instructions, 381 bytes sent to the sound board).

**The whole screen now renders natively, identical to MAME.** `src/runtime/video.cpp`
adds the segaic24 tilemap chip (four 64x64-tile layers, per-line scroll,
special window modes, 8-pixel window masks), the tilemap palette pens (as
MAME computes them at palette write time, refreshed each frame once a scroll
colour is written), the CRTC offsets, and MAME's composition order (2D back,
3D, 2D front). It runs at the i960 instruction count of each MAME screen
update (vblank end; patch 0002 `su`/`scr` lines) and is held to a hash of
MAME's composed screen: **596 of 596 attract frames and 5,996 of 5,996 race
frames identical**, 3D layer 5,587 of 5,587. Dumped frames (boot settings
screen, attract with HUD) are byte-identical to MAME's.

Everything on screen now comes from native code: the recompiled i960 and
TGP programs, and native C++ for the fixed-function chips (geometrizer,
rasterizer, tilemaps). Still replayed from MAME's trace in the harness: the
sound board (UART bytes) and the I/O board (dual-port RAM). Design change:
the sound 68000 will be statically recompiled, not interpreted.

**M3 started: the 3D layer renders natively, pixel-identical to MAME.**
`src/runtime/raster.cpp` is a CPU reference rasterizer (MAME's Model 2
renderer and the poly.h triangle/polygon setup, transplanted). In
`m2native`, at each vblank it draws the previous display list from our own
video memories (palette, colour translation, luma, texture RAM, all written
by the recompiled i960) and is held to a hash of MAME's 3D layer (patch 0002
`fb` lines): **423 of 423 rendered attract frames and 5,587 of 5,587 race
frames identical**, and a dumped frame is byte-identical to MAME's. A
mutant (one gamma entry off by one) fails every frame. Frame dumps:
`M2NATIVE_FBDUMP_DIR`/`M2NATIVE_FBDUMP_EVERY` (ours),
`M2TRACE_FBDUMP_DIR`/`M2TRACE_FBDUMP_EVERY` (MAME); `scripts/rgb2png.py`
converts either (game output: keep under traces/). Finding on the way: the
original Model 2's texture RAM keeps only 16 bits of each 32-bit write,
packing two writes per stored dword (MAME tex0_w/tex1_w); the bus now does
the same. CRTC offsets are still taken from MAME's log (they come from the
segaic24 tilemap chip, next).

**M2 geometry, native and matching MAME.** The whole geometry path now runs
natively inside `m2native`: recompiled i960, recompiled TGP, buffer RAM, and
the geometrizer (`src/runtime/geo.cpp`, MAME's HLE transplanted; the
original Model 2 geometrizer's DSP code is undumped). At each MAME vblank
(same i960 instruction count) the geometrizer walks our buffer RAM and is
held to MAME's log (patch 0002, `M2TRACE_GEOLOG`): every word it hands the
rasterizer and every polygon kept after culling and clipping, vertices bit
for bit.

| scenario | frames | rasterizer words | polygons kept | result |
| --- | --- | --- | --- | --- |
| attract | 597 | 11,954,823 | 403,475 | identical |
| race_steer_left | 5,997 | 151,874,210 | 6,658,999 | identical |
| time_attack | 5,997 | 136,744,927 | 5,937,107 | identical |

A mutant (luma off by one when it is exactly 100, in all four parsers)
diverges at frame 173, rasterizer word 385. A one-ulp change to a vertex
cannot show: the geometrizer hands the rasterizer 24-bit floats (MAME's
`f2u(x) >> 8`), which drop the low 8 bits. `scripts/m2_check.sh SCENARIO`
runs all of it (the race's geometrizer log is 2.8 GB of text; it is deleted
after the check unless M2_CHECK_KEEP is set).

**M2 started: the TGP program is statically recompiled and matches MAME.**
The TGP runs a 2,024-word program the i960 uploads from the data ROM;
`m2tgprecomp` turns it into native C++ (MAME's MB86233 semantics inlined per
instruction from `src/runtime/tgp.h`; no interpreter, no hand-written HLE),
and `m2tgpcheck` replays MAME's TGP-side log (patch 0002) through it:

| scenario | TGP instructions | input words | output words | banked accesses |
| --- | --- | --- | --- | --- |
| attract, 600 frames | 12,390,181 | 1,293,703 | 398,072 | 7,789 |
| race_steer_left | 230,600,970 | 21,489,825 | 6,727,552 | 3,233,496 |
| time_attack | 97,745,484 | 8,089,105 | 3,291,602 | 2,025,772 |
| test_tgp | 32,597,984 | 3,300,128 | 1,083,012 | 7,789 |

All identical to MAME; in attract every register was also checked after
every instruction (`M2TRACE_TGPPC`). A mutant (fml result off by one ulp when
A = 1.0) diverges at TGP instruction 2,339. Native speed ~500 M TGP
instructions/s (race: 0.48 s for 231 M).

**Native i960 + native TGP together** (`m2native` now models the geometry
ports, TGP FIFOs and buffer RAM instead of replaying them; the TGP runs on
demand, clockless, until its input FIFO is empty):

| scenario | i960 instructions | TGP instructions | TGP output words | buffer RAM hash |
| --- | --- | --- | --- | --- |
| attract | 70,926,456 | 12,390,181 | 398,072 identical | identical at 1,153 of 1,153 samples |
| race_steer_left | 643,000,979 | 230,600,970 | 6,727,552 identical | identical at 11,951 of 11,951 |
| time_attack | 663,975,129 | 97,745,385 | 3,291,602 identical | identical at 11,951 of 11,951 |

Buffer RAM (128 KB) is built natively from its three writers: the i960, the
geometrizer command port (0x800000) and the TGP's banked writes. The only
reads that differ from MAME (103 in attract, 64,956 of 5,451,850 in the race)
are all the i960 polling the TGP's mailbox, the last three dwords of buffer
RAM (0x91fff0-0x91fff8, TGP bank offsets 0x7ffc-0x7ffe): our TGP has already
finished when the i960 looks; MAME's, paced by cycle estimates, has not. A
timing artefact like the UART one, so lockstep uses MAME's value there and
counts it. Finding on the way: races read and write the TGP FIFOs 16 bits at
a time (MAME's 32-bit handlers see the whole dword, other lanes zero).

**M1 met with native code**: `m2recomp` statically recompiles 23,262
instructions (seeds: boot record + `seeds/daytona93.txt`) to portable C++,
and `m2native` runs them with no interpreter and no fallback. It matches
MAME for all 600 attract frames: 70,926,456 instructions, every one native,
1,153 samples, 3,315,201 device events, 627 interrupts; 1.3 s (55 M
instructions/s with lockstep checks on every instruction).
`scripts/recompile.sh` generates and builds it (into git-ignored
build/gen); `scripts/m2_check.sh` traces MAME and runs both harnesses.
An address with no recompiled code is a hard error naming it.

Beyond attract, the same native code matches MAME through seven scripted
scenarios (coin up, selects, races, test mode), every instruction, device
access and interrupt, including the sound-UART interrupts that land mid-code
during races:

| scenario | frames | instructions | device events | interrupts |
| --- | --- | --- | --- | --- |
| race_steer_left | 6,000 | 643,000,979 | 39,750,589 | 11,443 |
| course_advanced | 6,000 | 640,678,236 | 42,356,938 | 12,475 |
| course_expert | 6,000 | 641,018,396 | 42,010,671 | 12,419 |
| time_attack | 6,000 | 663,975,129 | 20,342,792 | 11,506 |
| test_mode | 4,000 | 439,587,066 | 6,954,614 | 4,051 |
| test_tgp | 3,500 | 385,086,539 | 6,708,822 | 3,578 |
| test_memory | 3,500 | 385,423,629 | 6,748,842 | 3,553 |

`scripts/m2_check.sh SCENARIO` reruns one (inputs from
`scripts/inputs/SCENARIO.txt`); each takes ~7 min of MAME plus ~15 s native.

The reference interpreter (`src/refcore`, MAME's executor) is a test
oracle only: `m2replay` uses it; the game build will never link it.

Earlier, **M1 reference milestone**: `m2replay` runs Daytona's own code through
the runtime (MAME's i960 semantics, transplanted) with devices replayed from a
MAME trace, and matches MAME for all 600 attract frames: 70,926,456
instructions, 1,153 samples (RAM hashes + registers), 3,315,201 device
events, 627 interrupts; 2.0 s.

M0 tooling runs against real MAME with the real game (`daytona93`, user's ROM
set, git-ignored `roms/` in this cloud container, never committed). Model-2-only
MAME with the harvest patch builds here (`scripts/build_mame.sh`, ~50 min cold,
15 s incremental); `scripts/run_trace.sh` runs it headless.
Built and tested here: i960 decoder + `i960dis`, trace library + `tracediff`,
and the MAME plugin `tools/mame-plugins/m2trace` (trace recorder, input
recorder/replayer). The plugin is tested only against a mock of MAME's Lua API.

Build: `./setup.sh` (Linux, macOS) or `setup.ps1` (Windows) installs the
toolchain, fetches the pinned dependencies, builds, recompiles the game if
`roms/daytona93.zip` is there, and runs the tests (see README.md). Verified
end to end on Linux with GCC and Clang; macOS and Windows (MSVC) not yet run.
The Lua tests need `pip install lupa` (Lua 5.4) and skip without it.

Running the plugin (user's machine, with their ROM set):

    M2TRACE_OUT=traces/attract.m2tr M2TRACE_FRAMES=600 \
    mame daytona -plugin m2trace -pluginspath "<mame>/plugins;<repo>/tools/mame-plugins"

`M2TRACE_RECORD_INPUT` / `M2TRACE_REPLAY_INPUT` record and replay inputs;
`docs/trace-format.md` has the rest.

## Complete

- Design document.
- Project rules (`rules.md`) and `.gitignore`.
- M0 step 1: "from memory" figures checked against MAME `sega/model2.cpp` at
  `dddd73680656e355bb2b5beecab1167c9f07bf81` and the Model 2 MiSTer core at
  `591e148e87d27e03d50cbf7318bf0b1d1328c4bf`. Design doc corrected in place
  (Target hardware summary, TGP HLE, Floating point, Audio, Open questions).
- M0 i960 disassembler: `src/i960` (decoder + MAME-syntax formatter),
  `tools/i960dis` (linear sweep; `--interleave` joins the ROM_LOAD32_WORD pair),
  `tests/test_decode` (66 hand-encoded checks), `tests/mame_oracle`
  (differential against MAME's own `i960dis.cpp`, compiled unmodified).
- Ghidra SLEIGH cross-check (`tests/ghidra_oracle.py`, third-party module
  mumbel/ghidra_i960 via pypcode, `scripts/fetch_ghidra_i960.sh`).
- `src/i960/reach` (recursive descent + boot-record seeds), `i960dis --follow`,
  `tests/test_reach` (19 checks).
- M0 trace format, `tracediff`, MAME plugin and input recorder
  (`docs/trace-format.md`, `src/trace`, `tools/tracediff`,
  `tools/mame-plugins/m2trace`), with `tests/test_trace` (30 checks),
  `tests/lua_core_test.py` (19) and `tests/lua_plugin_mock_test.py` (17).

## Next, in order

1. Scenarios that reach the other 167 seed_scan seeds (other endings, name
   entry, link play), checked with scripts/m2_check.sh like attract_long and
   race_to_end.
2. Hardware renderer: exact pixels against the CPU reference (stage 4).

## Open decisions

- FP oracle for lockstep: MAME disagrees with the hardware model on cvtri
  ties (see Findings). When a replay diverges there, the trace diff will show
  MAME's value; the recompiled build follows the model (rules: PCB > MAME).
  Settling which the PCB does needs a hardware measurement.
- `addc` carry. MAME never sets it. Recompile to the silicon and flag the diff
  when it fires (the MiSTer core made the same call, its study §2.3).

## Findings

Confirmed from MAME `model2.cpp` (all MAME figures, not PCB measurements):

| Item | Was (from memory) | MAME |
| --- | --- | --- |
| i960KB clock | ~25 MHz | 25 MHz (`50_MHz_XTAL / 2`) |
| TGP | MB86234 + microcode ROM | one MB86234 at 50 MHz; program uploaded by the i960, 2,024 words from the data ROM; run LLE |
| Resolution | ~496x384 | 496x384 active, 656x424 total, 16 MHz pixel clock |
| Refresh | ~57.5 Hz | 57.524 Hz; line rate 24.39 kHz (MAME: "TODO: from System 24") |
| Sound | 68000 + 2x SCSP, ~11 MHz | **Model 1 sound board**: 68000 @ 10 MHz + YM3438 + 2x MultiPCM |
| Main to sound | command latch | i8251 UART at 31.25 kbit/s; IRQ3 handler is the transmit loop |
| I/O | direct ADCs | Model 1 I/O board, own Z80 @ 4 MHz, via MB8421 dual-port RAM |
| IRQ order | unknown | bit 0 vblank -> IRQ0, bits 2-5 timers -> IRQ2, bit 10 UART -> IRQ3; ICR 0f0e0d0c (measured): all priority 1 |

i960 decoder (MAME `i960.cpp` / `i960dis.cpp` at `dddd7368`):

- MAME's executor implements 62 non-REG and 102 REG opcodes; its disassembler
  knows many more (whole-family table). The recompiler accepts only the
  executor's set.
- Executor and disassembler disagree on three encodings; decoder follows the
  executor and flags them: CTRL/COBR bits 1:0 (executor adds them to the
  target), MEMB bits 6:5 (executor ignores), MEMB scale > 4 (executor shifts).
- MAME's disassembler table has `ldtime` at 0x671, shadowed by `ediv`; it can
  never print. `movre` appears at 0x6e1 (undocumented, executed) and 0x6e9.
- Differential test, random + every opcode byte x 65,536 tails: 71,108,864
  words, 0 mismatches, 8.0 s on 4 threads (this container). Mutation check:
  three deliberate faults (MEMB scale bound, COBR displacement mask, one REG
  mnemonic) gave 79,619 / 2,220,826 / 9,837 mismatches, so the test can fail.
- Exhaustive run, `mame_oracle --exhaustive`: all 4,294,967,296 first words
  (second word random per word), 0 mismatches, 596 s on 4 threads (this
  container). Text and reported length both compared.

Trace tooling:

- Every signal the design doc asks for except indirect branch targets is
  reachable from MAME's Lua API alone (write/read taps, `read_range`,
  `state[]`), so the plugin needs no MAME patch yet.
- MAME's end-of-frame notifier is not on a guest instruction boundary, so it
  cannot be a lockstep sample point; the default is the vblank-ack store.
  Unverified for Daytona until the first trace.
- Mock-driven plugin test: record -> replay reproduces the trace exactly; a
  replay value applied one frame late is caught at the right frame; a memory
  change is reported as a region hash at the right epoch.
- The Lua encoder and the C++ writer produce byte-identical traces for the
  same content; the Lua hash equals an independent Python FNV over 8 sizes
  either side of the 1 KiB unpack block.
- Hashing reads 1.4 MiB per sample in Lua; cost unmeasured until a real run.
  If it is too slow, hash fewer regions per sample, not a weaker hash.

First real MAME runs (`daytona93`, MAME `dddd7368` + harvest patch, 600
frames of attract, headless, empty NVRAM each run):

- Plugin loads and every tap fires: 3,567,333 events in 600 frames. 68 s per
  run with tracing on this container (MAME reports 14.9% speed).
- Found and fixed: `screen.frame_number` is a method at this MAME, not the
  property the Lua reference documents; and MAME silently drops errors raised
  in tap callbacks, so the first run had 0 samples and no message. Samples are
  now pcall-wrapped and failures reported.
- **MAME is deterministic for Daytona**: two independent runs gave
  byte-identical traces (60,942,182 bytes, 1,154 epochs, 3,567,333 events)
  and identical branch harvests.
- vblank-ack sampling works, but the handler writes the ack (`fffffffe`) twice
  back to back: steady state is exactly 2 samples per frame, the second epoch
  holding only the second ack. Deterministic, so lockstep is fine; every other
  sample is redundant (cost, not correctness).
- ICR = `0f0e0d0c`, set once (synmov at 0x00000a40): IRQ0-3 -> vectors
  0x0c-0x0f, **all priority 1**. Taken in attract: vector 0x0c (vblank) 576x,
  handler 0x0e00; vector 0x0f (sound UART) 51x, handler 0x0f50. Timers
  (IRQ2) never fire; final enable mask = vblank only.
- Geometrizer program port (0x00804000) **is** written: 411,757 writes, from
  epoch 89. TGP FIFO: 773,279 writes, 697,078 reads.
- Harvest: 16 `bx` sites / 49 targets, 2 `callx` sites / 56 targets; no
  `balx`, no `calls`.
- Static reach with the 107 harvested targets as seeds: 89 -> 13,081
  instructions, 0 stops on non-executable opcodes, **0 quirk encodings in
  reachable code**, 31 indirect sites (18 exercised by attract).

Scripted gameplay in real MAME (`scripts/inputs/race_basic.txt`: 3 coins,
start, confirm selects, hold accelerator; 6,000 frames, no steering):

- Replay self-check passes in real MAME: "replay matched the recording for
  5997 frames", in two independent runs; the two branch harvests are identical.
- First attempt stayed in attract: default settings take 3 coins per credit
  (screen showed CREDIT 1/3). With 3 coins, snapshots show car select, then
  the Beginner course, lap 2 of 8 by frame 6,000.
- Full run 103 s emulated at ~14.5% speed, with or without tracing (MAME's
  software 3D dominates; the Lua taps cost ~nothing).
- Race harvest: 20 bx sites / 53 targets, 6 callx sites / 113 targets.
  Interrupts over 6,000 frames: vblank 5,975, sound UART 4,820, timers never.
- Attract + race seeds (168): static reach 21,850 instructions (attract alone
  13,081), 26 of 35 static indirect sites exercised, still 0 non-executable
  opcodes and 0 quirk encodings reached.

Harvest over 15 scripted runs (attract, races, manual gearbox, time attack,
test mode; all replays self-checked "matched"):

| runs | merged seeds | static reach | indirect sites hit / found |
| attract | 107 | 13,081 | 18 / 31 |
| + race | 168 | 21,850 | 26 / 35 |
| + manual, time attack, test mode | 319 | 23,138 | 46 / 67 |
| + round 3 | 333 | 23,258 | 47 / 68 |

- Still 0 non-executable opcodes and 0 quirk encodings in reachable code.
- FP in reachable code: 108 instructions, only `cvtri` 39, `cmpr` 37, `cvtir`
  22, `scaler` 8, `cvtzri` 2. No FP arithmetic, transcendentals or extended
  forms: the FP oracle question shrinks to five operations.
- Reached and confirmed by snapshot: Beginner race (auto and manual with
  shifting), time attack (start + accelerator at car select), test mode menu
  and sound test.
- Not reached: Advanced/Expert courses (circuit select stays on Beginner with
  steering pulses held 30 frames at 0xe0 from frame 1560 and from 1450), TGP
  and memory test items (7 red presses from frame 1500 land on SOUND TEST
  twice, deterministically). Cause unknown; not guessed further.
- Circuit and car select confirm on an accelerator press ("step to choose");
  holding the accelerator from the start picks the defaults.

FP, step 1 (`src/i960/fp`, `tests/test_fp`, `tests/fp_vs_mame`):

- Operand forms measured: all 108 reachable FP instructions use g/l registers
  (single precision in and out); none uses fp0-fp3 or FP literals. AC =
  `3f001000` in all 1,153 attract samples: round to nearest, exceptions masked.
- Reference = SoftFloat 3e extF80; native fast paths = host float, round to
  nearest. Exhaustive proof: cvtri, cvtzri, cvtir over every 2^32 input, scaler
  over every 2^32 single at n = 0, 1, -1, 127, 128, -126, -127, -149, -150, 254,
  -300; 0 mismatches, ~5 min on 4 cores. Plus 36 hand-computed IEEE values,
  specials cross-product, 2^26 random each. A MAME-style fast cvtri
  (`std::round`) fails with 130,905 mismatches (quick run), so the proof bites.
- Bug found on the way: SoftFloat's `extFloat80_t` field order depends on
  `LITTLEENDIAN`, defined only in its private platform.h; C++ callers saw the
  other order and every result was wrong. Now a public definition.
- MAME vs model, every input (`fp_vs_mame`, ~15 min on 4 cores):
  | op | inputs | disagree (MAME on x86-64) | cause |
  | cvtri | 4,294,967,296 | 8,388,608 | every exact .5 tie: MAME `round()` away from zero, IEEE to even |
  | cvtzri | 4,294,967,296 | 0 | |
  | cvtir | 4,294,967,296 | 0 | |
  | cmpr | 268,435,456 pairs | 0 | |
  | scaler | 77,309,411,328 (18 exponents) | 14 | 0 x 2^n (n >= 1024), inf x 2^n (n <= -1075): MAME pow() gives NaN |
  MAME built for ARM64 also differs on 830,472,191 cvtzri and 830,472,191 +
  8,388,608 cvtri inputs (NaN, out of range): its C casts are UB and
  saturate, so MAME's own result is host-dependent there.
- Practical risk for lockstep: a Daytona cvtri on an exact .5 value. The
  trace diff will show it as a register/RAM divergence right after a cvtri.
- MAME never sets FP exception flags in AC; the model reports them. Where the
  i960 records them and whether Daytona reads them is unconfirmed.

M1 groundwork:

- UART-interrupt lockstep: option (a) chosen (safe points in the shipped
  build; a test harness replays MAME's delivery points).
- Delivery points are keyed by MAME's completed-instruction count (patch):
  a stalled FIFO op counts once. Attract, 600 frames: 70,926,456
  instructions, 1,882 interrupt events (1,254 line changes, 622 immediate
  takes, 5 pending-table takes). Two runs: identical IRQ logs and traces.
- Stalled accesses: MAME's `i960_stall()` rewinds IP to PIP, so the plugin
  marks an access with ip == pip as stalled (new record types 0x12/0x13);
  comparisons drop them by default.
- Read taps now cover every non-RAM range the i960 reads (irq, timers, geo,
  copro status to 0x3f, comm, renderer), so the harness can answer them all.
- `scripts/m2import.py` builds program.bin and main_data.bin from the user's
  zip (CRC-checked, MAME's layout) into git-ignored build/rom_cache.

M1 reference core (`src/runtime`, `tools/m2replay`, test only):

- Semantics: MAME's `i960.cpp` transplanted (BSD-3, notice kept); runs at
  36 M instructions/s interpreted.
- Bugs found on the way to MATCH, in order: (1) the bus treated
  read-only-tapped ranges as write-checked; (2) **MAME's ldl/ldt/ldq and
  stores advance the address only on regions flagged BURST** (RAM/ROM, geo
  program port, TGP function port, comm); elsewhere they repeat the address
  (FIFO pops). The bus now carries MAME's BURST flags per region; (3) **the
  plugin's region hash was wrong**: `read_range(first, last, 32)` steps one
  *byte* at a time, so it hashed a dword at every byte address. Fixed with
  step 4; MAME-vs-MAME comparisons had still passed because it was
  deterministic; (4) buffer RAM is written by the geometrizer data port and
  by the TGP (`copro_tgp_memory_w`), not only the i960, so in M1 it is
  treated as a device: the i960's accesses are recorded and replayed, and
  its hash is left to M2; (5) the plugin's own `read_range` fired the
  buffer-RAM read taps while hashing; taps are now suppressed while sampling.
- A per-instruction log on both sides (`M2TRACE_PCLOG`, `M2REPLAY_PCLOG`:
  count, PIP, AC, register-file hash) located bug (2) at instruction 109.
- Mutation check: addo off by one when src1 == 1 (fired 129 times) diverges
  at epoch 3; one program-ROM byte flipped (copied to RAM at boot) diverges at
  epoch 0. Three earlier mutants never fired and so proved nothing either way
  (operand 12345, base 0x00500000, a data-ROM byte attract never reads).

Real program image (`daytona93`, epr-16530a/16531a, counts only):

- Linear sweep of the 256 KiB image: 55,098 lines, 16,280 undecodable words,
  7,356 flagged quirks, 759 non-executable opcodes. Mostly data decoded as
  code; not meaningful as code statistics.
- Recursive descent from the boot record (`i960dis --follow`, with the
  0x00220000 mirror): reset IP 0x860, 0 interrupt handlers (the PRCB's table
  is in RAM), 4 system procedures; 89 reachable instructions, 1 indirect site,
  0 quirks. The reset path ends in `b .` idle loops. Static analysis cannot
  get past boot without harvested targets.
- Mainline Ghidra has never shipped an i960 module (checked HEAD `8e9a8e7a`,
  the last 40 release tags, and full history). The user pointed to the
  third-party mumbel/ghidra_i960 (Apache-2.0), now used.

Ghidra SLEIGH cross-check (mumbel/ghidra_i960 `727ef787` via pypcode 3.3.3):

- All 23,258 reachable Daytona instructions: 0 differences (validity,
  mnemonic, length, target, operands).
- 1,000,000 random words, 1.5 s per 100k: after normalising syntax (MAME omits
  a x1 index scale, prints negative displacements unsigned, prints mode-5 as an
  absolute address; Ghidra prints `disp (ip)`), 72 differences in 5 groups,
  all known: MAME's disassembler names the integer src1 of cvtir/cvtilr/
  scaler/scalerl as an FP register (executor `get_1_ri` and SLEIGH read an
  integer); and SLEIGH decodes `movre` only at 0x6e1, MAME also at 0x6e9.
- The cross-check found four more encoding classes MAME treats specially;
  now decoder quirks: `sfr` (s1/s2, COBR bit 0: Cx special-function
  registers, ignored by MAME), `literaldst` (literal destination: MAME
  fatalerror, so no longer executable), `fpliteral` (FP literal other than
  fp0-3/+0.0/+1.0: MAME reads 0.0), `testfields` (test* with non-zero unused
  fields). 0 of any quirk in reachable code.
- Mutation check: making MEMB mode 7 read a displacement gives 2,871
  differences, exit 1.

Also found:

- A separate geometrizer (0x00800000 / 0x00804000) walks the display list in
  buffer RAM at vblank. It is not the TGP. MAME's is HLE in host `float`.
- The SCSP figures in the MiSTer core's design study are for 2A-CRX in general;
  its README confirms Daytona's working sound is 68000 + FM + MultiPCM. Both
  sources agree with MAME.
- MAME `subc` carry was fixed upstream since MAME 0.289; `addc` was not
  (operands still `uint32_t` before widening, `i960.cpp` ~line 1359).
- The design doc's "15 kHz capture" was wrong for this timing; changed to
  24 kHz medium resolution, pending PCB confirmation.
- macOS (Apple clang, arm64) failed to link `test_fp`/`fp_vs_mame`: C++ sees
  SoftFloat's globals as `extern thread_local` and calls a TLS wrapper
  function that the C (`_Thread_local`) definitions never emit. Linux accepts
  it, which is why it went unnoticed. C++ now gets `__thread` (GCC/Clang) and
  MSVC keeps `thread_local`. Checked: macOS `./setup.sh` builds with 5/5 tests
  passing; Ubuntu 24.04 arm64 GCC 13 and Clang 18 build `test_fp` with 0
  mismatches. Windows is untested, but MSVC's definition did not change.
- The harvest's seeds covered only the states the scripted MAME runs
  visited. The game reaches code four other ways, all now scanned by
  `scripts/seed_scan.py` to a fixed point (recompiling each round):
  - game-mode table at `0x18cc`, 31 entries, `ld 0x18cc[g0*4]; callx`
    at `0x18bc`, index the mode byte at `0x5010a0` (harvest reached 9);
  - task state chains: each state stores the next state's address with
    `lda` (`0x1dd4`: `lda 0x2266f8,r5; st r5,0xc(r3)`; `0x2266f8` itself
    stores `0x226764` at `0xc(g13)`);
  - jump tables read as `ld T[r*4]` then `bx` (`0x5808`, masked to 4) or
    `lda T[r*4]` then `ld`, `callx` (`0x225010`);
  - handler addresses in ROM data: 24-byte object records in the program
    ROM (`0x2375bc`: `0x223078` then floats) and tables in the main data
    ROM (`0x28660cc` -> `0x2265c4`, a bare `ret`).
  A candidate counts only if `i960dis` decodes it to a `ret` or branch with
  no `?`, `!noexec` or `!quirk` first. That rejected float constants that
  `lda` loads (`0x50d0` = 1.0f, `0xcdd8`). From the committed seeds the scan
  finds 275 entry points in one round and nothing in the next; 248 of them
  are the added seeds, the other 27 fall inside code those reach.
- Geometrizer reads past the end of memory (time attack, frame < 6,000):
  texture point/header data (`GeoPtr16`, 4-8 words from a masked start) and
  a polygon RAM walk (`GeoPtr`, index 32,768 of 32,768). MAME's raw
  pointers read whatever follows the array there. The display list is not
  ours at fault: time attack matched MAME to the word, buffer RAM included.
  Now wrapped to the memory size, as a hardware address counter would;
  unconfirmed on the PCB.

## What not to re-propose

- SCSP for Daytona. It is the Model 1 sound board (MAME `model2o` config and
  the MiSTer core's working sound on hardware).
- Five TGPs. One device; "5x" was a board-level package count.
- MAME's frame notifier as the lockstep sample point (see Trace tooling).
- `read_range(a, b, 32)` without a step of 4 (reads every byte address).
- A mutation test whose mutant is not shown to fire.
- Using MAME's disassembler as the decode authority. It is the text oracle
  only; semantics come from the executor.
- `THREAD_LOCAL=thread_local` for C++ users of SoftFloat (breaks the macOS
  link; see Findings).
- Seeding one "no recompiled code at X" at a time. Each state stores the
  next, so the game stops again one state on; run `scripts/seed_scan.py`.
- A function-start test (previous word is `ret` or `b`) on its own for code
  pointers: it rejected `0x2266f8`, whose previous word is the second word
  of an 8-byte instruction. `seed_scan.py` uses it only for the data ROM.
- Every aligned word of the 32 MB data ROM as a candidate without that test:
  5,497 candidates, mostly chance matches in graphics data.
- A TGP microcode ROM dump. The program is uploaded at boot from the game's
  data ROM; dump TGP program RAM only after the upload, or it is zeros.

M1 native (`tools/m2recomp`, `tools/m2native`, `src/runtime/lockstep`):

- One label per instruction; operands, branch targets and FP fast paths
  resolved at recompile time; `goto` for direct transfers, a dispatch switch
  for indirect ones. Unknown instructions or FP operand forms stop the
  recompile (exit 1), so nothing is left to run at runtime.
- Lockstep (`src/runtime/lockstep`) applies MAME's interrupt lines and takes
  at the same completed-instruction counts; shared by both harnesses.
- **IAC 0x93 (reinitialise) is an indirect transfer**: boot reinitialises to
  0x924 through `synmovq`. The first native run stopped there (no code); the
  harvest patch now logs IAC targets and the seeds include it.
- Mutation check: the generator's addo template off by one when src1 == 1
  diverges at epoch 3.
