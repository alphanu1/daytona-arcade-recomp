# Gamepad steering: how the arcade wheel behaves, and what the "Gamepad steering fix" does

This is the research behind the **Gamepad steering fix** option (Launcher > Controls). Everything below was
measured on the Revision A ROM set (`daytona.zip`, `M2_ROMSET=daytona`) with temporary logging that is not part of
this change. Addresses and thresholds on other revisions have not been checked.

## 1. The arcade's "heavy wheel at speed" is the drive board, not the game's steering

The cabinet's force feedback drive board (838-10646) is commanded through dual-port RAM byte 0x11 (already modelled
in `src/runtime/drive_board.h`). Over three tracks (about 8,600 frames, up to 303 km/h) the commands the game sent
in a race were logged next to the car's speed (HUD km/h, from the word at `0x505198`: about `1.055 * x - 14.5`):

| Speed (km/h) | Command | Effect | Relative strength `(7 + 2n) / 31` |
|---|---|---|---|
| below about 50 | `0x2-` | resistance (a brake), starts and stops | |
| from about 53 | `0x38` | centring spring, with dead zone, n = 0 | 0.23 |
| from about 119 | `0x39` | same, n = 1 | 0.29 |
| from about 187 | `0x3a` | same, n = 2 | 0.35 |
| from about 255 | `0x3b` | same, n = 3 | 0.42 |

The step down happens 2 to 10 km/h lower than the step up (a little hysteresis). So the wheel gets heavier in four
steps with speed, and at the top it is only about 1.85 times heavier than at the first step.

Other commands seen: `0x5-` / `0x6-` (constant force one way / the other: the wheel pulled in a corner), `0x4-`
(uncentring: pushed away from the centre, when the car slides) and a plain centring spring `0x32` for a moment after a
crash or slide.

## 2. The game turns the car from the wheel position as it is

A snapshot of the game's RAM (`0x200000-0x220000` and `0x500000-0x510000`, 599 snapshots, 88 distinct steering
values) was compared with the steering input:

| Address | Meaning |
|---|---|
| `0x501000`, `0x501020` | the steering ADC as sent (0x20 to 0xe0, centre 0x80) |
| `0x501050`, `0x50105c` | the signed wheel position, -127 to +127 (about `(ADC - 128) * 1.52`) |
| `0x50523c` | the 16-bit steering value: the wheel position times 256, exactly (-32512 to +32512), no frame of delay |
| `0x505198` | the car's speed |

`0x50523c / 0x501050` is 256 at every speed from 0 to 330 km/h. The game applies **no speed-dependent scaling to the
wheel position**. What the player feels at speed is the spring limiting how far a person turns the wheel. (The car's own
response, tyre angle and yaw, was not identified: `0x5053cc` correlates with steering at -0.84, but there was too little
clean data to say what it is.)

## 3. What the option does

A pad cannot push back, so the option imitates the limit instead.

**D-pad (and keys).** The D-pad is a steady push against the spring the game commands (`rt::DriveBoard`, read each
frame in `main.cpp`). The lock a held push balances at is `k0 / k` of the full lock, with `k0 = 7/31` the lightest
spring: 100% at the first step, then about 78%, 64% and 54%. Lock is gained more slowly as the spring stiffens and the
wheel comes back to centre faster. A tap gives a partial lock because the wheel needs a few frames to move.

**Pad stick.** Same idea for an analogue stick: most of the travel reaches only `stick_range` of the lock, the last 15%
opens up to full lock, and the lock shrinks a little with speed (`1 / (1 + 0.45 v^2)`, v = km/h / 215, square-rooted).
A stick that only ever reports 0 and +-1 (some handhelds) is treated like the D-pad.

**Not changed.** Wheels (joystick axes, including `Wheel steering sensitivity`), the touch screen and the menus keep
their direct mapping. With the tick off the race steering is exactly as before.

Settings (Launcher > Controls, saved in `launcher.ini`): `gamepad_fix` (default off), `wheel_weight`, `wheel_turnin`,
`wheel_return`, `stick_range`, `stick_expo`.
