#!/usr/bin/env python3
"""Unit tests for pure-Python shader & CI helpers under tools/ (no game dump required)."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shader"))
sys.path.insert(0, str(ROOT / "tools" / "shader" / "cache"))
sys.path.insert(0, str(ROOT / "tools" / "ci"))

import ard_index  # noqa: E402
import check_invariants  # noqa: E402
import patch_ds_samplers  # noqa: E402


class ShaderAndCiToolsTests(unittest.TestCase):
    def test_spirv_mod_parses_minimal_header_and_opcodes(self):
        # Minimal valid SPIR-V binary: magic, version 1.0, generator 0, bound 4, schema 0
        # + OpTypeFloat %1 32  (wc=3, op=22)
        # + OpTypeVector %2 %1 4 (wc=4, op=23)
        words = [
            0x07230203, 0x00010000, 0, 4, 0,
            (3 << 16) | patch_ds_samplers.OP_TYPE_FLOAT, 1, 32,
            (4 << 16) | patch_ds_samplers.OP_TYPE_VECTOR, 2, 1, 4,
        ]
        mod = patch_ds_samplers.Mod(struct.pack(f"<{len(words)}I", *words))
        self.assertEqual(len(mod.find(patch_ds_samplers.OP_TYPE_FLOAT)), 1)
        self.assertEqual(patch_ds_samplers.scalar_of(mod, 2), 1)
        self.assertEqual(patch_ds_samplers.scalar_of(mod, 1), 1)

    def test_ard_index_looks_valid_distinguishes_zlib_from_noise(self):
        import io
        import zlib

        payload = zlib.compress(b"xbc1 test payload " * 8)
        f_good = io.BytesIO((b"\x00" * ard_index.HEADER) + payload)
        self.assertTrue(ard_index.looks_valid(f_good, 0, len(payload)))
        f_bad = io.BytesIO((b"\x00" * ard_index.HEADER) + (b"\xff\x00\x11\x22" * 16))
        self.assertFalse(ard_index.looks_valid(f_bad, 0, 64))

    def test_ci_invariants_helpers(self):
        self.assertFalse(check_invariants.check_private_not_tracked(["work/foo.txt", "README.md"])[0])
        self.assertTrue(check_invariants.check_private_not_tracked(["README.md", "docs/images/a.jpg"])[0])
        self.assertFalse(check_invariants.check_game_data(["foo/bf2.ard"])[0])
        self.assertFalse(check_invariants.check_no_extension_exefs(["xc2/game/main"])[0])
        self.assertTrue(check_invariants.check_top_level(["README.md", "docs/COMPATIBILITY.md", "tools/tests/a.py"])[0])
        self.assertFalse(check_invariants.check_top_level(["stray_file.txt"])[0])

    def test_private_path_leak_guard(self):
        scan = check_invariants.find_private_path_leaks
        self.assertTrue(scan("README.md", "C:\\Users\\alice\\Desktop\\note.txt"))
        self.assertTrue(scan("AGENTS.md", "E:\\XC2\\dev\\work"))
        self.assertTrue(scan("AGENTS.md", "E:\\XC2-onboarding.md"))
        self.assertTrue(scan("tools/build/x.ps1", "E:\\Tools\\dev\\zig"))
        self.assertTrue(scan("AGENTS.md", "run sync-to-nas-quiet.cmd"))
        self.assertTrue(scan("tools/shader/PATCHES.md", "see docs/internal/handover/47-foo.md"))
        self.assertEqual(scan("tools/setup/README.md", "install D:\\XC2 --nup D:\\dumps\\xc2.nup"), [])
        self.assertEqual(scan("launcher/src-tauri/src/update.rs", r"C:\Windows\System32"), [])
        self.assertEqual(scan("tools/analysis/vk_entry_map.py", r"C:\VulkanSDK\1.4.363.0"), [])
        self.assertEqual(scan("AGENTS.md", "grep inside docs/internal/handover/ when needed"), [])


if __name__ == "__main__":
    unittest.main()
