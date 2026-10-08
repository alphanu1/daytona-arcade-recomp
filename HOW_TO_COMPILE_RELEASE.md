# How to compile the release version (PS Vita, vitaGL)

## You need

* A Linux, macOS or WSL computer where the project is set up: your ROM set in
  `roms/daytona93.zip`, then `python3 scripts/setup.py` (see [README.md](README.md)).
* [VitaSDK](https://vitasdk.org) with its default packages (SDL2 and taiHEN are used),
  and `VITASDK` set.
* On the Vita: homebrew enabled and `libshacccg.suprx` extracted
  ([guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx)).

## Build

```sh
export VITASDK=/usr/local/vitasdk
python3 scripts/setup_vitagl.py                                   # once: builds vitaGL in extern/vitagl
python3 scripts/build_vita.py --gpu-gl --fast-inaccuracy --release
```

The result is `build/vita/daytona_vita.vpk`. Install it with VitaShell and copy your
ROM set to `ux0:data/daytona93/daytona93.zip`.

## How it runs

There is no emulator. The arcade board's programs, the **i960** (the game itself), the
**TGP** (the 3D maths coprocessor) and the sound board's 68000, were translated ahead of
time into native code for the Vita's ARM CPU. The rest of the board (geometry, 2D,
sound chips) is rewritten as normal C++.

Each frame (57.5 per second, like the arcade) is shared between the Vita's 3 cores:

| Core | Job |
| --- | --- |
| 0 | Runs the game (i960 + TGP) and reads the controls, then prepares the picture: sorts the polygons in the arcade's drawing order and sends them to the GPU through vitaGL. |
| 1 | Turns the game's 3D models into screen polygons (position, clipping, lighting). |
| 2 | Plays the sound board (its 68000 program, music and sound-effect chips) and prepares the 2D layers (HUD, sky, text) as GPU textures. |
| GPU | Draws the polygons and the 2D layers; colours come from the arcade palette, looked up by a shader. |

The cores work like an assembly line: while core 0 computes frame N, cores 1 and 2
finish frame N-1. The picture is one frame (17 ms) behind, which you cannot notice.

The release build starts at **CPU 444 MHz / GPU 166 MHz** (changeable in OPTIONS; an
earlier install keeps its saved clocks until OPTIONS > RESET DEFAULTS), is
optimized across the whole program (link-time optimization) and writes no logs.
`--fast-inaccuracy` checks for arcade interrupts once per block of instructions instead
of after each one: faster, with interrupts at most a few instructions late.

Measured on a Vita at these clocks, it runs at **about 57.3 frames per second** for the
arcade's 57.52 (99.6%), with up to 1,700 polygons per frame and no overclocking plugin.

More details: [platform/vita/README.md](platform/vita/README.md).

## Credits

The vitaGL renderer uses **vitaGL**, **vitaShaRK** and the Vita port of **math-neon**
by **Rinnegatamante**, and **SceShaccCgExt** by **Bythos**. Thank you!
