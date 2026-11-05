"""World objects the arcade draws outside the scenery cells (course actor table).

The original keeps, per course, 256 pointers (program 236940[course] -> table,
one entry per scenery cell) to lists of 24-byte actor records
    x, y, z (float), heading (int16), spare byte, id byte, handler, argument
which 2002c turns into tasks at scene start. Each handler draws object lists
(16-byte records model, uv, headers, count; routine 1786c) when the cell of
its position, ((int(x)+1024)>>7 & 15) + 16*((int(z)+1024)>>7 & 15) (16e10), is
in the frame's draw bitmap 501520 or in its near bitmap 501540.

  prepare_world_objects.py lists IMAGES NEW_LISTS.txt [--course N] [--extra-models LOG_ADDRESSES.txt]
      the object lists of every actor of the course, for
      m2segment --export-native-objects NEW_LISTS.txt NEW_DIR
  prepare_world_objects.py actors IMAGES [--course N]
      prints the actor table with the class of every handler
  prepare_world_objects.py glass IMAGES OBJECT_EXPORT_DIR NEW_HEADER
      the slot machine's glass as translucent bands (slot_glass_data.h, render/scene/slot_glass.c)
  prepare_world_objects.py sources IMAGES OBJECT_EXPORT_DIR CELL_SOURCES NEW_SOURCES [--course N]
      CELL_SOURCES (m2segment --export-native-cells) plus one mesh per
      selection group of the objects this build draws, placed in world
      coordinates with the original's single-precision matrix arithmetic, and
      objects.txt for prepare_models.py --world-cells --world-objects; with
      frame-counter animations (Advanced) also clock.json (their groups) and
      clock_keys.txt (every texture header and luma their frames show, for
      prepare_palette_materials.py --extra-keys).
  prepare_world_objects.py clock IMAGES OBJECT_EXPORT_DIR SOURCES PACK.dnm NEW.dna [--course N]
      the frame-counter animations' frames for the renderer (include/clock_actors.h, DNA1): the base mesh of each
      group in PACK (the final world pack of SOURCES) and, per frame, its positions, normals, texture headers and
      lumas, and u, v where they change; prints a report (sizes, the position errors of the reduced rates).

ROM-derived output: write only below ignored build/ directories.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct

import numpy as np

import prepare_models as pm

ACTOR_TABLES = 0x236940
PIT_TABLES = 0x220e7c
# Handler entry -> (class, selection bitmap, what it draws). Classes:
#   a  static, constant light; b  static geometry chosen by a condition;
#   c  animated or moving (runtime state); -  draws nothing.
PROP_KINDS = (0x223114, 0x223078, 0x2230ac, 0x2230e0)   # the props' init entries (include/props.h)
HANDLERS = {
    0x222d30: ('-', None, 'prop collision manager (5036xx), no model'),
    0x22302c: ('-', None, 'prop sound manager, no model'),
    0x223114: ('b', 'draw', 'prop (cone) at rest; knocked over by the viewed car: 2231c0'),
    0x223078: ('b', 'draw', 'prop of its own kind (rest, knock sets, limit, sound), as 223114: 2231c0'),
    0x2230ac: ('b', 'draw', 'prop of its own kind (rest, knock sets, limit, sound), as 223114: 2231c0'),
    0x2230e0: ('b', 'draw', 'prop of its own kind (rest, knock sets, limit, sound), as 223114: 2231c0'),
    0x2203a4: ('a', 'near', 'static object, own heading'),
    0x220450: ('a', 'near', 'static object, own heading'),
    0x220be4: ('c', 'near', 'spins about Y, +0x100 per drawn frame'),
    0x220c94: ('c', 'near', 'spins about Y, -0x100 per drawn frame'),
    0x220b14: ('c', 'near', 'spins about Z, +0x100 per drawn frame'),
    0x220d44: ('c', 'near', 'pit crew: 4-step model cycle, counter advances per drawn frame'),
    0x22024c: ('c', 'near', '4-step model cycle from the frame counter 5010b0'),
    0x2204fc: ('c', 'draw', 'slot machine reels: 3 x 64 models, start button, time bonus (5010d0); slot_machine.h'),
    0x221d24: ('b', 'draw', 'gantry board: one of three models by race state'),
    0x221dd8: ('c', 'draw', 'leader board: five car-number models from the ranking 503080'),
    0x223b08: ('b', 'draw', 'one of 64 models by the camera bearing from a fixed point'),
    # Advanced: animations of the frame counter 5010b0, list[(5010b0 >> shift) & mask] in world coordinates
    # (meshpack.h NM_OBJECT_CLOCK; CLOCK_ACTORS below).
    0x220304: ('c', 'draw', 'animation, 64 lists by (5010b0 & 7f) >> 1, cells 238b14/238b16'),
    0x2209e4: ('c', 'draw', 'animation, 64 lists by 5010b0 & 3f, cells 7a/6a'),
    0x220a7c: ('c', 'near', 'animation, 64 lists by 5010b0 & 3f, cells 99/89'),
    0x221f10: ('c', 'draw', 'animation 238eb8 by 5010b0 & 3f and lists 288bbe0, 288bbf4; racing: profiles fa..168'),
    0x222030: ('c', 'near', 'animation, 64 lists by 5010b0 & 3f'),
    0x222090: ('c', 'near', 'animation 2391b8 (completed laps 0..2) or 2392b8 by 5010b0 & 3f'),
    0x222c48: ('c', 'near', 'animation, 32 lists by 5010b0 & 1f, cells 2393c0/c2/c4'),
    0x222cf0: ('c', '-', 'animation, 64 lists by 5010b0 & 3f, always drawn'),
    # Expert (EXPERT_ACTORS below): animations of 5010b0 placed by the TGP's matrix commands, a model by the bearing,
    # one by the player's profile, and the birds and the statue (include/expert_actors.h).
    0x22108c: ('c', 'near', 'nine figures T Ry 238d20.. [(5010b0>>3)&3]; not near: thirteen T Rz Ry Rx while racing profiles 2f8..33e'),
    0x22214c: ('c', 'near', 'animation T S Ry 239548[5010b0 & 3f] (completed laps 0) or 239748'),
    0x222594: ('c', 'draw', 'animation T S(0.75) Ry arg[5010b0 & 3f]'),
    0x222678: ('c', 'draw', 'animation T S(1.2) Ry arg[5010b0 & 3f]'),
    0x22275c: ('c', 'draw', 'animation T S(1) Ry arg[5010b0 & 3f]'),
    0x222840: ('c', 'draw', 'animation T S(1.1) Ry arg[5010b0 & 3f]'),
    0x223bc0: ('c', 'draw', 'one of 64 models by the angle (commands 43, 0a) of the camera from a fixed point'),
    0x223ce0: ('b', '-', 'static list while the player\'s profile is in 208..240'),
    0x223d48: ('-', 'draw', 'birds of the time attack (501a80): nothing in a race'),
    0x22430c: ('c', 'draw', 'birds circling: T Ry(5010b0<<7) T(15) 2368c0[..], four (six in a cinematic scene)'),
    0x224730: ('c', 'draw', 'bird T Ry S(scale) 2368c0[5010b0 & 1f], the scale grows by 0.5 a Start press'),
    0x224854: ('c', 'draw', 'bird circling until the player comes near, then flies off: state machine'),
    0x224cd0: ('c', '-', 'two birds of the cinematic scenes, moving along z'),
}


def images(directory):
    return (Path(directory)/'program.bin').read_bytes(), (Path(directory)/'main_data.bin').read_bytes()


class Rom:
    def __init__(self, directory):
        self.program, self.data = images(directory)

    def u32(self, at):
        if at >= 0x2000000:
            return struct.unpack_from('<I', self.data, at-0x2000000)[0]
        return struct.unpack_from('<I', self.program, at & 0x1fffff)[0]

    def f32(self, at):
        return struct.unpack('<f', struct.pack('<I', self.u32(at)))[0]


def cell_of(x, z):
    """16e10: cvtri rounds to nearest even; coordinates here are far from ties."""
    return ((int(np.rint(np.float32(x)))+1024) >> 7 & 15)+16*((int(np.rint(np.float32(z)))+1024) >> 7 & 15)


def actors(rom, course):
    table = rom.u32(ACTOR_TABLES+4*course)
    out = []
    for entry in range(256):
        at = rom.u32(table+4*entry)
        if not at:
            continue
        count = rom.u32(at)
        for i in range(count):
            r = at+4+24*i
            word = rom.u32(r+12)
            heading = word & 0xffff
            out.append(dict(list_cell=entry, record=r, x=rom.f32(r), y=rom.f32(r+4), z=rom.f32(r+8),
                            heading=heading-65536 if heading & 0x8000 else heading, id=(word >> 16) & 255,
                            handler=rom.u32(r+16), argument=rom.u32(r+20)))
    for a in out:
        a['cell'] = cell_of(a['x'], a['z'])
        a['class'], a['bitmap'], a['what'] = HANDLERS.get(a['handler'], ('?', None, 'unknown handler'))
    return out


def actor_lists(rom, actor, course):
    """Object list pointers an actor can draw, in the handler's own order."""
    h, arg = actor['handler'], actor['argument']
    if h in (0x223114, 0x223078, 0x2230ac, 0x2230e0, 0x2203a4, 0x220450, 0x220be4, 0x220c94, 0x220b14):
        return [arg]
    if h == 0x221d24:
        return [0x288b7bc, 0x288b7d0, 0x288b7f8]
    if h == 0x221dd8:
        return [0x288b384]+[rom.u32(0x238dfc+4*i) for i in range(46)]
    if h == 0x22024c:
        return [rom.u32(0x238b04+4*i) for i in range(4)]
    if h == 0x220d44:
        frames = rom.u32(rom.u32(PIT_TABLES+4*course)+16*actor['id'])
        return [rom.u32(frames+4*i) for i in range(4)]
    if h == 0x223b08:
        return [rom.u32(arg+4*i) for i in range(64)]
    if h == 0x2204fc:
        return [rom.u32(0x2385f8+4*i) for i in range(65)]
    if h in CLOCK_ACTORS:
        return [p for group in clock_groups(rom, actor) for frame in group['frames'] for p in frame]
    if h == 0x22108c:
        return [rom.u32(t+4*k) for t in FIGURE_TABLES for k in range(4)]
    if h == 0x22214c:
        return [rom.u32(t+4*k) for t in (0x239548, 0x239748) for k in range(64)]
    if h in (0x222594, 0x222678, 0x22275c, 0x222840, 0x223bc0):
        return [rom.u32(arg+4*k) for k in range(64)]
    if h == 0x223ce0:
        return [arg]
    if h in (0x22430c, 0x224730, 0x223d48):
        return [rom.u32(0x2368c0+4*k) for k in range(32)]
    if h in (0x224854, 0x224cd0):
        return [rom.u32(0x236840+4*k) for k in range(32)]
    if h == 0x22302c and arg == 0x222264:   # Expert's sound manager draws the statue 288f614 (222288)
        return [0x288f614]
    return []


