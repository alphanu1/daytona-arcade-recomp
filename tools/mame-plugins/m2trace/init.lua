-- m2trace: lockstep trace recorder and input recorder/replayer for Model 2
-- (docs/trace-format.md). Configured by environment variables so a run is one
-- reproducible command line:
--
--   M2TRACE_OUT=path            write a trace
--   M2TRACE_TRIGGER=vblank-ack  sample point: vblank-ack (default) or frame
--   M2TRACE_RECORD_INPUT=path   record the input stream
--   M2TRACE_REPLAY_INPUT=path   replay an input stream, and check it replayed
--   M2TRACE_FRAMES=n            exit after n frames
--   M2TRACE_DUMP_EPOCH=n        dump every hashed region at epoch n ...
--   M2TRACE_DUMP_DIR=dir        ... into dir (default "dumps")
--   M2TRACE_SNAP_EVERY=n        save a screenshot every n frames (MAME's
--                               snapshot directory; game output, never committed)
--
-- Enable with: mame daytona -plugin m2trace -pluginspath <mame plugins>;<this dir's parent>
--
-- Traces, input streams and dumps are game-derived and are never committed.

local exports = {
	name = "m2trace",
	version = "0.1.0",
	description = "Model 2 lockstep trace and input recorder",
	license = "project licence, undecided",
	author = { name = "daytona-arcade-recomp" },
}

local m2trace = exports
local core = require("m2trace/core")

-- Hashed regions and tapped ranges: docs/trace-format.md.
local REGIONS = {
	{ 0x00200000, 0x0021ffff },
	{ 0x00500000, 0x005fffff },
	{ 0x00900000, 0x0091ffff },
	{ 0x01d00000, 0x01d03fff },
	{ 0x01000000, 0x0100ffff },
	{ 0x01800000, 0x01803fff },
}
local WRITE_TAPS = {
	{ 0x00800000, 0x00803fff, "geo" },
	{ 0x00804000, 0x00807fff, "geo_prg" },
	{ 0x00880000, 0x00883fff, "copro_fn" },
	{ 0x00884000, 0x00887fff, "copro_fifo" },
	{ 0x00980000, 0x0098000f, "copro_ctl" },
	{ 0x00e80000, 0x00e80007, "irq" },
	{ 0x00f00000, 0x00f0000f, "timers" },
	{ 0x01c00000, 0x01c00fff, "dpram" },
	{ 0x01c80000, 0x01c80003, "uart" },
}
-- Every device the i960 reads, so a harness can answer them from the trace
-- (M1: devices replayed). RAM and ROM are not tapped; their contents are
-- covered by the region hashes.
local READ_TAPS = {
	{ 0x00800000, 0x00807fff, "geo" },
	{ 0x00880000, 0x00887fff, "copro_fifo" },
	{ 0x00980000, 0x0098003f, "copro_ctl" },
	{ 0x00e80000, 0x00e80007, "irq" },
	{ 0x00f00000, 0x00f0000f, "timers" },
	{ 0x01a00000, 0x01a1ffff, "comm" },
	{ 0x01c00000, 0x01c00fff, "dpram" },
	{ 0x01c80000, 0x01c80003, "uart" },
	{ 0x10000000, 0x105fffff, "render" },
}
local IRQ_ACK = 0x00e80000
local INPUT_PORTS = { ":IN0", ":IN1", ":GEARS", ":STEER", ":ACCEL", ":BRAKE" }
local GEAR_BITS = { port = ":IN1", mask = 0x70 } -- computed from GEARS, not replayed

local cfg = {}
local out            -- trace file
local epoch = 0
local taps = {}      -- keep pass-through handlers alive
local subs = {}      -- keep notifier subscriptions alive
local cpu, space, screen

local in_fields      -- recorded/replayed input fields
local in_rec_file
local replay_frames, replay_pos, replay_mismatch
local frames_run = 0

local function env(name)
	local v = os.getenv(name)
	if v == "" then return nil end
	return v
end

local function reg_values()
	local st = cpu.state
	local regs = {}
	for i, name in ipairs(core.REG_NAMES) do
		local e = st[name]
		regs[i] = e and e.value or 0 -- tc has no state entry in MAME
	end
	return regs
end

local function dump_regions()
	local dir = cfg.dump_dir or "dumps"
	require("lfs").mkdir(dir)
	for _, r in ipairs(REGIONS) do
		local path = string.format("%s/%s_e%d_%08x.bin", dir, emu.romname(), epoch, r[1])
		local f = io.open(path, "wb")
		if f then
			f:write(space:read_range(r[1], r[2], 8))
			f:close()
		end
	end
