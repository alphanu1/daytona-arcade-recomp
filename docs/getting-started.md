# Getting started

From nothing to playing, on Windows, macOS or Linux. Setup installs what it
needs, downloads the libraries, recompiles the game from your ROM set and
builds it. You run one command; it takes a while the first time.

## What you need

1. **Your own Daytona USA ROM set**, as a `.zip` or `.7z`, one of these
   two (or both: each is built as its own game). No game data comes with
   this project: the game is built from your copy.

   - **`daytona93`**: *Daytona USA Deluxe '93*, the set MAME calls
     `daytona93`. It contains `epr-16530a.12`, `epr-16531a.13`,
     `epr-16534a.6` and `epr-16535a.7`.
   - **`daytona`**: *Daytona USA Revision A* (1994), the set MAME calls
     `daytona`. It contains `epr-16722a.12`, `epr-16723a.13`,
     `epr-16724a.6` and `epr-16725a.7`.

   Other Daytona USA sets (`daytonas`, `daytonat`, `daytonase` and so on)
   have different program ROMs and are rejected, even if you rename the
   file.

2. **A 64-bit computer** running Windows 10 or 11, macOS, or a common Linux
   distribution (Debian/Ubuntu, Fedora, Arch or openSUSE), with an internet
   connection for the first setup and a few GB of free disk space.