# 22108c's figures: the four-step tables its draws index by (5010b0 >> 3) & 3 (238d40 is not one of them).
FIGURE_TABLES = (0x238d20, 0x238d30, 0x238d50, 0x238d60, 0x238d70, 0x238d80, 0x238d90, 0x238da0, 0x238db0)


# The frame-counter animations of Advanced (meshpack.h NM_OBJECT_CLOCK, include/clock_actors.h). Each handler draws
# list[(5010b0 >> shift) & (frames-1)] in world coordinates (with the view matrix itself) when its condition holds, read
# in its tick (the code behind each init entry):
#   220304 (220320..2203a0) cell 238b14 or 238b16 in the draw bitmap 501520: lists arg[(5010b0 & 7f) >> 1]
#   2209e4 (220a00..220a78) cell 7a or 6a in the draw bitmap: arg[5010b0 & 3f]
#   220a7c (220a98..220b10) cell 99 or 89 in the near bitmap 501540: arg[5010b0 & 3f]
#   221f10 (221f30..22202c) while 501090 & 0x400000 (phase 22, racing) the player's (501220) profile +128 (ldos) in
#          fa..168 (cmpo: unsigned), else its own cell +2e (16e10 at the init) in the draw bitmap: 238eb8[5010b0 & 3f],
#          then 288bbe0 and 288bbf4 (one group: the three lists are always drawn together)
#   222030 (222050..22208c) its own cell in the near bitmap: arg[5010b0 & 3f]
#   222090 (2220b0..222148) its own cell in the near bitmap: with the player's completed laps +6d (ldob) 0, 1 or 2
#          2391b8[5010b0 & 3f], else 2392b8[5010b0 & 3f] (two groups, the alternatives of one selection run)
#   222c48 (222c64..222cec) cell 2393c0, 2393c2 or 2393c4 in the near bitmap: arg[5010b0 & 1f]
#   222cf0 (222d10..222d2c) always: arg[5010b0 & 3f]
# Every bitmap is the pass before's (the actors run before the scenery object), as for the other world actors.
# Conditions (include/clock_actors.h NM_CLOCK_*): RUN the cells; ALWAYS; PROFILE (221f10); LAPS_LOW / LAPS_HIGH (222090).
CLOCK_RUN, CLOCK_ALWAYS, CLOCK_PROFILE, CLOCK_LAPS_LOW, CLOCK_LAPS_HIGH = 0, 1, 2, 3, 4
KIND_CLOCK = 15  # meshpack.h NM_OBJECT_CLOCK
CLOCK_ACTORS = {0x220304, 0x2209e4, 0x220a7c, 0x221f10, 0x222030, 0x222090, 0x222c48, 0x222cf0}


def clock_groups(rom, actor):
    """The groups of a frame-counter actor: dicts of frames (per frame the list pointers drawn, in drawing order), shift,
    cells (the first is the instance's cell), near, condition, profile window and variant (its place in the selection
    run: 222090's two alternatives)."""
    h, arg = actor['handler'], actor['argument']
    u16 = lambda at: struct.unpack_from('<H', rom.program, at & 0x1fffff)[0]
    table = lambda base, count: [[rom.u32(base+4*i)] for i in range(count)]
    own = actor['cell']
    plain = dict(shift=0, near=0, cond=CLOCK_RUN, window=(0, 0), variant=0)
    if h == 0x220304:
        return [dict(plain, frames=table(arg, 64), shift=1, cells=[u16(0x238b14), u16(0x238b16)])]
    if h == 0x2209e4:
        return [dict(plain, frames=table(arg, 64), cells=[0x7a, 0x6a])]
    if h == 0x220a7c:
        return [dict(plain, frames=table(arg, 64), cells=[0x99, 0x89], near=1)]
    if h == 0x221f10:
        return [dict(plain, frames=[[rom.u32(0x238eb8+4*i), 0x288bbe0, 0x288bbf4] for i in range(64)], cells=[own],
                     cond=CLOCK_PROFILE, window=(0xfa, 0x168))]
    if h == 0x222030:
        return [dict(plain, frames=table(arg, 64), cells=[own], near=1)]
    if h == 0x222090:
        return [dict(plain, frames=table(0x2391b8, 64), cells=[own], near=1, cond=CLOCK_LAPS_LOW),
                dict(plain, frames=table(0x2392b8, 64), cells=[own], near=1, cond=CLOCK_LAPS_HIGH, variant=1)]
    if h == 0x222c48:
        return [dict(plain, frames=table(arg, 32), cells=[u16(0x2393c0), u16(0x2393c2), u16(0x2393c4)], near=1)]
    if h == 0x222cf0:
        return [dict(plain, frames=table(arg, 64), cells=[own], cond=CLOCK_ALWAYS)]
    raise ValueError('not a frame-counter actor')


# Expert's world actors (include/expert_actors.h; clock_actors.h's conditions FAR_PROFILE, LAPS_NONE/SOME, WINDOW and
# ACTOR, and PLACED groups). A frame entry may be (list, placement): the operations the handler's TGP commands apply
# before the draw, ('T', x, y, z), ('RX'|'RY'|'RZ', angle), ('S', sx, sy, sz), baked into the group's world positions
# and normals (clock_frames). PLACED groups keep their lists in model coordinates; the actor's tick places them.
CLOCK_FAR_PROFILE, CLOCK_LAPS_NONE, CLOCK_LAPS_SOME, CLOCK_WINDOW, CLOCK_ACTOR = 5, 6, 7, 8, 9
GROUP_PLACED = 1
XA_BEARING, XA_BIRDS, XA_BIG_BIRD, XA_HORSE, XA_PAIR, XA_STATUE = 1, 2, 3, 4, 5, 6
XA_GROUPS = {XA_BEARING: 1, XA_BIRDS: 7, XA_BIG_BIRD: 1, XA_HORSE: 1, XA_PAIR: 2, XA_STATUE: 1}
EXPERT_ACTORS = {0x22108c, 0x22214c, 0x222594, 0x222678, 0x22275c, 0x222840, 0x223bc0, 0x223ce0, 0x22430c, 0x224730,
                 0x224854, 0x224cd0}
STATUE_ROUTINE, STATUE_LIST = 0x222264, 0x288f614   # the sound manager 22302c's argument on Expert, and what 222288 draws
# The scale the T S Ry animations' lda immediates load (x and z; y 1.0): 2221a8, 2225e4, 2226c8, 2227ac, 222890.
SCALE_LDA = {0x22214c: 0x2221a8, 0x222594: 0x2225e4, 0x222678: 0x2226c8, 0x22275c: 0x2227ac, 0x222840: 0x222890}
# 22108c's two straight blocks of figures: near (2210dc..2214f0) and far (221528..221d20).
FIGURE_BLOCKS = ((0x2210dc, 0x2214f0), (0x221528, 0x221d20))


