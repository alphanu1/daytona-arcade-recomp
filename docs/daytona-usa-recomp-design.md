# Daytona USA (Model 2) Static Recompilation — Design Document

Sep 24, 2026 · Ben

## Overview & goals

We statically recompile the i960 game code of Daytona USA (Sega Model 2, 1994) into portable C++, and replace every other board subsystem with high-level or reused emulation, to ship native builds for Windows, macOS and Linux. MAME is the behavioural oracle during development; an original Model 2 PCB is the ground truth when MAME and our build disagree.

**Goals**

- Native x86-64 and ARM64 executables on Windows, macOS (Apple Silicon + Intel), Linux, Android and Raspberry Pi (64-bit Pi OS), from one codebase.
- Frame-exact gameplay parity with the arcade: timing, physics, AI, attract mode, all three courses, all cars.
- User-supplied ROMs only. The repo ships the recompiler, runtime and a ROM-to-build pipeline, never Sega code or assets.
- Modern presentation as opt-in: native resolution, widescreen, higher internal refresh for rendering, filtered textures.
- Real hardware for input: wheel, pedals, 4-speed shifter, force feedback, and cabinet link play over LAN.

**Non-goals (v1)**

- Other Model 2 titles. The recompiler stays generic, but runtime HLE targets Daytona's code paths first.
- Daytona USA 2 (Model 3, PowerPC) and the Saturn / PC ports.

## Target hardware summary

Daytona runs on the original Model 2 board (MAME set `daytona`, driver `sega/model2.cpp`, machine `model2o_state::daytona`), not 2A/2B/2C, so the geometry processor is the Fujitsu TGP rather than a SHARC or TGPx4.

Figures below are confirmed against MAME at `dddd7368` (`src/mame/sega/model2.cpp`, `src/mame/shared/segam1audio.cpp`) and cross-checked against the Model 2 MiSTer core at `591e148e`. They are MAME's figures, not hardware measurements: rule 11 still applies, and the PCB settles anything marked provisional.

| Subsystem | Part (MAME) | Clock (MAME) | Strategy |
| --- | --- | --- | --- |
| Main CPU | Intel i960KB (`I80960KB`), little-endian | `50_MHz_XTAL / 2` = 25 MHz | Static recompilation to C++ |
| Coprocessor | One Fujitsu MB86234 TGP (`m_copro_tgp`); MAME's MB86234 is an empty subclass of its MB86233 | `50_MHz_XTAL` = 50 MHz | Its uploaded program statically recompiled to native C++, validated against MAME's LLE TGP |
| Geometrizer | Separate from the TGP: walks the display list in buffer RAM at vblank (`geo_parse`, `model2_v.cpp`) | — | Reimplemented; MAME's version is HLE |
| Rasterizer | Sega custom chips; MAME has no device, it is driver code | — | Replaced by host GPU renderer |
| 2D tilemaps / HUD | `S24TILE` (System 24 tilemap chip) | — | Reimplemented, composited on GPU |
| Screen | `set_raw(32_MHz_XTAL/2, 656, 0, 496, 424, 0, 384)` | 16 MHz pixel clock | 496x384 active, 656x424 total |
| Sound | Model 1 sound board (`SEGAM1AUDIO`): 68000 + YM3438 + 2x MultiPCM | 68000 `20_MHz_XTAL / 2` = 10 MHz, YM3438 8 MHz, MultiPCM 10 MHz each | 68000 program statically recompiled (same pipeline as the i960); FM/MultiPCM chips as native C++ |
| Main ↔ sound | i8251 UART (uPD71051C) at 0x01c80000 | 31.25 kbit/s (`16_MHz_XTAL / 2 / 16`) | Serial byte stream, not a latch |
| I/O | Model 1 I/O board (`SEGA_MODEL1IO`, BIOS `epr14869c`): own Z80, talks through an MB8421 dual-port RAM at 0x01c00000 | Z80 `32_MHz_XTAL / 8` = 4 MHz | HLE of the dual-port RAM protocol, SDL3 mapping |
| Drive board | SJ25-0207-01 / 838-10646: Z80 + 2x 315-5296 + MSM6253 ADC; commands arrive through the I/O board | Z80 `XTAL(8'000'000)/2` = 4 MHz, "confirmed" | HLE: decode commands, map to SDL haptics |
| Comm board | 837-10537: Z80 + uPD72103 HDLC, program EPR-16726; MAME simulates it (`M2COMM`), no Z80 runs | — | HLE shared-memory protocol over UDP |
| Timers | 4 down-counters at 0x00f00000 | 25 MHz | Runtime device |

**Derived timing.** Refresh = 16,000,000 / (656 x 424) = **57.524 Hz**. Line rate = 16,000,000 / 656 = **24.39 kHz**: medium resolution, not 15 kHz. MAME marks this line `// TODO: from System 24, might not be accurate for Model 2`, so the blanking figures are provisional until measured on a PCB.

**Interrupts.** The board has a 12-bit request register (0x00e80000, write-to-acknowledge by AND) and enable register (0x00e80004, updated 80 ns after the write). MAME routes it to the four i960 lines:

| Request bits | Source in `daytona` | i960 line |
| --- | --- | --- |
| 0 | vblank | IRQ0 |
| 1 | nothing in MAME | IRQ1 |
| 2-5 | timers 0-3 (bits 6-9 unused) | IRQ2 |
| 10 | UART RxRDY or TxRDY | IRQ3 |
| 11 | nothing in MAME | IRQ3 |

Priority is not fixed by the board. MAME takes each line's vector from the i960's ICR and uses `priority = vector / 8`, so the ordering is whatever Daytona programs. **Measured (MAME, `daytona93`):** Daytona sets ICR = `0f0e0d0c` once at boot, so IRQ0-3 take vectors 0x0c-0x0f and all four run at priority 1; no line pre-empts another. In attract only vblank (vector 0x0c, handler 0x0e00) and the sound UART (0x0f, handler 0x0f50) fire; the timers never do. On vblank MAME runs `geo_parse` first (when 60 Hz mode is set, or on even frames in 30 Hz mode, per `videocontrol` bit 0) and then raises bit 0.

