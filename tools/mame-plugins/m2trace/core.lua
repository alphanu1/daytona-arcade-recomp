-- m2trace core: encoders, the region hash and the input-stream codec.
-- Pure Lua 5.4, no MAME API, so it runs under the test harness unchanged.
-- Formats: docs/trace-format.md.

local core = {}

core.HASH_INIT = 0xcbf29ce484222325 -- wraps to a negative Lua integer; bits are what matter
core.HASH_PRIME = 0x100000001b3

-- FNV-1a-64 over 32-bit little-endian words. Lua 5.4 integers are 64-bit and
-- wrap on overflow, which is exactly mod 2^64.
local BLOCK = 256
local block_fmt = "<" .. string.rep("I4", BLOCK)
function core.hash_words(bytes, h)
	h = h or core.HASH_INIT
	local len = #bytes
	assert(len % 4 == 0, "hash input must be whole 32-bit words")
	local prime = core.HASH_PRIME
	local pos = 1
	local whole = len - (len % (BLOCK * 4))
	while pos <= whole do
		local w = { string.unpack(block_fmt, bytes, pos) }
		for i = 1, BLOCK do
			h = (h ~ w[i]) * prime
		end
		pos = pos + BLOCK * 4
	end
	while pos <= len do
		local w = string.unpack("<I4", bytes, pos)
		h = (h ~ w) * prime
		pos = pos + 4
	end
	return h
end

local function str16(s)
	return string.pack("<s2", s)
end

-- Trace file header.
function core.trace_header(game, producer, trigger)
	local body = str16(game) .. str16(producer) .. str16(trigger)
	return "M2TR" .. string.pack("<I4I4", 1, #body) .. body
end

local function record(rtype, payload)
	return string.pack("<BI4", rtype, #payload) .. payload
end

core.REC_SAMPLE, core.REC_WRITE, core.REC_READ, core.REC_NOTE = 0x01, 0x10, 0x11, 0x20

function core.rec_access(is_write, addr, data, mask)
	return record(is_write and core.REC_WRITE or core.REC_READ,
		string.pack("<I4I4I4", addr & 0xffffffff, data & 0xffffffff, mask & 0xffffffff))
end

function core.rec_note(text)
	return record(core.REC_NOTE, text)
end

-- regions: array of { base, bytes, hash }; regs: array of 36 register values.
function core.rec_sample(epoch, frame, regions, regs)
	local parts = { string.pack("<I4I8B", epoch, frame, #regions) }
	for _, r in ipairs(regions) do
		parts[#parts + 1] = string.pack("<I4I4i8", r[1], r[2], r[3])
	end
	assert(#regs == 36, "sample needs 36 registers")
	for i = 1, 36 do
		parts[#parts + 1] = string.pack("<I4", regs[i] & 0xffffffff)
	end
	return record(core.REC_SAMPLE, table.concat(parts))
end

-- Register order in a sample (matches trace::reg_name in src/trace).
core.REG_NAMES = {
	"pfp", "sp", "rip", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
	"r12", "r13", "r14", "r15", "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7",
	"g8", "g9", "g10", "g11", "g12", "g13", "g14", "fp", "pc", "ac", "ip", "tc",
}

-- ---------------------------------------------------------------------------
-- Input stream.

-- fields: array of { port = tag, mask = n, defvalue = n, analog = bool }
function core.input_header(game, fields)
	local parts = { "M2IN", string.pack("<I4", 1), str16(game), string.pack("<I2", #fields) }
	for _, f in ipairs(fields) do
		parts[#parts + 1] = str16(f.port) .. string.pack("<I4I4B", f.mask, f.defvalue, f.analog and 1 or 0)
	end
	return table.concat(parts)
end

function core.input_frame(values)
	local parts = {}
	for i, v in ipairs(values) do
		parts[i] = string.pack("<I4", v & 0xffffffff)
	end
	return table.concat(parts)
end

-- Parse a whole input stream. Returns header { game, fields } and an array of
-- frames, each an array of values in field order; or nil, message.
function core.input_parse(bytes)
	if bytes:sub(1, 4) ~= "M2IN" then
		return nil, "not an M2IN input stream"
	end
	local version, pos = string.unpack("<I4", bytes, 5)
	if version ~= 1 then
		return nil, "unsupported input version " .. version
	end
	local game
	game, pos = string.unpack("<s2", bytes, pos)
	local n
	n, pos = string.unpack("<I2", bytes, pos)
	local fields = {}
	for i = 1, n do
		local port, mask, defvalue, analog
		port, pos = string.unpack("<s2", bytes, pos)
		mask, defvalue, analog, pos = string.unpack("<I4I4B", bytes, pos)
		fields[i] = { port = port, mask = mask, defvalue = defvalue, analog = analog ~= 0 }
	end
	local frame_bytes = 4 * n
	local remain = #bytes - pos + 1
	if n == 0 or remain % frame_bytes ~= 0 then
		return nil, "input stream body is not whole frames"
	end
	local frames = {}
	local fmt = "<" .. string.rep("I4", n)
	for f = 1, remain // frame_bytes do
		frames[f] = { string.unpack(fmt, bytes, pos) }
		frames[f][n + 1] = nil -- drop the position string.unpack appends
		pos = pos + frame_bytes
	end
	return { game = game, fields = fields }, frames
end

local function trailing_zeros(mask)
	local s = 0
	while mask ~= 0 and (mask & 1) == 0 do
		mask = mask >> 1
		s = s + 1
	end
	return s
end

-- Turn a recorded (masked) port value into what field:set_value takes:
-- analog -> axis position, digital -> 1 active / 0 inactive.
function core.field_value(field, value)
	if field.analog then
		return (value & field.mask) >> trailing_zeros(field.mask)
	end
	return (((value ~ field.defvalue) & field.mask) ~= 0) and 1 or 0
end

return core
