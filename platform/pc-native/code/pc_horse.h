/* The Saturn version's secret horse on the PC (platform/pc-native/code/pc_horse.c).
 *
 * Saturn Daytona USA (APROG.BIN, loaded at 06003000), what this reproduces:
 *   0601d3c0  at the title, START with Up+Left+A+B+X+Z held (word 5d50) unlocks the horse (0605ab16/7 |= 7):
 *             the car select (060104e0) then offers entries 10 (automatic) and 11 (manual), entry & 1 the
 *             transmission. Here the same chord toggles horse mode (saved): Up+Left on the D-pad, A+B+X+Y.
 *   060039c8  the horse's palette by transmission (table 0605ca4c): automatic brown (0605c9cc), manual grey
 *             (0605c9ec). Same model, same gallop for both.
 *   06034754  the gallop: phase += speed / 300 (16.16; speed the car's +8, km/h) each tick, back to 0 at 6.0,
 *             the frame phase >> 16 of six models (0600b340 draws them over a base model). The arcade's horse
 *             has 32 frames: the stride rate is kept (the Saturn game ticks at 20 Hz), not the frame count.
 *   06034848  a hoof sound when the frame changes: frame 3, 4, 0 (and 5 below 260 km/h) play the sound
 *             commands ae113bff, ae113cff, ae113dff (effects 3b, 3c, 3d of bank 11: one drum note each).
 *   0602ed18  the car's tables by entry: horse 1's torque curve is car 2/3's (0604633c automatic, 060461cc
 *             manual; the Hornet's 0604605c equals the arcade's drive.dnd curve to 1/65536), its steering gains
 *             the Hornet's, its grip floor bytes +7e/+7f 40/20 (the Hornet 07/05: the lateral force a sliding
 *             tyre keeps, 0602d08a), its wall speed factor 0.75 (the Hornet 0.87, 06030dfe).
 * Gameplay changes are the player's only (a copy of the drive parameters, opponents keep the arcade's). */
#ifndef DAYTONA_PC_HORSE_H
#define DAYTONA_PC_HORSE_H
#include <stdint.h>
#include "drive.h"

#ifdef __cplusplus
extern "C" {
#endif
/* The saved toggle. */
int pc_horse_enabled(void);
/* START pressed this frame with the chord held: toggle horse mode. 1 when it toggled. buttons: enDjinn's raw
 * button word (two bits a button: A, B, X, Y, UP, DOWN, LEFT, RIGHT, START). */
int pc_horse_title_code(uint32_t buttons);
/* A race starts: horse mode taken for it (or not), transmission 0 automatic (brown) or 1 manual (grey). */
void pc_horse_race_begin(unsigned transmission);
/* Up+Left held on the D-pad: the chord is being entered, so A is not the title's start. */
int pc_horse_chord_held(uint32_t buttons);
/* The attract: the arcade's car. */
void pc_horse_race_end(void);
/* The race's player rides the horse. */
int pc_horse_riding(void);
unsigned pc_horse_transmission(void);
/* One race tick of the gallop at the player's speed (km/h, the speed index): the phase, the frame, the hoof
 * sounds. */
void pc_horse_tick(int32_t speed_kmh);
/* The gallop frame drawn (0..31 of car-horse.dnb). */
unsigned pc_horse_frame(void);
/* The player's drive parameters: base itself, or while riding the horse's copy of it. */
const nm_drive_params_t *pc_horse_player_math(const nm_drive_params_t *base);
/* The wall-impact speed factor of 0x8000-sided hits (include/boundary.h): the arcade's 0.8, the horse's that
 * scaled by the Saturn's ratio 0.75 / 0.87 when p is the horse's parameters. */
float pc_horse_wall_factor(const nm_drive_params_t *p);
#ifdef __cplusplus
}
#endif
#endif