**TGP program.** The TGP has no microcode ROM. It holds in halt from reset until the i960 uploads its program: setting `coproctl` bit 31 (0x00980000) routes FIFO writes at 0x00884000 into the 4 K-word program RAM, and clearing it boots the TGP. Daytona's program is 2,024 words, stored in the game's own data ROM (MiSTer core, `tools/extract_tgp_microcode.py`). On the CPU board, `opr-14742a`/`14743a` (`copro_tgp_tables`) are the tables behind the TGP's sin/cos, atan, 1/x and 1/sqrt I/O ports; MAME labels `opr-14744`..`14747` (`other_data`) as further 1/x and 1/sqrt tables. MAME runs this microcode at low level.

## Architecture

The build has three layers: generated game code, a Model 2 runtime that stands in for the board, and a thin host platform layer. Generated code only ever touches the machine through the runtime's memory bus, so the same output runs under a trace harness or in the shipping executable.

```mermaid
flowchart TD
  ROM[User ROMs] --> RC[i960 recompiler<br/>offline tool]
  RC --> GEN[Generated C++<br/>game functions]
  GEN --> BUS[Runtime memory bus]
  BUS --> TGP[TGP HLE]
  BUS --> VID[Tilemaps + display lists]
  BUS --> SND[Sound: 68k + YM3438 + MultiPCM]
  BUS --> IO[I/O, drive, comm HLE]
  TGP --> REN[GPU renderer]
  VID --> REN
  REN --> HOST[SDL3 host layer]
  SND --> HOST
  IO --> HOST
```

The recompiler runs at build time on the user's machine, so no generated Sega code is ever distributed.

**Frame loop.** One host frame = one Model 2 video frame. The runtime runs generated code until the game waits on vblank, fires the vblank interrupt handler, drains the TGP FIFO into a display list, then renders and presents. The sound board then advances one frame of board time on the same thread (see Audio).

**Memory bus.** Main RAM, work RAM and shared RAM are flat host arrays accessed inline. MMIO ranges (TGP FIFO, geometrizer, tilemap RAM, palette, I/O dual-port RAM, sound UART, comm RAM) go through a page-table of handlers, resolved at compile time where the address is constant.

**No fallback.** Every instruction the game runs is statically recompiled to native code. There is no interpreter in the shipped build. A jump to an address with no recompiled code is a hard error that names the address; the fix is to add it to `seeds/daytona93.txt` and recompile. The runtime (`src/runtime/cpu`) holds the i960 context and the services generated code calls (call/return with the register cache, interrupt entry, memory); it has no interpreter. The reference interpreter in `src/refcore` exists only for the test harness (`m2replay`) and is never linked into the game (`m2native` does not link it).

## i960 static recompiler

The recompiler turns the program ROM into one C++ function per i960 procedure, preserving the architectural register file in a context struct so behaviour matches instruction-for-instruction. Optimisation (register promotion, flag elision) comes after parity, never before.

**Pipeline**

1. Load and de-interleave the program ROMs into a flat image using MAME's ROM map for `daytona`.
2. Seed entry points: reset vector, the interrupt table, the fault table, the system procedure table (for `calls`), and any addresses from a hand-maintained `seeds.toml`.
3. Recursive-descent disassembly over the four formats (REG, COBR, CTRL, MEM). Follow `call`, `callx`, `bal`, `balx`, branches and compare-and-branch.
4. Resolve indirect targets: pattern-match jump tables (`ld` from a scaled index then `bx`/`callx`), and merge in targets observed by the MAME tracer (see Reference & validation).
5. Build a CFG per procedure, then emit C++ with one label per basic block and `goto` edges.
6. Emit a dispatch table (address → function pointer) for all indirect calls; a miss is a hard error naming the address (no fallback).

**Decoder** (`src/i960`, shared by `i960dis` and the recompiler)

- Decoding follows MAME's *executor* (`i960.cpp`), not its disassembler: the executor is the behavioural oracle. The accepted set is exactly what the executor implements: 62 CTRL/COBR/MEM opcodes and 102 REG opcodes. Anything else MAME's disassembler knows (Cx/Hx/Jx additions, `cmpibno`, `cmpibo`, `atadd`, `bswap`, ...) decodes but is marked not executable; the recompiler refuses it. `0x6e1` is decoded as `movre`, as MAME executes it; it is undocumented.
- Text output reproduces MAME's disassembler syntax exactly, so the two can be diffed as strings (`tests/mame_oracle.cpp` compiles MAME's `i960dis.cpp` unmodified against a small shim).
- Three encodings are read differently by MAME's executor and disassembler. The decoder follows the executor and flags each one (`Insn::quirks`); hardware behaviour for all three is unconfirmed, and any occurrence in Daytona's code is a finding:
  - CTRL/COBR bits 1:0 set: the executor adds them to the branch target (`sext(opcode, 24) - 4`); the disassembler masks them.
  - MEMB bits 6:5 set: the executor ignores them; the disassembler rejects the word.
  - MEMB scale > 4: the executor shifts by it; the disassembler rejects the word.
- The i960 manual (270567-001) is not yet a second check on the decode; the Ghidra SLEIGH cross-check below is the planned one.

**Ghidra as the analysis workbench**

> **Correction (checked 24 Sep 2026):** mainline Ghidra has never shipped an i960 processor module: none in `Ghidra/Processors` at `8e9a8e7a`, none in the last 40 release tags back to 9.2, and no i960 path anywhere in its history. The module in use is the third-party [mumbel/ghidra_i960](https://github.com/mumbel/ghidra_i960) (Apache-2.0, pinned at `727ef787`, fetched by `scripts/fetch_ghidra_i960.sh`), which loads into Ghidra from `Ghidra/Processors/` and into pypcode for the decoder cross-check.
>
> **Cross-check result** (`tests/ghidra_oracle.py`): all 23,258 statically reachable Daytona instructions decode identically in our decoder and in SLEIGH (validity, mnemonic, length, target, operands). On 1,000,000 random words the only differences left are (a) MAME's disassembler printing the integer src1 of `cvtir`/`cvtilr`/`scaler`/`scalerl` as an FP register (MAME's executor and SLEIGH both read an integer; text only), and (b) `movre`: MAME executes 0x6e9 and the undocumented 0x6e1, the module decodes only 0x6e1. Encodings that set bits the KB reserves are flagged as decoder quirks and counted, not compared; none occurs in reachable code.

