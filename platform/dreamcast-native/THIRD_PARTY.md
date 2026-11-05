# Dreamcast-native dependency record

The root [THIRD_PARTY.md](../../THIRD_PARTY.md) remains authoritative for MAME,
SoftFloat, ymfm and the shared runtime. No third-party source is vendored in
this folder: what the port builds with is fetched into `extern/` (git-ignored)
by `scripts/fetch_*.sh`, or comes from the installed KOS tree.

| Component | Upstream | Commit | Licence | Use | Changes |
| --- | --- | --- | --- | --- | --- |
| enDjinn | https://github.com/dfchil/enDjinn | `725f8b192c18ba56298eca1b5eaf8abc216e67be` (origin main, 28 October 2026), fetched into `extern/endjinn` by `scripts/fetch_endjinn.sh`; `Makefile` and `recipes/build_assets.sh` use it unless `ENDJINN_ROOT` names another checkout | MIT | Dreamcast frontend framework: initialization, mode/update loop, input, PVR lists, quick font, `base_link.mk`/`environ.sh` build integration and build-time asset tools. Linked into the native ELF (`libenDjinn.a`). | None. |
| SH4ZAM | https://github.com/gyrovorbis/sh4zam | `70dbb177b20c1734fbe4f97df0f8c6ec87a8e8d3` (v0.10.0), fetched into `extern/sh4zam` and built there (`lib/libsh4zam.a`) by `scripts/fetch_sh4zam.sh` | MIT | Linked into the native ELF for presentation math (transforms, lighting, clipping) and the `nm_mem` copy/fill dispatchers; not used for gameplay-visible results, which stay exact. `NATIVE_SH4ZAM_ROOT=` links KOS's installed copy instead. | None. |
| KallistiOS | https://github.com/KallistiOS/KallistiOS | `368a2eb9584a7d130a6e77e299468c0b64fa1a9f` (the installed tree the port is built and measured with) | KOS License (BSD-3-Clause; bundled tools/components have their own licenses) | SH-4 toolchain integration and Dreamcast runtime, PVR, Maple input and loader/fileserver support. | None by this project. |
| AICAflow | https://github.com/dfchil/aicaflow | `f4481210a8a9fa8248a74b6e6e56da38c0df5c61` (origin main, 16 October 2026; firmware ABI 8), fetched into `extern/aicaflow` by `scripts/fetch_aicaflow.sh` | MIT | Dreamcast AICA audio runtime: SH4 library `driver/sh4/libaicaflow_host.a`, checked-in ARM7 firmware `firmware/aicaflow.drv` (sha256 matches its `manifest.json`), host validator `driver/build/afx_validate` used by `recipes/build_music.sh`. Linked into the native ELF (`include/audio/sound.h`), firmware embedded; KOS's own sound startup is wrapped away. It takes the AICA's SH4 interrupt (`ASIC_EVT_SPU_IRQ`) for the firmware's status reports; nothing else may hook it. | None. Its `dependencies/enDjinn` submodule is not initialised; the port uses its own enDjinn. |
| AICAforge | https://github.com/dfchil/AICAforge | `e77de85282035a679887f3166cf7caf6a0d61b16` (origin main and AICAflow's pin), at `extern/aicaflow/dependencies/AICAforge`, initialised and pinned by `scripts/fetch_aicaflow.sh`; its own AICAflow SDK pin `53c464db213066d95fd9fd4390e2737e193a4c5f` | MIT | Host authoring tools; `afx_vgm` converts the MultiPCM VGZ logs below into AFB/AFX/AFC/AFV, and the effect VGMs of `tools/prepare_sfx.cpp` (`recipes/build_sfx.sh`); `afx_bank --merge` makes the effect bank. Invoked locally, not linked into any target. `prepare_sfx.cpp` links AICAflow's `driver/format/src/codec.c` (MIT) and restates `afx_vgm.c`'s MultiPCM-to-AICA formulas (`source_pitch`, `source_pan`, `source_mix`) so its engine tables agree with the flows. | None. |
| vgmrips "Daytona USA (Sega Model 2)" pack | https://vgmrips.net/packs/pack/daytona-usa-sega-model-2 | Archive sha256 `ac086fd5a246a9cfc125374dd43084d35e0d79dd7ef837b0bdf04784035234a2` (13,639,027 bytes) | Game-derived: Sega's music and MultiPCM samples, logged by vgmrips. Not redistributable by this project. | User-supplied input of `recipes/build_music.sh` (downloaded in a browser, like the ROM set) and of `tools/check_music_map.py`. Outputs stay in ignored `build/`; only their sha256 are committed (`recipes/music-expected.sha256`). | None. |
| pvrtex (host tool, `utils/pvrtex` of the installed KOS tree) | https://github.com/KallistiOS/KallistiOS/tree/master/utils/pvrtex | KOS tree at `368a2eb9584a7d130a6e77e299468c0b64fa1a9f` (pvrtex 2.00, last changed in 833c4c3d) | LGPL: its README says it uses FFmpeg code (LGPL) and so is LGPL as a whole, and that files not from FFmpeg may also be used as public domain/BSD/MIT. The FFmpeg-derived files (`elbg.c`, `avstring.c`, `mem.c`, `log.c`, ...) carry LGPL-2.1-or-later headers; the stb headers are MIT/public domain. KOS's `doc/LICENSE.md` says utils carry their own licences. | Invoked at build time (`recipes/build_assets.sh`, found on PATH after sourcing enDjinn's `environ.sh`) for twiddled/VQ texture encoding and previews. Not linked, copied or distributed; only its output (from the user's ROM) is used, and that stays in ignored `build/`. | None. |
| Pillow (already-installed host package) | https://github.com/python-pillow/Pillow | Installed 12.2.0 | MIT-CMU | Optional `check_materials.py --previews` PNG reading to cross-check actual decoded VQ bytes. Not needed by the default checker, ROM-free tests or Dreamcast target. | None. |
| Intel 80960KB Programmer's Reference Manual | https://www.arithmazium.org/classroom/lib/Intel_80960KB_Programmers_Reference_Manual_Mar88.pdf | March1988; archived PDF SHA256 `5ffd02f50251498875766421e484bf9451b70c2b4b3dab6cea63027200b1fbbd` | Intel copyrighted public documentation; read only | Pages12-23 and11-50, visually checked for masked NaN conversion and distinct magnitude-overflow behavior. | No copied manual text or distributed PDF; analysis and native implementation are our own. |

