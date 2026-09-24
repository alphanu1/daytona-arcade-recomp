# Daytona USA (Model 2) Static Recompilation — Design Document

Sep 24, 2026 · Ben

## Overview & goals

We statically recompile the i960 game code of Daytona USA (Sega Model 2, 1994) into portable C++, and replace every other board subsystem with high-level or reused emulation, to ship native builds for Windows, macOS and Linux. MAME is the behavioural oracle during development; an original Model 2 PCB is the ground truth when MAME and our build disagree.

**Goals**

- Native x86-64 and ARM64 executables on Windows, macOS (Apple Silicon + Intel) and Linux, from one codebase.
- Frame-exact gameplay parity with the arcade: timing, physics, AI, attract mode, all three courses, all cars.
- User-supplied ROMs only. The repo ships the recompiler, runtime and a ROM-to-build pipeline, never Sega code or assets.
- Modern presentation as opt-in: native resolution, widescreen, higher internal refresh for rendering, filtered textures.
- Real hardware for input: wheel, pedals, 4-speed shifter, force feedback, and cabinet link play over LAN.

**Non-goals (v1)**

- Other Model 2 titles. The recompiler stays generic, but runtime HLE targets Daytona's code paths first.
- Daytona USA 2 (Model 3, PowerPC) and the Saturn / PC ports.
- Recompiling the sound 68000 or the drive/comm board CPUs; those run interpreted (see Audio, inputs, force feedback, link play).

## Target hardware summary

Daytona runs on the original Model 2 board (MAME set `daytona`, driver `sega/model2.cpp`, machine `model2o_state::daytona`), not 2A/2B/2C, so the geometry processor is the Fujitsu TGP rather than a SHARC or TGPx4.

Figures below are confirmed against MAME at `dddd7368` (`src/mame/sega/model2.cpp`, `src/mame/shared/segam1audio.cpp`) and cross-checked against the Model 2 MiSTer core at `591e148e`. They are MAME's figures, not hardware measurements: rule 11 still applies, and the PCB settles anything marked provisional.

| Subsystem | Part (MAME) | Clock (MAME) | Strategy |
| --- | --- | --- | --- |
| Main CPU | Intel i960KB (`I80960KB`), little-endian | `50_MHz_XTAL / 2` = 25 MHz | Static recompilation to C++ |
| Coprocessor | One Fujitsu MB86234 TGP (`m_copro_tgp`); MAME's MB86234 is an empty subclass of its MB86233 | `50_MHz_XTAL` = 50 MHz | HLE in C++, validated against MAME's LLE TGP |
| Geometrizer | Separate from the TGP: walks the display list in buffer RAM at vblank (`geo_parse`, `model2_v.cpp`) | — | Reimplemented; MAME's version is HLE |
| Rasterizer | Sega custom chips; MAME has no device, it is driver code | — | Replaced by host GPU renderer |
| 2D tilemaps / HUD | `S24TILE` (System 24 tilemap chip) | — | Reimplemented, composited on GPU |
| Screen | `set_raw(32_MHz_XTAL/2, 656, 0, 496, 424, 0, 384)` | 16 MHz pixel clock | 496x384 active, 656x424 total |
| Sound | Model 1 sound board (`SEGAM1AUDIO`): 68000 + YM3438 + 2x MultiPCM | 68000 `20_MHz_XTAL / 2` = 10 MHz, YM3438 8 MHz, MultiPCM 10 MHz each | Interpreted 68k + existing FM/MultiPCM cores |
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

Priority is not fixed by the board. MAME takes each line's vector from the i960's ICR and uses `priority = vector / 8`, so **the ordering is whatever Daytona's PRCB programs**. Read it from a trace; do not assume one. On vblank MAME runs `geo_parse` first (when 60 Hz mode is set, or on even frames in 30 Hz mode, per `videocontrol` bit 0) and then raises bit 0.

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

**Frame loop.** One host frame = one Model 2 video frame. The runtime runs generated code until the game waits on vblank, fires the vblank interrupt handler, drains the TGP FIFO into a display list, then renders and presents. The sound CPU runs on its own thread in fixed time slices, synced by audio buffer position.

**Memory bus.** Main RAM, work RAM and shared RAM are flat host arrays accessed inline. MMIO ranges (TGP FIFO, geometrizer, tilemap RAM, palette, I/O dual-port RAM, sound UART, comm RAM) go through a page-table of handlers, resolved at compile time where the address is constant.

**Fallback interpreter.** A small i960 interpreter runs any code the recompiler did not reach (unexpected indirect targets, self-test paths). Every fallback hit is logged with its address so the next recompile can include it.

## i960 static recompiler

The recompiler turns the program ROM into one C++ function per i960 procedure, preserving the architectural register file in a context struct so behaviour matches instruction-for-instruction. Optimisation (register promotion, flag elision) comes after parity, never before.

**Pipeline**