def figure_draws(rom, start, end):
    """The draws of one of 22108c's straight blocks: [(table, placement)], decoded from its instructions (lda and mov 0
    into registers, the TGP command words and their arguments, ld table[r7*4] and call 1786c). Refuses anything
    else."""
    word = lambda at: rom.u32(at)
    regs, ops, draws, at, g0_table, command, args = {}, [], [], start, None, None, []
    nargs = {0x12: 3, 0x14: 1, 0x15: 1, 0x16: 1}

    def flush():
        nonlocal command, args
        if command is None:
            return
        if command in nargs:
            if len(args) != nargs[command]:
                raise ValueError(f'22108c block {start:x}: command {command:x} with {len(args)} arguments')
            if command == 0x12:
                ops.append(('T',)+tuple(struct.unpack('<f', struct.pack('<I', a))[0] for a in args))
            else:
                ops.append(({0x14: 'RX', 0x15: 'RY', 0x16: 'RZ'}[command], wrap16(args[0])))
        elif args:
            raise ValueError(f'22108c block {start:x}: arguments to command {command:x}')
        command, args = None, []
    while at < end:
        w = word(at)
        if (w & 0xff07ffff) == 0x8c003000:      # lda imm32,reg
            regs[(w >> 19) & 31] = word(at+4); at += 8; continue
        if (w >> 24) == 0x8c and not w & 0x1000:   # lda offset,reg (MEMA)
            regs[(w >> 19) & 31] = w & 0xfff; at += 4; continue
        if (w & 0xff07ffff) == 0x5c001e00:      # mov 0,reg
            regs[(w >> 19) & 31] = 0; at += 4; continue
        if (w & 0xfffff000) == 0x92f6e000:      # st g14,off(g11): a command word
            flush()
            off = w & 0xfff
            if regs.get(30) != (off >> 4)*0x101:
                raise ValueError(f'22108c block {start:x}: command word at {at:x}')
            command = off >> 4
            if command == 0x05:
                if ops:
                    raise ValueError('22108c: push inside a placement')
                command = None
            elif command == 0x06:
                command = None
            at += 4; continue
        if (w & 0xff07ffff) == 0x9206dc1c:      # st reg,(g11)[g12]: an argument
            args.append(regs[(w >> 19) & 31]); at += 4; continue
        if w == 0x90803907:                     # ld table[r7*4],g0
            flush(); g0_table = word(at+4); at += 8; continue
        if (w >> 24) == 0x09:                   # call
            disp = w & 0xfffffc
            target = (at + (disp - (1 << 24) if disp & 0x800000 else disp)) & 0xffffff
            if target != 0x1786c or g0_table is None:
                raise ValueError(f'22108c block {start:x}: call at {at:x}')
            flush(); draws.append((g0_table, ops)); ops, g0_table = [], None
            at += 4; continue
        raise ValueError(f'22108c block {start:x}: instruction {w:08x} at {at:x}')
    flush()
    return draws


def expert_groups(rom, actor):
    """The groups of one of Expert's actors (clock_groups' dicts plus placed and, for ACTOR groups, the actor's kind and
    params): one a draw of its tick, in the tick's order."""
    h, arg = actor['handler'], actor['argument']
    u16 = lambda at: struct.unpack_from('<H', rom.program, at & 0x1fffff)[0]
    table = lambda base, count: [rom.u32(base+4*i) for i in range(count)]
    own, pos, heading = actor['cell'], (actor['x'], actor['y'], actor['z']), actor['heading']
    plain = dict(shift=0, near=0, cond=CLOCK_RUN, window=(0, 0), variant=0, placed=0, cells=[own])
    if h == 0x22108c:
        out = []
        for (start, end), near, cond, window in zip(FIGURE_BLOCKS, (1, 1), (CLOCK_RUN, CLOCK_FAR_PROFILE),
                                                     ((0, 0), (0x2f8, 0x33e))):
            draws = figure_draws(rom, start, end)
            frames = [[(rom.u32(t+4*k), ops) for t, ops in draws] for k in range(4)]
            out.append(dict(plain, frames=frames, shift=3, near=near, cond=cond, window=window))
        return out
    if h in SCALE_LDA:
        s = rom.f32(SCALE_LDA[h]+4)
        ops = [('T',)+pos, ('S', s, 1.0, s), ('RY', heading)]
        if h == 0x22214c:
            return [dict(plain, frames=[[(p, ops)] for p in table(0x239548, 64)], near=1, cond=CLOCK_LAPS_NONE),
                    dict(plain, frames=[[(p, ops)] for p in table(0x239748, 64)], near=1, cond=CLOCK_LAPS_SOME, variant=1)]
        return [dict(plain, frames=[[(p, ops)] for p in table(arg, 64)])]
    if h == 0x223ce0:
        return [dict(plain, frames=[[arg]], cond=CLOCK_WINDOW, window=(u16(0x223d44), u16(0x223d46)))]
    actor_of = lambda kind, params=(0.0,)*6: dict(kind=kind, params=list(params))
    if h == 0x223bc0:
        points = [rom.f32(0x223cc8+4*i) for i in range(3)]+[rom.f32(0x223cd4+4*i) for i in range(3)]
        return [dict(plain, frames=[[p] for p in table(arg, 64)], cond=CLOCK_ACTOR, actor=actor_of(XA_BEARING, points))]
    birds = [[p] for p in table(0x2368c0, 32)]
    horses = [[p] for p in table(0x236840, 32)]
    placed = dict(plain, cond=CLOCK_ACTOR, placed=GROUP_PLACED)
    if h == 0x22430c:
        return [dict(placed, frames=birds, actor=actor_of(XA_BIRDS), variant=k) for k in range(7)]
    if h == 0x224730:
        return [dict(placed, frames=birds, actor=actor_of(XA_BIG_BIRD))]
    if h == 0x224854:
        return [dict(placed, frames=horses, actor=actor_of(XA_HORSE))]
    if h == 0x224cd0:
        return [dict(placed, frames=horses, actor=actor_of(XA_PAIR), variant=k) for k in range(2)]
    if h == 0x22302c and arg == STATUE_ROUTINE:
        return [dict(placed, frames=[[STATUE_LIST]], actor=actor_of(XA_STATUE))]
    raise ValueError('not one of Expert\'s actors')


def expert_split(group):
    """A group of expert_groups with its frames as list pointers and the placements apart (ops: per frame, per list)."""
    entries = group['frames']
    group = dict(group)
    group['frames'] = [[e[0] if isinstance(e, tuple) else e for e in frame] for frame in entries]
    group['ops'] = [[list(map(list, e[1])) if isinstance(e, tuple) else None for e in frame] for frame in entries]
    if not any(o for frame in group['ops'] for o in frame):
        group['ops'] = None
    return group


def is_expert(actor):
    return actor['handler'] in EXPERT_ACTORS or (actor['handler'] == 0x22302c and actor['argument'] == STATUE_ROUTINE)


def matrix_of(ops):
    """The placement of an operation list (float32 3x4 rows: R | t), as nm_xa_pose_matrix composes it."""
    m = np.eye(3, dtype=np.float32)
    t = np.zeros(3, np.float32)
    for op in ops:
        if op[0] == 'T':
            t = (t+m @ np.array(op[1:], np.float32)).astype(np.float32)
        elif op[0] == 'S':
            m = (m*np.array(op[1:], np.float32)[None, :]).astype(np.float32)
        else:
            a = float(op[1] & 0xffff)*math.pi/32768.0
            c, s = np.float32(math.cos(a)), np.float32(math.sin(a))
            r = {'RX': [[1, 0, 0], [0, c, s], [0, -s, c]], 'RY': [[c, 0, -s], [0, 1, 0], [s, 0, c]],
                 'RZ': [[c, s, 0], [-s, c, 0], [0, 0, 1]]}[op[0]]
            m = (m @ np.array(r, np.float32)).astype(np.float32)
    return m, t


def apply_ops(faces, ops):
    """Faces of a list drawn after the placement: positions M p + t, normals M n (not renormalised: lighting.h)."""
    if not ops:
        return faces
    m, t = matrix_of(ops)
    return [dict(head=f['head'], tail=f['tail'], normal=(m @ f['normal']).astype(np.float32),
                 v=[((m @ p + t).astype(np.float32), u, v) for p, u, v in f['v']]) for f in faces]


def clock_frames(lists, list_faces, group):
    """(mode, per frame its faces): the lists of a frame in drawing order. Every frame must have one layout: the same
    faces with the same corner counts, sides and texture header word 0 (the list a face goes to)."""
    mode, frames = None, []
    placements = group.get('ops') or [[None]*len(pointers) for pointers in group['frames']]
    for pointers, ops in zip(group['frames'], placements):
        faces = []
        for pointer, placement in zip(pointers, ops):
            if pointer not in lists:
                continue
            m, part, _ = list_faces(pointer)
            if mode not in (None, m):
                raise ValueError('a clock group mixes lighting modes')
            mode = m
            faces += apply_ops(part, placement)
        frames.append(faces)
    layout = lambda faces: [(len(f['v']), f['head'][0], int(f['head'][1]) & (1 << 17), f['head'][2]) for f in faces]
    if mode is None or any(layout(faces) != layout(frames[0]) for faces in frames):
        raise ValueError('a clock group whose frames differ in layout')
    if len(frames) & (len(frames)-1):
        raise ValueError('a clock animation has a power-of-two count of frames')
    if len(frames) == 1 and group['cond'] not in (CLOCK_WINDOW, CLOCK_ACTOR):
        raise ValueError('a clock animation of one frame')
    return mode, frames


def float32_up(value):
    return np.nextafter(np.float32(value), np.float32(np.inf), dtype=np.float32)