end

local sample_errors = 0

-- The Lua reference documents screen.frame_number as a property; at MAME
-- dddd7368 it is a method. Accept either.
local function frame_number()
	local f = screen.frame_number
	if type(f) == "function" then return f(screen) end
	return f
end

local function take_sample_unprotected()
	local regions = {}
	for i, r in ipairs(REGIONS) do
		-- read_range at width 32 returns host-order words; hashing unpacks
		-- them little-endian, so this assumes a little-endian host (x86-64,
		-- ARM64). Width 8 would be byte-order safe but four times slower.
		local bytes = space:read_range(r[1], r[2], 32)
		regions[i] = { r[1], r[2] - r[1] + 1, core.hash_words(bytes) }
	end
	if cfg.dump_epoch == epoch then dump_regions() end
	out:write(core.rec_sample(epoch, frame_number(), regions, reg_values()))
	epoch = epoch + 1
end

-- MAME drops errors raised inside tap callbacks silently, so a failing sample
-- would just vanish. Report it (first few, then a count at exit).
local function take_sample()
	local ok, err = pcall(take_sample_unprotected)
	if not ok then
		sample_errors = sample_errors + 1
		if sample_errors <= 3 then
			emu.print_error("m2trace: sample failed at epoch " .. epoch .. ": " .. tostring(err))
		end
	end
end

-- MAME's i960_stall() rewinds IP to PIP, so inside a tap an access that
-- stalled the CPU is the one where ip == pip.
local function stalled()
	local st = cpu.state
	return st["ip"].value == st["pip"].value
end

local function install_taps()
	for _, t in ipairs(WRITE_TAPS) do
		taps[#taps + 1] = space:install_write_tap(t[1], t[2], "m2trace_w_" .. t[3],
			function(offset, data, mask)
				local st = stalled()
				out:write(core.rec_access(true, offset, data, mask, st))
				if not st and cfg.trigger == "vblank-ack" and offset == IRQ_ACK
						and (mask & 1) ~= 0 and (data & 1) == 0 then
					take_sample()
				end
			end)
	end
	for _, t in ipairs(READ_TAPS) do
		taps[#taps + 1] = space:install_read_tap(t[1], t[2], "m2trace_r_" .. t[3],
			function(offset, data, mask)
				out:write(core.rec_access(false, offset, data, mask, stalled()))
			end)
	end
end

-- ---------------------------------------------------------------------------
-- Input.

local function collect_fields()
	local fields = {}
	local ports = manager.machine.ioport.ports
	for _, tag in ipairs(INPUT_PORTS) do
		local port = ports[tag]
		if port then
			for _, field in pairs(port.fields) do
				local skip = field.type_class == "dipswitch" or field.type_class == "config"
					or (tag == GEAR_BITS.port and (field.mask & GEAR_BITS.mask) ~= 0)
				if not skip then
					fields[#fields + 1] = {
						port = tag, mask = field.mask, defvalue = field.defvalue & field.mask,
						analog = field.is_analog, field = field,
					}
				end
			end
		end
	end
	-- pairs() order is not stable: sort so the header is reproducible.
	table.sort(fields, function(a, b)
		if a.port ~= b.port then return a.port < b.port end
		return a.mask < b.mask
	end)
	return fields
end

local function sample_inputs()
	local ports = manager.machine.ioport.ports
	local cache, values = {}, {}
	for i, f in ipairs(in_fields) do
		cache[f.port] = cache[f.port] or ports[f.port]:read()
		values[i] = cache[f.port] & f.mask
	end
	return values
end

local function apply_inputs(values)
	for i, f in ipairs(in_fields) do
		f.field:set_value(core.field_value(f, values[i]))
	end
end