- Load the de-interleaved program image into Ghidra with its i960 processor module. It becomes the shared, annotated map of the game code.
- Use it to name functions, mark jump tables, label MMIO accesses (TGP FIFO, sound UART, I/O dual-port RAM, comm RAM) and document data structures such as car state and course tables.
- A Ghidra script exports function starts, names and jump-table targets into `seeds.toml`, so every name reaches the generated C++ and trace logs read as `update_car_physics`, not `sub_0001A3F0`.
- Cross-check our disassembler against Ghidra's SLEIGH decode: any mismatch in instruction length, operand or branch target is a bug in one of them. Confirm which i960 variant the module models, since KB FP instructions matter here.
- Findings from MAME traces (indirect targets, code executed from RAM) are imported back into the Ghidra project so the map stays complete.

**Register and state model**

- `ctx.g[16]` globals (g15 = frame pointer), `ctx.r[16]` locals (r0 PFP, r1 SP, r2 RIP), `ctx.ac`, `ctx.pc`, `ctx.tc`, and `ctx.fp[4]` for the i960KB's extended FP registers.
- The condition code lives in `ac`; emit it lazily and only materialise it when a later `bx`/`test`/`modac` or a call boundary reads it.

**Calls and the local register cache**

The i960 saves the 16 local registers on every `call` and restores them on `ret`, via an on-chip cache that spills to the stack frame. The recompiled `call` becomes a C++ call plus an explicit save of `r[]` into the new frame's memory, so code that walks frames or does `flushreg` sees the same stack bytes as hardware. `bal`/`balx` are leaf links through g14 and compile to plain calls with no local save.

**Floating point**

The KB's FPU works in 80-bit extended precision, which ARM64 hosts lack. Any FP op whose result can reach memory or a compare goes through SoftFloat `extF80`; a fast path uses host `double` only where a unit test proves bit-identical results. Physics divergence from wrong rounding is the likeliest source of replay desync, so this is tested first.

**Measured FP use (MAME harvest, `daytona93`).** Of the 23,258 instructions statically reachable from 333 harvested entry points, 108 are FP, and all are conversions, compares and scaling: `cvtri` 39, `cmpr` 37, `cvtir` 22, `scaler` 8, `cvtzri` 2. There is no FP add, subtract, multiply or divide, no transcendental and no `...rl` (extended) form. The geometry maths is on the TGP. 21 of 68 indirect sites are still unexercised, so unreached code may add more; re-measure as coverage grows. For these five operations the extF80 path is small and each can be tested exhaustively or near it.

**As built** (`src/i960/fp.{h,cpp}`, measured operand forms: every one of the 108 FP instructions takes g/l registers, i.e. single precision in and out; never fp0-fp3 or FP literals; AC = `3f001000`, round to nearest, all exceptions masked):

