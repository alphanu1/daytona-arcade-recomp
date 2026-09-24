#!/usr/bin/env python3
"""Drive tools/mame-plugins/m2trace/init.lua against a mock of the MAME Lua
API (notifiers, memory taps, read_range, cpu state, I/O ports).

  lua_plugin_mock_test.py <build-dir> <scratch-dir>

What this proves: the plugin's own logic (tap -> event records, vblank-ack ->
sample, input record -> replay -> self-check, frame limit) behaves as
docs/trace-format.md says, and its output is readable by the C++ tools.
What it does not prove: that MAME's real API behaves like the mock. That needs
a MAME run with the user's ROM, and is recorded as untested until then.
"""

import os
import subprocess
import sys

try:
    import lupa.lua54 as lupa
except ImportError:
    print("lua_plugin_mock_test: SKIP (pip install lupa)")
    sys.exit(77)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGINS = os.path.join(ROOT, "tools", "mame-plugins")

MOCK = r"""
local M = { notifiers = { reset = {}, frame = {}, stop = {} }, wtaps = {}, rtaps = {},
            env = {}, log = {}, exited = false, frame = 0, mem_seed = 0 }

os.getenv = function(k) return M.env[k] end
package.preload["lfs"] = function() return { mkdir = function() end } end

emu = {
  romname = function() return "daytona" end,
  app_version = function() return "0.mock" end,
  print_info = function(s) M.log[#M.log + 1] = "I " .. s end,
  print_error = function(s) M.log[#M.log + 1] = "E " .. s end,
  add_machine_reset_notifier = function(cb) table.insert(M.notifiers.reset, cb) return {} end,
  add_machine_frame_notifier = function(cb) table.insert(M.notifiers.frame, cb) return {} end,
  add_machine_stop_notifier = function(cb) table.insert(M.notifiers.stop, cb) return {} end,
}

-- Memory: region content is a deterministic function of (address, mem_seed).
local space = {}
function space:install_write_tap(s, e, name, cb) table.insert(M.wtaps, { s, e, cb }) return {} end
function space:install_read_tap(s, e, name, cb) table.insert(M.rtaps, { s, e, cb }) return {} end
function space:read_range(s, e, width)
  local n = (e - s + 1) // 4
  local t = {}
  for i = 0, n - 1 do t[#t + 1] = string.pack("<I4", ((s + i * 4) * 2654435761 + M.mem_seed) & 0xffffffff) end
  return table.concat(t)
end

local state = {}
for i, name in ipairs({ "pfp","sp","rip","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13","r14","r15",
                        "g0","g1","g2","g3","g4","g5","g6","g7","g8","g9","g10","g11","g12","g13","g14","fp",
                        "pc","ac","ip" }) do
  state[name] = { value = i * 16 }
end

-- I/O ports: fields hold a current state; read() composes the port value.
local function mkfield(mask, def, analog, class)
  local f = { mask = mask, defvalue = def, is_analog = analog, type_class = class or "controller", cur = nil }
  function f:set_value(v) self.cur = v end
  return f
end
local function mkport(fields)
  local p = { fields = fields }
  function p:read()
    local v = 0
    for _, f in pairs(self.fields) do
      local fv
      if f.is_analog then
        local sh = 0; local m = f.mask; while (m & 1) == 0 do m = m >> 1; sh = sh + 1 end
        fv = ((f.cur or (f.defvalue >> sh)) << sh) & f.mask
      else
        fv = f.defvalue & f.mask
        if f.cur == 1 then fv = fv ~ f.mask end
      end
      v = v | fv
    end
    return v
  end
  return p
end
M.ports = {
  [":IN0"] = mkport({ START = mkfield(0x10, 0x10, false), COIN = mkfield(0x01, 0x01, false, "misc") }),
  [":IN1"] = mkport({ VR4 = mkfield(0x01, 0x01, false), GEARBITS = mkfield(0x70, 0x00, false, "misc") }),
  [":GEARS"] = mkport({ G1 = mkfield(0x02, 0x00, false) }),
  [":STEER"] = mkport({ STEER = mkfield(0xff, 0x80, true) }),
  [":ACCEL"] = mkport({ ACCEL = mkfield(0xff, 0x20, true) }),
  [":BRAKE"] = mkport({ BRAKE = mkfield(0xff, 0x20, true) }),
  [":DSW"] = mkport({ SW1 = mkfield(0x01, 0x01, false, "dipswitch") }),
}

manager = { machine = {
  devices = { [":maincpu"] = { state = state, spaces = { program = space } } },
  screens = { [":screen"] = { frame = 0, frame_number = function(self) return self.frame end } },  -- a method, as in real MAME
  ioport = { ports = M.ports },
  exit = function() M.exited = true end,
} }

-- Bus helpers for the test script.
function M.write(addr, data, mask)
  for _, t in ipairs(M.wtaps) do if addr >= t[1] and addr <= t[2] then t[3](addr, data, mask) end end
end
function M.read(addr, data, mask)
  for _, t in ipairs(M.rtaps) do if addr >= t[1] and addr <= t[2] then t[3](addr, data, mask) end end
end
function M.reset() for _, cb in ipairs(M.notifiers.reset) do cb() end end
function M.end_frame()
  for _, cb in ipairs(M.notifiers.frame) do cb() end
  M.frame = M.frame + 1
  manager.machine.screens[":screen"].frame = M.frame
end
function M.stop() for _, cb in ipairs(M.notifiers.stop) do cb() end end
return M
"""

