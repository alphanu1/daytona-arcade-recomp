#!/usr/bin/env python3
"""Fetch and build vitaGL for the Vita --gpu-gl build: pinned, inside the project.

  python3 scripts/setup_vitagl.py [--profile release|debug] [--flags "NAME=1 ..."]
                                  [--vitasdk DIR] [--dest DIR] [--jobs N] [--force]
                                  [--print-flags]

vitaGL and the libraries it is linked with are cloned at the commits pinned below
(a later vitaGL commit cannot break the build) and built with the VitaSDK toolchain
into extern/vitagl/, which is git-ignored:

  extern/vitagl/src/<name>   git checkouts: vitaGL, vitaShaRK, math-neon, SceShaccCgExt
  extern/vitagl/build/       SceShaccCgExt's CMake build directory
  extern/vitagl/install/     include/ + lib/ + licenses/, used by platform/vita/CMakeLists.txt
                             vitagl.cmake  commit, profile and flags (printed at configure)
                             build.json    what was built: a rerun with the same pins,
                                           flags and toolchain does nothing (--force rebuilds)

Dependencies: vitaGL needs vitaShaRK (runtime shader compiler) and math-neon; vitaShaRK
itself needs SceShaccCgExt. All four are built here. Only the base VitaSDK is used:
toolchain, vita-headers stubs and taihen.

Nothing is installed into $VITASDK. A vitaGL already installed there is neither used
nor modified: the headers built here come first (CPATH) and CMake links the libraries
by their full path.

Profiles (vitaGL make flags, see platform/vita/README.md "vitaGL build flags"):
  release  the fastest flags that are safe for gpu_gl.cpp (default)
  debug    vitaGL defaults + error and shader compiler logs (sceClibPrintf)
--flags adds flags to the profile or, with NAME=0, removes one, e.g.
  --flags "NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=0"
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEST = Path("extern/vitagl")

# Pinned sources (THIRD_PARTY.md). Update a commit here, never track a branch.
PINS = {
    "SceShaccCgExt": ("https://github.com/bythos14/SceShaccCgExt.git",
                      "fb0e9d338525b067f3679ab33571323336493cca"),  # 2026-07-29
    "math-neon": ("https://github.com/Rinnegatamante/math-neon.git",
                  "0faab814782c071ff4015527f1ca955ab1ccc470"),  # 2020-07-05
    "vitaShaRK": ("https://github.com/Rinnegatamante/vitaShaRK.git",
                  "df24065e65098b2d1ac533760109ad4367573f28"),  # 2026-08-22
    "vitaGL": ("https://github.com/Rinnegatamante/vitaGL.git",
               "dca4b9d143290d78ec043131a19be36c78cdc7b5"),  # 2026-10-07
}

# vitaGL make flags. Why each one is (or is not) used: platform/vita/README.md.
PROFILES = {
    "release": {
        "NO_DEBUG": "1",                  # no GL argument validation in every call
        "HAVE_VERTEX_LAYOUT_CACHE": "1",  # no vertex program re-patch per draw call
        "HAVE_SHADER_CACHE": "1",         # compiled shaders cached on ux0: (fast 2nd boot)
        "TEXTURES_SPEEDHACK": "1",        # no per-draw texture tracking (no glTexSubImage2D used)
        "INDICES_SPEEDHACK": "1",         # 16-bit indices only, no instancing
        "PRIMITIVES_SPEEDHACK": "1",      # triangles only (no GL_LINES / GL_POINTS)
    },
    "debug": {
        "LOG_ERRORS": "1",                # GL errors through sceClibPrintf
        "HAVE_SHARK_LOG": "1",            # shader compiler messages
    },
}

# Flags gpu_gl.cpp cannot work with, and why.
REJECTED = {
    ("DRAW_SPEEDHACK", "1"): "glDrawArrays then ignores its first vertex and gpu_gl.cpp's indexed quads "
                             "(glDrawElements) would read the wrong vertices; it also turns "
                             "HAVE_VERTEX_LAYOUT_CACHE off",
    ("SOFTFP_ABI", "1"): "VitaSDK and this project are built for the hard-float ABI",
}

EXE = ".exe" if os.name == "nt" else ""


class SetupError(RuntimeError):
    pass


def say(msg):
    print(f"\n== {msg}", flush=True)


def run(cmd, cwd=None, env=None, check=True, capture=False):
    cmd = [str(c) for c in cmd]
    if not capture:
        print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, env=env, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)


def parse_flags(values):
    """'A=1 B=2,C=0' (repeatable) -> {'A': '1', 'B': '2', 'C': '0'}."""
    flags = {}
    for value in values or ():
        for token in re.split(r"[\s,]+", value.strip()):
            if not token:
                continue
            m = re.fullmatch(r"([A-Z][A-Z0-9_]*)=([0-9]+)", token)
            if not m:
                raise SetupError(f"bad flag '{token}': expected NAME=VALUE, e.g. NO_SPLASHSCREEN=1")
            flags[m.group(1)] = m.group(2)
    return flags


def resolve_flags(profile, extra):
    """The profile's flags with --flags applied; NAME=0 removes NAME. Sorted."""
    flags = dict(PROFILES[profile])
    for name, value in extra.items():
        if value == "0":
            flags.pop(name, None)
        else:
            flags[name] = value
    return dict(sorted(flags.items()))


