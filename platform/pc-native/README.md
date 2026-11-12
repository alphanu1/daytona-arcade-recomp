# Daytona USA for PC, native (Daytona USA Native Ultimate)

**A real native port, not a recompilation.** This is the Dreamcast-native source port
(`platform/dreamcast-native`) built for the PC: the game written again in C from the arcade program,
system by system (driving model, AI, race, cameras, HUD, attract, select, results), and checked against
the original tick for tick; on SDL3, Vulkan and SDL's gamepads (XInput on Windows) instead of
KallistiOS and the Dreamcast. Nothing of the arcade's i960 or TGP code runs here, and nothing is
emulated or interpreted: the repository's main build (the static recompilation of the arcade program)
is a different program. The arcade's sound board program is the one exception by design: the music and
effects are the ROM set's own, played by the recomp's native sound engine.

On top of the arcade game it carries the Saturn versions' extras (the Daytona USA Ultimate idea): the
Saturn horse, Circuit Edition's National Park Speedway and Desert City, Circuit Edition's menus and
pause in the original Saturn game's style, the Revision A start camera, the whole course drawn, and the
US region's English screens.

No game data is in this repository: everything the game shows or plays is made from your own copies,
by the recipes below, into `build/` (ignored by git). Never distribute what they make.

## What runs where