The shared runtime carries read-only hooks for the port's host tools behind
`M2_DC_NATIVE_HOST`, off by default (`src/runtime/cpu.cpp`, MAME's `i960.cpp`
row of the root record: a pre-call and a normal-return observer;
`src/runtime/geo.cpp`, its geometrizer row: an object observer and an
inspection instance).

## Sources the native code follows

Native `reflection.h` independently expresses owned daytona93 routine6f24..7020
as integer texture-update intents. Host-only checks execute original19ee0 UV
uploads and compare observed window RAM geometry to its29c0000 ROM initializer.
No Sega asset, board runtime, texture converter or new dependency enters the
target. Course0 practice now advances this phase on fixed ticks and applies
offsets to its owned CPU mesh through the existing enDjinn/SQ renderer.
Host PlayerWindowExport reuses the attributed model decoder below, replacing
only its private raster snapshot's UV/header data with the owned ROM initializer.
Generated meshes/VQ assets stay local and ignored, not distributed source.
The combined-car packer independently compares the canonical window strip,
positions, normals, UVs and headers to SHA-validated owned main ROM before
admitting this specific ROM-initialized RAM model. No general RAM freeze path.

Native DNT3 material backing uses the already-installed KOS-ports zlib 1.3.1
(Jean-loup Gailly and Mark Adler, zlib license). Host preparation uses Python's
standard-library zlib; host C checks link the system zlib via CMake. No zlib
source is copied or modified. Each prepared VQ DcTx file is losslessly wrapped
in an independent checked stream; the GPU's VQ16 bytes/alpha are unchanged.
Runtime uses one fixed allocator workspace and a bounded decoded-data FIFO,
not a custom codec, per-frame file reads, or re-encoding. The installed
`zlib/inst/include/zlib.h` carries its copyright and full license notice.