failures = checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


# Per-frame "player" inputs for the recording run: (START pressed, steer, gear1).
PLAYER = [(0, 0x80, 0), (1, 0xa0, 1), (0, 0xe0, 1), (0, 0x40, 0)]


def run(env, scratch, player=None, stale_replay=False, mem_change_frame=None):
    """One emulated session of len(PLAYER) frames. Returns the mock."""
    lua = lupa.LuaRuntime(encoding=None)
    lua.execute(f'package.path = "{PLUGINS}/?.lua;{PLUGINS}/?/init.lua"'.encode())
    M = lua.execute(MOCK.encode())
    for k, v in env.items():
        M.env[k.encode()] = v.encode()
    plugin = lua.eval(b'(require("m2trace"))')
    plugin.startplugin()
    M.reset()
    ports = M.ports
    for fr in range(len(PLAYER)):
        if player:
            start, steer, gear = player[fr]
            ports[b":IN0"].fields[b"START"].cur = start
            ports[b":STEER"].fields[b"STEER"].cur = steer
            ports[b":GEARS"].fields[b"G1"].cur = gear
        if stale_replay and fr == 2:
            # Simulate MAME applying a replayed value one frame late.
            ports[b":STEER"].fields[b"STEER"].cur = 0x11
        if mem_change_frame is not None and fr == mem_change_frame:
            M.mem_seed = 7
        # The frame's bus traffic: TGP push, TGP result read, sound byte, vblank ack.
        M.write(0x00884000, 0x3F800000 + fr, 0xFFFFFFFF)
        M.read(0x00884000, 0x12340000 + fr, 0xFFFFFFFF)
        M.write(0x01C80000, 0x41 + fr, 0x000000FF)
        M.write(0x00E80000, 0xFFFFFFFE, 0xFFFFFFFF)  # vblank ack -> sample
        M.end_frame()
        if M.exited:
            break
    M.stop()
    return M


def log_lines(M):
    return [M.log[i + 1].decode() for i in range(len(M.log))]