| Dreamcast | PC |
| --- | --- |
| KallistiOS, enDjinn | enDjinn's core unchanged; KOS replaced by `backend/` (SDL3) and `include/` (KOS's own PVR headers, adapted) |
| PowerVR2 Tile Accelerator | `backend/host_pvr.cpp` reads the real TA parameters the game writes (`pvr_dr_target`/`pvr_dr_commit`, headers compiled by KOS's `pvr_prim.c`) and `backend/pc_endjinn_vulkan.cpp` draws them with Vulkan |
| 8 MiB VRAM, palette RAM, `FPU_SHAD_SCALE` | an emulated VRAM (16 MiB-aligned, so KOS's texture addresses work unchanged) and register file |
| AICA + AICAflow recordings | the arcade sound board's own program and MultiPCM samples through the recomp's `NativeSoundEngine` (`audio/pc_sound.cpp`): music, effects, engine and announcer from the ROM set, no vgmrips pack |
| Maple pad | SDL3 gamepads (XInput on Windows) and the keyboard (`backend/host_input.cpp`) |
| 59.94 Hz vertical blank | a 59.94 Hz frame clock (`backend/enj_platform_pc_endjinn.cpp`), so the game runs at arcade speed on any display |

Rendering keeps the PVR's rules: depth is the game's 1/w with the comparison
the header asks for (stored as 1/w · 2⁻¹⁶ in a 32-bit float buffer, an exact
scale), colours and UVs interpolate perspective-correctly, and each polygon's
TSP word decides texture environment, alpha use, UV clamp/flip, filtering,
mip bias and (translucent list) blend factors. The road receives the cars'
cheap shadows through the modifier-volume stencil path.

## What you need

| Game | Version | Needed for |
| --- | --- | --- |
| Daytona USA (arcade, Sega Model 2) | MAME's `daytona93` set: Daytona USA Deluxe '93 (either layout of its sound program: epr-16720/16721 or epr-16489/16490) | **required**: the game itself, its graphics, sound and music |
| Daytona USA (arcade) | MAME's `daytona` set: Revision A (1994) | optional: Revision A's race-start camera (`DAYTONA_REVA_ROM`); without it the start keeps Deluxe '93's camera |
| Daytona USA (Sega Saturn, 1995) | the original release's disc (see the files below) | optional: the horse's hoof sounds and handling, the Saturn-style menus and pause (`DAYTONA_SATURN_FILES`) |
| Daytona USA Circuit Edition (Sega Saturn, 1996) | the Japanese release, GS-9100 | optional: National Park Speedway and Desert City (`DAYTONA_CCE_DIR`) |
| [AJBats/daytona-cce95](https://github.com/AJBats/daytona-cce95) (Daytona USA Ultimate, public domain) | a checkout (tested at `121d9a3`) | with Circuit Edition: its `tools/col_generate.py` rebuilds the courses' ground (`DAYTONA_CCE_REPO`) |

The Saturn files these tools were made and tested with (SHA-1; other releases of the 1995 disc are
untested, and the tools check what they read):

    23b097fd6b7d5bcee6505f124264096cfd9ca4b3  APROG.BIN          (Daytona USA)
    f87b9d390450e81327feb3dad34f1748c710e662  SCROLL.BIN
    6b958fa411620b3f63c1e39e72dbe8c1793ec169  SOUNDS.BIN
    5b20809104ee78b0cff1805089446b4e52b286d3  GAMED.BIN
    110ee4701c78c169f0c36a2e8eddd907de9637a6  DAYTONA/COURSE3.MDL (Circuit Edition)
    7ac5c16e2bd6e613a758122a453fefa81401f47f  DAYTONA/COURSE3.TEX
    26283268846cb48a021ef3f8bae92a8593e30456  DAYTONA/COURSE4.MDL
    2a45cfac8c63855c3036e47aa875cc4060e157dc  DAYTONA/COURSE4.TEX
    e218099414693c8e91dfbea820375b3b826f5b82  DAYTONA/CS3_COL.BIN
    d1aef28e9cc938e387a7a799377b9624a2c3f0a5  DAYTONA/CS4_COL.BIN
    215a82d5ed6cf35c8186235a207c7a40e66c43fc  DAYTONA/CS3_BLK.BIN
    dc9792470612ed384ea6aafbf8139bbacb942a39  DAYTONA/CS4_BLK.BIN

## Dumping your games

- **The arcade sets**: from your own boards, as MAME-format `.zip` sets named after the set
  (`daytona93.zip`, `daytona.zip`); the recipes check every file's CRC. Put them in `roms/` as the
  repository's main README says.
- **The Saturn discs**: rip your own discs to `.cue`/`.bin` with a ripper that corrects the drive's read
  offset (redumper, DiscImageCreator), then take the files out of the data track (track 01). For example,
  on Linux:

      bchunk "Daytona USA (Track 01).bin" "Daytona USA.cue" dusa     # -> dusa01.iso
      7z x dusa01.iso -obuild/saturn/dusa                             # APROG.BIN, SCROLL.BIN, ... at the root
      bchunk "Daytona USA - Circuit Edition (Track 01).bin" "Daytona USA - Circuit Edition.cue" cce
      7z x cce01.iso -obuild/saturn/cce                               # its DAYTONA/ folder

  (Any tool that reads an ISO 9660 track works: mounting it, 7-Zip, PowerISO on Windows.) Keep the files
  under `build/`.
- **Circuit Edition's ground generator**: `git clone https://github.com/AJBats/daytona-cce95 build/daytona-cce95`.

## Build

0. The pinned libraries the port compiles (into `extern/`, ignored by git):

       sh platform/dreamcast-native/scripts/fetch_endjinn.sh
       sh platform/dreamcast-native/scripts/fetch_sh4zam.sh

   (`fetch_sh4zam.sh` also builds SH4ZAM for the Dreamcast when KallistiOS is installed; the PC build only
   needs its sources.)
1. The host build of this repository (`./setup.sh`), then the asset recipe on your own sets and discs
   (each `DAYTONA_*` is optional; leave one out and its feature is left out):

       DAYTONA_SATURN_FILES=build/saturn/dusa \
       DAYTONA_CCE_DIR=build/saturn/cce/DAYTONA DAYTONA_CCE_REPO=build/daytona-cce95 \
       DAYTONA_REVA_ROM=roms/daytona.zip \
       zsh platform/pc-native/recipes/build_pc_assets.sh roms/daytona93.zip build/my-assets

   It runs `platform/dreamcast-native/recipes/build_assets.sh --no-music` (the asset recipe of the
   Dreamcast-native port, with its lockstep checks against the recompiled arcade program; it needs the
   KallistiOS/enDjinn tools that recipe lists, for `pvrtex`), copies the sound board images into the
   asset set, makes the Saturn and Circuit Edition extras and Revision A's start camera from the files
   given, and records the game's build options for CMake. It takes a while (the arcade traces).
   `DAYTONA_COUNTRY=usa|jpn|export` picks the arcade's region (usa by default: English screens, mph).
2. Build (SDL3, the Vulkan loader and headers, `glslc`, zlib, Clang or GCC; both checked on Linux):

       cmake -S platform/pc-native -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAYTONA_ASSETS=build/my-assets
       cmake --build build/pc

3. Run:

       build/pc/daytona build/my-assets/assets-ssfar4f

   Or copy the asset set next to the executable as `assets/`, or set
   `DAYTONA_ASSETS`.

## Controls

| Pad (Xbox layout) | Keyboard | Arcade |
| --- | --- | --- |
| Left stick | Left / Right | Steering |
| RT / LT | Up / Down | Gas / brake |
| A / X | A / Z | Shift up / shift down (X held at the start: manual) |
| D-pad | 1 2 3 4 | VR1..VR4 views |
| Y | V | Next view |
| Start | Enter | Start, credit |
| B | Backspace | B |

Esc quits; F11 or Alt+Enter toggles fullscreen. Start with both triggers held
opens the operator settings, as on the Dreamcast.

Steering (`backend/pc_steering.cpp`): the arcade turns the car from the wheel
position as it is; its wheel feels heavy at speed because the drive board's
centring spring stiffens in four steps. A **pad stick** therefore drives a
virtual wheel: most of the stick's travel reaches part of the lock, the last
15% opens to full lock, and both the lock and how fast the wheel turns in fall
with speed (the model of `docs/gamepad-steering.md`). **Keys** push against the
spring for the current speed. A **steering wheel** goes to the game directly,
unshaped, as the cabinet's potentiometer did.

Wheels: any joystick SDL reports as a wheel, or that is not a gamepad. The
pedals calibrate themselves from where they rest; axis and button numbers
default to the Logitech G29/G920/G923 layout and are printed on connect.

Environment:

| Variable | Default | |
| --- | --- | --- |
| `DAYTONA_PAD_STEER` | | `direct`: the stick straight to the wheel, no model |
| `DAYTONA_STICK_RANGE` | 0.6 | share of the lock normal stick travel reaches (0.4..1) |
| `DAYTONA_STICK_EXPO` | 2.6 | stick curve, 1 linear (1..4) |
| `DAYTONA_STICK_DEADZONE` | 0.08 | |
| `DAYTONA_STEER_INVERT` | | `1` reverses steering |
| `DAYTONA_WHEEL_SENSITIVITY` | 1.0 | wheel response, 1 linear (0.25..3) |
| `DAYTONA_WHEEL_STEER` / `_GAS` / `_BRAKE` | 0 / 2 / 3 | wheel axis numbers |
| `DAYTONA_WHEEL_UP` / `_DOWN` / `_START` / `_VIEW` | 4 / 5 / 9 / 3 | wheel button numbers |
| `DAYTONA_VOLUME`, `DAYTONA_MUSIC_VOLUME`, `DAYTONA_SFX_VOLUME` | 0.8, 1, 1 | |
| `DAYTONA_VSYNC` | | `1`: FIFO presentation instead of mailbox |
| `DAYTONA_DRAW_DISTANCE` | 3 | scenery draw distance: -2 the camera's cell only, -1 one cell around, 0 the game's own, +1 the whole 5x5 the game chooses from, +2 a 7x7 (the desktop build's slider), +3 every cell of the course (presentation only: opponents, objects' logic and the checked gameplay keep the game's cells) |
| `DAYTONA_CAR_DETAIL` | | `arcade`: the opponents' models by distance as the arcade chooses them; by default every drawn opponent uses its full, nearest model (presentation only) |
| `DAYTONA_DRAW_STATS` | | `1`: print the game's and the drawn cell counts now and then |
| `DAYTONA_AUTOPLAY` / `DAYTONA_AUTOPLAY_COURSE` | | testing without hands: `1` presses Start through the selects, then holds the gas; the course select's cursor starts on slot N (0 Beginner, 1 Advanced, 2 Expert, 3 National Park, 4 Desert City) |
| `DAYTONA_AUTOPLAY_KEYS` / `DAYTONA_MENU_SCREEN` | | with `DAYTONA_AUTOPLAY=1`: `ms:BUTTON,...` presses (300 ms each) in place of the Start pulses; the Saturn menu opening on `options`, `sound` or `records` |
| `DAYTONA_ROLLING_START` | `reva` | Revision A's race-start camera (when the recipe made it from your `daytona` set: `DAYTONA_REVA_ROM`): the Beginner's rolling start flies around the Hornet instead of the opponents, Expert and Advanced open on Revision A's wide shots. Presentation only: the race stays Deluxe '93's. `93`: Deluxe '93's own camera |

## The Saturn menus

With `DAYTONA_SATURN_FILES` set, the recipe also makes the original Saturn Daytona USA's front-end art
(`tools/prepare_saturn_menus.py`, from its SCROLL.BIN and APROG.BIN) and the game then runs Circuit Edition's
menus in the original Saturn game's look (`code/pc_saturn_menu.c`):

- after the attract, or START in it: the arcade's own title, its prompt PRESS START BUTTON in the arcade's font (as
  the attract's line, which the PC build shows in place of INSERT COIN(S));
- START: the main menu, Circuit Edition's items on the original's Mode Select screen: ARCADE (the course and
  transmission select, then the race), TIME ATTACK, 2PLAYER BATTLE and LINK BATTLE (greyed: not in this port yet),
  RECORDS (each course's records), OPTIONS; B goes back to the title;
- OPTIONS, Circuit Edition's rows in the original's Options style: HANDLING (SLOW / NORMAL / QUICK: how fast the
  pad's or the keys' steering turns in, Circuit Edition's 1/22, 1/16, 1/10), GAME MODE (NORMAL, GRANDPRIX 2.5 times
  the laps, ENDURANCE 10 times, with Circuit Edition's longer starting times), DIFFICULTY (EASY / NORMAL / HARD: the
  arcade's own setting), SOUND (STEREO / MONAURAL, BGM VOLUME, a song per course of the arcade's four, BGM TEST),
  EXIT; greyed for now: the second player's handling, the time compare, section lap and ghost rows, KEY CONFIG.
  Kept in the save folder (`options.cfg`);
- in a race, a tap of START (pressed and released alone: START + direction and START + B stay the view and HUD
  chords) pauses: Circuit Edition's EXIT ? YES / NO / RESTART, the cursor on NO, START or A to choose. YES goes back
  to the main menu, RESTART runs the race again.

Without the Saturn files the arcade's own title and select run as before.

## The Circuit Edition courses

National Park Speedway and Desert City, the two courses Daytona USA Circuit Edition (Saturn, 1996) added, race
as course slots 3 and 4 after the arcade's three, converted from your Circuit Edition disc by
`tools/prepare_cce_course.py` (the recipe runs it with `DAYTONA_CCE_DIR` and `DAYTONA_CCE_REPO`): the course's
models and textures, its ground (through the CCE repository's `tools/col_generate.py`), the road and the routes
from its course map. Both have a standing start with 20 cars, as Advanced (the arcade's tables the race needs and
the courses lack are Advanced's: start camera, pit, the opponents' height pairs); National Park runs 6 laps, Desert
City 4.

- The course select shows all five courses in one row: the arcade's panels and maps at 5/8 and 90/112 of their
  size, and the new two's in the arcade's style (`tools/prepare_select_cce.py`: the panels from BEGINNER's and
  EXPERT's, "NATIONAL / PARK" and "DESERT / CITY" in their fonts; the maps the course from above, the road the
  arcade maps' way).
- The race's course map (the HUD's) is drawn from the course's centre line, laid out as Advanced's
  (`prepare_hud.py --generated-map`, the recipe's `--hud-map`).
- Not yet: the courses' own skies (they use Advanced's), trackside objects, the podium, positional calls, saved
  records.

## The Saturn horse

The Saturn version's secret: at the title (or during the attract), hold **Up+Left on the D-pad and A+B+X+Y**
and press **Start** (keyboard: 1+4, A+Backspace+Z+V, Enter). It toggles horse mode, kept in the save folder
(`horse.cfg`). In horse mode the race's player rides the arcade's own Expert-course horse: the automatic
transmission the brown horse, the manual the grey, as on the Saturn. What it does is decoded from the Saturn
program (`code/pc_horse.h`):

- the gallop: the Saturn's speed-paced stride (phase += speed / 300 a tick, six frames) on the arcade horse's
  32 frames, with the Saturn's hoof sounds on its frames (the disc's own samples, `tools/extract_saturn_horse.py`);
- the handling: the Saturn horse's torque curve (about 1.5 times the Hornet's; the Saturn's Hornet curve is the
  arcade's value for value), its higher grip floor, the Hornet's steering, and the Saturn's harder wall hits
  (0.75 of the speed kept against the Hornet's 0.87). The player's parameters only; the opponents are the arcade's.

The assets: `tools/prepare_horse.py` (run by `build_pc_assets.sh`) copies the horse from Expert's world pack into
the car pack; the hoof sounds and the horse's torque curves (`tools/extract_saturn_horse.py`) need
`DAYTONA_SATURN_FILES` pointing at your Saturn disc's files. Without them the horse gallops silently on the Hornet's
torque curve.

## Origin of the backend

`backend/` began as enDjinn's `backends/pc-enDjinn` and `host-common` (MIT,
SDL2). It is ported to SDL3, and its PVR layer now decodes hardware-format
parameters instead of pc-enDjinn's private header encoding, which the game's
prebuilt and bit-edited headers need.