Native `nm_player_road_tail` composes existing13a14 lateral measurement with
independently expressed10bac path offset,13de0 low16 lap/profile progression,
and13e3c..13ec0 shared course marks. One caller-owned byte array and timer
replace the source shared-memory addresses; no bus/runtime is imported.
Immutable DNR2 profiles/counts and DND23 math are reused without new ROM data.
Host-only tests execute original i960/TGP instructions and compare whole entity,
shared marks/timer and untouched memory. Source-entry checks remain distinct
from an autonomous native gameplay loop or target arithmetic validation.

Native `nm_player_tilt_step` independently expresses owned-program1550c..15674:
surface/speed/boost tilt waves, signed phase indexing and narrow shared stores.
DND23 reuses existing signed-byte shake samples and adds only the two missing
235280 halfwords and four235b54 boost halfwords (12 bytes). These ROM-derived
values stay in ignored local packs, not source. Whole-memory checks execute
the original routine through the existing host i960 oracle; no interpreter,
board memory map, ROM reads or new third-party dependency enters the target.

Native `nm_player_body_angles_step` independently expresses owned daytona93
11aa4..11bd0. It reuses existing DND inertial scale, wrapped integer widths
and80960KB masked-NaN conversion semantics; no new assets or dependency.
Two ordered TGP multiplies become strict scalar operations, verified against
the original host i960/TGP routine. No approximate SH4ZAM substitution or
target emulated instruction/bus path is introduced. Ground/force outputs feed
native render angles in the composed player update; binding them to a live
car model remains separate from diagnostic source-entry comparisons.

Native `checkpoint.h` independently expresses owned daytona93 routines
132b8..134f4 as integer lap/checkpoint state and an explicit lap-time write.
DNR2 appends ordered uint16 checkpoint profiles from the tables selected by
02872974; original upper halfwords are unused. ROM-derived packs remain local
and ignored. No target bus, instruction runtime, TGP, sound or new dependency.
The existing original-i960 scratch oracle checks whole memory; actual-player
checks feed retained native road/profile/flag outputs into this operation.
These source-entry diagnostics are not target replay assets or an autonomous
inter-frame race. Shared race fields must be round-tripped between cars.

Native `nm_race_timers_step`/`nm_traffic_pass` independently express owned
daytona93 routines12150/125b0..127fc, preserving signed widths, wrapped gap
comparisons, pointer-identity branch precedence via native indices, and silent
shared signal intents. Existing distance math and host i960/TGP oracle are
reused; no new table/dependency or target board/sound runtime. Synthetic
whole-memory tests and actual retained-state checks are diagnostic evidence,
not recorded gameplay consumed by the target.

Native `nm_traffic_near` independently expresses daytona93 routines12464/139d8
from owned-program analysis. It reuses the validated distance helper, native
indices and explicit caller progress inputs; no new dependency, ROM pack or
target bus. Preserve mixed16/32-bit marker stores and the caller's identity.
The source race worker initially follows null links into immutable ROM zeros;
the host checker validates/folds these inputs instead of inventing a vehicle
or importing guest-pointer semantics. Original i960/TGP whole-memory checks
include256 synthetic external-worker cases as well as8192 linked-car cases.
The actual-call checker carries native body state from ordering/proximity
into collision preparation/responses, not target snapshots or a replay.

Native `nm_traffic_order` independently expresses daytona93 routines1225c/122dc
from owned-program analysis. It preserves their single-pass ordering, signed
lap rotation, stable ties and alias-sensitive ring-link writes with native
indices. No new ROM table, dependency or target board runtime. Shared synthetic
fixtures use the existing host i960 whole-memory oracle; actual predicted
links feed the native collision preparation checks. Diagnostic source snapshots
are never target assets or a replacement for a live race update.

Native `collision.h`1406c..1417c pair traversal is independently expressed from
owned daytona93 i960/TGP analysis, using native indices and the previously
verified broad-phase helper. DND22 folds the four signed14-entry schedules at
13ec4 into56 window indices offline; assets remain ignored/local. The original
loop and TGP gates are exercised by the existing host oracle with explicitly
synthetic response effects, separate from whole-response live checks. No
board scheduler, instruction interpreter or new dependency enters the target.

