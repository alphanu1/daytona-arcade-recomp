#ifndef DAYTONA_NATIVE_PALETTE_BANKS_H
#define DAYTONA_NATIVE_PALETTE_BANKS_H
/* Palette RAM of a race scene: 1024 entries = 64 banks of 16, ARGB8888 or,
 * with make NATIVE_PAL16=1555, ARGB1555 (palette_ram.h: one format for every
 * bank, set once at startup; the packs keep ARGB8888 source colours).
 *   banks  0..1   finish actor (written at scene load)
 *   banks  2..59  cars and scenery while the 3D scene is drawn: palette i of
 *                 a DNT6 car pack is bank NM_CAR_PALETTE_BANK_FIRST+i; a
 *                 palette of a DNT6 world pack uses the bank of the equal car
 *                 palette, else the next free bank after the car's
 *                 (material_pack.h, nm_material_palette_share); all written
 *                 at scene load, none per frame;
 *                 the podium's static screen from its timer 383 (after the
 *                 cleanup) and result glyph materials from the result
 *                 hand-off (phase 25) until the scene is released: neither
 *                 cars nor scenery are drawn in between
 *                 (render/ui/podium_render.h; the results own the screen)
 *   banks 60..61  the podium's banner and photo (written on the cinematic's
 *                 first frame), then the result labels (written at the result
 *                 hand-off)
 *   banks 60..63  HUD (written at scene load, render/ui/hud_render.c; tools/prepare_hud.py --first-bank 60): the
 *                 HUD is drawn in phases 20..24 only, before the podium's cinematic and the result labels take
 *                 60..61, and writes its banks again at the next scene load
 * A DNT4 car pack uses no palette; the result palettes are still written at
 * the hand-off. Host tools read this header too: constants only.
 * Budget of course 0 (tools/prepare_palette_materials.py --share): the cars
 * use 54 banks (3 saturating buckets per channel); the complete scenery
 * wants 21 palettes of which 15 are car palettes; 4 are added and the two
 * rarest (16 faces) take their nearest allowed neighbour: 58 of 58 in use. */
#define NM_CAR_PALETTE_BANK_FIRST 2u
#define NM_CAR_PALETTE_BANKS 58u
/* Opaque car polygons set "ignore texture alpha": entry 15 of a palette shared
 * with the punch-through list has alpha 0 and must still draw in the opaque
 * list. Opaque blending (ONE, ZERO) does not read alpha either way; the bit
 * makes that independent of it. Set to 0 to submit texture alpha as before
 * (then a split pack, prepare_palette_materials.py --split-palettes, is the
 * alternative if entry 15 shows wrong in opaque polygons on hardware). */
#define NM_CAR_PALETTE_OPAQUE_IGNORES_ALPHA 1
#endif
