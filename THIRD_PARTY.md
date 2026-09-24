# Third-party components

Every component this project fetches, links or reads from is recorded here:
upstream, pinned commit, licence, how it is used and what was changed.
Vendored checkouts live in git-ignored `extern/` and are never committed.

| Component | Upstream | Commit | Licence | Use | Changes |
| --- | --- | --- | --- | --- | --- |
| MAME `src/devices/cpu/i960/i960dis.cpp`, `i960dis.h` | https://github.com/mamedev/mame | `dddd73680656e355bb2b5beecab1167c9f07bf81` | BSD-3-Clause (file header) | Compiled unmodified into the `mame_oracle` test only, as the reference disassembler. Not linked into any tool or shipped binary. | None. `tests/mame_shim/emu.h` (ours) stands in for MAME's `emu.h`. |
| MAME `src/devices/cpu/i960/i960.cpp` | same | same | BSD-3-Clause | Read, not compiled: the executor's opcode set, operand, EA and branch-target rules are what `src/i960/decode.cpp` follows. No code copied. | — |
| MAME `src/mame/sega/model2*.cpp`, `src/mame/shared/segam1audio.cpp` | same | same | BSD-3-Clause | Read, not compiled: hardware figures in the design document. | — |

| `patches/mame/0001-i960-indirect-branch-harvest.patch` | ours, against MAME `dddd7368` | — | BSD-3-Clause, as the file it patches | Applied by `scripts/fetch_mame.sh`. Logs every `bx`/`balx`/`callx`/`calls` target and interrupt vector target to `$M2TRACE_BRANCHES` at exit. Off unless that variable is set. | Adds `device_stop` and 5 log points to `src/devices/cpu/i960/i960.{h,cpp}`. |
| MAME Lua scripting API (`docs/source/luascript/`) | same | same | BSD-3-Clause | Read, not compiled: the API `tools/mame-plugins/m2trace` is written against. No code copied. | — |
| pypcode | https://github.com/angr/pypcode | PyPI 3.3.3 | BSD-2-Clause | Checked for an i960 SLEIGH spec (it has none). Not used by any build or test. | — |
| lupa (Python binding with Lua 5.4) | https://github.com/scoder/lupa | PyPI release, not pinned | MIT | Test-only: runs the plugin's Lua under `tests/lua_*_test.py`. Not shipped. Tests skip (exit 77) without it. | — |

Fetch with `scripts/fetch_mame.sh`, which checks out exactly the commit above.

The Model 2 MiSTer core (https://github.com/alphanu1/sega-model2-mister,
GPL-3.0-or-later, read at `591e148e87d27e03d50cbf7318bf0b1d1328c4bf`) is read as
a reference only. Nothing is lifted from it; the project licence is undecided.