def makefile_flags(makefile):
    """{NAME: {values}} of the 'ifeq ($(NAME),VALUE)' switches of vitaGL's Makefile."""
    known = {}
    for name, value in re.findall(r"^ifeq \(\$\(([A-Z0-9_]+)\),\s*([0-9]+)\)", makefile.read_text(), re.M):
        known.setdefault(name, set()).add(value)
    return known


def check_rules(flags):
    """This project's rules, checked before anything is fetched."""
    for name, value in flags.items():
        if (name, value) in REJECTED:
            raise SetupError(f"{name}={value} is not supported by this project: {REJECTED[(name, value)]}")
    if flags.get("HAVE_TEXTURE_CACHE") == "1" and flags.get("TEXTURES_SPEEDHACK") == "1":
        raise SetupError("HAVE_TEXTURE_CACHE=1 is incompatible with TEXTURES_SPEEDHACK=1 "
                         "(add TEXTURES_SPEEDHACK=0)")


def check_known(flags, known):
    """Every flag exists in the pinned Makefile (a typo would be silently ignored by make)."""
    for name, value in flags.items():
        if name not in known:
            raise SetupError(f"unknown vitaGL flag {name} (not in the pinned vitaGL Makefile)")
        if value not in known[name]:
            raise SetupError(f"{name}={value}: the pinned vitaGL accepts {name}=" + "|".join(sorted(known[name])))


def make_args(flags):
    return [f"{name}={value}" for name, value in flags.items()]


def check_sdk(sdk):
    """The base VitaSDK pieces this build needs; returns the toolchain file."""
    toolchain = sdk / "share/vita.toolchain.cmake"
    if not toolchain.is_file():
        raise SetupError(f"missing VitaSDK toolchain: {toolchain} (set VITASDK or pass --vitasdk)")
    if not (sdk / "bin" / f"arm-vita-eabi-gcc{EXE}").is_file():
        raise SetupError(f"missing {sdk / 'bin' / ('arm-vita-eabi-gcc' + EXE)}: incomplete VitaSDK")
    sysroot = sdk / "arm-vita-eabi"
    for need in (sysroot / "include/taihen.h", sysroot / "lib/libtaihen_stub.a"):
        if not need.is_file():
            raise SetupError(f"missing {need}: SceShaccCgExt needs taiHEN from the VitaSDK "
                             "(vdpm install taihen)")
    return toolchain


def check_tools():
    for tool, hint in (("git", "Install Git."), ("cmake", "Install CMake 3.20 or newer."),
                       ("make", "Install GNU make (VitaSDK's own build needs it too).")):
        if not shutil.which(tool):
            raise SetupError(f"{tool} not found. {hint}")


def toolchain_version(sdk, env):
    out = run([sdk / "bin" / f"arm-vita-eabi-gcc{EXE}", "-dumpfullversion", "-dumpversion"],
              env=env, check=False, capture=True)
    return (out.stdout or "").strip().splitlines()[0] if out.returncode == 0 and out.stdout.strip() else "unknown"


