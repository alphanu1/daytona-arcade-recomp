"""Host-side tests for Vita cross-build validation; no SDK or ROM required."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("build_vita", Path(__file__).resolve().parents[1] / "scripts/build_vita.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildVitaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk with spaces"
        (self.sdk / "share").mkdir(parents=True)
        (self.sdk / "share/vita.toolchain.cmake").touch()
        patcher = patch.object(BUILD, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.output = io.StringIO()

    def call(self, *args):
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.output):
            return BUILD.main(["--vitasdk", str(self.sdk), *args])

    def test_missing_sdk(self):
        with self.assertRaises(SystemExit):
            self.call("--vitasdk", str(self.root / "missing"))
        self.assertIn("missing VitaSDK toolchain", self.output.getvalue())

    def test_missing_generated_code(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call()
            run.assert_not_called()
        self.assertIn("host-generated code", self.output.getvalue())

    def test_reject_host_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build")

    def test_reject_generated_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build/gen/vita")

    def test_reject_nonpositive_jobs(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--jobs", "0")

    def test_compile_check_skips_rom_and_never_executes_importers(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.assertEqual(self.call("--compile-check"), 0)
        self.assertEqual(run.call_count, 2)
        configure, compile_ = run.call_args_list
        self.assertEqual(configure.args[0][0], "cmake")
        self.assertIn("-DDAYTONA_VITA_COMPILE_CHECK=ON", configure.args[0])
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={self.sdk}/share/vita.toolchain.cmake", configure.args[0])
        self.assertEqual(compile_.args[0][0:2], ["cmake", "--build"])
        self.assertEqual(configure.kwargs["env"]["VITASDK"], str(self.sdk))
        self.assertTrue(configure.kwargs["env"]["PATH"].startswith(str(self.sdk / "bin") + os.pathsep))
        self.assertIn("No linked game or VPK", self.output.getvalue())

    def test_default_optimization_enabled(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=ON", run.call_args_list[0].args[0])

    def test_reference_renderer(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--reference-renderer")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=OFF", run.call_args_list[0].args[0])

    def test_success_requires_package(self):
        for name in BUILD.REQUIRED:
            path = self.root / "build/gen" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()  # presence marker, not generated game code
        with patch.object(BUILD.subprocess, "run"):
            with self.assertRaises(RuntimeError):
                self.call()

    def test_gpu_package_rejects_missing_draw_distance_hook(self):
        for name in BUILD.REQUIRED:
            path = self.root / "build/gen" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-fast")
            run.assert_not_called()
        self.assertIn("draw-distance hook", self.output.getvalue())

    def test_cores_pinned_by_default(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--gpu-fast")
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=OFF", run.call_args_list[0].args[0])

    def test_free_core(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--gpu-gl", "--free-core")
        configure = run.call_args_list[0].args[0]
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=ON", configure)
        self.assertIn("-DDAYTONA_VITA_GPU_GL=ON", configure)
        self.assertIn("-DDAYTONA_VITA_GPU_FAST=OFF", configure)

    def test_free_core_requires_gpu_build(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--compile-check", "--free-core")
            run.assert_not_called()

    def test_renderers_are_exclusive(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--gpu-fast", "--gpu-gl")

    def test_custom_host_directory(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--host-build-dir", "host build", "--build-dir", "cross build")
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/host build/gen", run.call_args_list[0].args[0])


if __name__ == "__main__":
    unittest.main()