def clock_base(frames):
    """The base mesh of a group (frame 0's faces) and the identity of each corner. A point is a position sequence over
    the frames; a vertex record (prepare_models.py: one per position and u, v) a point with a u, v sequence. Positions
    a later frame tells apart must not be one vertex record or one shared position (prepare_models.py
    --share-positions) in the base mesh, so a corner whose frame-0 position another identity holds is moved up by
    float32 steps in x until it is free: the renderer writes every position of the frame it draws, the base positions
    serve the offline tools only. Returns (faces, corners: per face its corners' (point, vertex) identity indices,
    points: position sequences n x P x 3, vertices: [(point, u, v sequence)], base xyz per vertex)."""
    P = np.array([[p for f in faces for p, _, _ in f['v']] for faces in frames], np.float32)
    U = np.array([[(int(u), int(v)) for f in faces for _, u, v in f['v']] for faces in frames], np.int64)
    point_of, points, vertex_of, vertices = {}, [], {}, []
    corner_ids = []
    for c in range(P.shape[1]):
        pk = P[:, c].tobytes()
        if pk not in point_of:
            point_of[pk] = len(points)
            points.append(P[:, c])
        vk = (point_of[pk], U[:, c].tobytes())
        if vk not in vertex_of:
            vertex_of[vk] = len(vertices)
            vertices.append((point_of[pk], U[:, c]))
        corner_ids.append((point_of[pk], vertex_of[vk]))
    held = {}   # base xyz bytes -> [(point, u, v at frame 0, vertex)]
    base = []
    for v, (point, uv) in enumerate(vertices):
        xyz = points[point][0].copy()
        while True:
            entries = held.get(xyz.tobytes(), [])
            if not any(e[0] != point or (e[1] == tuple(uv[0]) and e[2] != v) for e in entries):
                break
            xyz[0] = float32_up(xyz[0])
        held.setdefault(xyz.tobytes(), []).append((point, tuple(uv[0]), v))
        base.append(xyz)
    faces, at, corners = [], 0, []
    for f in frames[0]:
        ids = corner_ids[at:at+len(f['v'])]
        faces.append(dict(head=f['head'], tail=f['tail'], normal=f['normal'],
                          v=[(base[vertex], u, w) for (point, vertex), (_, u, w) in zip(ids, f['v'])]))
        corners.append(ids)
        at += len(f['v'])
    return faces, corners, np.stack(points, 1), vertices, base


def static_lumas(face, light=(-0.45, -0.89, -0.45)):
    """(front, back) luma of a face under the race light: prepare_face_order.py static_lumas, the same single-precision
    arithmetic (back 0 for a one-sided face)."""
    f32 = lambda value: struct.unpack('<f', struct.pack('<f', value))[0]
    light = [f32(v) for v in light]
    diffuse, ambient, scale, control = float(face['tail'][0]), float(face['tail'][1]), float(face['tail'][3]), int(face['tail'][2])
    n = [f32(float(v)) for v in face['normal']]
    dotl = f32(f32(f32(n[0]*light[0])+f32(n[1]*light[1]))+f32(n[2]*light[2]))
    lit = f32(f32(abs(dotl)*f32(diffuse))+f32(ambient))
    dark = f32(ambient)
    lit, dark = (int(min(max(v, 0.0), 255.0)) for v in (lit, dark))
    two = int(face['head'][1]) & (1 << 17)
    return (lit if dotl > 0 else dark), ((lit if dotl < 0 else dark) if two else 0)


def clock_state(face):
    """What a face shows in a frame: texture header and its lumas (front, back)."""
    return tuple(int(x) for x in face['head'][2:6])+static_lumas(face)


# DNA1 (include/clock_actors.h): the frames of the frame-counter animations, per group. Little endian.
DNA_HEADER = struct.Struct('<4sIII4I')
DNA_ACTOR = struct.Struct('<I3fhHBBH6f')   # include/clock_actors.h nm_clock_actor_t
DNA_GROUP = struct.Struct('<IBBBB4B2H12H6f10I')
DNA_STATE = struct.Struct('<4HBBH')
NORMAL_SCALE = 8192.0   # stored normals: int16 / NORMAL_SCALE
RATES = (8, 4, 2, 1)


