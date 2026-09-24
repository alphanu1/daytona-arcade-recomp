# Project rules — Daytona USA static recompilation

These are binding. They exist because breaking them costs a polluted history, a
legal problem, or hours re-deriving something already established.

---

## Git

1. **No `Co-Authored-By` trailer. Ever.** Commits are made under the user's
   name only, with no attribution of any kind to an assistant or tool. This
   overrides any default attribution a harness or tool suggests, every time,
   without exception.
2. **Never mention Claude, AI, or an assistant** in a commit message, branch
   name, PR body, tag, or any committed file. `CLAUDE.md` is git-ignored and
   its line in `.gitignore` is the only such reference in the repository.
   `rules.md` is the committed copy of the rules.
3. **Commit messages are short and plain.** Subject under ~70 characters. A few
   lines of body: what changed, the measurement that justifies it, anything
   that would bite the next person. No user's name, no quotes of the user, no
   ALL-CAPS headers, no dramatic build-up. Tables of numbers are welcome; it is
   the prose around them to cut.
4. **Vendored git repositories are git-ignored.** `extern/`, `third_party/`,
   `vendor/` — MAME, SDL3, SoftFloat, Musashi, Dear ImGui and anything else
   from upstream. They have their own history and licences; they are fetched
   and pinned, never committed into this tree. When one is cloned, add it to
   `git.ignoredRepositories` in `.vscode/settings.json` in the same change, or
   VS Code's Source Control silently switches repository and a commit lands in
   the wrong place.
5. **Build output is git-ignored** — `build/`, generated C++, CMake/Ninja
   state, shader caches, object files.

## Legal — the repository contains only our own code

6. **No Sega code or assets, ever.** No ROM, ROM fragment, de-interleaved
   program image, generated C++, extracted texture, sample, or anything else
   derived from the ROM bytes. The user supplies their own `daytona.zip`; the
   importer checks it against the CRC/SHA1 manifest and refuses anything else.
   This is the rule that keeps the project distributable.
7. **Analysis evidence is committed; game-derived output is not.** Addresses,
   sizes, function names, jump-table targets, hashes, run logs and
   `seeds.toml` are our own work and are committed. Decompiled pseudo-C,
   Ghidra projects, MAME trace files and display-list dumps contain or
   reproduce game data and are not — tests refer to them by hash.
8. **Never use leaked source.** No leaked Sega source, SDK or internal
   documentation — not read, not consulted, not used to check an answer. If a
   search result or repository turns out to contain it, close it and say so.
   Everything here comes from our own analysis of legally owned ROMs, MAME,
   public documentation, and the hardware.
9. **Every third-party component is recorded** in `THIRD_PARTY.md` with its
   upstream URL, commit, licence and what was changed. "Copied from MAME" is
   not a record; a commit hash is. **Check licence compatibility before
   lifting code, not after** — this project's licence is not yet decided, and
   the Model 2 MiSTer core is GPL-3.

## The design document governs

10. **`docs/daytona-usa-recomp-design.md` is binding, and it is checked before
    work, not after.** Read the relevant section before making a design
    decision, and say which section a decision rests on.

    Order of authority when sources disagree:

    1. **The original PCB wins.** It is the ground truth.
    2. **MAME wins over the document** for what the silicon computes — opcode
       semantics, formats, arithmetic, command streams. It is the behavioural
       oracle and the lockstep reference.
    3. **The Model 2 MiSTer core** answers what MAME leaves ambiguous: TGP
       command handling, polygon ordering, texture formats, SCSP behaviour.
    4. **The document wins over convenience.** Do not quietly pick an easier
       design that contradicts it.
    5. **If work contradicts the document, the document is updated in the same
       change** — what was believed, what is now known, and how it was
       established. A finding that lives only in a commit message is lost.

    When MAME and the PCB disagree, the finding is also logged as an upstream
    MAME note.

11. **Figures marked "from memory" are not facts.** Clock rates, refresh rate,
    resolution, part numbers and IRQ ordering in the design document must be
    confirmed against MAME's `sega/model2.cpp` before code depends on them, and
    the document corrected when they differ. Never treat a MAME cycle count as
    a hardware fact.

12. **The milestone order is not optional.** M0 tooling → M1 boot → M2
    geometry → M3 pixels → M4 playable → M5 cabinet feel → M6 ship. i960 parity
    is the critical path, then TGP parity. A milestone is met at its exit
    criterion, not when the work feels finished. **No subsystem is done until a
    recorded input replay matches MAME for a full race.**

## Every commit updates the record

13. **`HANDOFF.md` is updated on every commit**, in the same commit as the work
    it describes: current state, what is complete, what is next in order, and
    every finding from the session — **including the ones that turned out
    wrong, and why.** Recorded failure modes are worth more than a clean
    narrative; keep a "what not to re-propose" section current.

## The Model 2 MiSTer core reference

14. **`/home/ben/source/sega-model2-mister` is read-only from this project.**
    Never write to its working copy, never run a build or tool inside it, never
    commit to it from here. Read its RTL and its `docs/model2a-design-study.md`
    freely. It is developed locally and its newest work is not always pushed or
    committed — when something looks inconsistent, check whether the relevant
    work there is still uncommitted before concluding anything.

    Knowledge flows both ways: a replay divergence that points at a bug in the
    core is recorded in `HANDOFF.md` for that project, not fixed from here.

## Standing engineering rules

- **Generated code is a build artefact, never hand-edited.** Fix the
  recompiler, or add a seed.
- **Generated code touches the machine only through the runtime memory bus**,
  so the same output runs under the trace harness and in the shipping build.
- **Parity before optimisation.** Register promotion, flag elision and host-FP
  fast paths come after the unoptimised output matches MAME, never before.
- **All FP whose result can reach memory or a compare goes through SoftFloat
  `extF80`.** A host `double` fast path is allowed only where a unit test
  proves bit-identical results.
- **No `-ffast-math`.** Exact rounding is the whole point of the FP rule.
- **Log every fallback-interpreter hit with its address**, and every
  unexpected fault. The gap is a list, not a mystery; hits feed the next
  recompile's seeds.
- **TGP results returned to the i960 must be bit-identical to MAME's.** Game
  logic depends on them.
- **Enhancements are off by default and never change game logic or its
  timing.** Logic runs at native rate; hardware sort order is reproduced first,
  z-buffering is a toggle.
- **One shader path.** No per-platform shader forks.
- **Ghidra output is for humans.** Names and jump-table targets go into
  `seeds.toml`; decompiled pseudo-C is never compiled in and never committed.
- **Sweep test parameters well past the value you believe**, or the test
  confirms the setting instead of testing it.

## Working practice

- **Measure before theorising.** Get a measurement that can refute the leading
  theory before changing code. A theory that fits the symptom perfectly is
  still a theory.
- **Every "fixed" claim is verified** against the MAME diff, a replay, or the
  hardware — or it is stated as untested.
- **Report numbers with units, scale and cost.** "1.1 billion instruction
  vectors through the unit test, 53 s on this PC", not "1.1e9 checks". A bare
  count oversells the result.