local function open_replay(path)
	local f = io.open(path, "rb")
	if not f then
		emu.print_error("m2trace: cannot open " .. path)
		return
	end
	local header, frames = core.input_parse(f:read("a"))
	f:close()
	if not header then
		emu.print_error("m2trace: " .. frames)
		return
	end
	-- Match stream fields to this machine's fields by (port, mask).
	local live = {}
	for _, lf in ipairs(collect_fields()) do live[lf.port .. ":" .. lf.mask] = lf end
	in_fields = {}
	for i, hf in ipairs(header.fields) do
		local lf = live[hf.port .. ":" .. hf.mask]
		if not lf then
			emu.print_error(string.format("m2trace: input field %s/%x not on this machine", hf.port, hf.mask))
			return
		end
		in_fields[i] = { port = hf.port, mask = hf.mask, defvalue = hf.defvalue, analog = hf.analog, field = lf.field }
	end
	replay_frames, replay_pos = frames, 1
	emu.print_info(string.format("m2trace: replaying %d frames from %s", #frames, path))
end

-- ---------------------------------------------------------------------------

local function on_start()
	cpu = manager.machine.devices[":maincpu"]
	space = cpu.spaces["program"]
	screen = manager.machine.screens[":screen"]

	if cfg.out then
		out = io.open(cfg.out, "wb")
		if not out then
			emu.print_error("m2trace: cannot open " .. cfg.out)
		else
			out:write(core.trace_header(emu.romname(), "mame " .. emu.app_version(), cfg.trigger))
			install_taps()
		end
	end
	if cfg.record_input then
		in_fields = collect_fields()
		in_rec_file = io.open(cfg.record_input, "wb")
		in_rec_file:write(core.input_header(emu.romname(), in_fields))
	elseif cfg.replay_input then
		open_replay(cfg.replay_input)
		if replay_frames and replay_frames[1] then apply_inputs(replay_frames[1]) end
	end
end

local function on_frame()
	frames_run = frames_run + 1
	if in_rec_file then
		in_rec_file:write(core.input_frame(sample_inputs()))
	elseif replay_frames and replay_pos <= #replay_frames then
		-- Check what the machine saw this frame against the recording.
		local seen = sample_inputs()
		local want = replay_frames[replay_pos]
		for i = 1, #want do
			if seen[i] ~= want[i] and not replay_mismatch then
				replay_mismatch = replay_pos
				emu.print_error(string.format(
					"m2trace: replay diverged at frame %d, field %s/%x: saw %x, recorded %x",
					replay_pos - 1, in_fields[i].port, in_fields[i].mask, seen[i], want[i]))
			end
		end
		replay_pos = replay_pos + 1
		if replay_frames[replay_pos] then apply_inputs(replay_frames[replay_pos]) end
	end
	if out and cfg.trigger == "frame" then take_sample() end
	if cfg.snap_every and frames_run % cfg.snap_every == 0 then manager.machine.video:snapshot() end
	if cfg.frames and frames_run >= cfg.frames then manager.machine:exit() end
end

local function on_stop()
	if sample_errors > 0 then
		emu.print_error(string.format("m2trace: %d samples FAILED; the trace is incomplete", sample_errors))
	end
	if replay_frames then
		if replay_mismatch then
			emu.print_error(string.format("m2trace: replay INVALID from frame %d", replay_mismatch - 1))
		else
			emu.print_info(string.format("m2trace: replay matched the recording for %d frames",
				math.min(replay_pos - 1, #replay_frames)))
		end
	end
	if out then out:close() out = nil end
	if in_rec_file then in_rec_file:close() in_rec_file = nil end
	taps = {}
end

function m2trace.startplugin()
	cfg.out = env("M2TRACE_OUT")
	cfg.trigger = env("M2TRACE_TRIGGER") or "vblank-ack"
	cfg.record_input = env("M2TRACE_RECORD_INPUT")
	cfg.replay_input = env("M2TRACE_REPLAY_INPUT")
	cfg.frames = tonumber(env("M2TRACE_FRAMES") or "")
	cfg.dump_epoch = tonumber(env("M2TRACE_DUMP_EPOCH") or "")
	cfg.dump_dir = env("M2TRACE_DUMP_DIR")
	cfg.snap_every = tonumber(env("M2TRACE_SNAP_EVERY") or "")
	if cfg.trigger ~= "vblank-ack" and cfg.trigger ~= "frame" then
		emu.print_error("m2trace: unknown trigger " .. cfg.trigger .. ", using vblank-ack")
		cfg.trigger = "vblank-ack"
	end

	subs[#subs + 1] = emu.add_machine_reset_notifier(function()
		if not space then
			on_start()
		elseif out then
			-- A later reset breaks the epoch chain; say so in the trace.
			out:write(core.rec_note(string.format("reset at epoch %d", epoch)))
		end
	end)
	subs[#subs + 1] = emu.add_machine_frame_notifier(on_frame)
	subs[#subs + 1] = emu.add_machine_stop_notifier(on_stop)
end

return exports