def lerp_error(P, rate):
    """Largest position error (units) of frames stored every `rate` frames and interpolated linearly (cyclic)."""
    n = len(P)
    if n % rate:
        return float('inf')
    worst = 0.0
    for k in range(n):
        a0 = (k//rate)*rate
        a1, t = (a0+rate) % n, (k-a0)/rate
        approx = P[a0].astype(np.float64)*(1-t)+P[a1].astype(np.float64)*t
        worst = max(worst, float(np.abs(approx-P[k]).max()))
    return worst


def clock_dna(rom, args):
    """The DNA1 file of the frame-counter animations (see the module text)."""
    import prepare_restrip as pr
    lists, list_faces = export_lists(args.objects)
    described = json.loads((args.sources/'clock.json').read_text())
    pack = pr.read_dnm6(args.pack.read_bytes())
    vertex_table = pack['vertices']
    groups, blobs, report = [], [], []
    expected = bytearray(struct.pack('<4sI', b'CKX1', len(described['groups'])))
    for g in described['groups']:
        group = dict(g, frames=g['frames'], window=tuple(g['window']))
        mode, frames = clock_frames(lists, list_faces, group)
        faces, corners, points, vertices, base = clock_base(frames)
        n = len(frames)
        mesh = pack['meshes'][g['mesh']]
        vertex_first, vertex_count, face_first, face_count = mesh[:4]
        if face_count != len(faces):
            raise ValueError(f'clock group of {g["handler"]:x}: the pack mesh has {face_count} faces, the frames {len(faces)}')
        # The pack's vertex records: one per base position and u, v (prepare_models.py).
        identity = {(base[v].tobytes(), int(uv[0][0]), int(uv[0][1])): v for v, (_, uv) in enumerate(vertices)}
        record_vertex = []
        for i in range(vertex_count):
            x, y, z, u, w = pm.VERTEX.unpack_from(vertex_table, (vertex_first+i)*pm.VERTEX.size)
            key = (np.array([x, y, z], np.float32).tobytes(), u, w)
            if key not in identity:
                raise ValueError(f'clock group of {g["handler"]:x}: pack vertex {i} is no corner of the base mesh')
            record_vertex.append(identity[key])
        # Faces: by their corners' vertex identities and texture header (the face order may permute and rotate them).
        sources = {}
        for i, (face, ids) in enumerate(zip(faces, corners)):
            sources.setdefault((tuple(sorted(v for _, v in ids)), tuple(int(x) for x in face['head'][2:6])), []).append(i)
        source_of = []
        materials = pack['materials']
        for f in range(face_count):
            face = pack['faces'][face_first+f]
            header = pm.MATERIAL.unpack_from(materials, face[4]*pm.MATERIAL.size)[:4]
            key = (tuple(sorted(record_vertex[face[k]] for k in range(face[5]))), tuple(header))
            if not sources.get(key):
                raise ValueError(f'clock group of {g["handler"]:x}: pack face {f} matches no source face')
            source_of.append(sources[key].pop(0))
        # States per face and frame; frame 0 must be what the pack baked.
        state_index, states = {}, []
        face_states = np.zeros((n, face_count), np.int64)
        placed = g.get('placed', 0)
        # A PLACED group's lumas are evaluated at run time: its states hold the header and the material's ambient.
        face_state = (lambda face: clock_state(face)[:4]+(int(float(face['tail'][1])), 0)) if placed else clock_state
        for k, frame in enumerate(frames):
            for f in range(face_count):
                state = face_state(frame[source_of[f]])
                if state not in state_index:
                    state_index[state] = len(states)
                    states.append(state)
                face_states[k, f] = state_index[state]
        for f in range(face_count if not placed else 0):
            face = pack['faces'][face_first+f]
            front, back = states[face_states[0, f]][4:6]
            if (face[6], face[13] if face[7] & (1 << 17) else 0) != (front, back):
                raise ValueError(f'clock group of {g["handler"]:x}: face {f} baked {face[6]}/{face[13]}, frame 0 {front}/{back}')
        if len(states) > 65535:
            raise ValueError('too many clock states')
        state_faces = [f for f in range(face_count) if (face_states[:, f] != face_states[0, f]).any()]
        # Normals: faces whose side a frame may test (one-sided, or two lumas) and whose normal changes.
        N = np.array([[frame[source_of[f]]['normal'] for f in range(face_count)] for frame in frames], np.float32)
        one_sided = lambda f: not int(faces[source_of[f]]['head'][1]) & (1 << 17)
        sided = [f for f in range(face_count)
                 if one_sided(f) or any(states[face_states[k, f]][4] != states[face_states[k, f]][5] for k in range(n))]
        normal_faces = [f for f in sided if (np.abs(N[:, f]-N[0, f]) > 0).any()] if not placed else list(range(face_count))
        # Positions: the points the pack's vertex records stand for, at the coarsest rate within the error bound.
        # Only the points that move are stored; a vertex record of a still point keeps its base position (0xffff).
        record_point = [vertices[v][0] for v in record_vertex]
        used_points = sorted(pnt for pnt in set(record_point) if (points[:, pnt] != points[0:1, pnt]).any())
        point_slot = {pnt: i for i, pnt in enumerate(used_points)}
        P = points[:, used_points]   # n x moving points x 3
        moving = bool(used_points) or bool(normal_faces)
        rate, error = 0, 0.0
        if placed:
            rate = 1   # every frame (the lighting reads every normal)
        elif moving:
            for r in RATES:
                e = lerp_error(P, r)
                normal_error = lerp_error(N[:, normal_faces], r) if normal_faces else 0.0
                if r == 1 or (e <= args.max_error and normal_error <= 0.05):
                    rate, error = r, (e if r > 1 else 0.0)
                    break
        low = P.reshape(-1, 3).min(0).astype(np.float64) if used_points else np.zeros(3)
        high = P.reshape(-1, 3).max(0).astype(np.float64) if used_points else np.zeros(3)
        origin = ((low+high)/2).astype(np.float32)
        step = np.maximum((high-low)/2/32000.0, 1e-6).astype(np.float32)
        stored = n//rate if rate else 0
        Q = np.rint((P[::rate].astype(np.float64)-origin)/step).astype(np.int64) if rate else np.zeros((0, len(used_points), 3), np.int64)
        if rate and Q.size and np.abs(Q).max() > 32767:
            raise ValueError('clock positions outside the int16 range')
        decoded = (origin+Q*step).astype(np.float32) if rate else None
        quantisation = float(np.abs(decoded-P[::rate]).max()) if rate and used_points else 0.0
        NQ = np.rint(N[::rate][:, normal_faces]*NORMAL_SCALE).astype(np.int64) if rate and normal_faces else np.zeros((0, 0, 3), np.int64)
        if NQ.size and np.abs(NQ).max() > 32767:
            raise ValueError('clock normal outside the int16 range')
        # u, v of the vertex records whose u, v change: the distinct sets of them and each frame's set.
        uv_vertices = [i for i, v in enumerate(record_vertex) if (vertices[v][1] != vertices[v][1][0:1]).any()]
        UV = np.array([[vertices[record_vertex[i]][1][k] for i in uv_vertices] for k in range(n)], np.int64).reshape(n, len(uv_vertices), 2)
        if UV.size and (UV.min() < 0 or UV.max() > 65535):
            raise ValueError('clock u, v outside 16 bits')
        uv_set_of, uv_sets, uv_frame = {}, [], []
        for k in range(n if uv_vertices else 0):
            key = UV[k].tobytes()
            if key not in uv_set_of:
                uv_set_of[key] = len(uv_sets)
                uv_sets.append(UV[k])
            uv_frame.append(uv_set_of[key])
        tables = [
            struct.pack(f'<{vertex_count}H', *(point_slot.get(pnt, 0xffff) for pnt in record_point)),
            Q.astype('<i2').tobytes(),
            b''.join(DNA_STATE.pack(*st[:4], st[4], st[5], 0) for st in states),
            struct.pack(f'<{len(state_faces)}H', *state_faces),
            face_states[:, state_faces].astype('<u2').tobytes(),
            struct.pack(f'<{len(normal_faces)}H', *normal_faces),
            (np.concatenate([NQ, np.zeros(NQ.shape[:2]+(1,), np.int64)], 2).astype('<i2').tobytes() if NQ.size else b''),
            struct.pack(f'<{len(uv_vertices)}H', *uv_vertices),
            np.array(uv_sets, np.int64).astype('<u2').tobytes() if uv_sets else b'',
            struct.pack(f'<{len(uv_frame)}H', *uv_frame)]
        if args.expected:
            # What every frame shows, from the export (the arcade's lists), in the pack's vertex and face order.
            expected += struct.pack('<3I', n, vertex_count, face_count)
            for k in range(n):
                expected += np.array([points[k, vertices[v][0]] for v in record_vertex], '<f4').tobytes()
                expected += np.array([vertices[v][1][k] for v in record_vertex], '<u2').tobytes()
                expected += np.array([frames[k][source_of[f]]['normal'] for f in range(face_count)], '<f4').tobytes()
                expected += b''.join(DNA_STATE.pack(*st[:4], st[4], st[5], 0)
                                     for st in (face_state(frames[k][source_of[f]]) for f in range(face_count)))
        cells = list(g['cells'])+[0]*(4-len(g['cells']))
        groups.append(dict(mesh=g['mesh'], cond=g['cond'], near=g['near'], shift=g['shift'], cells=cells, count=len(g['cells']),
                           window=g['window'], frames=n, rate=rate, points=len(used_points), vertices=vertex_count,
                           faces=face_count, states=len(states), state_faces=len(state_faces),
                           normal_faces=len(normal_faces), uv_vertices=len(uv_vertices), uv_sets=len(uv_sets),
                           variant=g['variant'], flags=placed, origin=origin, step=step, tables=tables,
                           actor=g.get('actor'), record=g['record'], handler=g['handler']))
        report.append(dict(handler='%x' % g['handler'], record='%x' % g['record'], mesh=g['mesh'], frames=n, faces=face_count,
                           vertices=vertex_count, points=len(used_points), rate=rate, interpolation_error=round(error, 4),
                           quantisation_error=round(quantisation, 5), states=len(states), state_faces=len(state_faces),
                           normal_faces=len(normal_faces), uv_vertices=len(uv_vertices), uv_sets=len(uv_sets),
                           bytes=sum(len(t) for t in tables)+DNA_GROUP.size))
    out = bytearray(DNA_HEADER.size+DNA_GROUP.size*len(groups))
    records, written = [], {}
    for gr in groups:
        offsets = []
        for t in gr['tables']:
            if t in written:   # groups that draw one list table share its frames
                offsets.append(written[t])
                continue
            out += bytes(-len(out) % 4)
            offsets.append(len(out))
            written[t] = len(out)
            out += t
        records.append(offsets)
    out += bytes(-len(out) % 4)
    # The actors (Expert): consecutive groups of one actor record with an actor kind.
    actors = []
    for i, gr in enumerate(groups):
        if gr['actor'] is None:
            continue
        if actors and actors[-1]['record'] == gr['record']:
            actors[-1]['groups'] += 1
            continue
        actors.append(dict(gr['actor'], record=gr['record'], handler=gr['handler'], first=i, groups=1))
    actor_offset = len(out) if actors else 0
    for a in actors:
        if a['groups'] != XA_GROUPS[a['kind']]:
            raise ValueError(f'actor {a["record"]:x}: {a["groups"]} groups')
        out += DNA_ACTOR.pack(a['handler'], *map(float, a['position']), a['heading'], a['cell'], a['kind'], a['groups'],
                              a['first'], *map(float, a['params']))
    for i, (gr, offsets) in enumerate(zip(groups, records)):
        DNA_GROUP.pack_into(out, DNA_HEADER.size+i*DNA_GROUP.size, gr['mesh'], gr['cond'], gr['near'], gr['shift'], gr['count'],
                            *gr['cells'], *gr['window'], gr['frames'], gr['rate'], gr['points'], gr['vertices'], gr['faces'],
                            gr['states'], gr['state_faces'], gr['normal_faces'], gr['uv_vertices'], gr['uv_sets'],
                            gr['variant'], gr['flags'],
                            *map(float, gr['origin']), *map(float, gr['step']), *offsets)
    DNA_HEADER.pack_into(out, 0, b'DNA1', len(out), len(groups), 1 if actors else 0, len(actors), actor_offset, 0, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as f:
        f.write(out)
    if args.expected:
        with args.expected.open('xb') as f:
            f.write(expected)
    print(json.dumps(dict(bytes=len(out), groups=report, actors=len(actors), shared_tables=sum(len(r) for r in records)-len(written)),
                     indent=1))


# Selector kinds of an object instance (meshpack.h, NM_OBJECT_*).
KIND_ALWAYS, KIND_BOARD, KIND_BEARING, KIND_TURN, KIND_CREW, KIND_SCREEN, KIND_RANK, KIND_SLOT, KIND_CONE = 0, 1, 2, 3, 4, 5, 6, 11, 14
# Leader board 221dd8: the panel translations 238dc0 (3 floats each, 221e48) and the
# 46 lists 238dfc indexed by a car's spawn slot (221e5c); 238dfc[0] is the player's.
RANK_OFFSETS, RANK_PANELS = 0x238dc0, 5
# Slot machine 2204fc: reel k's draw in 220534 (22061c, 2206b0, 220744) loads x, y, z and the Y angle as lda
# immediates at base+4, +12, +20, +28, then draws list 2385f8[reel] (65 entries, 64 is 0). The record's position
# is not used (reel 1's z is 430cb474, the record's 430cb47b).
SLOT_CODE, SLOT_LISTS, SLOT_MODELS = (0x22061c, 0x2206b0, 0x220744), 0x2385f8, 64
# Placement flags of an object instance (meshpack.h, NM_OBJECT_PLACED/SPIN/MESHES).
OBJECT_PLACED, OBJECT_SPIN, OBJECT_MESHES = 0x200, 0x400, 0x800
# Rest height of a prop above y of its record: its ground height (11620: 0x3dcccccc in cell 122, -0 in
# cell 118) plus the height 0.6 of the rest record 2896d84 (223500), in single precision; course 0, both
# groups on level ground (no slope turn). spec-cones.md section 4; the reference display lists agree.
PROP_REST_Y = {122: struct.unpack('<f', struct.pack('<I', 0x3f333334))[0], 118: struct.unpack('<f', struct.pack('<I', 0x3f19999a))[0]}


def parse_mesh(path):
    faces = []
    for line in path.read_text().splitlines():
        f = line.split()
        if not f or f[0] in ('#', 'bounds'):
            continue
        if f[0] == 'mode':
            mode = int(f[1])
        elif f[0] == 'face':
            faces.append(dict(head=f[1:7], normal=np.array(f[7:10], np.float32), tail=f[10:14], v=[]))
        else:
            faces[-1]['v'].append((np.array(f[1:4], np.float32), f[4], f[5]))
    return mode, faces


def wrap16(value):
    """int16 of a 16-bit angle word."""
    value &= 0xffff
    return value-65536 if value & 0x8000 else value


def placed(faces, position, heading, roll=0):
    """Opcode 12 translate, opcode 15 rotate-Y, opcode 16 rotate-Z on the current
    matrix: world = T * Ry * Rz * local, Ry = [[c,0,-s],[0,1,0],[s,0,c]],
    Rz = [[c,s,0],[-s,c,0],[0,0,1]] (angles in 1/65536 turn), in float32."""
    a = float(heading)*math.pi/32768.0
    c, s = np.float32(math.cos(a)), np.float32(math.sin(a))
    rotation = np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]], np.float32)
    if roll:
        b = float(roll)*math.pi/32768.0
        cz, sz = np.float32(math.cos(b)), np.float32(math.sin(b))
        rotation = (rotation @ np.array([[cz, sz, 0], [-sz, cz, 0], [0, 0, 1]], np.float32)).astype(np.float32)
    t = np.array(position, np.float32)
    out = []
    for f in faces:
        out.append(dict(head=f['head'], tail=f['tail'], normal=(rotation @ f['normal']).astype(np.float32),
                        v=[((rotation @ p + t).astype(np.float32), u, v) for p, u, v in f['v']]))
    return out