1. Load and de-interleave the program ROMs into a flat image using MAME's ROM map for `daytona`.
2. Seed entry points: reset vector, the interrupt table, the fault table, the system procedure table (for `calls`), and any addresses from a hand-maintained `seeds.toml`.
3. Recursive-descent disassembly over the four formats (REG, COBR, CTRL, MEM). Follow `call`, `callx`, `bal`, `balx`, branches and compare-and-branch.
4. Resolve indirect targets: pattern-match jump tables (`ld` from a scaled index then `bx`/`callx`), and merge in targets observed by the MAME tracer (see Reference & validation).
5. Build a CFG per procedure, then emit C++ with one label per basic block and `goto` edges.
6. Emit a dispatch table (address → function pointer) for all indirect calls; misses go to the fallback interpreter.

**Ghidra as the analysis workbench**

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

**MAME is not a bit-exact FP oracle.** MAME's i960 holds `fp0`-`fp3` as host `double` (`i960.h`, `double m_fp[4]`) and computes in `double`. Wherever Daytona's results depend on the extra bits of extended precision, correct extF80 output and MAME's output disagree, and the lockstep diff will report it. Unresolved; see Open questions.

**Interrupts and faults**

- Interrupts are taken only at safe points: backward branches, calls and returns emit a cheap `if (ctx.irq_pending)` check that dispatches through the interrupt table with the proper `intctl`/priority rules.
- Faults (e.g. integer overflow, alignment) dispatch through the fault table; in practice we log and abort on any unexpected fault during bring-up.

**Code outside ROM**

If the game copies routines into RAM or patches code, the tracer will show execution from RAM. Those regions are recompiled from a RAM snapshot taken after the copy and checked by hash at runtime; a mismatch falls back to the interpreter.

## Geometry (TGP) and rendering

The TGP is replaced by C++ that consumes the same FIFO command stream the i960 writes, and the rasterizer is replaced by a modern GPU backend fed from an intermediate display list. This split lets us diff geometry output against MAME numerically, independent of how pixels end up on screen.

**TGP HLE**

- The i960 uploads the TGP's program, then pushes commands and parameters into the copro FIFO and reads results back from the output FIFO.
- Two units share the geometry work: the TGP (programmable, results can return to the i960) and the geometrizer, which walks the display list in buffer RAM at vblank and transforms, lights, clips and projects polygons for the rasterizer. Which of the two does what for Daytona is established from traces, not assumed.
- MAME runs the TGP microcode at low level (MB86234 = MB86233 core), so we trace its output per command and match it bit-for-bit, including its fixed/float rounding. MAME's geometrizer is HLE in host `float`, so it is a weaker oracle for display-list output than the TGP is for FIFO results.
- Any command that returns results to the i960 (e.g. collision or matrix readback) must return identical values, since game logic depends on them.

**Display list**

One frame's output is a flat list: polygon (4 verts, screen xyz, uv, colour, texture page/format, translucency, fog, sort key) plus tilemap layer state. It is dumpable to disk, which makes renderer bugs reproducible without running the game.

**Renderer**

- Backend: SDL3 GPU API (Vulkan on Linux/Windows, Metal on macOS, D3D12 optional). One shader path, no per-platform shader forks.
- Textures: decode Model 2 texture RAM/ROM formats to RGBA8 atlases on upload, cached by content hash.
- Sorting: reproduce the hardware's priority/z-sort order first; a z-buffer mode is an enhancement toggle, since hardware ordering artefacts are part of the look.
- Tilemaps (HUD, speedometer, course map, text) render as a separate layer at native 496x384 and scale with nearest or sharp-bilinear filtering.

**Enhancements (all off by default)**

| Option | Approach | Risk |
| --- | --- | --- |
| Internal resolution | Render 3D at N× or window size | Low |
| Widescreen | Widen projection in TGP HLE viewport; HUD stays 4:3 centred | Culling pops at screen edges |
| Texture filtering | Bilinear/anisotropic on atlases | Low; atlas padding needed |
| High frame rate | Interpolate display lists between frames | High; logic stays at native rate |
| MSAA | Standard multisample target | Low |

## Audio, inputs, force feedback, link play

These subsystems are small, timing-tolerant and well emulated already, so they are interpreted or HLE'd rather than recompiled.

**Audio**