Native `collision.h`13f08..14068 pool/window preparation is independently
expressed from owned daytona93 i960/TGP analysis. It reuses the verified
one-body travel helper and substitutes bounded native indices for guest
pointers, preserving linked order, profile grouping, partial-byte resets and
primary-car identity. No new ROM table, pack, target scheduler/bus or dependency.
Shared synthetic fixtures and host-only whole-pool observations verify it;
the observed snapshots never become target assets.

Native `collision.h`14180 contact detection, including1469c/14794 record and
shared-signal effects, is independently expressed from owned daytona93
i960/TGP analysis. It reuses `nm_wheels_transform` and the command36 ordered
half-plane helper, now also checked directly against commands31/32. DND21
appends eight authored low signal bytes from235b40 in world-vertex order;
the corner inverse is verified offline instead of duplicating another table.
Assets stay ignored/local. Whole-memory scratch/actual-call tests use the
existing host oracle; its synthetic RAM now correctly advertises burst access
for multiword instructions. No target bus, generated-code transplant, new
dependency or approximate gameplay arithmetic.

Native `collision.h`140a4..14138 pair-candidate filtering is independently
expressed from owned daytona93 i960/TGP analysis. It reuses the existing
table-refined length, preserves the source immediate operands and raw-word
comparison, and retains the outer-loop centre snapshot across responses.
No new data pack, generated-code transplant or target board runtime. A const
host-only TGP data-word diagnostic reads the reference snapshot without
advancing execution or consuming FIFO data. Shared synthetic tests feed both
the independent host oracle and the optional SH4 startup diagnostic.

Native `collision.h` now independently expresses13f90..13fdc one-body travel
history and14630..14688 response dispatch/return values from owned daytona93
analysis. Existing math/choice helpers and DND20 data are reused unchanged.
Host-only scratch and actual-call checks use the existing i960/TGP oracle;
no generated routine transplant, new dependency or target board runtime.

Native `collision.h`14904..14a20 response choice is independently expressed
from the owned daytona93 image, using existing table distance/angle helpers.
DND20 folds four authored235b08 byte offsets into four bounded wheel indices;
the resulting asset stays ignored/local. Host oracle compares the selected
tail branch and unchanged memory, not a transplanted generated routine.
No new dependency, target bus/runtime, or approximate gameplay math.

Native `collision.h` now independently expresses owned daytona93 routines
14fcc..15234 (severity/damage/flags) and14c54..14e6c (speed transfer, correction,
yaw and shared effects). Existing crash entry is shared through `damage.h`.
DND18 added56 bytes of authored thresholds/pre-folded flag masks; DND19 adds
the original four-byte speed step, not a reconstruction of a rounded value.
These are ignored local ROM-derived assets. Host-only oracle uses existing
i960/TGP code; an optional normal-return observer reads results without driving
execution. No generated source transplant, new dependency, target board runtime
or approximate gameplay math is introduced.

Native `road.h`13a14 lateral measurement and `collision.h`15238 timers/relative
angle are independently expressed from owned daytona93 program analysis.
They reuse existing table sine/cosine/angle math. DND17 adds only20 low bytes
from the original correction/inhibit halfword tables as ignored local data;
the zero default selector is verified offline. No generated source transplant,
board runtime, new dependency or approximate gameplay math is introduced.

Native `collision.h` independently expresses owned daytona93 routines14a5c
and14b58 (pair separation/contact-side writes), reusing the existing native
table sine/cosine helper. Whole-pair host checks execute the existing i960/TGP
reference. No generated source transplant, new table/asset or linked dependency.

Native early-player e5f4/15314/154d0/158c8 and off-course10a14/10a34 behavior
is independently expressed from analysis of the owned daytona93 program,
reusing existing native math/contact helpers. DND16 appends original byte-indexed
shake/inhibit values and two coefficients as ignored ROM-derived local data.
Off-course recovery reuses DNC3's authored height/heading and offline ordered
vertex-average centers. No generated code transplant, new linked dependency,
target board runtime or new approximate gameplay arithmetic is introduced.

Native `nm_player_active_step` composes the existing lifecycle/input/gears/
resistance/proximity and player helpers, with independently expressed held
caller timer effects from the owned daytona93 program. Host checks reuse the
existing i960/TGP oracle and compare full shared state, not only selected
outputs. No generated source transplant, new pack/dependency or target board
runtime; ROM-derived assets and test outputs remain ignored local data.