def placement(position, heading):
    """Column-major 3x4 world = T * Ry(heading) * model of placed(), as instance.matrix."""
    a = float(heading)*math.pi/32768.0
    c, s = np.float32(math.cos(a)), np.float32(math.sin(a))
    rotation = np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]], np.float32)
    return [float(rotation[r, col]) for col in range(3) for r in range(3)]+[float(np.float32(v)) for v in position]


def mesh_text(mode, faces):
    def number(v):
        return '%.9g' % float(np.float32(v))
    points = np.array([p for f in faces for p, _, _ in f['v']], np.float32).reshape(-1, 3)
    bounds = list(points.min(0))+list(points.max(0)) if len(points) else [0.0]*6
    lines = ['# native source mesh v1; world coordinates (prepare_world_objects.py); UVs in original texture units',
             'mode %d' % mode,
             'bounds '+' '.join(number(v) for v in bounds)]
    for f in faces:
        lines.append('face '+' '.join(f['head'])+' '+' '.join(number(v) for v in f['normal'])+' '+' '.join(f['tail']))
        lines += ['v '+' '.join(number(v) for v in p)+' %s %s' % (u, v) for p, u, v in f['v']]
    return '\n'.join(lines)+'\n', sum(len(f['v']) for f in faces)


def export_lists(objects):
    """The object export's lists (m2segment --export-native-objects): pointer -> instances, and a reader of a list's faces."""
    lists = {}
    for line in (objects/'lists.txt').read_text().splitlines():
        f = line.split()
        if f[0] != '#' and int(f[6]) >= 0:
            lists.setdefault(int(f[0], 16), []).append(int(f[6]))
    rows = [l.split() for l in (objects/'instances.txt').read_text().splitlines() if not l.startswith('#')]

    def list_faces(pointer):
        mode, faces, addresses = None, [], []
        for instance in lists[pointer]:
            m, part = parse_mesh(objects/('mesh_%s.txt' % rows[instance][0]))
            if mode not in (None, m):
                raise ValueError('object list mixes lighting modes')
            mode = m
            faces += part
            addresses.append(int(rows[instance][1]))
        return mode, faces, addresses
    return lists, list_faces


# The glass bands' Gouraud levels: a level goes where the texels between its neighbours stay within this many grey
# steps (of 255) of the line through them (Douglas-Peucker): fewer triangles, the ramp within 1.2 % before alpha 1/2.
GLASS_GREY_TOLERANCE = 3


def simplify(levels, tolerance):
    """Keep the levels (position, grey) a linear interpolation needs to stay within tolerance of the rest."""
    if len(levels) <= 2:
        return levels
    (t0, c0), (t1, c1) = levels[0], levels[-1]
    worst, at = -1.0, 0
    for i in range(1, len(levels)-1):
        t, c = levels[i]
        error = abs(c-(c0+(c1-c0)*(t-t0)/(t1-t0)))
        if error > worst:
            worst, at = error, i
    if worst <= tolerance:
        return [levels[0], levels[-1]]
    return simplify(levels[:at+1], tolerance)[:-1]+simplify(levels[at:], tolerance)