def git_head(repo):
    out = run(["git", "-C", repo, "rev-parse", "HEAD"], check=False, capture=True)
    return out.stdout.strip() if out.returncode == 0 else None


def fetch(name, url, commit, dest):
    """dest becomes a pristine checkout of commit (only that commit is downloaded)."""
    if not (dest / ".git").is_dir():
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        run(["git", "init", "-q", dest])
        run(["git", "-C", dest, "remote", "add", "origin", url])
    else:
        run(["git", "-C", dest, "remote", "set-url", "origin", url])
    if git_head(dest) != commit:
        if run(["git", "-C", dest, "fetch", "-q", "--depth", "1", "origin", commit], check=False).returncode:
            run(["git", "-C", dest, "fetch", "-q", "origin"])  # a server that refuses fetch-by-SHA
        run(["git", "-C", dest, "checkout", "-q", "--force", "--detach", commit])
    # No object or library of an earlier build (other flags) may survive.
    run(["git", "-C", dest, "clean", "-q", "-f", "-d", "-x"])
    head = git_head(dest)
    if head != commit:
        raise SetupError(f"{name}: checkout is at {head}, expected the pinned {commit}")


def install(files, dest):
    dest.mkdir(parents=True, exist_ok=True)
    for f in files:
        if not f.is_file():
            raise SetupError(f"build finished without {f}")
        shutil.copy2(f, dest / f.name)


def build_all(src, build, prefix, toolchain, flags, jobs, env):
    include, lib, licenses = prefix / "include", prefix / "lib", prefix / "licenses"

    say("SceShaccCgExt (needed by vitaShaRK)")
    out = build / "SceShaccCgExt"
    shutil.rmtree(out, ignore_errors=True)
    # C17: GCC 15 defaults to C23, where taihen.h's TAI_CONTINUE cast 'type (*)()' means
    # "no arguments" and its calls with arguments no longer compile.
    run(["cmake", "-S", src / "SceShaccCgExt", "-B", out, f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_STANDARD=17", "-DCMAKE_C_STANDARD_REQUIRED=ON",
         "-DCMAKE_C_EXTENSIONS=ON"], env=env)
    run(["cmake", "--build", out, "--parallel", jobs], env=env)
    # --prefix: never the toolchain's default prefix, which is the VitaSDK itself.
    run(["cmake", "--install", out, "--prefix", prefix], env=env)
    if not (lib / "libSceShaccCgExt.a").is_file():  # a host whose default libdir is lib64
        found = sorted(prefix.rglob("libSceShaccCgExt.a"))
        if not found:
            raise SetupError("SceShaccCgExt built but libSceShaccCgExt.a was not installed")
        shutil.move(str(found[0]), lib / "libSceShaccCgExt.a")
    if not (include / "shacccg_ext.h").is_file():
        raise SetupError("SceShaccCgExt built but shacccg_ext.h was not installed")

    say("math-neon")
    run(["make", "-C", src / "math-neon", f"-j{jobs}"], env=env)
    install([src / "math-neon/libmathneon.a"], lib)
    install([src / "math-neon/source/math_neon.h"], include)

    say("vitaShaRK")
    run(["make", "-C", src / "vitaShaRK", f"-j{jobs}", "libvitashark.a"], env=env)
    install([src / "vitaShaRK/libvitashark.a"], lib)
    install([src / "vitaShaRK/source/vitashark.h"], include)

    say("vitaGL " + (" ".join(make_args(flags)) or "(default flags)"))
    run(["make", "-C", src / "vitaGL", f"-j{jobs}", "libvitaGL.a", *make_args(flags)], env=env)
    install([src / "vitaGL/libvitaGL.a"], lib)
    install([src / "vitaGL/source/vitaGL.h"], include)

    licenses.mkdir(parents=True, exist_ok=True)
    for name, licence in (("vitaGL", "COPYING.LESSER"), ("vitaShaRK", "LICENSE"),
                          ("SceShaccCgExt", "LICENSE"), ("math-neon", "README")):
        if (src / name / licence).is_file():
            shutil.copy2(src / name / licence, licenses / f"{name}.txt")