Native `player.h` now composes the existing independently expressed dynamics,
road and boundary helpers in the owned program's d8c8..d8fc order. Rear-surface
timers16da4 are independently expressed from that analysis and checked with
the existing host i960 reference. No new dependency, source transplant or
ROM-derived target code; immutable prepared assets remain ignored local data.

Native `nm_boundary_step` independently composes the locally analysed complete
16060 behavior from the owned daytona93 image using existing helpers. DND15
pivot sides/full unsigned-byte divisor tables are ROM-derived ignored local
data, not distributable repository assets. The host oracle uses existing
i960/TGP code and stops before its explicit DIV0 HACK; no new dependency,
copied generated source or accepted approximate gameplay math.

Native road selection10980..10a10 is independently expressed from the owned
daytona93 program analysis, reusing existing native cursor/math helpers.
DNR1 profiles/maps/combined immutable flags are ROM-derived ignored local
output, not distributable repository assets. Host tests execute the existing
i960/TGP reference against original tables; no generated source is copied
into the target and no new third-party dependency is added.

Native `damage.h` behavior is independently expressed from analysis of the
owned daytona93 image (16bd4,15cc8/15cd4,15e54..15fc0). Its oracle executes the
existing host i960/TGP reference; no generated code is copied into the target.
DND13 model keys/thresholds/animation rows are ROM-derived ignored local data,
not third-party source or distributable assets. No new linked dependency.
The same independently expressed helper now composes15fc4/16014/15d60 and
637c's state effects. DND14 adds only32 offline reset bytes from the owned
image; live backup-RAM descriptor selection stays explicit input. No generated
source or additional third-party implementation is copied/linked.

The native drive helper `include/drive.h` adapts the sine-table
quadrant lookup from `src/runtime/tgp.cpp`, following the MAME TGP I/O row
above (same pinned commit and BSD-3-Clause holders). It reads one bounded
quarter-wave table directly, retaining reflection, endpoint clamp and sign
bits; no TGP program, bus, register state or scheduling is imported.
`prepare_drive.py` uses the same reciprocal-port exponent/index semantics to
precompute the fixed divisor2000's refined reciprocal from the owned table ROM.
`include/force_math.h` also adapts that row's reciprocal,
inverse-square-root and arctangent table indexing/exponent/quadrant semantics
into pure vehicle math functions. No register file, FIFO, dispatcher, program
or instruction execution is imported; BSD-3 holders and pinned provenance
remain the same.
`include/tires.h` reuses those existing math functions for the
locally analysed tire-response operation; no additional third-party code or
target TGP runtime is introduced. Its three coefficients are extracted only
into the user's ignored DND7 pack, never embedded in source.
`include/gears.h` likewise reuses the existing reciprocal math for the locally
analysed automatic/manual gear operations. DND8 adds owned-ROM shift scales,
signed RPM thresholds/filter limits and selector mapping only as ignored data;
no new third-party implementation or simulated input device is imported.
The resistance/proximity steps in `include/forces.h` reuse those same table-based
length/angle helpers. DND9 adds five locally extracted resistance coefficients;
ordered neighbor positions are live inputs, not copied board state or assets.
`include/motion.h` reuses the same sine/Cartesian helpers for locally analysed
normal/spin world movement and held displacement. DND10 adds32 authored spin
coefficients and one low-speed distance, extracted only into ignored data.
No new third-party implementation, recorded trajectory or target TGP runtime.
`code/contact.c` reuses the same reciprocal helper for the locally analysed
wheel-surface lookup. `prepare_contact.py` repacks the owned course grid,
ordered candidates and collision quads into ignored DNC1 data. No generated
TGP program, FIFO/bus machinery or third-party implementation is imported.
`include/wheels.h` reuses those same sine/angle helpers for locally analysed wheel
transforms, suspension and road-frame math. DND11 adds four authored wheel
points; DNC2 adds the source quad heading. Both remain ignored owned-ROM data.
No new third-party code, target TGP matrix stack or recorded vehicle poses.
`include/path.h` reuses the same validated angle helper for locally analysed
forward/reverse authored-path selection. `prepare_path.py` writes only owned
ROM-derived DNP1 rows/guard data under ignored build output; no new third-party
implementation, generated instruction runtime or captured trajectory is used.
`include/guided.h` reuses the same validated helpers for locally analysed PLAYER
guided movement. DND12 adds an owned-ROM adjustment limit and256 offline-folded
player-mode coefficients, only in ignored data. No new third-party code,
opponent identity approximation, board runtime or recorded motion is imported.
`include/player.h` composes these existing locally analysed operations using
explicit native state and their existing mathematical helpers. Scenery-cell
selection reuses contact-coordinate rounding. The host-only block/continuity
checker reads the existing reference oracle; no new third-party implementation,
dependency, generated instruction runtime or recorded target trajectory.
`include/boundary.h` reuses that same table-refined division for the locally
analysed segment test167b4. Its host checker executes the existing i960/TGP
oracle on synthetic and actual-call inputs after reference execution. No new
third-party code, geometry library or target instruction runtime is introduced.
Its boundary-pivot response composes the locally analysed1638c..167b0 branch
with the same helpers and existing DNC2 quads. The silent target exposes sound
intent as an event; the host oracle checks the source's gated request. No new
third-party implementation, sound runtime or embedded ROM parameters is added.
Its16060 response selection also reuses the existing contact query and DNC2
headings; branch-order/height thresholds come from local analysis. The host
oracle stops before the selected reaction, without importing a target bus or
another third-party dependency.
Recovery161f8..1632c uses the same division helper and locally analysed
command2b/2c behavior. DNC3 stores the owned-ROM height and an offline float32
vertex-center sum, only in ignored build output. No new third-party code,
target command interpreter or sampled recovery trajectory is added.