3. **Git**, to download the project. Setup installs everything else.

   - Windows: `winget install Git.Git` in PowerShell, or
     <https://git-scm.com>.
   - macOS: type `git` in Terminal and accept the offer to install the
     command line tools.
   - Linux: `sudo apt install git` (or your distribution's equivalent).

## 1. Download the project

Open a terminal (PowerShell on Windows, Terminal on macOS) and run:

    git clone https://github.com/alphanu1/daytona-arcade-recomp.git
    cd daytona-arcade-recomp

Stay in this folder for every command below.

## 2. Put your ROM set in place

Make a folder called `roms` inside the project and copy your set into it,
named **exactly** after the set: `daytona93.zip` for Deluxe '93,
`daytona.zip` for Revision A (or `.7z`). Both can be there.

    daytona-arcade-recomp/
        roms/
            daytona93.zip
            daytona.zip       (optional: Revision A, 1994)

macOS and Linux:

    mkdir -p roms
    cp /path/to/your/daytona93.zip roms/daytona93.zip

Windows (PowerShell):

    mkdir roms
    copy C:\path\to\your\daytona93.zip roms\daytona93.zip

Do this before setup. Without it, setup builds only the tools and there is
no game to run. The `roms` folder is never uploaded anywhere: git ignores
it.

## 3. Run setup

**macOS and Linux:**

    ./setup.sh

The name ends in `.sh`: `./setup` on its own is "no such file".

- macOS: needs Homebrew (<https://brew.sh>); setup tells you how to install
  it if it is missing. The first time, macOS may open a window offering the
  Xcode command line tools: install them, then run `./setup.sh` again.
- Linux: setup installs packages with `sudo`, so it asks for your password.

**Windows** (PowerShell):

    powershell -ExecutionPolicy Bypass -File setup.ps1

It installs Git, CMake, Ninja, Python and the Visual Studio 2022 Build
Tools, with their Clang compiler, using winget. If you already have Visual
Studio or the Build Tools, it adds the C++ and Clang tools to them: Windows
asks for permission for that (the Visual Studio Installer needs administrator
rights). The game is built with Clang (`Compiler: Clang` in the output). If
the Clang tools cannot be added, setup stops and says what to add by hand;
`setup.ps1 --msvc` builds with Microsoft's compiler instead. The Build Tools
are a large download. If it says Python "is
not on PATH yet", close PowerShell, open a new one and run the same command
again.

Setup prints each step. It is done when you see:

    == Done

    Daytona USA Deluxe '93 (daytona93) is built. Start it with:

        build/daytona

With Revision A in `roms/` too, it is built as well, beside the first:

    Daytona USA Revision A, 1994 (daytona) is built (build/daytona-rev).

    Start it with the launcher (choose your ROM set there; it opens that game):

        build/daytona

If it stops before that, see [Troubleshooting](#troubleshooting).

## 4. Play

macOS and Linux:

    build/daytona

Windows:

    build\Release\daytona.exe

(Use the exact path setup printed: it is `build\daytona.exe` if setup
used Ninja.) On Windows the command prompt comes back straight away, as for
any windowed program; the game's messages still appear in that window. To
have the window wait until the game closes, use
`start /wait build\Release\daytona.exe`. Started from Explorer or a
shortcut, the game writes its messages to `daytona.log` in its settings
folder (see Starting again from scratch for where that is).

`build/daytona` (`build\Release\daytona.exe` on Windows) is the launcher for
both games: the one file to start. Beside it are the games themselves,
`daytona-93` (Deluxe '93) and `daytona-rev` (Revision A), each with its own
settings and saves. Choose either set's ROM in the launcher: for the other
set, **Start Revision A (1994)** (or **Start Deluxe '93**) opens that game
with it. If only one set is built, the launcher says so when you choose the
other.

A launcher opens first. On the **Game** tab, click **Browse...**, choose
the same `roms/daytona93.zip` (`roms/daytona.zip` for Revision A), and
wait for the line under it to say "All 30 files verified." Then click
**Start**. The launcher remembers the file
next time. **Controls** sets your keys, gamepad and wheel. In the game,
**Esc** brings the launcher back.

**Wheel and pedals** (optional, experimental: not yet tested on a real
wheel; reports welcome): on the **Controls** tab, the **Wheel / joystick**
column takes any wheel, pedal set or shifter, even as separate
USB devices. Click the box next to **Steer left**, turn the wheel left as
far as you want full lock to be, and let go; do the same for **Steer
right**, then press each pedal fully and let go for **Accelerate** and
**Brake**. The meters at the top show the result. **Force feedback** sets
how strongly the wheel pushes back (Off turns it off); tick **Invert force**
if the wheel pulls the wrong way. **Legacy Logitech wheel support** (Linux
and macOS) runs Logitech wheels through the system's driver; the original
Driving Force needs it. It is on by default on Linux. On Linux, if such a wheel pushes on crashes but
doesn't centre on bends, tick **Centring driver override**. On macOS it is off,
because SDL's own driver gives the newer Logitech wheels force feedback
there; tick it if a Logitech wheel is listed but does nothing when you bind
it. After changing it, restart the game and bind the wheel again.

**Fullscreen mode and frame pacing** (optional, on the Game tab): the game
runs at the arcade's own speed, 57.52 frames/s, on any display. On a 60 Hz
display that means a frame shown twice about every 0.4 s, and on a 144 Hz
one a slight unevenness. **Fullscreen mode** picks an exclusive mode, such
as a 57.52 Hz mode made in your graphics driver's settings; then tick
**Smooth pacing on a 57.52 Hz display**. **VRR pacing** is for G-Sync or
FreeSync displays: the display refreshes at the game's rate. **Sync to
display** runs the game at 60 frames/s on a 60, 120 or 240 Hz display,
perfectly smooth but about 4% faster than the arcade. Hover over each for
details; the line under them says what is in effect.

**Widescreen** (optional): in the launcher, under **Enhancements**, set
**Widescreen** to 16:10, 16:9 or 21:9. You see more of the scene at the
sides, nothing is stretched, and the HUD stays 4:3 in the centre; tick **HUD
at the screen edges (Experimental)** to move the lap times, position and maps out to the
sides. In a race the sky at the sides is plain blue; tick **Stretch tile
background (Experimental)** to stretch the game's own sky picture across
the whole screen instead. All apply straight away, even mid-race.

**Draw mode** (on the Game tab): **Double buffered** draws every frame, as
the arcade game does. **Single buffered** draws every second frame and
**Every third frame** every third: much less work for slower machines. The
game itself still runs at full speed; only the picture updates less often.

**Super sampling** (on the Game tab, with **Renderer** set to **Hardware**):
**Off** by default; **2x**, **3x** or **4x** draws the 3D at that many times the original
resolution, with sharper textures; the HUD and text keep their pixel-art
look. On a screen smaller than the picture it is scaled down, which also
smooths jagged edges. Higher settings need a stronger GPU.

**Draw distance** (optional): the slider under **Enhancements** sets how far
ahead trees, rocks and buildings are drawn. **Default** is the game's own.
**Shorter** and **Shortest** draw less and run faster, which helps slower
machines; **Further** and **Furthest** draw more. The road itself is not
affected yet.

**Audio** tab: **Volume** and **Mute**, and **Music** and **Effects**
for the balance between the music and everything else (the engine, skids,
crashes). Both at 100% is the game as the arcade's sound board mixes it.

**Test menu without an F2 key** (phones, tablets): tick **Hold Test button**
next to Start, then Start or Resume. Once the game is running, the Test
button is held for 3 seconds and the test menu opens (coin settings, game
type, cabinet); the box clears itself.

Default keys: arrows to steer, accelerate and brake; 5 inserts a coin,
Enter is start; A S D F are the view buttons; 1-4 or Q/W change gear. The
full table is in the [README](../README.md#playing).

**Revision A, first run:** its factory settings are a linked twin
cabinet, so it waits at the settings screen for a second cabinet. Press
**F2** (Test), go to the game settings, set the cabinet to a single cabinet
(and the link off), and leave test mode. That is saved, once.

**Link play (Revision A, experimental):** two or more computers on the same
network (Wi-Fi or wired) race each other, like linked arcade cabinets. On
each one: Game tab, **Link to other cabinets**; **This cabinet's port** (any
free port, e.g. 15112); **Next cabinet**: the next computer's address and
port, e.g. `192.168.1.20:15112` (the cabinets form a ring: with two, each one's
next is the other). Then in test mode (F2) > GAME SYSTEM set **LINK ID** to
MASTER on one and SLAVE on the others, and a different **CAR NUMBER** on each
(the factory setting is MASTER, car 1). Reset. The Game tab shows the link's
state ("linked: cabinet 1 of 2"). The attract screen says 通信システム
(communication system) when the link is up. To try it on one computer, start
a second copy with `build/daytona --profile 2` (its own settings and
saves), with port 15113 and next `127.0.0.1:15112`, and the first with port
15112 and next `127.0.0.1:15113`. A firewall may ask to allow the
connections.

To skip the launcher, tick **Skip launcher** on the Game tab: from then on
the game starts straight away (Esc still brings the launcher back, where you
can untick it). If the ROM set is missing or wrong, the launcher shows
anyway, with the reason. For one run only:

    build/daytona --rom roms/daytona93.zip --autostart

## Dreamcast (in progress)

A Dreamcast build of the game, from the `daytona` (Revision A) set only. It
runs in the Flycast emulator, with sound; it is not yet tested on a
console. Details: [platform/dreamcast/README.md](../platform/dreamcast/README.md).

You need, besides the above: [DreamSDK](https://dreamsdk.org/) (R4, at
`C:/DreamSDK`) on Windows, or a KallistiOS toolchain elsewhere, and
[Flycast](https://github.com/flyinghead/flycast) to run it.

1. Run setup as above, then recompile the Revision A set into its own
   build folder:

       python scripts/recompile.py --set daytona --build-dir build-daytona

2. Start `build-daytona`'s game once, set it to a single cabinet in test
   mode, and quit: the Dreamcast build starts from those saved settings.
3. Build the Dreamcast disc image and run it in Flycast:

       python platform/dreamcast/build_dreamcast.py game

   The first time it also sets up KallistiOS. The disc image is
   `build-daytona/dreamcast/game.cdi` (`--no-run` builds it without
   starting Flycast). It plays on the controller in port A: Y coin, Start,
   stick or D-pad to steer, triggers for the pedals, D-pad up/down to shift.
   `--draw-every N` sets how many frames per picture (default 2).
   `--no-native-geo` keeps the desktop's arithmetic for the 3D transforms
   (the default uses the SH-4's vector instruction: slightly faster, the
   same game).

## Updating

To get a newer version, from the project folder:

    git pull
    ./setup.sh

(`setup.ps1` on Windows.) Always run setup after pulling: updates can
change how the game is recompiled, and setup redoes that from your ROM
set. It is much quicker the second time.

## Troubleshooting

**`zsh: no such file or directory: ./setup`**
The script is `./setup.sh`.

**`No ROM set: the tools are built, the game is not`**, or
**`build/daytona: no such file or directory`**
Setup did not find `roms/daytona93.zip` or `roms/daytona.zip` (or `.7z`).
Check the folder is called `roms`, is inside the project folder, and the
file is named exactly after its set (`daytona93.zip` for Deluxe '93,
`daytona.zip` for Revision A; not `daytona93.zip.zip`: Windows can hide the
extension). Setup lists any archives it found in `roms/` under other names.
Fix it and run setup again.

**`m2import: missing epr-16530a.12`** (or another file), then
**`your daytona93 ROM set ... was rejected`**
The file is not the set its name says (`daytona93.zip` must be Deluxe '93,
`daytona.zip` Revision A), or is incomplete. See
[What you need](#what-you-need). A set under the other name works; a
different set renamed does not: its program is different.

**`your ROM set was accepted, but recompiling or building the game
failed`**
Your ROM set is fine; the build has a problem. First run `git pull` and
setup again: it may already be fixed. If not, search the output for lines
containing `error` and report them (an issue on GitHub), with the ten or so
lines around them. To retry from a clean state, delete the `build` folder
first (see Starting again from scratch).

**Windows: `unresolved external symbol WinMain`** (error LNK2019)
An older version of the project. Run `git pull`, then `setup.ps1` again.

**Windows: `error C4235: nonstandard extension used: '__int128'`**, or
**`'__builtin_clz' undefined`**
An older version of the project, built with Microsoft's compiler. Run
`git pull`, then `setup.ps1` again: it installs the Clang tools and
switches the build to Clang (both compilers work now).

**Windows: `error C2099: initializer is not a constant` in SDL's
`yuv_rgb_internal.h`**
An older version of the project, built with Microsoft's compiler. Run
`git pull`, then `setup.ps1` again.

**Windows: "The Clang tools for Visual Studio are not installed"**
Open the Visual Studio Installer, choose Modify on your Visual Studio or
Build Tools, and under Individual components tick "C++ Clang Compiler for
Windows" and "MSBuild support for LLVM (clang-cl) toolset". Then run
`setup.ps1` again. Or run `setup.ps1 --msvc` to use Microsoft's compiler.

**Start is greyed out in the launcher**
The line under the ROM path says why. If it says files are missing or
wrong, the file you chose is not the set this build is for (`daytona93`
for Deluxe '93, `daytona` for Revision A); with both built, the button
offers the other game instead. If it cannot open the
file, click **Browse...** and choose it again.

**The game closes straight away, mentioning `SDL_CreateGPUDevice`**
The graphics option in the launcher is set to one your computer lacks
(Vulkan on a Mac, for instance). Current versions fall back to automatic;
on an older one, set **Graphics API** to **Automatic**, or delete the
settings file below.

**`no recompiled code at 00xxxxxx: add it to the seeds`**
The game reached code this version does not have yet. Run `git pull` and
setup again; if it still happens, open an issue on GitHub with the address
and what you were doing in the game.

**Reporting a problem with the game itself**
Include the game's messages: the lines starting `daytona:` in the window you
started it from, or on Windows the file `daytona.log` in the settings folder
below. They say which graphics driver and renderer are in use (for example
`daytona: renderer hardware (GPU)`), and why the hardware renderer could not
start if it could not.

**Revision A stays on a screen of settings (LINK ID, CABINET TWIN)**
It is waiting for a second, linked cabinet. Set a single cabinet in test
mode (F2), as under [Play](#4-play).

**Starting again from scratch**
Delete the `build` folder (and `build-daytona`) and run setup again. Your
ROM sets in `roms/` are kept. Launcher settings, the game's settings EEPROM
and backup RAM are in (`daytona` instead of `daytona93` for Revision A):

- macOS: `~/Library/Application Support/daytona-recomp/daytona93/`
- Windows: `%APPDATA%\daytona-recomp\daytona93\`
- Linux: `~/.local/share/daytona-recomp/daytona93/`

Delete that folder to reset them.

## Status

The game builds and plays on macOS (Apple silicon, Metal). Every change is
built and tested by GitHub Actions on Windows (Clang and MSVC), macOS and
Linux (GCC and Clang), without a ROM set; the Windows and Linux games
themselves have had less testing.
Problems on any platform are worth an issue on GitHub, with the full
output of setup.
