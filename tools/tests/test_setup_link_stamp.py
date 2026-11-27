#!/usr/bin/env python3
"""The setup keeps an existing xc2_boot.exe only when it still matches the runtime it was linked against.

A release upgrade replaces runtime/xc2_rt.dll and keeps the exe (step_link_package), but the exe carries the lifted
game code: pairing a new runtime with an exe from an older release is a mixed build, and on the 0.2.1 video a player
reported exactly that - rendering faults that went away after a fresh build.
"""

import importlib.util
import json
import pathlib
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_setup():
    spec = importlib.util.spec_from_file_location("xc2_setup", ROOT / "tools" / "setup" / "xc2_setup.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LinkStampTest(unittest.TestCase):
    def setUp(self):
        self.setup = load_setup()
        self.dir = pathlib.Path(tempfile.mkdtemp())
        self.dll = self.dir / "xc2_rt.dll"
        self.dll.write_bytes(b"x" * 128)
        self.stamp = self.dir / "link-stamp.json"

    def test_missing_stamp_means_relink(self):
        """An install from before the stamp existed has no proof, so it relinks once."""
        self.assertFalse(self.setup.link_stamp_matches(self.stamp, self.setup.runtime_signature(self.dll)))

    def test_matching_stamp_keeps_the_exe(self):
        sig = self.setup.runtime_signature(self.dll)
        self.stamp.write_text(json.dumps(sig), encoding="utf-8")
        self.assertTrue(self.setup.link_stamp_matches(self.stamp, sig))

    def test_replaced_runtime_forces_relink(self):
        sig = self.setup.runtime_signature(self.dll)
        self.stamp.write_text(json.dumps(sig), encoding="utf-8")
        time.sleep(1.1)                      # the file system stamps mtimes to the second on some volumes
        self.dll.write_bytes(b"y" * 256)     # what an upgrade does: new runtime, same exe
        self.assertFalse(self.setup.link_stamp_matches(self.stamp, self.setup.runtime_signature(self.dll)))

    def test_install_signature_prefers_the_package_copy(self):
        runtime = self.dir / "runtime"
        runtime.mkdir()
        (runtime / "xc2_rt.dll").write_bytes(b"z" * 64)
        self.assertEqual(self.setup.install_runtime_signature(self.dir)["size"], 64)

    def test_install_signature_falls_back_to_the_one_beside_the_exe(self):
        self.assertEqual(self.setup.install_runtime_signature(self.dir)["size"], 128)

    def test_install_without_a_runtime_has_no_signature(self):
        empty = pathlib.Path(tempfile.mkdtemp())
        self.assertIsNone(self.setup.install_runtime_signature(empty))

    def test_missing_runtime_is_not_a_signature(self):
        self.assertIsNone(self.setup.runtime_signature(self.dir / "nope.dll"))
        self.assertFalse(self.setup.link_stamp_matches(self.stamp, None))

    def test_stale_prefill_marker_rechecks_real_path_after_upgrade(self):
        """Regression for #83: after a runtime upgrade, step_shaders_package must check build/prefilled, not .stale."""
        install = pathlib.Path(tempfile.mkdtemp())
        (install / "runtime").mkdir(parents=True)
        (install / "runtime" / "xc2_rt.dll").write_bytes(b"new-runtime" * 32)
        spirv = install / "shader_cache" / "spirv"
        spirv.mkdir(parents=True)
        (spirv / "results.tsv").write_text("ok\n", encoding="utf-8")
        (spirv / "0001.spv").write_bytes(b"\x03\x02\x23\x07")
        (install / "build").mkdir(parents=True)
        (install / "build" / "prefilled").write_text("old\n", encoding="utf-8")
        (install / "build" / "prefill-stamp.json").write_text(json.dumps({"size": 1, "mtime": 1}), encoding="utf-8")

        class FakeRunner:
            dry = False
            def __init__(self):
                self.logs = []
                self.ended = None
            def step(self, name): return 0.0
            def log(self, msg): self.logs.append(msg)
            def note_progress(self, msg): pass
            def run(self, *args, **kwargs): return 0
            def end_step(self, name, started, ok, note=""):
                self.ended = (name, ok, note)
                return ok

        runner = FakeRunner()
        orig_prefill = self.setup.prefill_package
        def fake_prefill(r, repo, inst, py, harness, sp, jobs=0):
            (inst / "build" / "prefilled").write_text("0 collected: nothing was missing\n", encoding="utf-8")
            return ""
        self.setup.prefill_package = fake_prefill
        try:
            ok = self.setup.step_shaders_package(runner, ROOT, install, pathlib.Path("python"), jobs=2)
        finally:
            self.setup.prefill_package = orig_prefill

        self.assertTrue(ok)
        self.assertTrue((install / "build" / "prefilled").is_file())
        self.assertTrue(self.setup.link_stamp_matches(
            install / "build" / "prefill-stamp.json",
            self.setup.install_runtime_signature(install),
        ))


if __name__ == "__main__":
    unittest.main()