The Dreamcast indexed System24 compositor
(`tools/system24.hpp`) follows the System24 row above and
the existing runtime/desktop shader's scroll, window, category and opacity
rules. BSD-3-Clause attribution is retained; palette compaction and PVR
presentation are local frontend code.

The Dreamcast offline texture preparer (`tools/dreamcast_assets.h`)
and its shared live material-state helper (`tools/material.h`)
also follow the renderer row above for texel shading and gamma equations,
with BSD-3-Clause attribution retained; texture addressing reuses this
repository's `raster_texel.h`. No pvrtex implementation is copied into the
project; it is invoked as an external host program only.
The native VQ byte checker reads the installed DcTx header/decoder as format
references only; its small base-level decoder is local validation code, not
a copied pvrtex implementation. No encoder/decoder tool is linked into DC.
`prepare_material_crops.py` reuses that local decoder to verify lossless cropping
of already encoded VQ codeword indices. It adds no upstream code/dependency;
codebook pixels are retained, and original VQ loss is not claimed removed.
The optional native framebuffer comparison calls KOS's existing
`vid_screen_shot_data` API; no screenshot implementation is copied or modified.

The native source-mesh extractor (`tools/export_models.hpp`)
follows the geometrizer row above for object point order, strip linking,
texture addresses/header deltas and the transform oracle. BSD-3-Clause
attribution is retained. The optional host observer leaves the reference
pipeline intact; no geometrizer runtime is linked into the native DC target.
The complete scenery-table exporter (`tools/export_course.hpp`)
reuses that decoder and an isolated `Geo` inspection snapshot. Its new host entry
point copies mutable state and borrows ROM storage; it invokes the existing
object command rather than transplanting another parser. No additional upstream
code or dependency is introduced, and sampled reference output is checked.

The native triangle projector (`code/geometry.c`) follows
the renderer row's center/CRTC/viewport and perspective-UV conventions. Its
bounded homogeneous clipper and vertex cache are local frontend code; the
old PVR adapter and an independent double-precision clipper are host-test
oracles. Neither the CPU rasterizer nor a board runtime enters the DC target.

The native face-light function (`include/lighting.h`) transplants
the geometrizer row's normal/cross-product, diffuse/specular and rear-bit
equations into a bounded operation on view-space inputs. BSD-3-Clause notice
is retained. No command stream, board runtime or guest state is imported.
The host exporter compares its result against the unchanged reference parser.