def main():
    build, scratch = sys.argv[1], sys.argv[2]
    os.makedirs(scratch, exist_ok=True)
    t1, t2, inp = (os.path.join(scratch, n) for n in ("rec.m2tr", "replay.m2tr", "rec.m2in"))
    tracediff = os.path.join(build, "tracediff")

    # Recording run.
    M = run({"M2TRACE_OUT": t1, "M2TRACE_RECORD_INPUT": inp}, scratch, player=PLAYER)
    check(not any(l.startswith("E ") for l in log_lines(M)), f"record run errors: {log_lines(M)}")
    r = subprocess.run([tracediff, t1, t1], capture_output=True, text=True)
    check(r.returncode == 0 and f"identical: {len(PLAYER)} epochs, {4 * len(PLAYER)} events" in r.stdout,
          "recorded trace has one epoch per vblank ack and four events each\n" + r.stdout + r.stderr)

    # Input stream: DSW (dipswitch) and IN1 gear bits left out; the rest recorded.
    with open(inp, "rb") as f:
        blob = f.read()
    for tag in (b":IN0", b":IN1", b":GEARS", b":STEER", b":ACCEL", b":BRAKE"):
        check(tag in blob, f"input header has {tag}")
    check(b":DSW" not in blob, "dipswitch port not recorded")

    # Replay run: no player; the plugin drives the fields.
    M = run({"M2TRACE_OUT": t2, "M2TRACE_REPLAY_INPUT": inp}, scratch)
    log = log_lines(M)
    check(any(f"replay matched the recording for {len(PLAYER)} frames" in l for l in log),
          f"replay self-check passes: {log}")
    r = subprocess.run([tracediff, t1, t2], capture_output=True, text=True)
    check(r.returncode == 0, "record and replay traces identical\n" + r.stdout)

    # A replay that lands one frame late must be caught by the self-check.
    M = run({"M2TRACE_REPLAY_INPUT": inp}, scratch, stale_replay=True)
    log = log_lines(M)
    check(any("replay diverged at frame 2, field :STEER/ff" in l for l in log) and
          any("replay INVALID from frame 2" in l for l in log), f"late replay detected: {log}")

    # Memory that changes from frame 1 shows up as a region hash at epoch 1.
    run({"M2TRACE_OUT": t2}, scratch, player=PLAYER, mem_change_frame=1)
    r = subprocess.run([tracediff, t1, t2], capture_output=True, text=True)
    check(r.returncode == 1 and "DIVERGED at epoch 1" in r.stdout and "region 00200000" in r.stdout,
          "memory change detected at epoch 1\n" + r.stdout)

    # A second reset mid-run leaves a note in the trace.
    lua = lupa.LuaRuntime(encoding=None)
    lua.execute(f'package.path = "{PLUGINS}/?.lua;{PLUGINS}/?/init.lua"'.encode())
    Mr = lua.execute(MOCK.encode())
    Mr.env[b"M2TRACE_OUT"] = t2.encode()
    lua.eval(b'(require("m2trace"))').startplugin()
    Mr.reset()
    Mr.write(0x00E80000, 0xFFFFFFFE, 0xFFFFFFFF)
    Mr.reset()
    Mr.write(0x00E80000, 0xFFFFFFFE, 0xFFFFFFFF)
    Mr.stop()
    r = subprocess.run([tracediff, t2, t2], capture_output=True, text=True)
    check("note @1: reset at epoch 1" in r.stdout, "second reset noted in trace\n" + r.stdout)

    # Frame limit: exits after 2 frames.
    M = run({"M2TRACE_OUT": t2, "M2TRACE_FRAMES": "2"}, scratch, player=PLAYER)
    check(M.exited and M.frame == 2, "M2TRACE_FRAMES=2 exits after two frames")

    # frame trigger: samples come from the frame notifier, still one per frame.
    M = run({"M2TRACE_OUT": t2, "M2TRACE_TRIGGER": "frame"}, scratch, player=PLAYER)
    r = subprocess.run([tracediff, t2, t2], capture_output=True, text=True)
    check("trigger frame" in r.stdout and f"identical: {len(PLAYER)} epochs" in r.stdout,
          "frame trigger gives one epoch per frame\n" + r.stdout)
    r = subprocess.run([tracediff, t1, t2], capture_output=True, text=True)
    check("sample triggers differ" in r.stdout, "tracediff warns when triggers differ")

    print(f"lua_plugin_mock_test: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