def glass(rom, args):
    """The slot machine's glass (face 4 of every reel list 2385f8[k]: a screen-checker face, texture header
    e401 with texel 15 transparent) as Gouraud bands for the translucent list (render/scene/slot_glass.c): the
    texture's opaque columns along u (the reel's height) are bands across the face; each band a strip of
    levels at its edges and at its texel centres, coloured with those texels (the bilinear reading of the
    texture), in the reels' model coordinates (Ry of the reels baked, as their meshes). The arcade draws the
    face on every other pixel (raster.cpp checker); alpha 0x80 on the translucent list is that average."""
    _, list_faces = export_lists(args.objects)
    pointers = [rom.u32(SLOT_LISTS+4*i) for i in range(SLOT_MODELS+1)]
    angles = {rom.u32(base+28) for base in SLOT_CODE}
    if len(angles) != 1:
        raise ValueError('slot machine reels with different angles')
    angle = wrap16(angles.pop())
    found = []
    for p in pointers[:SLOT_MODELS]:
        _, faces, _ = list_faces(p)
        g = [f for f in faces if int(f['head'][2]) & 0x8000]
        if len(g) != 1:
            raise ValueError('reel list %x: one screen-checker face expected' % p)
        found.append(g[0])
    face = found[0]
    key = lambda f: (tuple(f['head']), tuple(f['tail']), tuple(float(x) for x in f['normal']),
                     tuple((tuple(float(c) for c in q), u, v) for q, u, v in f['v']))
    if any(key(f) != key(face) for f in found) or len(face['v']) != 4:
        raise ValueError('the glass differs between the reel lists')
    h = [int(x) for x in face['head'][2:6]]
    texture = None
    for line in (args.objects/'materials'/'materials.txt').read_text().splitlines():
        f = line.split()
        if len(f) >= 9 and f[0].endswith('.tga') and [int(x) for x in f[4:8]] == h:
            texture = (f[0], int(f[2]), int(f[3]))
    if texture is None:
        raise ValueError('no exported texture for the glass header %s' % h)
    data = (args.objects/'materials'/texture[0]).read_bytes()
    w, hgt = texture[1], texture[2]
    if data[2] != 2 or data[16] != 32 or struct.unpack_from('<HH', data, 12) != (w, hgt):
        raise ValueError('glass texture: 32-bit TGA expected')
    top_left = bool(data[17] & 0x20)
    pixel = lambda x, y: data[18+data[0]+4*(w*(y if top_left else hgt-1-y)+x):][:4]   # B, G, R, A
    us = [int(u) for _, u, _ in face['v']]
    vs = [int(v) for _, _, v in face['v']]
    rows = range(max(0, min(vs)//8), min(hgt, (max(vs)+7)//8))
    columns = []
    for x in range(w):
        texels = {pixel(x, y) for y in rows}
        if len(texels) != 1:
            raise ValueError('glass texture: not constant across the face (column %d)' % x)
        b, g, r, a = texels.pop()
        if a and not r == g == b:
            raise ValueError('glass texture: grey texels expected (column %d)' % x)
        columns.append(r if a else None)
    if sorted(set(us)) != [0, 8*w]:
        raise ValueError('glass face: u from 0 to the texture width expected')
    rotated = placed([face], (0, 0, 0), angle)[0]
    corners = [(q, int(u), int(v)) for q, u, v in rotated['v']]
    sides = []
    for side in (min(vs), max(vs)):
        ends = {u: q for q, u, v in corners if v == side}
        if sorted(ends) != [0, 8*w]:
            raise ValueError('glass face: each side runs from u 0 to the texture width')
        sides.append((ends[0], ends[8*w]))

    def at(side, texels):
        a, b = sides[side]
        return (a+(b-a)*np.float32(texels/w)).astype(np.float32)
    bands, x = [], 0
    while x < w:
        if columns[x] is None:
            x += 1
            continue
        first = x
        while x < w and columns[x] is not None:
            x += 1
        levels = [(first, columns[first])]+[(i+0.5, columns[i]) for i in range(first, x)]+[(x, columns[x-1])]
        levels = simplify(levels, GLASS_GREY_TOLERANCE)
        bands.append([(at(0, t), at(1, t), c) for t, c in levels])
    if not bands:
        raise ValueError('glass: no opaque texel')

    def flt(v):
        v = float(np.float32(v))
        return ('-' if math.copysign(1.0, v) < 0 else '')+float.hex(abs(v))+'f'
    normal = rotated['normal']
    out = ['/* Local ROM-derived data: do not distribute. The slot machine glass (prepare_world_objects.py glass). */',
           '#ifndef DAYTONA_SLOT_GLASS_DATA_H', '#define DAYTONA_SLOT_GLASS_DATA_H',
           '#define NM_SLOT_GLASS_DATA 1',
           'enum { NM_SLOT_GLASS_BANDS = %d, NM_SLOT_GLASS_LEVELS = %d };' % (len(bands), sum(len(b) for b in bands)),
           '/* Band b: levels first[b] .. first[b+1]-1, from one edge of the band to the other. */',
           'static const uint8_t nm_slot_glass_first[NM_SLOT_GLASS_BANDS+1] = {%s};' % ','.join(
               str(sum(len(b) for b in bands[:k])) for k in range(len(bands)+1)),
           '/* The face\'s normal and corner 2 (the arcade\'s facing test), Ry of the reels baked. */',
           'static const float nm_slot_glass_normal[3] = {%s};' % ','.join(flt(v) for v in normal),
           'static const float nm_slot_glass_corner[3] = {%s};' % ','.join(flt(v) for v in corners[2][0]),
           '/* Per band and level: the point on each side of the face, the texel\'s grey (0..255). */',
           'static const struct { float side[2][3]; uint8_t grey; } nm_slot_glass_levels[NM_SLOT_GLASS_LEVELS] = {']
    out.append(',\n'.join('{{{%s},{%s}},%d}' % (','.join(flt(v) for v in p), ','.join(flt(v) for v in q), c)
                           for band in bands for p, q, c in band)+'};')
    out += ['#endif', '']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x') as f:
        f.write('\n'.join(out))
    print(json.dumps(dict(texture=texture[0], bands=len(bands), levels=[len(b) for b in bands],
                          opaque_columns=sum(c is not None for c in columns), greys=sorted({c for c in columns if c is not None}))))


def build(rom, table, args):
    """Combined source directory: the cells, then one mesh per selection group."""
    lists, list_faces = export_lists(args.objects)
    groups = {}   # (cell, bitmap, kind, variant) -> [mode, faces, addresses, actors]
    specials = []   # meshes and instances of their own (PLACED, SPIN, MESHES; meshpack.h)
    clock_keys = set()   # (header words 0..3, luma) of every frame of the frame-counter animations
    skipped = []
    dropped = []   # faces left out, with the reason
    for a in table:
        pointers = actor_lists(rom, a, args.course)
        h = a['handler']
        if h in (0x2203a4, 0x220450):
            members = [(KIND_ALWAYS, 0, pointers[0], (a['x'], a['y'], a['z']), a['heading'])]
        elif h in PROP_KINDS:   # a prop: its own PLACED instance, posed by its state (include/props.h)
            if args.course == 0:
                if h != 0x223114 or a['cell'] not in PROP_REST_Y or a['heading'] or a['y'] or not 1 <= a['id'] <= 10:
                    raise ValueError('prop without a rest pose from the reference')
                rest = PROP_REST_Y[a['cell']]
            else:
                # The props draw every prop from its state (the rest record's height above the ground the init's
                # query finds, its heading and the ground's slope); the instance is the pose without prop state:
                # the rest record's height (the kind's init entry, +0x10) on the actor's y, heading only.
                if a['y'] or not 1 <= a['id'] <= 35:
                    raise ValueError('prop actor off the ground plane')
                rest = rom.f32(rom.u32(h+0x10)+4)
            share = bool(specials) and specials[-1]['kind'] == KIND_CONE and specials[-1]['pointer'] == pointers[0]
            specials.append(dict(cell=a['cell'], near=0, kind=KIND_CONE, pointer=pointers[0],
                                 meshes='previous' if share else [list_faces(pointers[0])], record=a['record'],
                                 instances=[(a['id']-1, OBJECT_PLACED, placement((a['x'], rest, a['z']), a['heading']))]))
            continue
        elif h == 0x221d24:   # drawn with the view matrix itself: 221db4
            members = [(KIND_BOARD, v, p, (0, 0, 0), 0) for v, p in enumerate(pointers)]
        elif h == 0x223b08:
            members = [(KIND_BEARING, v, p, (0, 0, 0), 0) for v, p in enumerate(pointers)]
        elif h == 0x220be4:   # die: heading int16(26031 + 256n) at the n-th near tick (220c34..220c48)
            # 256 baked poses (its side faces' light follows the angle) behind one MESHES instance
            meshes = []
            for n in range(256):
                mode, faces, addresses = list_faces(pointers[0])
                meshes.append((mode, placed(faces, (a['x'], a['y'], a['z']), wrap16(a['heading']+256*n)), addresses))
            specials.append(dict(cell=a['cell'], near=1, kind=KIND_TURN, meshes=meshes, record=a['record'],
                                 instances=[(0, OBJECT_MESHES, None)]))
            continue
        elif h == 0x220b14:   # symbol wheel: T Ry(0) Rz(int16(256n)) (220b64..220b80); its light does not change
            mode, faces, addresses = list_faces(pointers[0])
            specials.append(dict(cell=a['cell'], near=1, kind=KIND_ALWAYS, meshes=[(mode, faces, addresses)], record=a['record'],
                                 instances=[(0, OBJECT_PLACED | OBJECT_SPIN, placement((a['x'], a['y'], a['z']), 0))]))
            continue
        elif h == 0x220d44:   # pit crew: lists A, B, C, B by (n mod 25)/8 (220dc0..220e64); T Ry(heading)
            members = [(KIND_CREW, v, p, (a['x'], a['y'], a['z']), a['heading']) for v, p in enumerate(pointers)]
        elif h == 0x22024c:   # video screen: list 238b04[(5010b0>>3)&3], T Ry(heading) (2202a8..2202f0)
            members = [(KIND_SCREEN, v, p, (a['x'], a['y'], a['z']), a['heading']) for v, p in enumerate(pointers)]
        elif h == 0x221dd8:   # leader board: panel k at 238dc0[k], list 238dfc[slot] (221e3c..221e88)
            # the numbers of slots 0.. (the drawable lists; 41..45 are blank) in model coordinates, shared by
            # five PLACED instances; each panel only translates, so the baked light holds
            numbers = []
            for p in pointers[1:]:
                if p not in lists:
                    break
                numbers.append(list_faces(p))
            for k in range(RANK_PANELS):
                offset = tuple(rom.f32(RANK_OFFSETS+12*k+4*i) for i in range(3))
                specials.append(dict(cell=a['cell'], near=0, kind=KIND_RANK+k, meshes=numbers if k == 0 else 'previous',
                                     record=a['record'], instances=[(len(numbers)-1, OBJECT_PLACED | OBJECT_MESHES,
                                                                     placement(offset, 0))]))
            continue
        elif h == 0x2204fc:   # slot machine: reel k is T(p_k) Ry(angle) list 2385f8[r_k]
            # 64 meshes with Ry baked in (their light follows the rotated normals), in model coordinates around
            # the reel's centre and shared by three PLACED instances that only translate, as the leader board.
            # Face 4 of every list is the glass in front of the reel, a screen-checker (stipple) face: the
            # material build has no source binding for one outside the cells, so it is left out for now.
            if len(pointers) != SLOT_MODELS+1 or pointers[SLOT_MODELS] != pointers[0] or len(set(pointers[:SLOT_MODELS])) != SLOT_MODELS:
                raise ValueError('slot machine lists: 64 distinct lists and list 64 == list 0 expected')
            angles = {rom.u32(base+28) for base in SLOT_CODE}
            if len(angles) != 1:
                raise ValueError('slot machine reels with different angles')
            angle = wrap16(angles.pop())
            meshes = []
            for p in pointers[:SLOT_MODELS]:
                mode, faces, addresses = list_faces(p)
                kept = [f for f in faces if not int(f['head'][2]) & 0x8000]
                if len(faces)-len(kept):
                    dropped.append(dict(list='%x' % p, faces=len(faces)-len(kept), why='screen-checker glass (header 8000)'))
                meshes.append((mode, placed(kept, (0, 0, 0), angle), addresses))
            for k, base in enumerate(SLOT_CODE):
                offset = tuple(rom.f32(base+4+8*i) for i in range(3))
                specials.append(dict(cell=a['cell'], near=0, kind=KIND_SLOT+k, meshes=meshes if k == 0 else 'previous',
                                     record=a['record'], instances=[(SLOT_MODELS-1, OBJECT_PLACED | OBJECT_MESHES,
                                                                     placement(offset, 0))]))
            continue
        elif is_expert(a):
            # Expert's actors (include/expert_actors.h): one instance of kind CLOCK a group as the frame-counter
            # animations; a PLACED group's mesh in model coordinates, its instance PLACED at the actor's position (the
            # tick's placement replaces it). Its lumas are evaluated at run time: every luma from its ambient up has a
            # binding.
            for group in map(expert_split, expert_groups(rom, a)):
                mode, frames = clock_frames(lists, list_faces, group)
                faces = clock_base(frames)[0]
                addresses = [address for p in group['frames'][0] if p in lists for address in list_faces(p)[2]]
                for frame in frames:
                    for face in frame:
                        state = clock_state(face)
                        if group['placed']:
                            for luma in range(int(float(face['tail'][1])), 256):
                                clock_keys.add(state[:4]+(luma,))
                            continue
                        clock_keys.add(state[:4]+(state[4],))
                        if int(face['head'][1]) & (1 << 17):
                            clock_keys.add(state[:4]+(state[5],))
                flags, matrix = (OBJECT_PLACED, placement((a['x'], a['y'], a['z']), 0)) if group['placed'] else (0, None)
                info = dict(handler=a['handler'], record=a['record'], cells=group['cells'], near=group['near'],
                            cond=group['cond'], shift=group['shift'], window=list(group['window']), variant=group['variant'],
                            frames=group['frames'], ops=group['ops'], placed=group['placed'],
                            position=[a['x'], a['y'], a['z']], heading=a['heading'])
                if 'actor' in group:
                    info['actor'] = dict(group['actor'], position=[a['x'], a['y'], a['z']], heading=a['heading'], cell=a['cell'])
                specials.append(dict(cell=group['cells'][0], near=group['near'], kind=KIND_CLOCK,
                                     meshes=[(mode, faces, addresses)], record=a['record'],
                                     instances=[(group['variant'], flags, matrix)], clock=info))
            continue
        elif h in CLOCK_ACTORS:
            # A frame-counter animation (include/clock_actors.h): one instance of kind CLOCK a group, its base mesh
            # (frame 0, every position a later frame tells apart kept apart) in world coordinates; the frames are
            # the clock subcommand's DNA1 file. Every texture header and luma a frame shows needs a binding.
            for group in clock_groups(rom, a):
                mode, frames = clock_frames(lists, list_faces, group)
                faces = clock_base(frames)[0]
                addresses = [address for p in group['frames'][0] if p in lists for address in list_faces(p)[2]]
                for frame in frames:
                    for face in frame:
                        state = clock_state(face)
                        clock_keys.add(state[:4]+(state[4],))
                        if int(face['head'][1]) & (1 << 17):
                            clock_keys.add(state[:4]+(state[5],))
                specials.append(dict(cell=group['cells'][0], near=group['near'], kind=KIND_CLOCK,
                                     meshes=[(mode, faces, addresses)], record=a['record'],
                                     instances=[(group['variant'], 0, None)],
                                     clock=dict(handler=a['handler'], record=a['record'], cells=group['cells'],
                                                near=group['near'], cond=group['cond'], shift=group['shift'],
                                                window=list(group['window']), variant=group['variant'],
                                                frames=[[p for p in frame] for frame in group['frames']])))
            continue
        else:
            if a['class'] != '-':
                skipped.append(a)
            continue
        for kind, variant, pointer, position, heading in members:
            if pointer not in lists:   # a list without a drawable face (blank panel models)
                continue
            mode, faces, addresses = list_faces(pointer)
            heading, roll = heading if isinstance(heading, tuple) else (heading, 0)
            if position != (0, 0, 0) or heading or roll:
                faces = placed(faces, position, heading, roll)
            g = groups.setdefault((a['cell'], a['bitmap'], kind, variant), [mode, [], [], []])
            if g[0] != mode:
                raise ValueError('group mixes lighting modes')
            g[1] += faces
            g[2] += addresses
            g[3].append(a['record'])
    manifest = json.loads((args.cells/'manifest.json').read_text())
    base_meshes, base_instances = manifest['meshes'], manifest['instances']
    cell_rows = [l for l in (args.cells/'instances.txt').read_text().splitlines()]
    context = [l for l in cell_rows if not l.startswith('#')][0].split()[2:]
    args.output.mkdir()
    for name in ('cells.txt',):
        shutil.copy(args.cells/name, args.output/name)
    for m in range(base_meshes):
        shutil.copy(args.cells/('mesh_%d.txt' % m), args.output/('mesh_%d.txt' % m))
    objects = ['# instance cell bitmap(0 draw, 1 near) kind variant flags ; source addresses ; actor records ; placement '
               '(12 floats, column-major, PLACED only)']
    faces_added = vertices_added = 0
    mesh_count = 0
    rows = []   # (sort key, instance row text, objects.txt fields)

    def emit_mesh(mode, faces, addresses):
        nonlocal mesh_count, faces_added, vertices_added
        text, corners = mesh_text(mode, faces)
        (args.output/('mesh_%d.txt' % (base_meshes+mesh_count))).write_text(text)
        faces_added += len(faces)
        vertices_added += corners
        mesh_count += 1
        return base_meshes+mesh_count-1
    for key, (mode, faces, addresses, records) in sorted(groups.items()):
        mesh = emit_mesh(mode, faces, addresses)
        rows.append(((key[0], key[1] == 'near', key[2], key[3]), mesh, addresses[0],
                     (key[0], key[1] == 'near', key[2], key[3], 0, addresses, records, None, None)))
    shared = None
    clock = []
    for sp in specials:
        if sp['meshes'] == 'previous':
            first, addresses = shared
        else:
            first = None
            for mode, faces, mesh_addresses in sp['meshes']:
                mesh = emit_mesh(mode, faces, mesh_addresses)
                first = mesh if first is None else first
            addresses = [a for _, _, m in sp['meshes'] for a in m]
            shared = (first, addresses)
        if 'clock' in sp:
            clock.append(dict(sp['clock'], mesh=first))
        for variant, flags, matrix in sp['instances']:
            if flags & OBJECT_MESHES:
                variant = (len(sp['meshes']) if sp['meshes'] != 'previous' else variant+1)-1
            rows.append(((sp['cell'], sp['near'], sp['kind'], variant), first, addresses[0],
                         (sp['cell'], sp['near'], sp['kind'], variant, flags, addresses, [sp['record']], matrix,
                          None)))
    # selection runs (meshpack.h): one cell, near flag and kind together, variants ascending
    rows.sort(key=lambda r: r[0])
    for n, (_, mesh, address, (cell, near, kind, variant, flags, addresses, records, matrix, params)) in enumerate(rows):
        cell_rows.append(' '.join([str(mesh), str(address)]+context))
        objects.append('%d %d %d %d %d %d ; %s ; %s ; %s' % (
            base_instances+n, cell, near, kind, variant, flags, ' '.join('%x' % v for v in addresses),
            ' '.join('%x' % v for v in records), ' '.join('%.9g' % v for v in matrix) if matrix is not None else '-')
            + (' ; %x %x %x' % params if params else ''))
    (args.output/'instances.txt').write_text('\n'.join(cell_rows)+'\n')
    (args.output/'objects.txt').write_text('\n'.join(objects)+'\n')
    manifest.update(meshes=base_meshes+mesh_count, instances=base_instances+len(rows),
                    unique_faces=manifest['unique_faces']+faces_added, face_instances=manifest['face_instances']+faces_added,
                    unindexed_vertices=manifest['unindexed_vertices']+vertices_added,
                    coverage=manifest['coverage']+'; plus world objects of the actor table (prepare_world_objects.py)')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    shutil.copy(args.objects/'solid_materials.txt', args.output/'object_solid_materials.txt')
    if clock:
        (args.output/'clock.json').write_text(json.dumps(dict(course=args.course, groups=clock), indent=1)+'\n')
        (args.output/'clock_keys.txt').write_text(''.join('%d %d %d %d %d\n' % k for k in sorted(clock_keys)))
    glass = sum(d['faces'] for d in dropped)
    print(json.dumps(dict(groups=len(groups), meshes=mesh_count, instances=len(rows), faces=faces_added, corners=vertices_added,
                          dropped_faces=dict(count=glass, lists=len(dropped), why=sorted({d['why'] for d in dropped})),
                          not_drawn=[dict(record='%x' % a['record'], handler='%x' % a['handler'], kind=a['class'], what=a['what'])
                                     for a in skipped]), indent=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('lists', 'actors', 'sources', 'glass', 'clock'):
        p = sub.add_parser(name)
        p.add_argument('images', type=Path)
        p.add_argument('--course', type=int, default=0)
        if name == 'lists':
            p.add_argument('output', type=Path)
            p.add_argument('--extra-models', type=Path, help='hex model addresses seen in a display-list log: their lists are added')
        if name == 'sources':
            p.add_argument('objects', type=Path)
            p.add_argument('cells', type=Path)
            p.add_argument('output', type=Path)
        if name == 'glass':
            p.add_argument('objects', type=Path)
            p.add_argument('output', type=Path, help='NEW header: slot_glass_data.h')
        if name == 'clock':
            p.add_argument('objects', type=Path)
            p.add_argument('sources', type=Path, help='the sources directory with clock.json')
            p.add_argument('pack', type=Path, help='the final world pack (DNM6) of those sources')
            p.add_argument('output', type=Path, help='NEW DNA1 file')
            p.add_argument('--expected', type=Path, metavar='NEW_FILE',
                           help='also every frame as the export shows it, in the pack\'s order (CKX1: tests/test_clock.c)')
            p.add_argument('--max-error', type=float, default=0.07,
                           help='largest interpolation error (units) a reduced position rate may have (default 0.07)')
    args = parser.parse_args()
    rom = Rom(args.images)
    table = actors(rom, args.course)
    if args.command == 'actors':
        for a in table:
            print(f"{a['record']:06x} list {a['list_cell']:3d} cell {a['cell']:3d} ({a['x']:9.3f},{a['y']:7.3f},{a['z']:9.3f}) heading {a['heading']:6d} id {a['id']:2d} "
                  f"handler {a['handler']:06x} arg {a['argument']:07x} class {a['class']} {a['bitmap'] or '-':4s} {a['what']}")
        return
    if args.command == 'lists':
        lists = []
        for a in table:
            lists += actor_lists(rom, a, args.course)
        if args.extra_models:
            for word in args.extra_models.read_text().split():
                key = struct.pack('<I', int(word, 16))
                at = rom.data.find(key, 0x880000, 0x8a0000)
                while at >= 0:
                    if at % 4 == 0:
                        lists.append(0x2000000+at)
                    at = rom.data.find(key, at+1, 0x8a0000)
        seen = []
        for pointer in lists:
            if pointer and pointer not in seen:
                seen.append(pointer)
        with args.output.open('x') as out:
            out.write('\n'.join('%x' % p for p in seen)+'\n')
        print(len(seen), 'lists')
        return
    if args.command == 'glass':
        glass(rom, args)
        return
    if args.command == 'clock':
        clock_dna(rom, args)
        return
    build(rom, table, args)


if __name__ == '__main__':
    main()