def write_cmake(prefix, stamp):
    """Read by platform/vita/CMakeLists.txt (configure log + sanity check)."""
    flags = ";".join(make_args(stamp["flags"]))
    text = ("# Written by scripts/setup_vitagl.py; do not edit.\n"
            f'set(DAYTONA_VITAGL_COMMIT "{stamp["pins"]["vitaGL"]}")\n'
            f'set(DAYTONA_VITAGL_PROFILE "{stamp["profile"]}")\n'
            f'set(DAYTONA_VITAGL_FLAGS "{flags}")\n')
    (prefix / "vitagl.cmake").write_text(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", choices=sorted(PROFILES), default="release")
    ap.add_argument("--flags", action="append", metavar="'NAME=VALUE ...'",
                    help="add vitaGL make flags to the profile, NAME=0 removes one (repeatable)")
    ap.add_argument("--vitasdk", type=Path, default=os.environ.get("VITASDK"))
    ap.add_argument("--dest", type=Path, default=DEFAULT_DEST, help="work directory (default extern/vitagl)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--force", action="store_true", help="rebuild even if up to date")
    ap.add_argument("--print-flags", action="store_true", help="print the vitaGL make flags and exit")
    args = ap.parse_args(argv)
    if args.jobs < 1:
        ap.error("--jobs must be positive")
    try:
        flags = resolve_flags(args.profile, parse_flags(args.flags))
        check_rules(flags)
    except SetupError as error:
        ap.error(str(error))
    if args.print_flags:
        print(" ".join(make_args(flags)))
        return 0
    if args.vitasdk is None:
        ap.error("set VITASDK or pass --vitasdk /path/to/vitasdk")

    sdk = args.vitasdk.expanduser().resolve()
    dest = (ROOT / args.dest).resolve()
    src, build, prefix = dest / "src", dest / "build", dest / "install"
    toolchain = check_sdk(sdk)
    check_tools()

    env = os.environ.copy()
    env["VITASDK"] = str(sdk)
    env["PATH"] = str(sdk / "bin") + os.pathsep + env.get("PATH", "")
    # Our headers before any copy in the SDK (CPATH = -I, searched before system dirs).
    env["CPATH"] = os.pathsep.join(filter(None, [str(prefix / "include"), env.get("CPATH")]))

    stamp = {"pins": {name: commit for name, (_, commit) in PINS.items()}, "profile": args.profile,
             "flags": flags, "vitasdk": str(sdk), "gcc": toolchain_version(sdk, env)}
    stamp_file = prefix / "build.json"
    libs = [prefix / "lib" / f"lib{n}.a" for n in ("vitaGL", "vitashark", "mathneon", "SceShaccCgExt")]
    if not args.force and stamp_file.is_file() and all(p.is_file() for p in libs):
        try:
            if json.loads(stamp_file.read_text()) == stamp:
                print(f"vitaGL {PINS['vitaGL'][1][:10]} ({args.profile}) is up to date in {prefix}"
                      " (--force rebuilds)")
                return 0
        except ValueError:
            pass

    say("Fetching the pinned sources")
    for name, (url, commit) in PINS.items():
        fetch(name, url, commit, src / name)
    check_known(flags, makefile_flags(src / "vitaGL/Makefile"))

    # A failed build must not look complete: the stamp goes last.
    shutil.rmtree(prefix, ignore_errors=True)
    (prefix / "include").mkdir(parents=True)
    (prefix / "lib").mkdir()
    build.mkdir(parents=True, exist_ok=True)
    build_all(src, build, prefix, toolchain, flags, args.jobs, env)
    write_cmake(prefix, stamp)
    stamp_file.write_text(json.dumps(stamp, indent=2) + "\n")

    say("Done")
    print(f"vitaGL {PINS['vitaGL'][1]} ({args.profile}): {' '.join(make_args(flags)) or 'default flags'}\n"
          f"Installed in {prefix}\n"
          "Next: python3 scripts/build_vita.py --gpu-gl  (uses it automatically)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, SetupError) as error:
        print(f"setup_vitagl: {error}", file=sys.stderr)
        sys.exit(1)
