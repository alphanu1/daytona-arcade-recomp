# Trace and input formats

The lockstep contract between MAME (via `tools/mame-plugins/m2trace`) and the
recompiled build. Both produce a **trace**; `tracediff` compares two traces and
stops at the first divergence. Both consume the same **input stream**, so a
recorded session replays identically in each. Design doc, Reference &
validation, governs.

Traces and input streams are game-derived working files: they are git-ignored
(`traces/`) and never committed (rules.md rule 7). Tests refer to them by hash.

All integers are little-endian.

## Sample points: the epoch

A trace is a sequence of **epochs**. Each epoch is the bus events the i960
made since the previous sample, followed by one **sample** of machine state.
Two traces are compared epoch by epoch.

The sample must fall at the same *guest* instant in both builds, which MAME's
end-of-frame notifier does not guarantee: it fires wherever the i960 happens to
be when the frame ends, mid-routine and on no instruction boundary our build
can reproduce. So the default trigger is a guest action:

| Trigger | When | Instruction-aligned |
| --- | --- | --- |
| `vblank-ack` (default) | the i960 writes the IRQ acknowledge register (0x00e80000) with bit 0 of the written value clear, i.e. acknowledges vblank | yes: fires inside the store that acks |
| `frame` | MAME frame notifier | no; for bring-up only |

`vblank-ack` assumes Daytona's vblank handler acknowledges by writing a mask
with bit 0 clear (MAME: `m_intreq &= data`). That is how MAME's handler
semantics work, but whether Daytona acks once per frame, and from where, is
**unverified** until the first trace. If it acks more than once per frame the
trace simply has more epochs; both sides see the same stores.

Register values in a sample are those visible to the store that triggered it.
`ip` is MAME's `m_IP`, which is already advanced past the store (by 4 or 8), so
it is recorded but compared only on request.

## Trace file

```
header:
  char[4]  magic    "M2TR"
  u32      version  1
  u32      header_bytes            (bytes of the fields below)
  str      game                    (MAME short name, e.g. "daytona")
  str      producer                ("mame" or "recomp"), then free text
  str      trigger                 ("vblank-ack" or "frame")
str = u16 length + bytes, no terminator

records, until end of file:
  u8   type
  u32  payload_bytes
  payload
```

| Type | Name | Payload |
| --- | --- | --- |
| 0x01 | SAMPLE | `u32 epoch`, `u64 frame`, `u8 nregions`, then per region `u32 base`, `u32 bytes`, `u64 hash`; then `u32 regs[36]`: r0-r15, g0-g15, pc, ac, ip, tc (tc is 0 from MAME, which has no state entry for it) |
| 0x10 | WRITE | `u32 addr`, `u32 data`, `u32 mem_mask` |
| 0x11 | READ | `u32 addr`, `u32 data`, `u32 mem_mask` |
| 0x12 | READ_STALLED | as READ; the access stalled the i960 (TGP FIFO empty), which MAME rewinds and repeats, so it did not complete |
| 0x13 | WRITE_STALLED | as WRITE, for a stall on a full FIFO |
| 0x20 | NOTE | UTF-8 text (reset, state load, anything that breaks the epoch chain) |

A stalled access is detected in the tap as `ip == pip`: MAME's `i960_stall()` rewinds IP to the instruction's start. Comparisons drop stalled accesses by default, since a native build never stalls.

Unknown record types are skipped by length, so the format can grow without
breaking old readers. The epoch field counts samples from 0; `frame` is the
screen frame number and is informational (not compared by default), since our
build has no MAME screen.

### Tapped ranges (events)

WRITE/READ events are recorded for the ranges below. Everything else the i960
does is covered by the RAM hashes.

| Range | What | Direction |
| --- | --- | --- |
| 0x00800000-0x00803fff | geometrizer registers | W, R |
| 0x00804000-0x00807fff | geometrizer program upload | W, R |
| 0x00880000-0x00883fff | TGP function port | W, R |
| 0x00884000-0x00887fff | TGP FIFO (program upload and data in; results out) | W, R |
| 0x00980000-0x0098003f | copro control, FIFO status, video control, TGP id (reads to 0x3f) | W, R |
| 0x00e80000-0x00e80007 | IRQ request/ack and enable | W, R |
| 0x00f00000-0x00f0000f | timers | W, R |
| 0x01a00000-0x01a1ffff | comm board shared RAM and flags | R |
| 0x01c00000-0x01c00fff | I/O board dual-port RAM (inputs, drive board, lamps) | W, R |
| 0x01c80000-0x01c80003 | sound UART | W, R |
| 0x10000000-0x105fffff | renderer mode and polygon count registers | R |

Every non-RAM range the i960 reads is tapped, so a harness can answer device reads from the trace (M1).

Reads of the TGP FIFO are what the game consumes from the TGP; they are the
values that must be bit-identical (rules.md, standing rules).

### Region hashes

| Region | Range | Bytes |
| --- | --- | --- |
| RAM | 0x00200000-0x0021ffff | 128 KiB |
| work RAM | 0x00500000-0x005fffff | 1 MiB |
| buffer RAM (display lists) | 0x00900000-0x0091ffff | 128 KiB |
| backup SRAM | 0x01d00000-0x01d03fff | 16 KiB |
| tilemap RAM | 0x01000000-0x0100ffff | 64 KiB |
| palette | 0x01800000-0x01803fff | 16 KiB |

The hash is FNV-1a-64 taken over **32-bit little-endian words**, not bytes, so
Lua can compute it at a quarter of the cost:

```
h = 0xcbf29ce484222325
for each u32 word w:  h = (h xor w) * 0x100000001b3   (mod 2^64)
```

It detects divergence; it does not locate it. To locate a RAM divergence, the
plugin can dump a region at a given epoch (`M2TRACE_DUMP`), and the dump is
diffed locally. Dumps are game data and stay in `dumps/`.

## Input stream

```
header:
  char[4]  magic    "M2IN"
  u32      version  1
  str      game
  u16      nfields
  per field:  str port_tag, u32 mask, u32 defvalue, u8 analog
frames:
  per frame: u32 port values, one per field, in header order, as port:read()
             returned them for that field's port, masked to the field
```

Recording captures `port:read()` for each port at every frame notifier, split by
field. The header carries each field's mask and default, so a reader can turn a
value back into "pressed" or "axis position" without MAME's port definitions:

- digital: active when `(value xor defvalue) and mask` is non-zero (Daytona's
  buttons are active-low);
- analog: position is `(value and mask) >> shift`, shift = trailing zeros of mask.

Recorded fields: every non-dipswitch, non-config field of IN0, IN1, GEARS,
STEER, ACCEL and BRAKE, **except** IN1 0x70, the gear bits MAME computes from
GEARS (`daytona_gearbox_r`), which are replayed through GEARS instead.

Replay applies each frame's values with `field:set_value` at the frame
notifier, and re-records `port:read()` as it goes. A replay is valid only if
that re-recording matches the input stream exactly; the plugin reports the
first frame where it does not. Whether MAME applies a value set at the end of
frame N during frame N+1 is **measured by that check**, not assumed.