- `ref_*`: the hardware model. Operands widened to extF80, computed and rounded by SoftFloat 3e in any AC rounding mode, IEEE flags reported. Invalid conversions return `0x80000000` (SoftFloat's Intel integer indefinite), unconfirmed for the i960.
- `fast_*`: native host float operations, round to nearest only; this is what recompiled code runs, at native speed. Legal only because `tests/test_fp --exhaustive` proves them bit-identical to `ref_*`: every 2^32 input for cvtri, cvtzri and cvtir, and every 2^32 single for scaler at 11 boundary exponents, 0 mismatches (about 5 minutes on 4 cores). A fast path that rounds like MAME (`std::round`) fails it with 130,905 mismatches in the quick run.
- A different rounding mode, or FP on fp0-fp3, would not have a proven fast path and must go through `ref_*` until one is proven.

**MAME is not a bit-exact FP oracle.** MAME's i960 holds `fp0`-`fp3` as host `double` (`i960.h`, `double m_fp[4]`) and computes in `double`. Wherever Daytona's results depend on the extra bits of extended precision, correct extF80 output and MAME's output disagree, and the lockstep diff will report it. Unresolved; see Open questions.

**Interrupts and faults**

- Interrupts are taken only at safe points: backward branches, calls and returns emit a cheap `if (ctx.irq_pending)` check that dispatches through the interrupt table with the proper `intctl`/priority rules.
- Faults (e.g. integer overflow, alignment) dispatch through the fault table; in practice we log and abort on any unexpected fault during bring-up.

**Code outside ROM**

If the game copies routines into RAM or patches code, the tracer will show execution from RAM. Those regions are recompiled from a RAM snapshot taken after the copy and checked by hash at runtime; a mismatch is a hard error.

## Geometry (TGP) and rendering

The TGP is replaced by C++ that consumes the same FIFO command stream the i960 writes, and the rasterizer is replaced by a modern GPU backend fed from an intermediate display list. This split lets us diff geometry output against MAME numerically, independent of how pixels end up on screen.

**TGP: statically recompiled**

- The i960 uploads the TGP's program, then pushes commands and parameters into the copro FIFO and reads results back from the output FIFO.
- The TGP has no fixed microcode: its whole program is the 2,024 words the i960 uploads from the game's data ROM (main_data 0x860020, CRC 0xd6d611dd). So the TGP gets the same treatment as the i960: `tools/m2tgprecomp` statically recompiles that program to native C++ (one label per word, MAME's MB86233 semantics inlined per instruction from `src/runtime/tgp.h`, direct gotos for constant branches, a switch for computed ones). No MB86233 interpreter or hand-written HLE; nothing is interpreted at run time. At boot the runtime checks that the uploaded words are the ones recompiled (hard error otherwise).
- Clockless coupling: the TGP runs only when the i960 needs it (reading the output FIFO or its status) and returns when it stalls on an empty input FIFO. FIFO contents are order-deterministic, so no cycle model is needed for them. **Measured**: `m2tgpcheck` replays MAME's TGP-side log and matches all of attract: 12,390,181 TGP instructions, all native, every register checked after every instruction, 1,293,703 input words, 398,072 output words and 7,789 banked accesses identical to MAME; 0.02 s without the per-instruction check. Also identical through a 6,000-frame race (230.6 M TGP instructions, 3.2 M banked accesses), time attack (97.7 M) and the TGP self-test (32.6 M).
- **Measured, native i960 + native TGP together** (`m2native` models the geometry ports, the TGP FIFOs and buffer RAM): attract, a 6,000-frame race and time attack match MAME, every TGP output word identical (6.7 M in the race) and the whole 128 KB buffer RAM hash identical at every sample (11,951 in the race). Buffer RAM is built from its three writers (the i960, the geometrizer command port, the TGP's banked writes). The only differing reads are the i960 polling the TGP's mailbox (the last three dwords of buffer RAM): our TGP has finished when the i960 looks, MAME's (paced by cycle estimates) has not. Rule for the shipped build: the TGP runs to its FIFO wait before the i960 reads the FIFO, its status or the mailbox; the game sees fewer poll iterations, nothing else.
- Two units share the geometry work: the TGP (programmable, results can return to the i960) and the geometrizer, which walks the display list in buffer RAM at vblank and transforms, lights, clips and projects polygons for the rasterizer. Which of the two does what for Daytona is established from traces, not assumed.
- MAME runs the TGP microcode at low level (MB86234 = MB86233 core); it is the oracle the recompiled program is matched against bit for bit, including its float rounding (host IEEE single, no FP contraction). MAME's geometrizer is HLE in host `float`, so it is a weaker oracle for display-list output than the TGP is for FIFO results.
- Any command that returns results to the i960 (e.g. collision or matrix readback) must return identical values, since game logic depends on them.

**Geometrizer: native HLE**

The original Model 2's geometrizer runs code from ROM inside its DSP; that code is not dumped, so there is nothing to recompile. It is native C++ transplanted from MAME's high-level implementation (`src/runtime/geo.cpp`): at vblank it walks the display list in buffer RAM, transforms, lights, culls and clips, and produces the polygon list. Measured against MAME at every vblank (same i960 instruction count), on our own buffer RAM: identical rasterizer input and kept polygons through a whole race (see Milestones). The rasterizer takes 24-bit floats (MAME's `f2u(x) >> 8`). Its libm calls (`hypot`, `sqrt`) are to be pinned to a correctly rounded implementation so all hosts agree.

**Display list**

One frame's output is a flat list: polygon (4 verts, screen xyz, uv, colour, texture page/format, translucency, fog, sort key) plus tilemap layer state. It is dumpable to disk, which makes renderer bugs reproducible without running the game.

**Renderer**

- Backend: SDL3 GPU API (Vulkan on Linux/Windows, Metal on macOS, D3D12 optional). One shader path, no per-platform shader forks.
- Textures: decode Model 2 texture RAM/ROM formats to RGBA8 atlases on upload, cached by content hash.
- Sorting: reproduce the hardware's priority/z-sort order first; a z-buffer mode is an enhancement toggle, since hardware ordering artefacts are part of the look.
- Tilemaps (HUD, speedometer, course map, text) render as a separate layer at native 496x384 and scale with nearest or sharp-bilinear filtering.

**Vita GXM presentation enhancements.** The Vita branch exposes the same
options described below, but the wide GPU tile capability is currently disabled
after hardware feedback reported worse speed. Widescreen uses the CPU tile
composition fallback; original-aspect GPU tiles and GPU 3D remain enabled.
The GPU tile implementation is retained for profiling, not claimed faster.

The retained implementation supports the same aspect, scenery-distance and
per-item HUD policies. Original
mode uses GPU System24 composition. Wide mode now uses the same GXM tile
compositor for its backdrop, with only background destination x coordinates
scaled when stretch is enabled. Unstretched margins use palette pen0, like
the original GXM clear. As in main3044f3b, foreground tiles also use GXM except
when per-item HUD relocation needs CPU composition. Tile decoding remains CPU.
Road subdivision additionally measures texture error using main's reciprocal
depth interpolation, targeting0.5texel edge-midpoint error within the existing
eight-way cap and pool limits. This remains an affine approximation, not
main's per-fragment shader, and can add vertex work. The previous Wide2 CPU backdrop remains
available when the explicit Vita GPU-background capability is disabled.
Unchanged foreground pixels reuse HUD grouping/uploads. The Vita stretch option
applies to all wide backdrops; desktop retains its exact 3D-coverage gating. The homogeneous-WVP and wide GPU tile changes were
withdrawn after IMG_2856 showed missing textured geometry on real hardware.
The previous affine subdivision path is restored. Road-line wobble and wide-mode
cost remain unresolved; host matrix tests did not validate actual GXM output. Fixed896x384 layer textures
cover up to21:9; panel fitting preserves aspect on960x544. The layer arena grows
from10 to12MiB (32MiB total GPU arenas), without per-frame GPU allocations.
These options are not a promise of full-speed hardware performance.

**Vita GPU buffering.** Upstream's Double Buffered / Single Buffered / Every
Third Frame labels select frame skipping, not physical buffer counts. That
Vita integration was withdrawn after hardware game/audio stutter reports.
The subsequent configurable physical ring has also been removed after a
hardware report of 30 FPS. Vita now uses unmodified upstream libvita2d
presentation and its standard three-surface display queue, with one vblank
wait in the display callback. There is no buffer selector, ring switching,
single-surface path or software frame skipping. Old gpu_buffers settings are
ignored. Native audio stays device-clocked and board timing stays native;
this rollback is not evidence of 60 FPS on hardware.

**Enhancements (all off by default)**

With every enhancement off the build is the game as MAME runs it; parity checks run that way. When on, an enhancement may change game logic (rules.md, changed 1 Oct 2026: previously "never change game logic", which ruled out widening the game's own culling).

| Option | Approach | Risk |
| --- | --- | --- |
| Internal resolution | Render 3D at N× or window size | Low |
| Widescreen | **Done** (launcher: 16:10, 16:9, 21:9). Same focal length, viewport widened: the geometrizer's side clip planes and the rasterizer's clip move out by a margin for full-width viewports, the 3D layer is drawn margin-shifted into a wider buffer, tilemaps (HUD, text) stay 496 wide in the centre, and the side margins continue the back tilemaps in 3D scenes (drawn margin to margin with draw()'s own scroll, layer-split and mask rules) or each row's edge colours on 2D screens. Option "HUD at the screen edges": the race HUD's side groups move out by the margin, decided per item (front-layer pixels grouped into blobs; a blob moves only if wholly inside a group, so banners crossing a group stay whole), plus the condition panel's own overlay quads (the polygons at its box's sort z inside its outline), only while that box is on screen. The scenery list includes nearby cells omitted by the game's narrow directional mask and retains course exclusions and the original selected-cell order | Widening selection can increase object-list work; the geometrizer still clips to the view |
| Draw distance and polygon budget | **Scenery done** (launcher slider: Shortest, Shorter, Default, Further, Furthest; polygon budget: Automatic or Custom; `m2run --draw-distance/--draw-budget`). The game draws scenery by course cell: a 16x16 grid, the 5x5 cells around the car filtered by visibility masks into a list (0x16f74..0x17070; count 0x5016c0, cells from 0x5016c1, room for 63), then object lists until a per-frame cost allowance (0x5010f4, 5000 at boot) is exceeded. At native height, Automatic uses `max(ceil(5000 * max(width, 496) / 496), 5000 * (1 + max(draw_distance, 0)))`, capped at 1,000,000. This gives 5,000 at original width, 6,875 at 16:9 and 9,033 at 21:9 with Default distance. A positive Custom allowance overrides that calculation. The road is a separate 14-section window (0x13f5c: 5 behind, 8 ahead) shared with game logic and is unchanged | Further adds scenery but not road; cost allowance measures object metadata rather than visible polygon count |
| Texture filtering | Bilinear/anisotropic on atlases | Low; atlas padding needed |
| High frame rate | Interpolate display lists between frames | High; logic stays at native rate |
| MSAA | Standard multisample target | Low |

The headless `m2run` and `m2gpushot` tools can exercise the same scenery
selection and automatic budget at 32:9. The launcher remains limited to 21:9
until the wider view's sky boundary and presentation are addressed.

## Audio, inputs, force feedback, link play

Nothing here is interpreted either. A board whose CPU runs a program gets that program statically recompiled, like the i960 and the TGP; a board whose behaviour is a fixed protocol gets native C++ for that protocol (HLE). If an HLE turns out not to be exact, the board's own program is recompiled instead; there is no fallback to an interpreter.

**Audio**

- Daytona uses the Model 1 sound board, not the SCSP board later Model 2 revisions carry (see Target hardware summary). Its 68000 program (`epr-16489`/`16490`, loaded by the importer) is statically recompiled to native C++ by the same approach as the i960 (**done**): `src/m68k` decodes (checked word for word against MAME's 68000 disassembler on all 1,916 reachable instructions), `tools/m2sndrecomp` emits each instruction inline with operands resolved, direct branches as gotos and returns/interrupts through a dispatch switch; flag arithmetic is in `src/runtime/snd_cpu.h`. The program has no indirect jumps, so reachability from the vector table finds all of it. No 68000 interpreter.
- Lockstep (`tools/m2sndcheck`, MAME patch 0003): MAME logs every device access, interrupt and a register snapshot every 4,096 instructions; the recompiled code replays it. Attract (15.7M instructions, 48 interrupts) and race (77.9M instructions, 3,648 interrupts, 5.16M device accesses) match exactly.
- The driver is polled: the UART's RxRDY raises IPL 2 and the handler queues each byte; the main loop polls YM3438 timer B (reload 0xfc: 868 Hz) for its tick. So the board needs no clock: time is counted in completed 68000 instructions at the rate MAME's 68000 runs this program (752,000 per second), and events land on that count: a command byte arriving one line-time (10 bits at 31.25 kbit/s) after the previous, a YM timer expiring (kept in exact YM clocks so the tick does not drift). The chips are rendered up to the current count before each register write.
- The YM3438 is ymfm (BSD-3, the same core MAME uses; identical in the OPN2 path at the pinned commit). The MultiPCMs are MAME's `multipcm`/`gew` (BSD-3) transplanted into `src/runtime/multipcm.cpp`. Mix gains are MAME's (YM 0.30, each MultiPCM 0.50).
- Music and effects volumes (launcher, Audio tab; an enhancement, 100% and 100% by default, and then the output is MAME's bit for bit): at each MultiPCM key-on the board reads which driver channel owns the voice from the 68000's voice pools (0xf01500 and 0xf01618, 28 records of 10 bytes each; byte 3 the channel, byte 6 0xff on the engine layers' reserved records), and each chip sums music and effect voices apart before its 16-bit clamp. Music is channels 0-9 and 15, the ones the driver's "stop music" command stops; the engine's fixed slots are effects. Checked against the native sequencer's channels, note for note, in attract and race on both sets (`docs/issues.md`, #10).
- Main CPU ↔ sound CPU traffic is a serial byte stream through an i8251 UART at 31.25 kbit/s. The i960's IRQ3 handler (request bit 10) is the transmit loop: Daytona never polls the UART status (MiSTer core, R87), so the UART interrupt must be modelled or no sound data is sent. The i960 side hands its bytes over each frame; the sound board receives them at the line rate.
- Output via SDL3 audio: the YM (55.6 kHz) and MultiPCM (44.6 kHz) outputs go to two SDL audio streams at their own rates, which SDL resamples and mixes. The game runs on the display clock and the device on its own, so a speed trim of at most 0.5% holds the queue near 60 ms.

**Native audio replacement (experimental, shared across frontends)**

A separate, user-requested backend replaces the recompiled sound program and
sound-chip device models with direct ROM command/sequence decoding and a
48 kHz PCM voice mixer. The existing backend remains the accuracy reference;
native mode never silently falls back to it. This is an explicit exception to
the sound-board recompilation policy above, not a claim of chip-level parity.

The shared engine is portable C++. SDL2 on Vita and SDL3 on desktop provide
device-clocked callbacks; bounded command queues separate main-board updates
from sequencing and sample generation. Menu pause, reset and shutdown stop the
callback before destroying its engine or ROM storage. Native mode deliberately
keeps music/sample playback at device speed even when graphics frames are slow.

The initial native mixer uses linear interpolation, a short linear ADSR and
equal-power panning rather than reproducing MultiPCM envelopes/LFOs bit for bit.
It must be labelled experimental until the command/voice behavior and listening
tests cover the full game. Unsupported commands, invalid data and queue
overflows must remain visible, not be replaced with invented sounds.

The native output calibration uses master gain 1.95, increased from GPU24's
0.75 after 6,000-frame attract and race measurements found native RMS about
8.25 dB below the reference. This is a 2.6x amplitude boost before protection
and frontend volume. A stereo-linked peak limiter caps peaks at 0.98, with
immediate attack and a 50 ms exponential recovery time constant. It allocates
nothing, adds no lookahead latency and keeps state across render blocks.
Limiter activity is counted separately from hard clipping. The shared change
does not alter reference audio, command/voice parameters or game timing;
aggregate output-level calibration is not waveform or perceived-loudness parity.

FM was audited with both 6,000-frame attract and race replays: all 23,178,664
FM float samples were exactly zero; no channel key-ons or DAC enable occurred.
Static sound-driver call sites only initialize YM registers and use timer
control. The native sequencer supplies that event clock without an FM generator.
The measurements cover these replays, not an assertion about other ROM sets.

**Inputs**

| Arcade control | Host mapping |
| --- | --- |
| Steering (ADC) | Wheel axis or gamepad left stick, with deadzone and linearity curves |
| Gas / brake (ADC) | Pedals or triggers |
| 4-speed shifter | H-pattern shifter buttons, or gear up/down on bumpers |
| VR (view) buttons, start | Face buttons |
| Coin, test, service | Keyboard; free-play toggle in settings |

The game's test menu is left intact for calibration, but the runtime also exposes calibrated values directly so players never need it.

**Force feedback**

The game sends motor commands to the drive board through an output port (I/O board dual-port RAM byte 0x11). The HLE decodes them and maps them to SDL3 haptic effects on wheels, and to rumble on gamepads.

This section first assumed the board plays centring, jolts, road rumble and off-road shake, and the first decoder took its meanings from Supermodel's notes on Sega's later drive boards. Both were wrong for Daytona. The board's own program (EPR-16488A), disassembled in MAME 0.289 and run there with commands and wheel positions fed to it, runs one effect at a time from the wheel's position (its own ADC): motor on/off, resistance, a centring spring with or without a dead zone, uncentring, and a constant force either way, each with strength 0-7. It has no vibration or rumble. The command table is in `src/runtime/drive_board.h` and `docs/issues.md` (#9). A wheel whose haptics fail to open gets no force rather than rumble; gamepads rumble only for the constant force and uncentring.

**Link play**

The Revision A comm board exposes shared RAM that each cabinet reads in a
ring. The implementation imported from MAME's m2comm simulation uses TCP,
not the originally planned UDP. Each cabinet listens for the preceding one
and connects to the next; frame synchronization is optional and off by
default. Vita uses SceNet IPv4 with bounded non-blocking transmit queues
and the same shared CommBoard protocol. LAN first; internet traversal is
out of scope. The 1993 set does not support this Revision A link protocol.

The Vita package can contain two native executables, one per recompiled ROM
set. The options ROM selector replaces the current executable using
sceAppMgrLoadExec after saving and stopping audio, rather than retaining
both games in memory. Each set has its own ROM validation, settings, and
NVRAM directory. Neither ZIP nor any extracted ROM assets are packaged.

## Reference & validation

Correctness is proven by lockstep differential testing against MAME, frame by frame, with the original PCB used to settle cases where MAME itself may be wrong. No subsystem is "done" until a recorded input replay matches MAME for a full race.

**MAME as oracle**

- Build a patched MAME with a trace plugin (Lua `emu` API plus small C++ hooks in the i960 core and copro FIFO) that records per frame: hash of main/work RAM, i960 register file at vblank, every TGP FIFO word, TGP results returned to the CPU, sound UART bytes, and output ports.
- **As built** (`tools/mame-plugins/m2trace`, format in `docs/trace-format.md`): everything above is reachable from Lua alone through memory write/read taps and `read_range`, so no C++ hook is needed for the first version. The indirect-branch harvest (below) is the one item that still needs a C++ hook or the debugger.
- **Sample point.** A per-frame sample must fall at the same guest instant in both builds, which MAME's end-of-frame notifier does not guarantee: it is not on an instruction boundary our build can reproduce. The default sample is taken inside the i960 store that acknowledges vblank (write to 0x00e80000 with bit 0 clear). That this is once per frame in Daytona is unverified until the first trace.
- Record input as a per-frame ADC/button stream (`.inp`-style, our own format). The same stream drives both MAME and our build. As built, the stream carries each field's mask and default so it is readable without MAME's port definitions, and a replay re-records what the machine saw and is valid only if that matches the stream exactly.
- A diff tool walks both traces and stops at the first divergent frame, then narrows to the first divergent FIFO word or RAM range.
- Also harvest every indirect branch target MAME executes; these feed back into recompiler seeds.

**Original hardware as ground truth**

- Capture from a real Daytona PCB: video via a capture setup that accepts 24 kHz medium resolution (per MAME's timing; confirm on the PCB), audio line out, and (if feasible) a logic analyser on the TGP FIFO bus.
- Use it for: exact refresh rate and frame pacing, polygon sort artefacts, texture filtering look, sound mix levels, and force-feedback behaviour.
- When MAME and hardware disagree, hardware wins and the finding is logged as an upstream MAME note.

**Sega Model 2 MiSTer core (Ben's FPGA project) as a third reference**

- Link: https://github.com/alphanu1/sega-model2-mister (GPL-3). It targets `daytona93` and runs attract mode with sound; 3D does not reach the screen yet.
- The HDL is a readable, hardware-level description of the board: TGP command handling, rasterizer polygon ordering, texture formats and sound board behaviour. Use it to answer questions MAME's source leaves ambiguous.
- A Verilator simulation of the core can emit the same per-frame trace format as the MAME plugin, giving a second independent oracle for the diff tool.
- Running the core on MiSTer gives native-rate video output for side-by-side capture when a real PCB isn't to hand.
- Knowledge flows both ways: divergences found by the recomp's replay tests point at bugs in the core, and vice versa.

**Test tiers**

| Tier | What | When |
| --- | --- | --- |
| Unit | Per-instruction i960 tests vs MAME core, incl. extF80 FP | Every commit |
| Boot | Reset to attract mode, RAM hash match for 600 frames | Every commit |
| Replay | Full race per course from recorded inputs, zero divergence | Nightly |
| Visual | Display-list and screenshot diff vs MAME at native res | Nightly |
| Hardware | Side-by-side capture vs PCB | Per milestone |

## Platform layer & build

One CMake project, C++20, SDL3 for window, input, haptics, audio and GPU, so platform-specific code stays under a few hundred lines. The user runs a first-launch importer that verifies their ROMs and generates the game code locally.

| Platform | Arch | Graphics | Toolchain | Package |
| --- | --- | --- | --- | --- |
| Windows 10/11 | x86-64, ARM64 | Vulkan or D3D12 | MSVC or clang-cl | Zip with exe |
| macOS 12+ | ARM64, x86-64 | Metal | Apple clang | Signed, notarised .app (universal) |
| Linux | x86-64, ARM64 | Vulkan | GCC or clang | AppImage + Flatpak |
| Android 10+ | ARM64 (x86-64 for emulators) | Vulkan | NDK clang | APK; generated code built by the desktop importer, or on device, then loaded (decision pending) |
| Raspberry Pi 4/5 | ARM64 (64-bit Pi OS) | Vulkan (V3DV) | GCC or clang | Linux ARM64 build |

The generated code is plain portable C++20 (no host assembly, no JIT), so the same output compiles for every row above.

**ROM handling**

- The importer accepts a MAME-format `daytona.zip` (or parent/clone set), checks every file against a CRC/SHA1 manifest, and refuses unknown revisions with a clear message.
- It then runs the recompiler and a bundled compiler step, or, simpler for users, loads a prebuilt runtime plus a generated shared library compiled on first run. Decision pending (see Open questions).
- Assets (textures, samples) stay in the ROM images; nothing is extracted to loose files.

**Other runtime features**

- Settings UI (Dear ImGui overlay): controls, enhancements, audio, link peers, DIP-switch equivalents.
- Save states for debugging only, built from the context struct plus RAM; not a player feature in v1.
- Crash reports include the last guest PC and any address that had no recompiled code.

## M1 plan: boot to attract, i960 parity only

M1's exit criterion is "recompiled code reaches attract mode with RAM hashes matching MAME; no graphics" (Test tiers: Boot, 600 frames). It tests the recompiler and the i960 runtime, nothing else, so everything outside the i960 is taken from MAME's trace for now.

**Devices are replayed from the MAME trace.** In 600 frames of attract the i960 reads the TGP FIFO 697,078 times, the I/O board's dual-port RAM 1,127,280 times, plus FIFO status, video control and the sound UART. Those values determine RAM. The TGP is M2's work and the I/O board and sound are M4's, so in M1 the runtime answers every read of a tapped device range with the value MAME returned at the same position in the trace, and checks every device write against the trace. A mismatch is reported with epoch and event index, which is the lockstep diff for free. Untapped MMIO (tilemap, palette) is plain RAM in M1.

**Native execution, no instruction clock.** Generated code is straight C++ over the context struct and the bus; there is no per-instruction cycle counting and no clocked device model in the shipped path (the user's constraint, and the design's). Interrupts are taken at safe points (backward branches, calls, returns). Whether that can match MAME without a clock depends on where MAME takes interrupts. **Measured** (harvest patch logs the IP of each):

| run | interrupts | in the idle loops (0x12b0/0x12b8, 0x12f0/0x12f8) | elsewhere |
| --- | --- | --- | --- |
| attract, 600 frames | 627 | 603 | 24, mostly boot |
| race, 6,000 frames | 10,795 | ~6,450 | ~4,300, nearly all the sound UART (vector 0x0f) in main-line code, e.g. 1,664 at 0x19cfc, 568 at 0x1924c |

vblank (vector 0x0c) lands in an idle loop ~99% of the time, so clockless safe-point delivery matches MAME for it. The UART interrupt does not: during races it lands wherever MAME's approximate cycle count puts it, inside code that does real work between frames. A native build cannot reproduce that without a clock, and MAME's placement is itself an artefact of its cycle estimates (the PCB places it by real bus timing). Open decision, below.

**Register cache.** MAME's i960 keeps 4 frames of local registers on chip: a call with the cache full writes the current set to its own frame, a return above depth 4 reloads it from memory, `flushreg` writes all cached sets. Stack RAM, and so the RAM hashes, depend on this, so the runtime reproduces MAME's model exactly (a depth counter and a 4-slot array, memcpy cost). The real KB spills the oldest set instead; code that reads frame memory without `flushreg` would see the difference. Recorded as a MAME-vs-hardware question; MAME's model is used for lockstep.

**Order of work.**

1. Runtime core: context (g/l registers, AC, PC, TC, IP, register cache), bus (flat RAM arrays for ROM, RAM 0x00200000, work RAM, buffer RAM, backup SRAM; page table for MMIO), trace-replay device, interrupt controller (request/enable registers, ICR, pending table as MAME keeps it).
2. Instruction semantics as inline C++ functions, one per opcode in MAME's executable set, shared by the reference core (test harness only) and the generated code, so there is one definition of each instruction. FP through `src/i960/fp` fast paths.
3. Reference core over those semantics (test harness only, never shipped). First target: interpreter alone reaches attract with RAM hashes matching MAME for 600 frames. This proves the runtime, the replay and the semantics before any code generation. **Met**: `tools/m2replay` matches all 600 frames (70,926,456 instructions, 1,153 samples, 3,315,201 device events, 627 interrupts). Semantics are MAME's `i960.cpp`, transplanted (BSD-3). Two findings shaped the bus: MAME's multi-word loads and stores advance the address only on regions flagged `BURST` (so a FIFO is popped repeatedly), and buffer RAM is also written by the geometrizer and the TGP, so M1 replays the i960's accesses to it and leaves its hash to M2.
4. Recompiler: C++ from `reach` (seeds: boot record + MAME harvest in `seeds/daytona93.txt`), one label per instruction, a dispatch switch for indirect targets, no fallback. Exit: generated code reaches attract with matching hashes for 600 frames, all native. **Met**: `tools/m2native` runs 23,262 recompiled instructions and matches all 600 frames (70,926,456 instructions, every one native, 1,153 samples, 3,315,201 device events, 627 interrupts) in 1.3 s with lockstep checks on. The same code also matches through seven scripted scenarios of up to 6,000 frames (races on all three courses, time attack, test mode; up to 664 M instructions and 12,475 interrupts each), including the UART interrupts mid-code, because lockstep replays MAME's interrupt placement. Finding: IAC 0x93 (reinitialise) is an indirect transfer (to 0x924 at boot); the harvest patch now logs it.
5. Shipped build: interrupts at safe points instead of a check per instruction, RAM accessed inline instead of through the virtual bus, no reference core linked.

## Milestones, risks, open questions

The critical path is i960 parity, then TGP parity; rendering and polish can proceed in parallel once display lists are stable.

**Milestones**

1. **M0 Tooling:** MAME trace plugin, input recorder, trace diff tool, i960 disassembler.
2. **M1 Boot:** Recompiled code reaches attract mode with RAM hashes matching MAME; no graphics.
3. **M2 Geometry:** the recompiled TGP program matches MAME's FIFO output for attract mode; display lists dump correctly. **Met**: native i960 + recompiled TGP + native buffer RAM + native geometrizer match MAME's rasterizer input and kept polygons bit for bit through attract, a 6,000-frame race and time attack (152 M rasterizer words, 6.7 M polygons in the race).
4. **M3 Pixels:** GPU renderer draws attract mode and a race at native res; tilemaps and HUD work. **Progress**: a CPU reference rasterizer (MAME's renderer transplanted, `src/runtime/raster.cpp`) draws the 3D layer from the native pipeline pixel-identical to MAME: 423 attract frames and 5,587 race frames. It is the ground truth the GPU backend is measured against. The segaic24 tilemaps, palette pens, CRTC offsets and composition are native too (`src/runtime/video.cpp`): the whole composed screen is identical to MAME for 596 attract frames and 5,996 race frames.
5. **M4 Playable:** Sound, inputs, full-race replay parity on all three courses.
6. **M5 Cabinet feel:** Force feedback, link play on LAN, PCB side-by-side validation.
7. **M6 Ship:** Importer, packaging on all three OSes, enhancements, settings UI.

**Risks**

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Extended-precision FP mismatch | Replay desync, AI/physics drift | SoftFloat extF80 everywhere first; optimise later with proof. Measured: reachable i960 FP is only cvtri/cmpr/cvtir/scaler/cvtzri (108 instructions); no FP arithmetic. Risk much lower than assumed, pending full coverage |
| Indirect branches missed statically | Hard error naming the address | MAME-harvested targets in `seeds/daytona93.txt`; 21 of 68 indirect sites still unharvested (circuit select, test mode) |
| TGP behaviour poorly documented | Wrong geometry, collision | Trace MAME per command; logic-analyse the real bus if needed |
| Hardware sort order hard to reproduce on GPU | Visual artefacts differ | CPU-side sort replicating hardware keys; z-buffer optional |
| Interrupt timing differences | Rare hangs, audio drift | Safe-point IRQ checks; cycle-count estimates per block if required |
| Licensing of reused cores | Can't ship | Transplant only BSD/MIT code (MAME's newer files); audit early |

**Open questions**

- [ ] Which ROM revision(s) to support first (Japan, export, Special Edition / Hornet)?
- [ ] Does Daytona copy or patch any i960 code in RAM at runtime?
- [x] Lockstep with the sound UART interrupt (fires mid-code during races): **(a)**, decided. The shipped build takes it at safe points at native speed; lockstep tests replay MAME's exact delivery points, keyed by MAME's completed-instruction count, in a test-only harness.
- [ ] Which third-party i960 SLEIGH module to use for Ghidra, if any (mainline has none), and is its licence compatible?
- [ ] Is Daytona entirely interrupt-driven? Static reach from the boot record finds 89 instructions ending in a `b .` idle loop; the MAME harvest shows the vblank handler at 0x0e00 taken 576 times in 600 frames. Seeded with the 107 harvested targets, static reach grows to 13,081 instructions. Consistent with interrupt-driven; confirm by where time is spent.
- [x] Does MAME run the `daytona` TGP at low level or with HLE handlers? **Low level**: the MB86234 core executes the microcode the i960 uploads. Its accuracy against the PCB is still unmeasured.
- [ ] MAME's i960 FP is host `double`. When extF80 and MAME disagree, which does lockstep treat as correct: a MAME-compatible `double` mode for the diff, or a patched MAME with extF80?
- [ ] MAME's `addc` never sets carry (both operands are `uint32_t`, so bit 32 of the sum is always 0; `subc` was fixed upstream, `addc` was not). Does Daytona execute `addc` with a carry-out that matters? Recompile to the silicon and flag the diff, as the MiSTer core does.
- [ ] Does Daytona upload geometrizer code (`geo_prg_w`, 0x00804000), or run its fixed transform loops only?
- [ ] Ship a prebuilt runtime with runtime codegen, or require a local C++ compiler at import?
- [ ] PCB access: which board revision is available, and can the TGP FIFO be probed?
- [ ] Refresh is 57.524 Hz per MAME (provisional; PCB unmeasured).
- [x] Should frame pacing lock to host 60 Hz or run at native rate with VRR? Native rate by default, on the wall clock, on any display; locking to the display (60 frames/s on 60, 120, 240 Hz), smooth pacing on a 57.52 Hz display and VRR pacing are launcher settings, all off (`src/app/pacing.h`, issue #7).
