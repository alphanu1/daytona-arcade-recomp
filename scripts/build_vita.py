#!/usr/bin/env python3
"""Cross-compile the Vita frontend using already generated host game code.

First run the setup/recompile pipeline with your own selected ROM set
(python3 scripts/recompile.py, once, in any mode). --fast-inaccuracy and
--fast-gen then make the Vita's own game code here from the host tools and
ROM cache, whatever mode the host code was generated in (see vita_code).
This command never executes cross-built importers, copies ROMs, or packages
ROM data. --compile-check builds objects without ROMs and does not make a VPK.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ("daytona93/gen_table.cpp", "daytona93_tgp/tgp_gen.cpp", "daytona93_snd/snd_gen.cpp")
# The Dreamcast's pass over the generated i960 code, used unchanged by the Vita build
# (its runtime support is M2_FAST_GEN, platform/vita/CMakeLists.txt).
FAST_GEN = Path("platform/dreamcast/scripts/fast_gen.py")


def run(command, env):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def host_tool(host, name):
    """A tool of the host build: in the build directory, or build/Release/ (Visual Studio)."""
    exe = name + (".exe" if os.name == "nt" else "")
    for path in (host / exe, host / "Release" / exe):
        if path.is_file():
            return path
    return None


def write_if_changed(path, data):
    """Rewritten only when it changes: make recompiles only those files."""
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def sync(src, dst):
    """dst becomes a copy of src's files; unchanged files keep their date and
    files gone from src are removed (the build compiles every *.cpp there)."""
    dst.mkdir(parents=True, exist_ok=True)
    names = set()
    for f in sorted(src.iterdir()):
        if f.is_file():
            names.add(f.name)
            write_if_changed(dst / f.name, f.read_bytes())
    for stale in dst.iterdir():
        if stale.is_file() and stale.name not in names:
            stale.unlink()


def load_fast_gen():
    import importlib.util
    spec = importlib.util.spec_from_file_location("vita_fast_gen", ROOT / FAST_GEN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def direct_ram(folder):
    """--fast-inaccuracy: fast_gen.py's direct work-RAM accesses only, in place. A load or
    store at a fixed, aligned address in 0x00500000-0x005fffff (plain RAM: no
    device, nothing watches its writes) becomes gen::wram_* instead of a call
    through the bus; the frame-wait byte (0x00500000) keeps its call. The lockstep
    code (--fast_inaccuracy's checks per block, chunk chaining) is not touched."""
    direct_wram = load_fast_gen().direct_wram
    stats = {"wram": 0}
    for f in sorted(folder.glob("chunk_*.cpp")):
        text = f.read_text()
        f.write_text("\n".join(direct_wram(line, stats) for line in text.split("\n")), newline="\n")
    print(f"build_vita: {stats['wram']} work-RAM accesses direct", flush=True)


def vita_code(host, gen, build, mode, game):
    """The game code of the Vita build, made here from the host build's tools and ROM
    cache (scripts/recompile.py, run once in any mode): the host's own generated code
    and the desktop build are left as they are. Into BUILD_DIR/gen_vita:

      --fast-inaccuracy  i960 m2recomp --fast_inaccuracy (lockstep checks once per
                         block, direct chunk chaining: interrupts a few instructions late)
                         plus direct work-RAM accesses (direct_ram)
      --fast-gen         i960 default output rewritten by fast_gen.py (exact; slower
                         on the Vita)

    TGP always m2tgprecomp --fast_inaccuracy: the game's unbudgeted calls counted once
    per basic block, without the budget test; exact (the TGP has no interrupts). The
    68000 code as the host generated it."""
    cache = host / "rom_cache" / game
    i960_tool, tgp_tool = host_tool(host, "m2recomp"), host_tool(host, "m2tgprecomp")
    for need in (i960_tool, tgp_tool, cache / "program.bin", cache / "tgp_program.bin"):
        if not need or not Path(need).is_file():
            raise RuntimeError(f"missing {need or 'm2recomp/m2tgprecomp in ' + str(host)}: "
                               "run python3 scripts/recompile.py once first (it builds the tools and the ROM cache)")
    tmp = build / "gen_tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    (tmp / "i960").mkdir(parents=True)
    (tmp / "tgp").mkdir(parents=True)
    i960 = [i960_tool, cache / "program.bin", tmp / "i960", "--seeds", f"seeds/{game}.txt"]
    if (ROOT / f"seeds/{game}_hooks.txt").is_file():
        i960 += ["--hooks", f"seeds/{game}_hooks.txt"]
    if mode != "fast_gen":
        i960.append("--fast_inaccuracy")
    run(i960, os.environ)
    i960_out = tmp / "i960"
    if mode == "fast_inaccuracy":
        direct_ram(i960_out)
    elif mode == "fast_gen":
        run([sys.executable, ROOT / FAST_GEN, i960_out, tmp / "i960_fast_gen"], os.environ)
        i960_out = tmp / "i960_fast_gen"
    run([tgp_tool, cache / "tgp_program.bin", tmp / "tgp/tgp_gen.cpp", "--fast_inaccuracy"], os.environ)
    out = build / "gen_vita"
    sync(i960_out, out / game)
    sync(tmp / "tgp", out / f"{game}_tgp")
    sync(gen / f"{game}_snd", out / f"{game}_snd")
    shutil.rmtree(tmp, ignore_errors=True)
    for old in ("gen_fast", "gen_ram"):  # earlier versions of this script
        shutil.rmtree(build / old, ignore_errors=True)
    return out, {"fast_inaccuracy": "i960 --fast_inaccuracy + direct work-RAM accesses",
                 "fast_gen": "i960 fast_gen.py rewrite"}[mode] + ", TGP fast mode"


def vitagl_summary(prefix):
    """Which vitaGL a --gpu-gl build links (platform/vita/CMakeLists.txt makes the same choice)."""
    try:
        stamp = json.loads((prefix / "build.json").read_text())
    except (OSError, ValueError):
        stamp = None
    if not stamp or not (prefix / "lib/libvitaGL.a").is_file():
        return "the VitaSDK's (not pinned: run python3 scripts/setup_vitagl.py)"
    flags = " ".join(f"{k}={v}" for k, v in stamp.get("flags", {}).items())
    return f"pinned {stamp['pins']['vitaGL'][:10]}, {stamp.get('profile')}: {flags or 'default flags'}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host-build-dir", type=Path, default=Path("build"))
    ap.add_argument("--set", choices=("daytona93", "daytona"), default="daytona93")
    ap.add_argument("--revision-a-self", type=Path, help="bundle the built Revision A eboot.bin as daytona.self")
    ap.add_argument("--build-dir", type=Path, default=Path("build/vita"))
    ap.add_argument("--vitasdk", type=Path, default=os.environ.get("VITASDK"))
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    ap.add_argument("--compile-check", action="store_true")
    ap.add_argument("--reference-renderer", action="store_true", help="disable OPT03 renderer changes for comparison")
    gpu = ap.add_mutually_exclusive_group()
    gpu.add_argument("--gpu-fast", action="store_true", help="build experimental vita2d/GXM 3D renderer (requires vdpm libvita2d)")
    gpu.add_argument("--gpu-gl", action="store_true", help="build experimental vitaGL 3D renderer, Model 2 draw priority "
                     "(vitaGL pinned by scripts/setup_vitagl.py, else the VitaSDK's; libshacccg.suprx on the Vita)")
    ap.add_argument("--vitagl-dir", type=Path, default=Path("extern/vitagl/install"),
                    help="--gpu-gl: vitaGL built by scripts/setup_vitagl.py (default extern/vitagl/install)")
    ap.add_argument("--free-core", action="store_true",
                    help="GPU builds: leave the main, geometry, sound and 2D threads unpinned (default: one core each); "
                         "the 4TH CORE option can then take any of them")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--diagnostics", action="store_true", help="enable GXM startup/performance file logging (faults always recorded)")
    mode.add_argument("--release", action="store_true", help="fastest build: no diagnostic logging + link-time optimization (LTO; longer link, more host memory)")
    code = ap.add_mutually_exclusive_group()
    code.add_argument("--fast-inaccuracy", dest="code", action="store_const", const="fast_inaccuracy",
                      help="Vita game code made here (BUILD_DIR/gen_vita): i960 m2recomp --fast_inaccuracy with direct "
                           "work-RAM accesses (fast_gen.py's), TGP fast mode")
    code.add_argument("--fast-gen", dest="code", action="store_const", const="fast_gen",
                      help="i960 default output rewritten by platform/dreamcast/scripts/fast_gen.py (exact, slower on the Vita), "
                           "TGP fast mode. Without either: the host-generated code as it is")
    args = ap.parse_args(argv)
    gpu_build = args.gpu_fast or args.gpu_gl
    if args.set != "daytona93" and not gpu_build:
        ap.error("Revision A selection requires --gpu-fast or --gpu-gl")
    if args.revision_a_self and (args.set != "daytona93" or not gpu_build):
        ap.error("--revision-a-self requires the daytona93 --gpu-fast or --gpu-gl launcher")
    if args.free_core and not gpu_build:
        ap.error("--free-core applies to the GPU builds (--gpu-fast or --gpu-gl)")
    if args.jobs < 1:
        ap.error("--jobs must be positive")
    if args.vitasdk is None:
        ap.error("set VITASDK or pass --vitasdk /path/to/vitasdk")
    sdk = args.vitasdk.expanduser().resolve()
    toolchain = sdk / "share/vita.toolchain.cmake"
    if not toolchain.is_file():
        ap.error(f"missing VitaSDK toolchain: {toolchain}")
    host = (ROOT / args.host_build_dir).resolve()
    build = (ROOT / args.build_dir).resolve()
    gen = host / "gen"
    if build == host or build == ROOT or build == gen or gen in build.parents:
        ap.error("use a separate cross-build directory, not the host build or its generated sources")
    if not args.compile_check:
        required = tuple(name.replace("daytona93", args.set) for name in REQUIRED)
        missing = [str(gen / name) for name in required if not (gen / name).is_file()]
        if missing:
            ap.error("missing host-generated code:\n" + "\n".join(missing) +
                     "\nRun python3 scripts/recompile.py with the host compiler first.")
    if gpu_build and not args.compile_check:
        if not any("rt::hook_draw_list(" in source.read_text()
                   for source in (gen / args.set).glob("chunk_*.cpp")):
            ap.error("generated game code lacks the draw-distance hook; regenerate with scripts/recompile.py")
    code = "host-generated (scripts/recompile.py output as is)"
    if args.code and not args.compile_check:
        gen, code = vita_code(host, gen, build, args.code, args.set)
    vitagl_dir = (ROOT / args.vitagl_dir).resolve()
    vitagl = vitagl_summary(vitagl_dir) if args.gpu_gl else "-"
    env = os.environ.copy()
    env["VITASDK"] = str(sdk)
    env["PATH"] = str(sdk / "bin") + os.pathsep + env.get("PATH", "")
    run(["cmake", "-S", ROOT / "platform/vita", "-B", build,
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
         f"-DDAYTONA_GEN_ROOT={gen}",
         f"-DDAYTONA_VITA_ROMSET={args.set}",
         f"-DDAYTONA_VITA_REV_A_SELF={(ROOT / args.revision_a_self).resolve() if args.revision_a_self else ''}",
         f"-DDAYTONA_VITA_RENDER_OPT={'OFF' if args.reference_renderer else 'ON'}",
         f"-DDAYTONA_VITA_DIAGNOSTICS={'ON' if args.diagnostics else 'OFF'}",
         f"-DDAYTONA_VITA_LTO={'ON' if args.release else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_FAST={'ON' if args.gpu_fast else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_GL={'ON' if args.gpu_gl else 'OFF'}",
         f"-DDAYTONA_VITA_FREE_CORES={'ON' if args.free_core else 'OFF'}",
         f"-DDAYTONA_VITA_COMPILE_CHECK={'ON' if args.compile_check else 'OFF'}",
         *([f"-DDAYTONA_VITAGL_DIR={vitagl_dir}"] if args.gpu_gl else [])], env)
    run(["cmake", "--build", build, "--parallel", args.jobs], env)
    if args.compile_check:
        print("Compile check complete. No linked game or VPK was built." + (f"\nvitaGL: {vitagl}" if args.gpu_gl else ""))
    else:
        package = build / "daytona_vita.vpk"
        if not package.is_file():
            raise RuntimeError(f"build completed without the expected package: {package}")
        mode = "GPU GL (vitaGL)" if args.gpu_gl else ("GPU FAST" if args.gpu_fast else "CPU EXACT")
        cores = ("free (--free-core)" if args.free_core else "pinned: main 0, geometry 1, sound 2") if gpu_build else "-"
        kind = "release (LTO, no logs)" if args.release else ("diagnostics (logs)" if args.diagnostics else "normal (no logs)")
        print(f"VPK: {package}\nRenderer build: {mode}\nvitaGL: {vitagl}\nCores: {cores}\nBuild: {kind}\nGame code: {code}\n"
              f"ROM location on Vita: ux0:data/{args.set}/{args.set}.zip")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"build_vita: {error}", file=sys.stderr)
        sys.exit(1)