- Daytona uses the Model 1 sound board, not the SCSP board later Model 2 revisions carry (see Target hardware summary). Its 68000 runs in an embedded interpreter (e.g. Musashi, MIT-licensed) on its own thread; it is cheap at 10 MHz.
- The YM3438 and the two MultiPCMs use proven cores (MAME's `ymopn`/`multipcm`, or others). Licence check before choosing.
- Main CPU ↔ sound CPU traffic is a serial byte stream through an i8251 UART at 31.25 kbit/s. The runtime queues bytes with a timestamp so ordering matches the arcade even across threads. The i960's IRQ3 handler (request bit 10) is the transmit loop: Daytona never polls the UART status (MiSTer core, R87), so the UART interrupt must be modelled or no sound data is sent.
- Output via SDL3 audio at 44.1 kHz with a resampler; the chips' native rates are kept internally.

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

The game sends motor commands to the drive board through an output port. The HLE decodes them (centering, jolts, road rumble, off-road shake) and maps them to SDL3 haptic effects on wheels, falling back to gamepad rumble. MAME's drive-board notes and output logs are the reference for the command set.

**Link play**

The comm board exposes shared RAM that each cabinet reads in a ring. The HLE implements that ring over UDP with lockstep per frame: each peer sends its outgoing block, waits for the others, then advances. LAN first; internet play needs rollback and is out of scope for v1.

## Reference & validation

Correctness is proven by lockstep differential testing against MAME, frame by frame, with the original PCB used to settle cases where MAME itself may be wrong. No subsystem is "done" until a recorded input replay matches MAME for a full race.

**MAME as oracle**

- Build a patched MAME with a trace plugin (Lua `emu` API plus small C++ hooks in the i960 core and copro FIFO) that records per frame: hash of main/work RAM, i960 register file at vblank, every TGP FIFO word, TGP results returned to the CPU, sound UART bytes, and output ports.
- Record input as a per-frame ADC/button stream (`.inp`-style, our own format). The same stream drives both MAME and our build.
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

**ROM handling**

- The importer accepts a MAME-format `daytona.zip` (or parent/clone set), checks every file against a CRC/SHA1 manifest, and refuses unknown revisions with a clear message.
- It then runs the recompiler and a bundled compiler step, or, simpler for users, loads a prebuilt runtime plus a generated shared library compiled on first run. Decision pending (see Open questions).
- Assets (textures, samples) stay in the ROM images; nothing is extracted to loose files.

**Other runtime features**

- Settings UI (Dear ImGui overlay): controls, enhancements, audio, link peers, DIP-switch equivalents.
- Save states for debugging only, built from the context struct plus RAM; not a player feature in v1.
- Crash reports include the last guest PC and fallback-interpreter hits.

## Milestones, risks, open questions

The critical path is i960 parity, then TGP parity; rendering and polish can proceed in parallel once display lists are stable.

**Milestones**

1. **M0 Tooling:** MAME trace plugin, input recorder, trace diff tool, i960 disassembler.
2. **M1 Boot:** Recompiled code reaches attract mode with RAM hashes matching MAME; no graphics.
3. **M2 Geometry:** TGP HLE matches MAME's FIFO output for attract mode; display lists dump correctly.
4. **M3 Pixels:** GPU renderer draws attract mode and a race at native res; tilemaps and HUD work.
5. **M4 Playable:** Sound, inputs, full-race replay parity on all three courses.
6. **M5 Cabinet feel:** Force feedback, link play on LAN, PCB side-by-side validation.
7. **M6 Ship:** Importer, packaging on all three OSes, enhancements, settings UI.

**Risks**

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Extended-precision FP mismatch | Replay desync, AI/physics drift | SoftFloat extF80 everywhere first; optimise later with proof |
| Indirect branches missed statically | Crashes, fallback slowdown | MAME-harvested targets + interpreter fallback with logging |
| TGP behaviour poorly documented | Wrong geometry, collision | Trace MAME per command; logic-analyse the real bus if needed |
| Hardware sort order hard to reproduce on GPU | Visual artefacts differ | CPU-side sort replicating hardware keys; z-buffer optional |
| Interrupt timing differences | Rare hangs, audio drift | Safe-point IRQ checks; cycle-count estimates per block if required |
| Licensing of reused cores | Can't ship | Pick BSD/MIT cores (MAME's newer files, Musashi); audit early |

**Open questions**

- [ ] Which ROM revision(s) to support first (Japan, export, Special Edition / Hornet)?
- [ ] Does Daytona copy or patch any i960 code in RAM at runtime?
- [x] Does MAME run the `daytona` TGP at low level or with HLE handlers? **Low level**: the MB86234 core executes the microcode the i960 uploads. Its accuracy against the PCB is still unmeasured.
- [ ] MAME's i960 FP is host `double`. When extF80 and MAME disagree, which does lockstep treat as correct: a MAME-compatible `double` mode for the diff, or a patched MAME with extF80?
- [ ] MAME's `addc` never sets carry (both operands are `uint32_t`, so bit 32 of the sum is always 0; `subc` was fixed upstream, `addc` was not). Does Daytona execute `addc` with a carry-out that matters? Recompile to the silicon and flag the diff, as the MiSTer core does.
- [ ] Does Daytona upload geometrizer code (`geo_prg_w`, 0x00804000), or run its fixed transform loops only?
- [ ] Ship a prebuilt runtime with runtime codegen, or require a local C++ compiler at import?
- [ ] PCB access: which board revision is available, and can the TGP FIFO be probed?
- [ ] Refresh is 57.524 Hz per MAME (provisional; PCB unmeasured). Should frame pacing lock to host 60 Hz or run at native rate with VRR?
