# Design notes of the native port

The root design document (`docs/daytona-usa-recomp-design.md`) describes the
recompiled game. This file holds what the port added to it: corrections found
while reimplementing the game, and the record of the native direction.

## i960 floating point, as the port found it

- `ref_*`: the candidate hardware model. Operands widened to extF80, computed and rounded by SoftFloat 3e in any AC rounding mode, IEEE flags reported. Its invalid conversions all return `0x80000000`; that blanket rule is not a verified model of every i960 conversion (see the manual correction below).
- `fast_*`: separately tested native host operations, round to nearest/ties-even only. Contrary to the earlier wording, the current recompiler does NOT call these for cvtri: `tools/m2recomp/main.cpp` emits `gen::round_to_int` with ties-away in mode0. The native vehicle import retains that finite PC-reference behavior. Historical `tests/test_fp --exhaustive` results compare fast_* against ref_*, not the active generated path: every 2^32 input for cvtri, cvtzri and cvtir, and every 2^32 single for scaler at 11 boundary exponents, 0 mismatches (about 5 minutes on 4 cores). A fast path that rounds like MAME (`std::round`) fails that comparison with 130,905 mismatches in the quick run.

**Manual correction, 2026-10-03.** Intel's public
[80960KB Programmer's Reference Manual, March1988](https://www.arithmazium.org/classroom/lib/Intel_80960KB_Programmers_Reference_Manual_Mar88.pdf),
page12-23, specifies INT32_MIN for a masked NaN-to-integer conversion and sets
the invalid-operation flag. Page11-50 separately specifies signed endpoint
saturation for unrepresentable magnitudes. Both pages were visually checked.
Do not conflate these cases or claim all invalid conversions are verified by
the existing SoftFloat specialization. The native yaw import now handles
masked NaN explicitly; since 16 October 2026 finite and infinite overflow
saturate to the signed endpoint as page 11-50 says (AC 3f001000 masks integer
overflow, bit 12): live play reaches it in the yaw step (fbd0 converts -inf
when the front and rear wheel angles are equal), and the reference agrees on
every row after it (platform/dreamcast-native/tests/test_negative_speed_lockstep.py).
The scratch oracle substitutes only the documented NaN result before the
reference's undefined C++ cast. It does not claim CPU exception-flag parity.
No desktop rounding/conversion semantics were changed.

## Audio

**Dreamcast audio (AICAflow; music only so far)**

The Dreamcast build neither recompiles the 68000 program nor runs the native
sequencer: AICAflow (`extern/aicaflow`) plays recordings of the original board.
The source is vgmrips' Daytona USA pack, converted offline by AICAforge into
one sample bank and control flow per piece (`platform/dreamcast-native/recipes/
build_music.sh`). Gameplay hands the original program's packed sound commands
to one module (`platform/dreamcast-native/include/audio/sound.h`), which alone decides
what plays; which `ae10xx` start produces which recording is proven, not read
from titles (`tools/check_music_map.py`: the native sequencer above against
each log's MultiPCM key-ons). The pack holds the music chip only, so effects
and voices need another source. A recording cannot drop single channels, and
AICAflow flows end, so channel mutes and loop points are approximations that
the map and handoff list.

### Sound effects (AICAflow)

The Dreamcast-native port does not run this backend. Its music is the
recordings of `platform/dreamcast-native/recipes/build_music.sh`; its effects and voices
are prepared offline by `platform/dreamcast-native/recipes/build_sfx.sh` with this
backend's sequencer: `platform/dreamcast-native/tools/prepare_sfx.cpp` plays each
effect command into a MultiPCM VGM for AICAforge's `afx_vgm`, which needs the
driver's register values, so `VoiceEvent` carries them (`rate_code`, `level`,
`pan_code`) and `tracks_idle()` tells when a sequence is over. Desktop and Vita
behaviour is unchanged. Playback on the console: `platform/dreamcast-native/include/audio/sfx.h`;
the commands the native game derives from gameplay state:
`platform/dreamcast-native/include/race_sound.h`; how they are checked against the
original: `platform/dreamcast-native/recipes/README.md`, "Sound effects".

## Native direction and the record of its imports

**Dreamcast-native direction, 2026-10-03 (supersedes the prototype below).**
The active target is now `platform/dreamcast-native/`, started independently. This repository is a behavioral/code reference, not a runtime
architecture to squeeze onto Dreamcast. Import validated pieces incrementally;
do not link the existing GameLoop, board simulation or CPU compositor into
the new target. enDjinn still owns startup, input, the outer loop and PVR;
gameplay state/update and scene residency are redesigned for the console.
Preprocess ROM-derived geometry, textures and stage resources offline into
bounded shared/screen/course/sector packs. Keep live gameplay live, not replayed.
Use 16-bit VQ for richer art, exact PAL4 for suitable UI, and no PAL8.
SH4ZAM may accelerate validated gameplay/TGP-derived math as well as rendering;
24bpp without dithering and silent output remain requirements. RAM, VRAM,
transition overlap and frame time are acceptance criteria before each import.
The working objective and import gates are in `platform/dreamcast-native/HANDOFF.md`.
The desktop reference remains unchanged by this architectural exception.

The native player now starts ate5f4: speed index/wheel phases, impact displacement,
shake/inhibit, lifecycle, input, gears, resistance/proximity and
dynamics/ground/scenery/10980/16060/16da4, followed by off-course10a14/10a34.
Its held branch instead
decrements recovery state and runs movement/ground/scenery through16e10.
520-byte single-copy state/68-byte input; helper views never refresh from
reference snapshots. Low16 input brake writes and32-bit gear brake writes
share one field. Lifecycle effects feed the same tick's input and physics.
Each normal call consumes one selected DNP1/DNR1, allocates nothing and
publishes atomically.5714 actual entry/return blocks and12000 reference frames
match;32768 additional scratch cases cover normal preparation and complete
held chains across four packs. Actual held/guided/crash-animation coverage is
still missing. Adding15314 reduced between-entry X/Z corrections from22 to
one per scenario, Y/yaw zero. The call-interval audit locates the remaining
update in inter-entity collision response14a5c (g8/g9 X/Z stores), not spawn.
Both pair separation variants14a5c/14b58 now use one native helper with12-byte
body views, actual angle/depth/side inputs and existing table sine/cosine.
262144 whole-memory host i960/TGP cases matchf620abb4, covering all angle bits
and same-body store ordering. It allocates nothing and changes no pack/VRAM;
1369 actual pair-entry/return matches (two player pairs) and all12000 unchanged
reference frames validate real call inputs, not autonomous native collision.
The next15238 stage now uses native13a14 lane4-relative lateral measurement,
signed relative angle and authored correction/inhibit timers.16-byte view;
131072 whole-pair cases and65548 lateral cases (five explicit invalid-conversion
rejections) matcha9013475. Another76960 cases cover all1924 authored profiles
across four tables. Native separation feeds this stage without reference state
refresh:1369 actual whole-pair/register matches, all12000 frames unchanged.
14fcc now computes severity/flags/damage and reuses shared crash initialization;
131078 host cases match3004e2a7, including9001 synthetic crash starts and four
explicit conversion-overflow rejections. Its g4 severity is retained for14c54.
The three-operation chain matches1369 actual pairs without body-state refresh;
neither live scenario starts a crash. Original thresholds are NOT sorted:
player low999 exceeds mid120, so the player shake16 branch is unreachable
with these assets. Ordered synthetic thresholds separately test that branch.
14c54 now transfers speed and stores next-tick correction/yaw/shared effects:
32-byte body views,4-byte shared state, no allocation/VRAM.131072 whole-memory
host i960/TGP cases matcha77137ab. Preserve signed25 versus unsigned30 severity
tests, unsigned FLOAT-BIT speed ordering, same-body store order and negative
zero correction. Undefined/nonfinite arithmetic rejects atomically, not with
invented arcade output. The native four-operation chain matches1369 actual
pairs through14c54's return with no between-operation state refresh. A read-only
normal-return observer is null outside host diagnostics. All12000 reference
frames/counts remain unchanged; see HANDOFF. No live crash-start coverage.
14904..14a20 now chooses between the two separation/response chains using
relative travel, overlap depths and selected wheel points.56-byte body views
and20-byte contact input; no allocation/VRAM.196608 host i960/TGP branch and
whole-memory checks match11c5844c, including65536 same-body cases and79376
angular-path cases. Keep strict depth-versus-distance BIT comparisons, early
equal-depth preference and bit15 of wrapped angle differences. Bounds/finite
checks apply only to consumed corners; no invented valid result on failure.
Correction to earlier notes:294/298 are NOT previous positions.13f90..13fdc
computes current1c/24 minus saved28c/290 and then updates the saved position.
These travel deltas are distinct from physics9c/a4. One-body travel preparation
is now native:16-byte state,65536 whole-memory host cases matchd135ea61, but no
actual-entry/list-traversal proof yet.14630..14688 now dispatches empty/aligned,
single-contact and double-contact cases using the existing14904 helper.
Wrapped heading differences[-64,64] suppress only non-double contacts;
flags3 bypasses that gate.28-byte input/4-byte result;393216 scratch branch/
memory cases match7d75ffb0. Actual response chains now reach the parent return:
1369/1369 whole-pair/record/g3 matches;4369 empty and119 aligned returns match
the skip rule separately (return-side evidence, not detection entry/return).
Return values150/300 are deducted at14164 from the caller's1000-unit work
budget, not timers. Preserve traversal order and the separate5/10-unit charges;
an unbounded all-pairs replacement would change behavior.
`nm_collision_prepare` now implements13f08..14068 pool reset, fourteen-body
linked-window/travel preparation and backward profile grouping.44-byte native
index-based view,28-byte ordered index output; uses the existing travel helper,
no new pack/VRAM/heap. The reset prefix excludes the primary in actual observed
calls: preserve that car's flags even when resetting its contact-side byte.
Unselected reset cars retain the high24 bits of2a4, while selected cars receive
the full32-bit relative index[-5,8]. Initial profile threshold is signed,
subsequent profile reads are unsigned, wrapping add truncates only on store.
8192 whole-memory i960/TGP cases match91a31cc8, including3584 outside-primary
and2606 repeated-window cases. Selected/backward links need not be physical
array order. All writes publish only after link/arithmetic checks; unused final
links and unselected positions do not need validation. Actual-call checks
compare whole pool/record before the FIRST contact, or on an empty-pass return,
not after responses have modified the same bodies. See HANDOFF for run counts.
`nm_collision_walk_next` now supplies1406c..1417c traversal:20-byte continuation,
16-byte body probe/link/count view, no heap/VRAM or board scheduler. Start with
budget1000 and cost0; finish each returned pair before advancing with its150/300
charge. Source charges five per nonempty outer group and ten per candidate;
only the inner-candidate boundary checks for exhaustion, including an extra
outer charge after an exhausted last candidate. Cache the outer centre once,
read both raw5c words live and swap the primary to second only for the callee.
DND22/DNDM adds56 offline window indices (four validated permutations from
signed13ec4),280084 resident/280100 file bytes. OldDND1..21 reject.
8192 original-i960/TGP loop tests check28077 candidates/9156 accepted pairs,
3473 primary swaps and5277 exhausted passes, hash577933dd. Their response
effects are explicitly synthetic; live checks separately retain the native
whole pool/contact record across pairs when complete response data is loaded.
This is still not a persistent native race or hardware timing result; native
list/race setup and integration into the enDjinn car update remain unfinished.
The first two1224c calls are now `nm_traffic_order` (1225c/122dc):10-byte body
view and44-entry rank/track index arrays. Preserve ONE descending adjacent pass
per update, not a complete sort. Signed rank key60 sorts only withflags500000;
profile keys are unsigned16. A signed lap advance rotates1..4 leading entries
instead; stable ties and ring-entry0-last stores preserve source aliases.
Ring size is reset_count+1, next links go backward through the array.8192
whole-memory cases hash203b7e22.11516 actual ordering calls feed741232 native
links into the existing collision chain with all12000 reference frames unchanged.
No extra asset/heap/VRAM; tested SH4 stack224 bytes.12464 proximity effects,
remaining race callers and persistent enDjinn car integration remain open.
`nm_collision_detect` now imports14180's wheel-cache preparation, bidirectional
first-corner tests and1469c/14794 contact-record/side/shared-signal writes,
stopping before14630 response dispatch. It reuses existing wheel transforms
and ordered command36 half-planes (independently checked against31/32).
The source is NOT a generic SAT/deepest-overlap solver: test storage vertices
0,1,2,3 against perimeter0,1,3,2, retaining the FIRST hit in each direction.
flags bit5 holds the transformed corners across earlier pair responses;
13f08 clears it for the next collision pass. Do not invalidate on every move.
65536 whole-memory host cases, including aliases and all four cache states,
match0f8c0042 (none/first/second/both27629/8752/8905/20250).72-byte body view,
44-byte contact state, no heap/VRAM. Shared synthetic fixtures also run in
NATIVE_CONTACT_CHECK startup; compilation is not SH4 execution proof.
The host scratch bus lacked burst flags, causing LDT to reread one word:
fixed at the shared bus, not by changing target math. Existing routine checks
must remain exact after this correction. See HANDOFF for live coverage.
The140a4..14138 broad-phase gate is now `nm_collision_candidate`, using the
existing table-refined length minus the original immediate radius, followed
by the absolute FLOAT-BIT comparison of current5c words. No new pack or VRAM;
two12-byte probe views. The circle coordinates are the OUTER-LOOP snapshot,
not its current body position: the manual scenario changes that position28
times before later accepted pairs, and refreshing it changes two decisions
(first counterexample frame5487). Both5c words are still read live;5c is also
used for crash overlay model bits, so do not assume it is world height.
196608 independent i960/TGP branch/whole-memory cases,98304 with a moved
centre body after command40, match44675 hits/hash6caa7b7a.5857 actual accepted
pair checks match across left/manual scenarios; actual skipped-pair/list
coverage is not claimed. Shared cases now run in NATIVE_CONTACT_CHECK startup
as well, but SH4 execution is still unverified. The diagnostic's const TGP
snapshot accessor never advances the reference and is not linked on target.
The host parent-return checker reports whole-memory coverage separately:
without road data it checks only its imported separation and dispatch result,
not memory after response stages it did not run. Full road-backed checks
retain complete response-memory comparison. See HANDOFF for commands/evidence.
513 actual14904 calls match both branches, feeding the unchanged native entry
snapshot into the existing four-stage response. All1369 responses/12000 frames
remain unchanged; complete coverage limits are recorded in HANDOFF.
Remaining earlier gates/later callers/race updates and
native spawn/model/camera integration are required for autonomous ticks.
DND21 is now required:280028 resident/280044 file bytes, adding eight signal
bytes from235b40 in tested vertex order. Verify the countdown-to-corner map
is the inverse of the existing four-corner permutation offline. DND20 used
280020 resident/280036 file bytes. Four source corner
offsets become four bounded wheel indices offline; no target entity offsets.
DND19 used280016 resident bytes. DND18 appended56 bytes
of collision thresholds/flag masks; DND19 appends the raw four-byte speed step.
Do not recover it from the quarter-step: underflow/rounding can lose bits.
DND17 appended20 low bytes
from collision timer halfword tables to the unchanged DND16 payload. DND16
added2056 bytes of coefficients/four byte-domain shake/inhibit tables to DND15.
Signed shake indices and unsigned predecrement inhibit indices are resolved
offline; oldDND1..20 reject. Off-course recovery adds no pack/VRAM data:
reuse DNC3 height/heading/center, preserve signed wrapped timer comparison,
surface-mask precedence, refined division and64-tick recovery (not21).
396160 whole-memory i960/TGP scratch cases across all four packs match.
The extended block matches5714 actual calls/all12000 left/manual frames;
neither scenario starts64-tick off-course recovery, so live coverage remains open.
No new math approximation; shared globals still
round-trip across entities. Main/CDI remain camera diagnostics; hardware math
and the full gameplay/frame-budget gates remain open. Details in HANDOFF.

Complete16060 now composes the existing native selection/recovery/pivot/spin
operations into240-byte state/12-byte input with atomic publication. Shared
heights/timers/damage fields remain shared across cars; event intents are silent.
DND15 appends4100 bytes of original sides and full unsigned-byte-index numeric
divisors (277880 resident total), folded offline without target ROM pointers.
16718 is LDOB, not a signed/clamped lap index. Stored zero divisors reject only
if consumed; do not adopt I960Ref's explicit DIV0 HACK as hardware semantics.
524288 host cases comprise523312 accepted whole-memory/event matches and976
atomic invalid-domain rejections.2853 actual left-run entries/returns match;
there is no actual recovery/crash-animation-start coverage. No new VRAM or
math approximation. DNC3/DNP1/DNR1 unchanged, DND1..14 reject. Persistent
enDjinn car composition and hardware math/frame-budget verification remain
required; the camera diagnostic does not execute this operation. See HANDOFF.

DNR1 now carries original164-byte road profiles (including the terminal row)
and4-byte map records with immutable profile flags ORed offline. It adds
34156..166780 resident bytes per slot, no VRAM, not a complete course budget.
Native10980 updates explicit16-byte road/phase/flag/cursor state; its held
branch reuses validated DNP1 cursor math and preserves both flag fields.
The host compares original separate ROM tables,534451 whole-operation cases
and2853 actual active-player entries/returns (44 held-forward); all reference
frames remain unchanged. Slot3's normal map is supported, not its unvalidated
path layout. This follows the offline/static versus live/native split above;
it is not a bus adapter or pose replay. Persistent enDjinn car composition,
pivot parameters, hardware arithmetic and frame budgets remain gates.

Pre-dynamics15fc4/16014/15d60 now compose as one bounded native operation,
including637c's auxiliary/camera reset for the packed boundary-crash sequence.
104-byte state/16-byte input; camera/shared fields remain shared, not per-car
copies. Restore writes path cursor/heading and overlapping signal fields.
DND14 folds32 reset bytes offline (273780 resident total), while the selected
view descriptor byte remains explicit live state. No target ROM pointers,
program dispatcher or trajectory replay.65536 host cases per pack and2853
actual left-run inputs match; the latter contain3 spin stops but no active
animations. Other entry choices/model binding, persistent enDjinn composition
and hardware arithmetic/frame-budget verification remain gates. See HANDOFF.

Spin/damage16bd4 now has explicit52-byte native state and32-byte input, including
the first15eb8 animation advance. DND13 adds3468 bytes of offline model keys,
thresholds and257 compact authored rows; both boundary entry paths' consumed
sequence bytes are verified identical before deduplication. No program/bus
pointer dereference, target audio playback, allocation or VRAM is added.
262144 host spin cases and4128 row-advance cases per course match; three actual
left-run spins match, but no actual crash start was observed. Outer timer/
animation callers, asset-catalog binding and persistent enDjinn integration
remain required. Total DND13 resident273748 bytes; oldDND1..12 reject. See
HANDOFF for scope, fixed digests and pending hardware checks.

Recovery161f8..1632c now has a bounded32-byte native result and a DNC3 asset
extension:72-byte quads retain authored recovery height plus an offline-folded
vertex center. These are distinct source quantities; float32-round each sum
offline, retain live subtraction/refined division and the wide signed angular
delta/21. DNC1/2 reject. Largest collision payload598124 bytes, zero VRAM;
198080 complete host i960/TGP memory cases match across all four packs. No
target board runtime, new approximation or gameplay replay. Spin/damage,
persistent player and SH4 arithmetic/timing are still gates; see HANDOFF.

The16060 prefix now selects the native boundary reaction using the existing
bounded contact query and DNC2 headings: NONE/RECOVER/PIVOT/SPIN. Preserve mask
precedence, the center query's Y/mask-only writes, first qualifying wheel and
ordered height/heading tests.12-byte result, no new allocation/VRAM/asset format.
All524288 host prefix cases across four packs and2853 actual left-run entries
match complete state/branch selection;6000 reference frames remain unchanged.
Actual actions are1518/0/1332/3, so no live recovery claim. This does not yet
execute recovery/spin/damage or connect a persistent car. Strict SH4 compilation
is not target arithmetic or timing proof. See HANDOFF/select-* evidence.

The1638c..167b0 boundary branch now composes ordered edge tests, pivot pose,
speed and timer effects as a bounded native operation, with sound intent only.
All198080 host i960/TGP branch cases across the four DNC2 packs match; actual
inputs from the first intersection call are also checked after the scenario.
This does not cover16060's prefix/recovery/spin/damage branches or prove SH4
arithmetic. Caller side/divisor parameters still need offline asset packing.
Preserve the degenerate-quad X/X overwrite and matrix zero terms established
by the whole-state oracle. No additional target bus/runtime or VRAM is added.

The surface-response dependency167b4 now has a bounded native segment test
using the existing table division. All65536 host oracle cases match both point
bits and hit flag (2f83822f), including nonfinite candidates on misses. Such
candidates are not car positions; only a hit authorizes their use. Directional
endpoint and signed-zero behavior must not be replaced by a generic geometric
predicate. This is not the complete16060 response or native car integration.
Left/gear scenarios add4416 actual-call matches and12000 unchanged reference
frames. No asset format change; SH4 compile is not target arithmetic proof.
See HANDOFF.

The native active-player dynamics block now composes the validated operations
in source order through ground and scenery-cell selection.312 bytes of explicit
state replace that block's scattered fields; no target board runtime is added.
Actual-entry host parity covers8567 blocks through ground and2853 extended
scenery-cell blocks. This is not a persistent race: between-entry auditing
finds substantial XYZ/yaw writes outside the block. Caller/tail behavior must
be imported rather than lost or replaced with recorded entry snapshots. The
top-level interval audit attributes most pose changes to surface response16060,
including boundary/pivot correction, not a presentation-only adjustment. The
enDjinn car connection and target arithmetic/performance proof remain open.
See HANDOFF and `build/dc-native-chain.hx4Nl3` for scope and reproduction.

PLAYER guided10ccc now has a direct native implementation using the existing
path/profile/table math. DND12 adds1028 bytes of offline player-mode targets and
adjustment limit (270280 resident total); no target board state or VRAM. Across
24 routes,172296 scratch cases match complete reference memory and24312 reject
atomically before out-of-range CVTZRI. Negative-zero speed first produces
-Infinity at10f2c; no undefined host cast or guessed overflow result is accepted.
Actual guided entry/exception reachability remains an integration gate. Tests
and SH4 build pass, not hardware execution. Road profile/map pack, persistent
player chain, enDjinn gameplay connection and<16ms remain incomplete. Details
and reproducible evidence are in HANDOFF, dc-native-guided.f6MtWQ. The following
DND11/path-only descriptions are chronological rather than current formats.

DNP1 now prepares the eight authored28-byte route tables per course0..2,
including eight guard records per route. Complete forward/reverse path cursor
10bcc/10c50 uses direct table math, no target board/ROM pointers. All24 paths'
13584 rows are source-verified;268320 whole-memory i960/TGP cursor cases match.
Largest one-route payload27972 bytes,8-byte cursor, no VRAM. Slot3's aliased
disabled/sentinel directory is not supported as a normal course. Guided10ccc
still needs its lane/profile/rank/pose updates; this asset/cursor milestone is
not connected player gameplay or SH4 arithmetic proof. HANDOFF records the
reverse-wrap correction and evidence in dc-native-path.e9bWXQ.

Complete native wheel/ground update11bd4/116d0 now uses144-byte explicit state,
including four probes, pitch/roll/height and road-relative distances/heading.
DND11 adds48 bytes of authored wheel points (269252 resident); DNC2 adds each
quad's authored heading (56-byte record). No target bus/FIFO/program execution.
All65536 transform and49520 complete ground scratch cases match actual host
i960/TGP memory; three6000-frame course0 scenarios match8567 actual player calls
without reference mutation, including93 spin motion calls but no held movement.
Largest collision+math payload751552 bytes, zero VRAM. Silent24bpp,
SQ and enDjinn outer loop are unchanged. Main still drives diagnostic camera,
not a native car; guided movement, persistent chain, hardware arithmetic and
full-course/frame budgets remain gates. See HANDOFF, dc-native-wheels.QzsgrL.
The DNC1/DND10 notes immediately below describe the preceding milestone.

Native wheel contact now has a course-specific DNC1 pack:128x128 spatial grid,
ordered candidate lists and four-vertex collision surfaces/type bytes, distinct
from rendering meshes. Course payloads103764/453344/208620/97872 bytes; at most
33 candidates per query and zero VRAM. Shared DND10 math remains269204 bytes.
Native lookup/height/type selection matches517016 host i960/TGP query cases;
all prepared data is compared with source. No target board/FIFO runtime or
per-query allocation. This does not yet cover wheel transforms/suspension,
live car integration, hardware arithmetic or a full-course rendering budget.
Evidence and exact query/branch limits: HANDOFF.md, dc-native-ground.otFiRq.

Latest SQ submission refinement retains material binding identity in existing
triangle padding and omits only consecutive repeated headers within each
OP/PT callback. No reordering or cross-callback state assumption. Expanded
header-state checks pass at27 camera offsets; fixed-view RGB24 captures before/
after/repeat match exactly. Hardware submission3327->2924us, whole-loop
30738->29964us mean in repeated capture builds. Geometry transfer saves55776
bytes, while PVR internal TA use and VRAM stay unchanged. A byte-comparison
implementation was measured slower and removed. enDjinn's queued lifecycle
is unchanged; optional direct-OP dispatch remains a proposal, not a fix for
the approximately27ms CPU preparation. Live gameplay and sub16ms remain unmet.

Triangle commands subsequently borrow those immutable material headers instead
of copying them per face/triangle, using existing padding and unchanged960000-byte
command capacity. Exact frame and all27offset reference checks pass; repeated
hardware quiet loop29964->29163us, preparation26827->25978us. Submission rises
2924->2974us, still a net improvement. Separate instrumented profiling identifies
view/bounds at797us versus transform/cache9349 and clip/emission10689us; timer
overhead prevents treating those as quiet-frame phases. A shared-position
lighting/projection cache was tested correct but slower and removed. No engine,
source math, visibility coverage or asset changes were needed. Full gameplay,
world/road calibration, streaming and16ms remain incomplete.

The first isolated native road/spawn operation now has an exact host oracle:
`road.h` updates lateral state and samples authored XYZ/angles without the
board runtime. I960Ref plus the booted reference TGP verifies280904 cases over
all1924 authored records and4312 synthetic edge cases. This is not acceleration,
braking, ground contact or a playable car. Keep strict float operation order;
the fraction table is i/255, and the reference takes upper-lane angles rather
than interpolating them. No approximate SH4ZAM replacement is introduced.
Authored profile/spatial-map source sizes range34156..166780bytes per table,
separate from road render/collision geometry and textures. Logical counts omit
one distinct map-addressable terminal record: retain it. The former14-section
road-window interpretation is corrected below; those pointers refer to cars.
Tests/build pass, SH4 execution remains pending because the network loader
was unavailable. No native gameplay/performance completion is claimed.

The native input stage now separately decodes reference ADC values and applies
player steering filtering/pedal overrides. All65536 synthetic cases match the
actual i960 instructions, including wrapped integer arithmetic, signed inhibit
field cancellation and pedal high-bit edges. Existing host oracle/startup
checks are reused; no board runtime is imported into the console. The ADC
interface is not a calibrated Dreamcast controller mapping, and input filtering
alone is not driving physics. Next import the actual drive/contact operations
and compact parameter tables, then connect native vehicle state to the enDjinn
loop. SH4 execution is still pending loader availability.

The full drive/brake/slope/drag operation0xecc8 and its contact-count helper
now match202752 scratch i960/TGP cases; the shared sine lookup also matches
all65536 angles. DND1 offline parameters cost72256 resident bytes (65536 sine,
6720 drive), no VRAM. Ordinary division by2000 failed exact parity: preserve
the TGP table-refined reciprocal as one offline constant and multiply live
compensation by it. This is not approval for approximate gameplay math.
The optional SH4 startup check builds but awaits hardware; gear/contact and
velocity/position integration plus calibrated live world/road remain missing.
The next milestone is one controllable real car, not another fixed-view demo.

Axle-load filtering0xef80 and its surface-category helper0xf278 now also match
196608 exact i960/TGP cases. DND2 adds40 coefficient bytes to the same pack
(72296 resident total), with no extra lookup table or VRAM. Preserve the source
signed comparisons of float bit patterns and integer surface-category average;
ordinary float clamping/wheel-count averaging changes behavior. The optional
startup check includes both operations but awaits SH4 execution. Collision,
force-to-motion integration and actual controller-driven gameplay remain open.

Full force combination0xf31c with nested0xf830 and helper10480/104ec state/events
now matches196608 exact host i960/TGP cases (2fd6285e). Direct table-based
division/Cartesian/polar math replaces those TGP commands without any target
program execution:262144 division cases ef97a891,65536 vector cases a3d27a9d.
DND4 holds268928 resident bytes (262144 math tables +6784 coefficients), no VRAM.
Auxiliary scheduling becomes an explicit native event, not board state. This
produces grip/slip and longitudinal delta, NOT a complete moving-car loop.
SH4 builds pass; hardware arithmetic remains unverified and KOS starts with
DN=1, so host subnormal results cannot establish target equivalence. Contact,
yaw, speed/RPM, placement and enDjinn gameplay integration remain next.

Traction regulation10568 and speed/RPM integration10338 are now direct native
operations as well. DND5 adds4 bytes for028b5774 (268932 total resident bytes),
reusing rear-load limit028b57d0. Speed131072 cases match0091ea86; preserve cvtri
ties-away rounding and sign-bit zeroing, not generic truncation/clamping.
Traction126579 finite cases match;4493 synthetic reference-nonfinite cases are
rejected without publication, with acceptance/state digestb03c174e. Actual-entry
race reachability of that exceptional domain remains unverified and is NOT an
accepted gameplay-parity deviation. This operation regulates supplied contact
data, not wheel/road collisions. Yaw/tire-angle/world/input integration and
SH4 verification remain open; full gameplay and16ms are still unmet.

Native yaw/direction0xf9d0..fdf4 now updates explicit24-byte state with the
existing table math. DND6 adds20 coefficient bytes (268952 resident total,
no VRAM). Direct atan0x0a and accumulator-division commands are checked too.
Of131072 synthetic yaw cases,76796 match whole reference memory exactly;
54276 reach an invalid reference float-to-int conversion and are rejected
without native state publication. The scratch oracle stops before that
undefined host C++ cast; first case is zero speed/steer/tire angles producing
NaN7ffffc00. Digest6dcbf33a includes acceptance/state, not universal parity.
Actual-entry reachability is an open integration gate for yaw and traction;
no new tolerance, guessed conversion result or approximate physics is accepted.
Tests/SH4 builds pass but target arithmetic remains unverified. Tire response,
gear, road contact/XYZ and enDjinn gameplay integration remain incomplete.

Actual player-call observation supersedes the yaw-domain uncertainty above:
race_basic's first player call at frame3147 reaches the rejected straight-angle
case. Native masked-NaN conversion is now explicit (see Floating point manual
correction), not an undefined C++ cast. Digest2fefd38c covers131068 accepted
memory matches including54272 masked NaNs;4 Infinity cases remain rejected.
The opt-in host call observer reads RAM only and compares native outputs at
the next player operation. Both traction and yaw match2853/2853 actual calls
in race_basic and2861/2861 in race_steer_right; every frame still matches the
unobserved reference's counts/screens. This resolves the observed straight-drive
defect, not universal input-domain parity or a native playable car. enDjinn
integration, tire response, gear, contact/world data and target math remain.
The existing steering/braking scenario race_steer_left also passes2853 calls
of each operation and6000 screen/count comparisons (91beca2f553c93d6).

Native tire response0xfdf8..10268 now updates24-byte explicit state, using the
existing table math and the same auxiliary-event convention as the forces
operation. All131072 scratch cases match entire entity/global/auxiliary memory;
digest1a5bee04,68189 auxiliary triggers and23370/23419 lower/upper rear-angle
clamps. Front and rear divisions are deliberately not algebraically unified.
The negative clamp endpoint sets the override even on equality; the positive
endpoint does not. Signed-zero drive, floor-negative filtering, stored16-bit
wrap and signed trigger timers remain observable behavior.
DND7 adds12 coefficient bytes (268964 resident bytes,268980 file), no VRAM.
Actual-entry checks also cover tire calls/returns and both auxiliary objects:
race_basic2853/2853 and race_steer_right2861/2861 match without rejected calls.
race_steer_left also matches2853/2853 (8567 actual tire calls across the three
scenarios); all18000 reference frames preserve screen/count parity.
This still does not connect native vehicle state to enDjinn or solve wheel
contact/world placement. Host arithmetic checks and SH4 build success are
not SH4 execution evidence; the DN=1 and approximately29ms renderer gates
remain. Next import gear and real road/world integration, then connect the car.

Native gear selection now implements both11d60..11e7c (automatic) and
11e80..12064 (manual) using explicit24-byte state and12-byte input. Preserve
signed shift countdown, manual downshift RPM history, over-rev brake word,
post-shift filter limits, wrapping integer arithmetic and manual-only final
filter clamp. Automatic and manual RPM conversions use different formulas;
reuse table-refined division, not host division. Current/selected gear bounds
are checked before indexing, and failed transitions do not partially publish.
DND8 adds88 bytes,269052 resident/269068 file, no VRAM.131072 scratch cases
match whole memory (eab110f6), with up/down coverage in both modes and19509
over-rev brake changes. Actual player checks match2853 automatic and2861
manual calls; those scenarios retain all6000 reference frame/count comparisons.
The new race_gear_changes input also matches2861 manual calls, including four
upshifts/four downshifts and skipped-gear requests, for8575 actual gear calls
across18000 unchanged reference frames. Auto upshift coverage is synthetic
only in these scenarios; do not infer a completed native race.
The live observer also accounts for the caller's optional hold decrement and
rear-angle clear before the next call, checking rather than omitting those
bytes; neither baseline scenario enters that hold branch. Scratch tests cover
the gear routines themselves, not a claimed live hold-branch measurement.
enDjinn startup diagnostic includes gears, but there is still no persistent
native player chain, road contact/world integration or new SH4 arithmetic
execution evidence. Those, and the approximately29ms render work, remain gates.

Player force-input steps1132c and11370 are now native too. Resistance selects
one of five offline floats after a signed-byte countdown; the table selector
uses the wide decremented value BEFORE the byte store wraps. Proximity takes
three ordered neighbor X/Z positions and updates the existing drag timer:
preserve strict distance/angle thresholds and signed float-bit comparison.
The target performs no linked entity-address reads or board dispatch. Full
source-memory comparison covers all256 resistance byte inputs (707d6da1) and
65536 proximity cases (99b6b08d); both TGP43 distance and0f angle are separately
checked for all three neighbors per case, including branches not taken by the
player routine. First/second/third-neighbor hits all have coverage. DND9 adds
20 bytes:269072 resident/269088 file, no VRAM. Live basic/manual-gear scenarios
match2853/2861 calls of each step, including125/11 confirmed drag refreshes.
The steering/braking scenario adds2853 matching calls and131 refreshes,
8567 actual calls per operation over18000 unchanged reference frames. It also
supplies actual automatic-upshift coverage (3up/4down) missing in the previous
gear milestone's scenarios. No new target execution/performance evidence.
Disabled mode has synthetic, not live, coverage in those runs. Road contact,
world placement and a persistent native car in enDjinn remain unimplemented;
successful host/SH4 builds do not establish target arithmetic or16ms performance.

Normal/spin world movement (ea04, excluding its10ccc guided branch) and held
displacement (ec44) now use native explicit state in `motion.h`. All196608
scratch cases match whole entity/lookahead memory, digest028f30e6. DND10
(wire magic DNDA) adds132 parameter bytes:269204 resident/269220 file, no VRAM.
Normal lookahead uses the OLD position and16x displacement; spin changes
stored speed but may use a different low-speed displacement. Held motion
preserves speed/deltas/lookahead. Unsupported guided motion rejects without
mutating state; it is not silently replaced with normal driving. Actual basic
and steering scenarios match2853 calls each, including93 spin calls in the
steering run. A manual-gear scenario adds2861 normal calls, for8567 total;
all18000 reference frames retain parity. None exercises guided or held movement. Hardware arithmetic,
wheel/road contact, persistent player integration and frame budget remain open.

The first native asset milestone is an animated title using source art and
the ROM's authored displacement curve, with exact offline layout/PAL4 checks
and lossy RGB565 VQ background. It uses 33 quads in two enDjinn OP/PT batches,
163,840 texture bytes, and no target-side System24/machine runtime. The same
portable title pose function reconstructs all 415 no-input reference frames
2948..3362 exactly before VQ, including intro, footer and 8-tick prompt blink.
Its provisional 17,384 us reference clock is independent of Dreamcast scanout.
On hardware, 596 quiet samples give average/max PVR render 7.059/9.739 ms and
registration 0.117/0.134 ms; no sampled sum reaches 16 ms. An earlier run with
console output inside the window showed a 45.971 ms spike; dc-load masks IRQs,
including PVR completion timestamp delivery. Logging now follows measurement.
These are title-only asynchronous PVR timings, not complete gameplay work.
Credits, scene transitions and live gameplay remain
unimplemented; the test holds the title, rather than faking the next scene.

The next offline stage now extracts deduplicated local-coordinate source
meshes through an optional host observer in the geometrizer. Source transforms
are checked against reference rasterizer words; synthetic tests independently
check topology, UVs and headers across all four parsing modes and ROM/RAM paths.
Frame 240's forest/road view yields 34 meshes/51 instances and 2525 unique
faces. Instance matrices still include the reference view transform: they are
not world placements, and one display list is not complete course coverage.
The inspectable intermediate retains bounds/material/normal data, with no
target-side board parser. A DNM1 binary pack and the same bounded C reader on
host/DC now reduce 9844 unindexed vertices to 7068 shared position/UV vertices;
the complete geometry/parameter pack is 209,840 bytes, with textures separate.
All source face vertices roundtrip exactly; malformed-input and sanitizer
checks pass. Hardware loads/validates it in 162.702 ms at startup. SH4ZAM affine
checks on 7664 instance vertices stay within the documented magnitude-based
floating-point bound, but 215 components differ after reference 24-bit
truncation: this is not bit-exact TGP or gameplay parity. The initial target
displayed only the title; the newer visible-3D milestone is described below.
A native triangle projection/clipping
kernel and per-instance vertex cache now pass 105,205 host cases and a hardware
near-plane check. It clips before division, preserves perspective UV/depth,
and uses double only for intersected triangles; a provisional 0.01 near bound
is not an arcade calibration. Hardware processes 5192 source triangles into
3801 frustum survivors in median **20.658 ms**, down from 91.570 ms after
removing hot library calls and caching shared vertex work. This is CPU-only,
with no submission, backface/sector visibility, GPU, or live gameplay, so the
16 ms target is explicitly not met. Full source-to-reference projected
coverage/window semantics, complete materials, 3D drawing, direct geometry,
world/camera calibration and sector transitions remain. Detailed
load/ELF/allocator/PVR observations and commands
are in the native working objective and handoff; a 2 MiB per-pack guard is not
a measured course budget.

Pre-cull material extraction now covers every source-model header in that
sample: 678 textured header/luma variants plus 124 solid variants. Resident
and paged exports are byte-identical; geometry is unchanged. Existing offline
VQ16 encoding produces 644 unique textures using 2,914,816 payload bytes and
no palettes. Maximum sampled size is 256x256, but three families of baked
light levels alone consume 1,529,856 bytes. Solve lighting/residency before
assuming texture dimensions require subdivision. The actual-DT decoder checks
all files against source proofs and encoder previews. Alpha masks are NOT
exact: 1334 changed pixels with small codebooks, still 131 with full books
costing another 310,496 bytes. One checker material also requires explicit
screen-space transparency semantics. Neither representation is approved for
final cutouts. The subsequent DNT1 pack is now loaded on hardware; the old
prototype pack ceiling is not the new target's residency budget. The measured
CPU budget above is unchanged. Commands and detailed limits are in the native
working objective and handoff.

Native face lighting is now isolated from the geometrizer's board/command
structure and checked against its emitted luma/rear bits. All four modes and
specular controls pass 41,024 synthetic host cases; all 2742 sampled source
faces match. On Dreamcast, scalar and SH4ZAM inputs produce identical nine-bit
results and the same host reference hash (5b0dca6b). Transform+light alone
takes median 5.840 ms, still repeating vertex work that must be shared with
geometry. No live gameplay or 3D drawing is implied. Baked texture lighting
cannot simply become one brightest texture plus RGB modulation: a best-fit
host probe still differs by up to 96/255 before VQ. Face-luma validation and
the nonlinear light-to-RGB/material-residency problem are separate gates.

The first visible native 3D diagnostic now draws source meshes through enDjinn
OP/PT callbacks using file-backed DNT1 VQ16 materials. HDMI verifies road,
guardrails, trees and cars: 1558 opaque and 954 cutout triangles; 41 checker
faces are explicitly omitted. It has a fixed source-view camera and rebuilds
geometry each update, not a live game loop or replay substitute. All 2742
sampled face bindings resolve, and byte-verified texture uploads allocate
2,935,424 VRAM bytes, leaving 905,800 free. Resident material metadata and
descriptors use 54,048 RAM bytes; source staging/stdio are released. Failure
rollback and success teardown are verified, with GPU resources freed before
enDjinn shutdown. No PAL8 or board runtime was introduced.

The integrated draw path is still over budget: 600 updates give CPU preparation
average/max 49.938/52.343 ms and submission average 3.017 ms. 596 quiet PVR
samples give registration 3.187/5.734 ms and render 13.654/16.146 ms avg/max;
last ready period 66.560 ms. These asynchronous phases overlap, so their sum
is not a full-frame measurement. Current repeated light transforms, material
lookups and header compilation need phase profiling and invariant work moved
out of the frame. Live world/camera/vehicle state, sector residency, background,
checker/alpha fidelity and full source raster ordering remain unimplemented.
Hardware depth and nearest filtering here are diagnostic choices, not an
approved equivalence claim. Details and commands are in the native handoff.

Subsequent profiling moved invariant PVR-header compilation and material
search out of the update: headers compile after startup upload; an exact
material/luma lookup retains missing-light rejection. This adds 25,664 header
and 128,000 lookup RAM bytes, not VRAM. Startup compares all 2512 submitted
triangles byte-for-byte with the uncached path. SQ submission is preserved
and checked in the emitted SH4 code. Instrumented material/header time fell
16.629->2.287 ms; quiet preparation fell 49.938->34.271 ms. The final build,
including full-loop interval accounting, measures 35.936/38.429 ms preparation
avg/max and **39.438/41.913 ms update-to-update** over 599 intervals. Thus the
target is still not met, even for this fixed view. Asynchronous PVR counters
must not be interpreted as complete frame periods. Detailed phase profiling
is a separate optional build, not hidden per-face overhead in the default.

The next measured change culls faces before vertex preparation and then
transforms required vertices in source order. It saves 2207 of 7664 projected
vertices without changing any of 2512 submitted triangles' bytes. A 778-byte
per-instance light scratch is rebuilt every update; no captured pose is reused.
Final hardware repeat gives 29.925/32.396 ms preparation avg/max and
**33.457/35.924 ms whole-loop interval**, with unchanged visible HDMI output,
SQ submission and VRAM. Per-face lazy transforms and a separate byte-marker
buffer were both measured slower and not retained. <16 ms remains unmet;
coarse sector visibility, shared transform/light work and live gameplay are
still required, not inferred from this fixed-view optimization.

Whole-instance culling now reuses the pack's local bounds, conservatively
expanded for FTRV/focus/plane rounding. It rejects 7/51 instances before face
lighting and projection, reducing prepared vertices 5457->4586 without changing
any of 2512 submitted triangles' bytes. Fifty thousand synthetic boxes with
400000 perturbed triangles and the real pack check false-rejection behavior.
Hardware whole-loop avg/max is now 31.464/33.933 ms; preparation 27.971/30.468 ms,
SQ submission 3.293 ms average. HDMI and resource teardown remain verified,
but this is still over budget and not a native gameplay/world implementation.

Lighting now shares geometry's IEEE finite check without altering arithmetic,
and vertex records drop 44->36 bytes by deriving perspective UV products at
emission. Overflow rejection remains at cache creation; host oracle/byte and
sanitizer checks pass. The 993-entry cache saves 7944 RAM bytes. Repeated
hardware measurement gives preparation 26.024/28.543 ms avg/max and whole-loop
29.517/32.009 ms, versus 31.464 ms mean before these two changes. All 2512
submitted triangles still match the full path, nine-bit lights match and HDMI
shows the scene. No enDjinn OP-dispatch change was needed. SQ, VRAM and silent
24bpp output are unchanged; <16 ms and live gameplay remain unmet.

An offline anchor-relative check now compares source instances across frames
240/241/270/300 using unique address/window/reverse/mesh-content matches.
Six candidate environment meshes share a matrix through 270, while car-sized
objects move relative to them; two environment meshes remain selected at 300.
This is evidence for separating world placement from camera, not proof of
physical units or a camera replay to put on the target. Repeated instances
remain ambiguous, and equal movement cannot by itself distinguish scenery
from parts of one car. Native live camera/entity state and matching dynamic
light/material coverage remain required. The current runtime stays at
29.517 ms mean: an attempted per-face PVR-vertex cache was measured slower
and removed rather than retained as an assumed optimization.

An optional native translation camera now uses enDjinn input and elapsed time
within a provisional +/-4 source-view-unit box; no recorded poses or board
simulation. It moves the same instance matrix for lighting/culling/projection.
This is an inspection diagnostic, not calibrated world/gameplay camera logic.
Host preparation analytically bounds possible front/rear light branches for
supplied-normal modes in that box, preserving observed keys. It adds 74 textured
variants and all solid lights. Baking every textured branch without a bound
cost 4582400 payload bytes and was rejected, not admitted by raising the budget.
The bounded pack has 712 unique VQ16 textures, 3023744 payload bytes and 3046528
actual VRAM allocation, leaving 794696 free. Only 270408 bytes remain above the
512 KiB reserve; adjacent-sector overlap is unproven.

Hardware matches complete reference/optimized draw streams at 27 offsets and
HDMI shows changing views. Synthetic-input and idle-controller-build tests
return cleanly through enDjinn; physical controls were not retested. The moving
test averages 28.792 ms whole loop, idle 29.254 ms: still over 16 ms and not
directly comparable as an optimization against the previous fixed view.
Alpha/checker fidelity, rotations/world calibration, live entities/collision,
course coverage and transitions remain open. Detailed checks/costs/artifact
paths are in HANDOFF; supplied enDjinn is unchanged.

Authored scenery inventory now follows the existing course-cell data rather
than arbitrarily slicing ROM files. The verified daytona93 table at 0x39b0
selects four 256-cell tables; these are table slots, not four named playable
courses. Read-only reference exports confirm slot 0/table 0x02895d80 for the
current scene. All six environment candidates match unique authored cell
records and share a view matrix; their local coordinates give the scenery
basis. Their meshes total 99744 bytes excluding materials/instances/pack header,
and every mesh spills outside its nominal 128-unit X/Z cell. Cell-based residency
must retain actual mesh bounds for culling. Correction: the linked 14-entry
window at 0x13f5c is a vehicle neighborhood, not road sections. Membership in
the live player/opponent pool is verified at frames240/300. Authored road
profiles are separate. Host inventory checks/hashes/addresses are in HANDOFF.

Full slot-0 scenery extraction now decodes all 256 table objects through an
isolated host reference snapshot: 22 drawable meshes/234 empty objects, 5091
faces, 429696-byte indexed pack. Sampled meshes/context match exactly; frame
240/300 local mesh/material records are identical while instance records change.
This is complete scenery-table geometry at captured state, not road/entities,
world calibration or dynamic lighting. Host reference behavior remains checked.
Per-cell dependencies and all eight neighboring transitions (including wrapped
grid edges) are measured from validated VQ bytes. Full scenery uses 7329536
payload bytes; peak 3x3 allocation/transition estimates are 4874976/5525792,
already over foundation free VRAM before road/cars/HUD/reserve. Whole-atlas baked
light families dominate. Keep native memory guards and solve exact footprint/
light residency before uploading; simple RGB tint was already rejected for large
colour errors. These estimates are not a visibility proof or hardware residency
test. Native gameplay/world/streaming and sub-16-ms work remain incomplete;
the target runtime and performance are unchanged by this host-only work.

Offline UV unions now allow exact crops of existing VQ index maps without
re-encoding: decode checks prove unchanged texels, not yet native sampling.
Full slot-0 scenery payload falls 7329536 -> 5159232 bytes after retaining
shared originals where splitting would cost more. 3x3 transition allocation
still estimates 4004608 before road/cars/HUD/reserve. Snapshot material keys
do not imply stable UV coverage: one frame300 union exceeds a frame240 crop.
The bounded +/-4 translation-camera export includes all relevant face/light
pairs and yields a 1664096-byte candidate payload instead of 3023744.
These are not target-ready packs; material metadata/UV rebasing, 27-offset
coverage/sampling checks and hardware verification precede adoption. No target
performance improvement or new alpha/colour fidelity claim is made.

DNT2 now carries crop origins without growing material records; DNT1 remains
compatible. Native rebasing and live per-face halo checks run through the same
enDjinn/SQ path. The bounded camera pack allocates1686976 VRAM bytes on hardware,
leaving2154248 free (1359552 recovered). All27 camera stream checks and a600-update
moving test pass. UV setup is cached with headers, adding138752 RAM bytes; idle/
moving loops remain about31/30ms, not under16ms or faster than the prior renderer.
KOS RGB24 readbacks show stable original output, but8 of307200 pixels differ
after cropping (max33/255 channel delta). Exact encoded texels are not proof of
identical PVR interpolation/sampling; that discrepancy remains explicitly open.
This is verified bounded residency, not final visual/full-course/gameplay parity.

**Historical Dreamcast prototype (not the active architecture).** The separate enDjinn build
owns initialization, controller latching, the mode-update loop and PVR
submission. The mode updater advances the existing native GameLoop; render
callbacks only consume its completed Video state. SH4ZAM is restricted to
presentation transforms, never TGP results or guest-visible math. The first
game build is silent (no reference sound board); AICAFlow follows later.
The framebuffer is 640x480 packed 24-bit RGB (`PM_RGB888P`), with dithering
disabled. KOS/PVR allocate three bytes per pixel: 1,843,200 bytes for both
framebuffers. With the existing TA/composite/UI/slack reservations, this
leaves a planned 1,498,368-byte 3D texture cache (1.429 MiB).
The five program/main/copro/polygon/texture ROM regions use bounded file-backed caches through the same
memory bus, and GPU-only Video construction omits CPU raster/composition
targets. Desktop defaults retain resident images and software targets.
An optional offline DRP1 manifest lists sorted ROM page addresses in fixed
program/main/copro/polygon/texture order. Before the loop, the Dreamcast loads
at most 1 MiB of these pages from the original imported images and pins them
separately from its LRU caches. Unlisted data always falls back to the original
ROM path, so replay-derived hints never define gameplay coverage or alter
guest state. This is bounded startup prefetch, not model conversion or a
replacement for stage transitions. No automatic stage switching is implied.
**Texture format constraint (2026-10-03): no PAL8 textures.** 8bpp paletted
rendering is rejected for the performance target. The PAL4/PAL8 compositor
described below is the existing diagnostic baseline, not the approved final
architecture. Do not extend its PAL8 fallback or implement the proposed four
PAL8 source layers. Exact fonts/UI require PAL4 palette groups; other art
uses the agreed offline VQ formats. Do not silently quantize exact UI to fit
16 colours. Palette-bank occupancy, extra geometry and any duplicated tiles
must be measured together; 512 KiB for four raw PAL4 layers is only a texel
storage lower bound, not a completed layout budget. The current console ELF
still contains PAL8 until that replacement is implemented and verified.

The native PVR frontend also disables decoded CPU tile pixmaps (3 MiB),
exposing the raw 4bpp tile/character/palette view instead. This removes work
the bridge does not consume; it does not substitute prerecorded UI or game
frames. The initial System24 compositor builds background and foreground
indexed surfaces around the 3D pass. Exact ARGB colours are compacted into
one shared palette (transparent entry plus at most 255 opaque colours); each
surface uses PAL4 when possible, otherwise PAL8. Overflow is an explicit
error, never font quantization. Palette entries 768–1023 (PAL4 banks 48–63)
are reserved and conflicting prepared packs are rejected. Two 512x512 PAL8
capacity surfaces reserve 512 KiB VRAM. Staging uses two 4 KiB stack blocks
and 128 bytes of row indices,
not a retained CPU surface: compose a 64x64 block, use KOS to twiddle it in
cached RAM, then store-queue upload to its contiguous VRAM region. PAL4
promotion to PAL8 restarts the current surface once with stable palette
indices. The existing 1 MiB composite VRAM allowance remains conservative.
Tracked raw tile/char
and palette changes invalidate the surfaces; unchanged generations reuse
them. A render-completion fence precedes texture and palette writes.
The original full-surface staging baseline took 1.37–1.48 seconds on console,
and its 256 KiB retained RAM exposed an allocation failure at frame 187 while
the polygon vector attempted to grow. The block version must be measured
against that baseline, not assumed fast from a host test. Block staging
passed frame 240 without the reproduced allocation failure; HDMI at frame
217 showed the AM2/SEGA logos and credit text over live 3D. Block-only
composition still took 1.47–2.15 seconds: repeated per-pixel source decoding,
not upload (66–125 ms), dominates. Row/tile-span decoding now hoists repeated
reads in normal and all three split modes; the independent pixel rule remains
the test/profiling reference. Split-mode category/opacity semantics are kept.
Measured span preparation is 262 ms at boot and 421 ms at the first 3D
scene (the corresponding original full-image times were 1,479 and 1,483 ms).
That is presentation only, not overall frame time or a real-time renderer.
The 59-colour PAL8/PAL8 pair at frame 240 still costs 875 ms; that frame
passes the old RAM failure with 1,081,104 free heap bytes. Later-stage
capacity remains unproven. Extending spans to split modes reduced the matched
frame-240 pair to 385 ms, while normal-mode boot/first-scene samples rose to
273/438 ms. Host endpoint profiling finds only two tile-register dwords
changed at frame 240, with no character/palette changes: native handling of
scroll/windows is preferable to continual full-screen recomposition, subject
to exact category/mask rules and the same VRAM budget.
The RAM bus already suppresses unchanged-value notifications;
invalidation optimization must inspect actual changes, not add that twice.
Full-race visual parity, speed and RAM headroom remain unproven.
The default enDjinn soft-reset/exit chord stays enabled. Native compilation
is not the final performance architecture: constant asset decoding/encoding
belongs offline, while runtime specialization must remove unnecessary board
machinery without replacing player-dependent gameplay with recorded replay.
TGP vector/matrix sequences are SH4ZAM candidates only after their arithmetic
and live-out state are validated against the reference. Its trigonometric
and inverse-square-root ports already use ROM tables, not libm calls;
substituting SH-4 approximations is not assumed bit-identical.

The fixed-function texture plan mixes exact paletted 4bpp for suitable
fonts/UI, VQ-paletted 4bpp for low-colour art that tolerates compression, and
VQ RGB565/ARGB1555 for richer 3D art. Encoding belongs in local offline asset
preparation, not the gameplay loop. Small textures may stay uncompressed
when VQ's codebook costs more than it saves. The initial live renderer now
binds these prepared resources; full-stage coverage is not established.
The initial host asset exporter prepares selected-frame base textures with
exact material headers/luminance and current palette/luma/translation state;
the existing pvrtex tool produces twiddled VQ RGB565/ARGB1555 or PAL4 assets.
Explicit source palettes avoid quantizing the lossless PAL4 path. Identical
images share assets, but different lighting states are not silently merged.
Prepared packs now embed exact pre-compression state: all sixteen shaded
colours and packed source indices, deduplicated separately from the encoded
textures. The native lookup first matches full headers/luma, then compares
the current colours and source indices; texture-write generations invalidate
cached comparisons. Variants share an index-validation result only for the
same dimensions, live sheet/origin and embedded proof range; their shaded
colours are still checked separately on every lookup. No hash or frame number
is treated as proof of a match.
This adds bounded RAM and comparison cost; dirty-page tracking is the next
optimization if measured necessary. The PVR bridge clips reciprocal depth
and UV-over-depth to each window, uses reverse reference painter ordering
on the unsorted TR list and binds only exact matches. Automatic depth writes
are disabled; nearest sampling, base levels only and diagnostic magenta for
missing states/checker shading make this an incomplete renderer. Geometry
submission is capped below the TA buffer limit, with counted drops. Live
textured road/trees are hardware-verified, not full visual parity or full speed.
Original mip/detail levels, checker effects, full material coverage and offline
model preparation remain required. ROM-page working-set reports count cache hits too; stage
unions from an input replay are lower bounds, not proof of complete coverage.
Budgets must include both KOS TA-buffer sets, OPBs and tile matrices, not
just scanout and texture payloads; mipmaps, codebooks and colour/luma
variants are additional costs. Console-private prepared assets stay in
ignored build/staging directories, an exception to the desktop's ROM-only
asset storage below; no ROM-derived data is distributed with this repository.
