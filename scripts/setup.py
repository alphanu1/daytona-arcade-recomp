#!/usr/bin/env python3
"""Fetch everything needed to build daytona-arcade-recomp and build it. The
same steps on Linux, macOS and Windows; run ./setup.sh (Linux, macOS) or
setup.ps1 (Windows) first to install the toolchain, or run this directly if
you already have one.

  setup.py [--test-extras] [--with-mame] [--build-mame] [--no-build]

Always:
  - checks the toolchain: git, CMake >= 3.20, a C++20 compiler (Ninja used
    when present; Visual Studio's generator on Windows);
  - fetches Berkeley SoftFloat 3e, SDL 3, ymfm, the LZMA SDK and Dear ImGui
    at their pinned commits into extern/ (all built from source with the
    project, statically);
  - configures and builds the tools and tests, runs the tests;
  - if your ROM set is at roms/daytona93.zip (or .7z): imports it, recompiles the
    game's code to native C++ and builds it (all under build/, git-ignored);
  - the same for the 1994 version (MAME's daytona, Revision A) if it is at
    roms/daytona.zip (or .7z), in build-daytona/ (a separate build: each
    build directory is recompiled from one ROM set).
Options:
  --test-extras  pip packages and modules for the optional tests (lupa for the
                 Lua plugin tests, pypcode + the Ghidra i960 module for the
                 second decode oracle).
  --with-mame    MAME source at its pinned commit, patched (the disassembler
                 oracle test; needed before --build-mame).
  --build-mame   build the patched, Model-2-only MAME that records the traces
                 the lockstep checks compare against (Linux and macOS; tens of
                 minutes). Only for validation: the game never uses MAME.

Nothing fetched here is committed (extern/ is git-ignored), and no ROM data
ever leaves roms/ and build/.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERN = os.path.join(ROOT, "extern")
WINDOWS = os.name == "nt"

# Pinned third-party sources (THIRD_PARTY.md).
SOFTFLOAT = ("https://github.com/ucb-bar/berkeley-softfloat-3.git", "a0c6494cdc11865811dec815d5c0049fba9d82a8")
SDL3 = ("https://github.com/libsdl-org/SDL.git", "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c")  # release-3.4.16
IMGUI = ("https://github.com/ocornut/imgui.git", "f1cc2ae15e53a861a874c3034aae6798fde194ab")  # v1.92.9b
YMFM = ("https://github.com/aaronsgiles/ymfm.git", "81aec25ccbb98f4873a255f7551ac4dadac59b4a")  # YM3438 (sound board FM)
LZMA_SDK = ("https://github.com/ip7z/7zip.git", "0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17")  # 7-Zip 26.03 (C/: LZMA SDK)
GHIDRA_I960 = ("https://github.com/mumbel/ghidra_i960.git", "727ef7872c5b1cd6ceb5a81f5e474d1ced92945c")
MAME_COMMIT = "dddd73680656e355bb2b5beecab1167c9f07bf81"


def say(msg):
    print(f"\n== {msg}", flush=True)


def run(cmd, cwd=ROOT, check=True):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, check=check)


def need(tool, hint):
    path = shutil.which(tool)
    if not path:
        sys.exit(f"setup: {tool} not found. {hint}")
    return path


def check_toolchain():
    say("Checking the toolchain")
    need("git", "Install Git (setup.sh / setup.ps1 do this).")
    need("cmake", "Install CMake 3.20 or newer (setup.sh / setup.ps1 do this).")
    out = subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    if not m or (int(m.group(1)), int(m.group(2))) < (3, 20):
        sys.exit(f"setup: CMake 3.20 or newer is needed, found {out.splitlines()[0] if out else 'none'}")
    if not WINDOWS and not (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")):
        sys.exit("setup: no C++ compiler found (setup.sh installs one)")
    print(f"{platform.system()} {platform.machine()}, Python {platform.python_version()}, {out.splitlines()[0]}")


def fetch(url, commit, dest):
    dest = os.path.join(EXTERN, dest)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--no-checkout", url, dest])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", commit], check=False)
    if run(["git", "-C", dest, "checkout", "--detach", commit], check=False).returncode:
        # our patches (patches/) changed files the new commit changes too: start
        # clean at the commit; apply_patches puts them back
        run(["git", "-C", dest, "checkout", "--force", "--detach", commit])


def apply_patches(dest, name):
    """Apply patches/<name>/*.patch to extern/<dest>, skipping ones already applied."""
    dest = os.path.join(EXTERN, dest)
    patches = os.path.join(ROOT, "patches", name)
    for p in sorted(os.listdir(patches)):
        if not p.endswith(".patch"):
            continue
        path = os.path.join(patches, p)
        if subprocess.run(["git", "-C", dest, "apply", "--check", path], capture_output=True).returncode == 0:
            run(["git", "-C", dest, "apply", path])
        elif subprocess.run(["git", "-C", dest, "apply", "--reverse", "--check", path], capture_output=True).returncode:
            print(f"setup: warning: patches/{name}/{p} does not apply to extern/{os.path.basename(dest)}")


def fetch_mame(full):
    """The pinned MAME with our oracle patches. Sparse (the files the tests
    read) unless the full tree is needed to build it."""
    dest = os.path.join(EXTERN, "mame")
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--filter=blob:none", "--no-checkout", "https://github.com/mamedev/mame.git", dest])
    if full:
        run(["git", "-C", dest, "sparse-checkout", "disable"], check=False)
    else:
        run(["git", "-C", dest, "sparse-checkout", "set", "--no-cone",
             "/src/devices/cpu/i960/", "/src/devices/cpu/mb86233/",
             "/src/devices/cpu/m68000/m68000.cpp", "/src/devices/cpu/m68000/m68000.h",
             "/src/mame/sega/model2.cpp", "/src/mame/sega/model2.h",
             "/src/mame/sega/model2_v.cpp", "/src/mame/sega/model2_m.cpp",
             "/src/mame/shared/segam1audio.cpp", "/src/mame/shared/segam1audio.h"])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", MAME_COMMIT])
    run(["git", "-C", dest, "checkout", "--detach", MAME_COMMIT])
    apply_patches("mame", "mame")


VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")


def vs_with_clang():
    """Return a Visual Studio 2022 instance with both compiler and toolset.

    Match setup.ps1's VS 2022 install, even when a newer VS also has Clang.
    CMake must use this same instance, not its default (possibly newer) VS.
    """
    if not os.path.exists(VSWHERE):
        return None
    out = subprocess.run([VSWHERE, "-latest", "-version", "[17,18)", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
                          "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset", "-format", "json", "-utf8"],
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    instances = json.loads(out)
    return instances[0] if instances else None


def cached(build, key):
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as cache:
            for line in cache:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def configure_and_build(build, romset="daytona93"):
    say("Configuring and building" + ("" if romset == "daytona93" else f" ({romset})"))
    cmd = ["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DM2_ROMSET=" + romset]
    want = {}  # cache entries that must match, or the build directory is reconfigured
    if WINDOWS and not (shutil.which("cl") and shutil.which("ninja")):
        # Visual Studio generator: finds the compiler without a developer prompt.
        # Clang (clang-cl) when Visual Studio has it, else MSVC.
        cmd += ["-A", "x64"]
        choice = os.environ.get("M2_COMPILER", "").lower()  # clang or msvc forces one (CI builds both)
        clang_vs = vs_with_clang() if choice != "msvc" else None
        if choice == "clang" and not clang_vs:
            sys.exit("setup: M2_COMPILER=clang, but Visual Studio 2022 has no Clang tools (setup.ps1 adds them)")
        toolset = "ClangCL" if clang_vs else ""
        if toolset:
            major = clang_vs["installationVersion"].split(".", 1)[0]
            capabilities = json.loads(subprocess.run(["cmake", "-E", "capabilities"],
                                      capture_output=True, text=True, check=True).stdout)
            generator = next((g["name"] for g in capabilities["generators"]
                              if g["name"].startswith(f"Visual Studio {major} ")), None)
            if not generator:
                sys.exit(f"setup: this CMake does not support Visual Studio {major}, where Clang is installed")
            instance = clang_vs["installationPath"].replace("\\", "/")
            cmd += ["-G", generator, "-DCMAKE_GENERATOR_INSTANCE=" + instance, "-T", toolset]
            want["CMAKE_GENERATOR"] = generator
            want["CMAKE_GENERATOR_INSTANCE"] = instance
            print(f"Compiler: Clang ({generator}, {instance})")
        else:
            print("Compiler: MSVC. For Clang, run setup.ps1, which adds Visual Studio's Clang tools.")
        want["CMAKE_GENERATOR_TOOLSET"] = toolset
    elif shutil.which("ninja"):
        cmd += ["-G", "Ninja"]
        if WINDOWS and shutil.which("clang-cl"):  # a developer prompt with Clang on PATH
            cmd += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    if os.path.exists(os.path.join(build, "CMakeCache.txt")):
        if any((cached(build, k) or "") != v for k, v in want.items()):
            # A generator, instance or toolset cannot change in place.
            print("The build directory was configured for another compiler or Visual Studio installation; reconfiguring it.")
            os.remove(os.path.join(build, "CMakeCache.txt"))
            shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        run(cmd)
    else:
        run(["cmake", "-S", ".", "-B", build, "-DM2_ROMSET=" + romset])
    run(["cmake", "--build", build, "--config", "Release", "--parallel"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test-extras", action="store_true")
    ap.add_argument("--with-mame", action="store_true")
    ap.add_argument("--build-mame", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir)

    check_toolchain()
    os.makedirs(EXTERN, exist_ok=True)

    say("Fetching SoftFloat 3e (extF80 reference for i960 FP)")
    fetch(*SOFTFLOAT, "softfloat")

    say("Fetching SDL 3.4.16 (window, input, SDL_GPU: Vulkan / Direct3D 12 / Metal)")
    fetch(*SDL3, "sdl3")
    apply_patches("sdl3", "sdl3")  # Vulkan application name (THIRD_PARTY.md)

    say("Fetching ymfm (the sound board's YM3438)")
    fetch(*YMFM, "ymfm")

    say("Fetching the 7-Zip LZMA SDK 26.03 (7z ROM sets)")
    fetch(*LZMA_SDK, "lzma")

    say("Fetching Dear ImGui 1.92.9b (launcher interface)")
    fetch(*IMGUI, "imgui")

    if args.test_extras:
        say("Optional test extras")
        run([sys.executable, "-m", "pip", "install", "--user", "lupa", "pypcode"], check=False)
        fetch(*GHIDRA_I960, "ghidra_i960")

    if args.with_mame or args.build_mame:
        say("Fetching MAME (validation oracle only) and applying our patches")
        fetch_mame(full=args.build_mame)

    if args.build_mame:
        if WINDOWS:
            print("setup: building MAME on Windows needs MAME's MSYS2 toolchain; see "
                  "https://docs.mamedev.org/initialsetup/compilingmame.html. Skipped.")
        else:
            say("Building the Model-2-only MAME (this takes a while)")
            run(["sh", os.path.join("scripts", "build_mame.sh")])

    if args.no_build:
        return
    configure_and_build(build)

    games = []  # (set, executable)
    for romset, label in SETS:
        if not any(os.path.exists(os.path.join(ROOT, "roms", romset + "." + e)) for e in ("zip", "7z")):
            continue
        set_dir = args.build_dir if romset == "daytona93" else args.build_dir + "-" + romset
        set_build = os.path.join(ROOT, set_dir)
        if set_build != build:
            configure_and_build(set_build, romset)
        say(f"Recompiling {label} to native code (your ROM set, kept in {set_dir}/)")
        r = run([sys.executable, os.path.join("scripts", "recompile.py"), "--set", romset, "--build-dir", set_dir],
                check=False)
        if r.returncode == 3:  # recompile.py ROM_REJECTED_EXIT
            sys.exit(ROM_REJECTED.format(set=romset))
        if r.returncode:
            sys.exit(BUILD_FAILED)
        # One launcher for both games: build/daytona starts daytona-93 (Deluxe '93) or daytona-rev
        # (Revision A), both beside it in the main build folder (CMake puts them there).
        name = ("daytona-93" if romset == "daytona93" else "daytona-rev") + EXE
        exe = next((p for d in (build, set_build) for p in (os.path.join(d, name), os.path.join(d, "Release", name))
                    if os.path.exists(p)), None)
        if exe:
            games.append((label, exe))
    if not games:
        say("No ROM set: the tools are built, the game is not")
        print(no_rom_help())

    say("Running the tests")
    run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure"], check=False)
    say("Done")
    for label, exe in games:
        print(f"\n{label} is built ({os.path.relpath(exe, ROOT)}).")
    if games:
        launcher = next((p for p in (os.path.join(build, "daytona" + EXE), os.path.join(build, "Release", "daytona" + EXE))
                         if os.path.exists(p)), None)
        if launcher:
            print("\nStart it with the launcher (choose your ROM set there; it opens that game):\n\n    "
                  + os.path.relpath(launcher, ROOT))
    if games:
        print()


EXE = ".exe" if WINDOWS else ""
# The ROM sets the game can be recompiled from: roms/<set>.zip or .7z.
SETS = [("daytona93", "Daytona USA Deluxe '93 (daytona93)"), ("daytona", "Daytona USA Revision A, 1994 (daytona)")]

BUILD_FAILED = """
setup: your ROM set was accepted, but recompiling or building the game
failed (the errors are above, just before this message). That is a problem
in the build, not in your ROM set: please report it with those errors.
"""

ROM_REJECTED = """
setup: your {set} ROM set (roms/{set}.zip or .7z) was rejected (the line
starting "m2import:" above names the first file that is missing or wrong),
so that game was not built.

The game can be built from two of MAME's Daytona USA sets, each under its own
name: daytona93 (Daytona USA Deluxe '93: epr-16530a.12, epr-16531a.13,
epr-16534a.6, epr-16535a.7) at roms/daytona93.zip, and daytona (Revision A,
1994: epr-16722a.12, epr-16723a.13, epr-16724a.6, epr-16725a.7) at
roms/daytona.zip (or .7z). Other sets (daytonas, daytonat, daytonase, ...)
have different program ROMs and cannot be used, whatever the file is called.

Put the right set under the right name and run setup again.
See docs/getting-started.md, "Troubleshooting".
"""


def no_rom_help():
    roms = os.path.join(ROOT, "roms")
    named = {romset + ext for romset, _ in SETS for ext in (".zip", ".7z")}
    found = sorted(f for f in os.listdir(roms) if f.lower().endswith((".zip", ".7z")) and f not in named) \
        if os.path.isdir(roms) else []
    lines = ["To build the game, put your own ROM set at roms/daytona93.zip (Daytona USA Deluxe '93,",
             "MAME's daytona93) or roms/daytona.zip (Revision A, 1994, MAME's daytona), or .7z,",
             "exactly that name, and run setup again. Both can be there: each is built."]
    if found:
        lines += ["", "Found in roms/, but not under those names: " + ", ".join(found),
                  "If one of these is one of those sets, rename it to that set's name."]
    return "\n".join(lines)


if __name__ == "__main__":
    main()
