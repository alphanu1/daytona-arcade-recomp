# Third-party components

Every component this project fetches, links or reads from is recorded here:
upstream, pinned commit, licence, how it is used and what was changed.
Vendored checkouts live in git-ignored `extern/` and are never committed.

| Component | Upstream | Commit | Licence | Use | Changes |
| --- | --- | --- | --- | --- | --- |
| MAME `src/devices/cpu/i960/i960dis.cpp`, `i960dis.h` | https://github.com/mamedev/mame | `dddd73680656e355bb2b5beecab1167c9f07bf81` | BSD-3-Clause (file header) | Compiled unmodified into the `mame_oracle` test only, as the reference disassembler. Not linked into any tool or shipped binary. | None. `tests/mame_shim/emu.h` (ours) stands in for MAME's `emu.h`. |
| MAME `src/devices/cpu/i960/i960.cpp` | same | same | BSD-3-Clause | Read, not compiled: the executor's opcode set, operand, EA and branch-target rules are what `src/i960/decode.cpp` follows. No code copied. | — |
| MAME `src/mame/sega/model2*.cpp`, `src/mame/shared/segam1audio.cpp` | same | same | BSD-3-Clause | Read, not compiled: hardware figures in the design document. | — |

| `patches/mame/0001-i960-indirect-branch-harvest.patch` | ours, against MAME `dddd7368` | — | BSD-3-Clause, as the file it patches | Applied by `scripts/fetch_mame.sh`. Logs every `bx`/`balx`/`callx`/`calls` target, interrupt vector target and ICR value set by `synmov` to `$M2TRACE_BRANCHES` at exit. Off unless that variable is set. | Adds `device_stop` and 7 log points (including the IP each interrupt is taken at) to `src/devices/cpu/i960/i960.{h,cpp}`. |
| MAME Lua scripting API (`docs/source/luascript/`) | same | same | BSD-3-Clause | Read, not compiled: the API `tools/mame-plugins/m2trace` is written against. No code copied. | — |
| Berkeley SoftFloat 3e | https://github.com/ucb-bar/berkeley-softfloat-3 | `a0c6494cdc11865811dec815d5c0049fba9d82a8` | BSD-3-Clause | Linked into the `i960` library as the extF80 reference model for i960 FP (`src/i960/fp.cpp`, `ref_*`). Fetched by `scripts/fetch_softfloat.sh`; built from `cmake/softfloat_sources.cmake` (FAST_INT64, 8086-SSE specialisation). | None. Built with its own options; `THREAD_LOCAL` set so rounding mode and flags are per thread. |
| pypcode | https://github.com/angr/pypcode | PyPI 3.3.3 | BSD-2-Clause | Test-only: its SLEIGH compiler and decoder run the i960 module in `tests/ghidra_oracle.py`. Not shipped; the test skips (77) without it. | — |
| ghidra_i960 (third-party Ghidra i960 processor module) | https://github.com/mumbel/ghidra_i960 | `727ef7872c5b1cd6ceb5a81f5e474d1ced92945c` | Apache-2.0 | Test-only second decode reference (SLEIGH), and the Ghidra module for the analysis workbench. Fetched by `scripts/fetch_ghidra_i960.sh` into git-ignored `extern/`. Not shipped. | None; compiled `.sla` is a local build artefact. |
| lupa (Python binding with Lua 5.4) | https://github.com/scoder/lupa | PyPI release, not pinned | MIT | Test-only: runs the plugin's Lua under `tests/lua_*_test.py`. Not shipped. Tests skip (exit 77) without it. | — |

Fetch with `scripts/fetch_mame.sh`, which checks out exactly the commit above.

The Model 2 MiSTer core (https://github.com/alphanu1/sega-model2-mister,
GPL-3.0-or-later, read at `591e148e87d27e03d50cbf7318bf0b1d1328c4bf`) is read as
a reference only. Nothing is lifted from it; the project licence is undecided.
