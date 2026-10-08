"""Host-side tests for scripts/setup_vitagl.py; no VitaSDK, network or git required."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("setup_vitagl", Path(__file__).resolve().parents[1] / "scripts/setup_vitagl.py")
SETUP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SETUP)

MAKEFILE = """\
ifeq ($(NO_DEBUG),1)
CFLAGS += -DSKIP_ERROR_HANDLING
endif
ifeq ($(DRAW_SPEEDHACK),1)
endif
ifeq ($(DRAW_SPEEDHACK),2)
endif
ifeq ($(SHARED_RENDERTARGETS),1)
endif
"""


class FlagTests(unittest.TestCase):
    def test_pins_are_full_commits(self):
        for name, (url, commit) in SETUP.PINS.items():
            self.assertRegex(commit, r"^[0-9a-f]{40}$", name)
            self.assertTrue(url.startswith("https://github.com/"), name)

    def test_release_profile(self):
        flags = SETUP.resolve_flags("release", {})
        self.assertEqual(flags["NO_DEBUG"], "1")
        self.assertEqual(flags["HAVE_VERTEX_LAYOUT_CACHE"], "1")
        self.assertNotIn("DRAW_SPEEDHACK", flags)  # gpu_gl.cpp's indexed quads need 'first'
        SETUP.check_rules(flags)

    def test_extra_flags_add_and_remove(self):
        extra = SETUP.parse_flags(["NO_SPLASHSCREEN=1, HAVE_SHADER_CACHE=0", "LOG_ERRORS=1"])
        flags = SETUP.resolve_flags("release", extra)
        self.assertEqual(flags["NO_SPLASHSCREEN"], "1")
        self.assertEqual(flags["LOG_ERRORS"], "1")
        self.assertNotIn("HAVE_SHADER_CACHE", flags)
        self.assertEqual(list(flags), sorted(flags))  # stable stamp and command line

    def test_bad_flag_syntax(self):
        for bad in ("NO_DEBUG", "no_debug=1", "NO_DEBUG=yes"):
            with self.assertRaises(SETUP.SetupError):
                SETUP.parse_flags([bad])

    def test_rejected_flags(self):
        for flags in ({"DRAW_SPEEDHACK": "1"}, {"SOFTFP_ABI": "1"},
                      {"HAVE_TEXTURE_CACHE": "1", "TEXTURES_SPEEDHACK": "1"}):
            with self.assertRaises(SETUP.SetupError):
                SETUP.check_rules(flags)
        SETUP.check_rules({"DRAW_SPEEDHACK": "2"})  # client-side arrays only: harmless here

    def test_known_flags_come_from_the_makefile(self):
        with tempfile.TemporaryDirectory() as temp:
            makefile = Path(temp) / "Makefile"
            makefile.write_text(MAKEFILE)
            known = SETUP.makefile_flags(makefile)
        self.assertEqual(known["DRAW_SPEEDHACK"], {"1", "2"})
        SETUP.check_known({"NO_DEBUG": "1", "SHARED_RENDERTARGETS": "1"}, known)
        with self.assertRaises(SETUP.SetupError):
            SETUP.check_known({"NO_DEBUGG": "1"}, known)  # a typo make would silently ignore
        with self.assertRaises(SETUP.SetupError):
            SETUP.check_known({"SHARED_RENDERTARGETS": "3"}, known)

    def test_real_vitagl_makefile_if_fetched(self):
        makefile = SETUP.ROOT / SETUP.DEFAULT_DEST / "src/vitaGL/Makefile"
        if not makefile.is_file():
            self.skipTest("vitaGL not fetched (scripts/setup_vitagl.py)")
        known = SETUP.makefile_flags(makefile)
        for profile in SETUP.PROFILES:
            SETUP.check_known(SETUP.resolve_flags(profile, {}), known)


class MainTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk with spaces"
        for f in ("share/vita.toolchain.cmake", "bin/arm-vita-eabi-gcc" + SETUP.EXE,
                  "arm-vita-eabi/include/taihen.h", "arm-vita-eabi/lib/libtaihen_stub.a"):
            (self.sdk / f).parent.mkdir(parents=True, exist_ok=True)
            (self.sdk / f).touch()
        patcher = patch.object(SETUP, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.output = io.StringIO()

    def call(self, *args):
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.output):
            return SETUP.main(["--vitasdk", str(self.sdk), *args])

    def test_print_flags_needs_no_sdk(self):
        with contextlib.redirect_stdout(self.output):
            self.assertEqual(SETUP.main(["--print-flags", "--profile", "debug", "--flags", "NO_SPLASHSCREEN=1"]), 0)
        self.assertEqual(self.output.getvalue().strip(), "HAVE_SHARK_LOG=1 LOG_ERRORS=1 NO_SPLASHSCREEN=1")

    def test_rejected_flag_fails_before_any_command(self):
        with patch.object(SETUP.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--flags", "DRAW_SPEEDHACK=1")
            run.assert_not_called()

    def test_missing_taihen(self):
        (self.sdk / "arm-vita-eabi/lib/libtaihen_stub.a").unlink()
        with patch.object(SETUP.subprocess, "run") as run, self.assertRaises(SETUP.SetupError) as error:
            self.call()
        run.assert_not_called()
        self.assertIn("taihen", str(error.exception))

    def test_up_to_date_does_nothing(self):
        prefix = self.root / SETUP.DEFAULT_DEST / "install"
        (prefix / "lib").mkdir(parents=True)
        for lib in ("vitaGL", "vitashark", "mathneon", "SceShaccCgExt"):
            (prefix / "lib" / f"lib{lib}.a").touch()
        stamp = {"pins": {n: c for n, (_, c) in SETUP.PINS.items()}, "profile": "release",
                 "flags": SETUP.resolve_flags("release", {}), "vitasdk": str(self.sdk.resolve()), "gcc": "15.2.0"}
        (prefix / "build.json").write_text(json.dumps(stamp))
        with patch.object(SETUP, "check_tools"), patch.object(SETUP, "toolchain_version", return_value="15.2.0"), \
                patch.object(SETUP, "fetch") as fetch:
            self.assertEqual(self.call(), 0)
            fetch.assert_not_called()
            self.assertIn("up to date", self.output.getvalue())
            # Another profile (or pin, or toolchain) rebuilds.
            fetch.side_effect = SETUP.SetupError("stop")
            with self.assertRaises(SETUP.SetupError):
                self.call("--profile", "debug")
            fetch.assert_called()


if __name__ == "__main__":
    unittest.main()
