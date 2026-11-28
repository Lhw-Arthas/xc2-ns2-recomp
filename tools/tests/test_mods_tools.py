#!/usr/bin/env python3
"""Unit tests for pure-Python mod tooling under tools/mods/ (no game dump required)."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mods"))

import ard_pack  # noqa: E402
import arh_index  # noqa: E402
import asset_index  # noqa: E402
import exefs_patch  # noqa: E402
import mod_check  # noqa: E402


class ArdPackTests(unittest.TestCase):
    def test_build_xbc1_header_layout(self):
        hdr = ard_pack.build_xbc1_header("common/font_binary2.txt", 1024, 512, 0x12345678)
        self.assertEqual(len(hdr), ard_pack.HDR)
        magic, ver, unc, comp, checksum, raw_name = struct.unpack("<4sIIII28s", hdr)
        self.assertEqual(magic, b"xbc1")
        self.assertEqual(ver, 1)
        self.assertEqual(unc, 1024)
        self.assertEqual(comp, 512)
        self.assertEqual(checksum, 0x12345678)
        self.assertEqual(raw_name.split(b"\x00", 1)[0], b"common/font_binary2.txt")

    def test_build_xbc1_header_truncates_long_name_preserving_nul(self):
        hdr = ard_pack.build_xbc1_header("a" * 50, 10, 8, 1)
        self.assertEqual(len(hdr), ard_pack.HDR)
        self.assertEqual(hdr[-1], 0)
        self.assertEqual(hdr[20:].rstrip(b"\x00"), b"a" * 27)

    def test_patch_index_arh1_structured_and_invalid_rejected(self):
        # Construct minimal arh1 header: magic "arh1", then ent_off/ent_count at +0x1C.
        ent_off = 0x30
        arh = bytearray(ent_off + 2 * 24)
        arh[0:4] = b"arh1"
        struct.pack_into("<II", arh, 0x1C, ent_off, 2)
        struct.pack_into("<QII", arh, ent_off, 0x1000, 100, 200)
        struct.pack_into("<QII", arh, ent_off + 24, 0x2000, 300, 150)  # malformed comp > unc

        patched = ard_pack.patch_index(bytes(arh), 0x1000, 0x90000, 120, 400)
        self.assertIsNotNone(patched)
        blob, pos = patched
        self.assertEqual(pos, ent_off)
        self.assertEqual(struct.unpack_from("<QII", blob, ent_off), (0x90000, 120, 400))

        # Entry outside comp <= unc guard or unknown offset returns None
        self.assertIsNone(ard_pack.patch_index(bytes(arh), 0x2000, 0x90000, 120, 400))
        self.assertIsNone(ard_pack.patch_index(bytes(arh), 0x3000, 0x90000, 120, 400))

    def test_main_inline_m2_and_append_m2b_and_multipart_guard(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            mod_dir = tmp / "my_mod"
            romfs = mod_dir / "romfs"
            (romfs / "common").mkdir(parents=True)
            small_payload = b"hello mod world " * 4
            big_payload = bytes((i * 37) & 0xFF for i in range(4096))
            (romfs / "common" / "small.txt").write_bytes(small_payload)
            (romfs / "common" / "big.bin").write_bytes(big_payload)

            cat = tmp / "assets.tsv"
            cat.write_text(
                "source\tpath\tkind\tsize\tunc\tcomp\toffset\tchecksum\tsha256\n"
                "loose\tcommon/ignored.txt\ttext\t10\t\t\t\t\t\n"
                "ard\t//common/small.txt\ttext\t\t64\t128\t4096\t99\t\n"
                "ard\t//common/big.bin\tbinary\t\t4096\t100\t8192\t77\t\n",
                encoding="utf-8",
            )

            arh = bytearray(0x30 + 2 * 24)
            arh[0:4] = b"arh1"
            struct.pack_into("<II", arh, 0x1C, 0x30, 2)
            struct.pack_into("<QII", arh, 0x30, 4096, 128, 64)
            struct.pack_into("<QII", arh, 0x30 + 24, 8192, 100, 4096)
            arh_path = tmp / "bf2.arh"
            arh_path.write_bytes(bytes(arh))

            fake_script = tmp / "tools" / "mods" / "ard_pack.py"
            fake_script.parent.mkdir(parents=True)
            fake_script.write_text("", encoding="utf-8")

            with mock.patch.object(ard_pack, "__file__", str(fake_script)):
                rc = ard_pack.main([
                    "--mod", str(mod_dir),
                    "--catalog", str(cat),
                    "--arh", str(arh_path),
                    "--ard-size", "1000000",
                ])
            self.assertEqual(rc, 0)

            rec = mod_dir / ".ard" / "common" / "small.txt.rec"
            self.assertTrue(rec.is_file())
            self.assertEqual(len(rec.read_bytes()), ard_pack.HDR + 128)

            append_bin = mod_dir / ".ard" / "append.bin"
            patched_arh = mod_dir / "romfs" / "bf2.arh"
            self.assertTrue(append_bin.is_file())
            self.assertTrue(patched_arh.is_file())
            new_off, new_comp, new_unc = struct.unpack_from("<QII", patched_arh.read_bytes(), 0x30 + 24)
            self.assertEqual(new_off, 1000000)
            self.assertEqual(new_unc, len(big_payload))
            self.assertGreater(new_comp, 100)

            rep = json.loads((tmp / "work" / "mods" / "ard-pack.json").read_text(encoding="utf-8"))
            statuses = {r["file"]: r["status"] for r in rep["report"]}
            self.assertEqual(statuses["common/small.txt"], "packed")
            self.assertEqual(statuses["common/big.bin"], "appended")

    def test_load_catalog_keeps_duplicates_and_main_rejects_multipart(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            mod_dir = tmp / "mod" / "romfs"
            mod_dir.mkdir(parents=True)
            (mod_dir / "tex.bin").write_bytes(b"abc")
            cat = tmp / "assets.tsv"
            cat.write_text(
                "source\tpath\tkind\tsize\tunc\tcomp\toffset\tchecksum\tsha256\n"
                "ard\t//tex.bin\ttexture\t\t100\t50\t1000\t1\t\n"
                "ard\t//tex.bin\ttexture\t\t200\t80\t2000\t2\t\n",
                encoding="utf-8",
            )
            fake_script = tmp / "tools" / "mods" / "ard_pack.py"
            fake_script.parent.mkdir(parents=True)
            fake_script.write_text("", encoding="utf-8")
            with mock.patch.object(ard_pack, "__file__", str(fake_script)):
                rc = ard_pack.main(["--mod", str(tmp / "mod"), "--catalog", str(cat)])
            self.assertEqual(rc, 1)

    def test_arh1_full_path_decode_resolves_diff_unc_and_raw_entries(self):
        # Build a synthetic arh1 with trie + XOR'd strings mirroring bf2.arh:
        #   0: /bdat/cn/msg.bdat  (flags=2 compressed, a>0 tail)
        #   1: /bdat/gb/msg.bdat  (flags=2 compressed, a>0 tail)
        #   2: /bdat/cn/raw.bdat  (flags=0 uncompressed, unc=0)
        #   3: /event/a.motstm    (flags=2 compressed, edge==0 & a<0 in-trie NUL leaf)
        entries = [
            (b"/bdat/cn/msg.bdat", 0x1000, 30, 50, 2, 0),
            (b"/bdat/gb/msg.bdat", 0x2000, 40, 80, 2, 1),
            (b"/bdat/cn/raw.bdat", 0x3000, 64, 0, 0, 2),
            (b"/event/a.motstm", 0x4000, 50, 90, 2, 3),
        ]
        # Construct deterministic XOR radix trie:
        # Allocate node indices sequentially using unique xor keys per branch (pa = -(next_base + 1)).
        # Node 0 is root. For node parent with branch offset `base`, child = base ^ ch, parent.a = -(base + 1), child.b = -(parent + 1).
        nodes = {0: [0, 0]}
        next_free = 256
        str_buf = bytearray(4)

        def insert_in_trie(path_bytes, entry_idx, tail_bytes=None):
            nonlocal next_free
            # For entries with tail_bytes != None, walk prefix bytes then place leaf at final char node with a = str_off - 1.
            # For tail_bytes is None, walk all path_bytes then add NUL (0x00) edge leaf with a = -(entry_idx + 1).
            walk = path_bytes if tail_bytes is None else path_bytes[:-len(tail_bytes)]
            cur = 0
            for ch in walk:
                pa, pb = nodes[cur]
                if pa == 0:
                    # Pick base that gives unused child indices for likely ASCII/NUL bytes
                    base = next_free
                    next_free += 256
                    nodes[cur][0] = -(base + 1)
                else:
                    base = -pa - 1
                nxt = base ^ ch
                if nxt not in nodes:
                    nodes[nxt] = [0, -(cur + 1)]
                cur = nxt
            if tail_bytes is None:
                pa, _ = nodes[cur]
                if pa == 0:
                    base = next_free
                    next_free += 256
                    nodes[cur][0] = -(base + 1)
                else:
                    base = -pa - 1
                leaf = base ^ 0
                nodes[leaf] = [-(entry_idx + 1), -(cur + 1)]
            else:
                # Split last prefix byte before tail? Match arh1 convention: split at split_pos, leaf has a = tail_pos - 1
                str_buf.extend(b"X")
                tail_pos = len(str_buf)
                str_buf.extend(tail_bytes + b"\x00" + struct.pack("<I", entry_idx))
                nodes[cur][0] = tail_pos - 1

        insert_in_trie(entries[0][0], 0, b"sg.bdat")
        insert_in_trie(entries[1][0], 1, b"sg.bdat")
        insert_in_trie(entries[2][0], 2, b"aw.bdat")
        insert_in_trie(entries[3][0], 3, None)

        max_node = max(nodes) + 1
        plain_trie = bytearray(max_node * 8)
        for n, (a, b) in nodes.items():
            struct.pack_into("<ii", plain_trie, n * 8, a, b)
        struct.pack_into("<I", str_buf, 0, len(str_buf))

        key = 0x12345678
        str_off = 0x28
        trie_off = str_off + len(str_buf)
        ent_off = trie_off + len(plain_trie)
        arh = bytearray(ent_off + len(entries) * 24)
        struct.pack_into(
            "<10I", arh, 0,
            0x31687261, len(str_buf), 0, str_off, len(str_buf), trie_off, len(plain_trie), ent_off, len(entries), key,
        )
        arh[str_off:trie_off] = arh_index._xor4(bytes(str_buf), key ^ arh_index.TRIE_XOR ^ 0xFFFFFFFF)
        arh[trie_off:ent_off] = arh_index._xor4(bytes(plain_trie), key ^ arh_index.TRIE_XOR)
        for i, (_, off, comp, unc, flags, serial) in enumerate(entries):
            struct.pack_into("<QIIII", arh, ent_off + i * 24, off, comp, unc, flags, serial)

        decoded = arh_index.decode_arh_paths(bytes(arh))
        self.assertEqual(decoded[0], "/bdat/cn/msg.bdat")
        self.assertEqual(decoded[1], "/bdat/gb/msg.bdat")
        self.assertEqual(decoded[2], "/bdat/cn/raw.bdat")
        self.assertEqual(decoded[3], "/event/a.motstm")

        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            arh_path = tmp / "bf2.arh"
            arh_path.write_bytes(bytes(arh))
            # Deliberately put truncated ambiguous names into assets.tsv (diff_unc!)
            cat = tmp / "assets.tsv"
            cat.write_text(
                "source\tpath\tkind\tsize\tunc\tcomp\toffset\tchecksum\tsha256\n"
                "ard\tmsg.bdat\ttable\t\t50\t30\t4096\t111\t\n"
                "ard\tmsg.bdat\ttable\t\t80\t40\t8192\t222\t\n",
                encoding="utf-8",
            )
            mod_dir = tmp / "bdat_mod"
            (mod_dir / "romfs" / "bdat" / "gb").mkdir(parents=True)
            (mod_dir / "romfs" / "bdat" / "cn").mkdir(parents=True)
            gb_payload = bytes((i * 19) & 0xFF for i in range(512))
            cn_raw = b"raw-replacement-payload"
            (mod_dir / "romfs" / "bdat" / "gb" / "msg.bdat").write_bytes(gb_payload)
            (mod_dir / "romfs" / "bdat" / "cn" / "raw.bdat").write_bytes(cn_raw)

            fake_script = tmp / "tools" / "mods" / "ard_pack.py"
            fake_script.parent.mkdir(parents=True)
            fake_script.write_text("", encoding="utf-8")
            with mock.patch.object(ard_pack, "__file__", str(fake_script)):
                rc = ard_pack.main([
                    "--mod", str(mod_dir),
                    "--catalog", str(cat),
                    "--arh", str(arh_path),
                    "--ard-size", "200000",
                ])
            self.assertEqual(rc, 0)

            rep = {r["file"]: r for r in json.loads((tmp / "work" / "mods" / "ard-pack.json").read_text(encoding="utf-8"))["report"]}
            self.assertEqual(rep["bdat/gb/msg.bdat"]["status"], "appended")
            self.assertEqual(rep["bdat/cn/raw.bdat"]["status"], "appended-raw")
            out_arh = (mod_dir / "romfs" / "bf2.arh").read_bytes()
            # Entry 0 (cn/msg.bdat) must remain untouched; entry 1 (gb/msg.bdat) and entry 2 (cn/raw.bdat) updated.
            self.assertEqual(struct.unpack_from("<QII", out_arh, ent_off)[0], 0x1000)
            self.assertEqual(struct.unpack_from("<QII", out_arh, ent_off + 24)[0], 200000 + len(cn_raw))
            self.assertEqual(struct.unpack_from("<QII", out_arh, ent_off + 48), (200000, len(cn_raw), 0))


class ExefsPatchM3Tests(unittest.TestCase):
    @staticmethod
    def _make_nso(bid_hex: str, text: bytes, ro: bytes, data: bytes) -> bytes:
        hdr = bytearray(0x100)
        hdr[0:4] = b"NSO0"
        struct.pack_into("<I", hdr, 0x0C, 0x38)  # uncompressed, hash flags set
        bid = bytes.fromhex(bid_hex.ljust(64, "0"))
        hdr[0x40:0x60] = bid
        cursor = 0x100
        va = 0
        for i, ((ho, cso), seg) in enumerate(zip(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68)), (text, ro, data))):
            struct.pack_into("<III", hdr, ho, cursor, va, len(seg))
            struct.pack_into("<I", hdr, cso, len(seg))
            import hashlib
            hdr[0xA0 + i * 0x20 : 0xC0 + i * 0x20] = hashlib.sha256(seg).digest()
            cursor += len(seg)
            va += len(seg)
        return bytes(hdr) + text + ro + data

    def test_parse_pchtxt_and_ips32_formats(self):
        bid = "8bef68ed1eebc24bf7278040e63b83f6e417e2e2"
        txt = (
            "// comment\n"
            f"@nsobid-{bid}\n"
            "@flag offset_shift 0x100\n"
            "00000004 AAAAAAAA // before @enabled, must be ignored\n"
            "@enabled\n"
            "00000010 1F 20 03 D5 # ARM64 NOP\n"
            '00000100 "Hi"\n'
            "@disabled\n"
            "00000020 FFFFFFFF\n"
            "@enabled\n"
            "@stop\n"
            "00000030 11223344\n"
        )
        p = exefs_patch.parse_pchtxt(txt)
        self.assertEqual(p["build_id"], bid)
        self.assertEqual(p["records"], [(0x110, bytes.fromhex("1F2003D5")), (0x200, b"Hi")])

        # IPS32 + RLE record
        ips32 = (
            b"IPS32"
            + struct.pack(">IH", 0x104, 4) + b"\x01\x02\x03\x04"
            + struct.pack(">IHHB", 0x120, 0, 3, 0xAA)
            + b"EEOF"
        )
        ip = exefs_patch.parse_ips(ips32, build_id_hint=bid)
        self.assertEqual(ip["records"], [(0x104, b"\x01\x02\x03\x04"), (0x120, b"\xAA\xAA\xAA")])

        # IPS 24-bit
        ips24 = b"PATCH" + (0x108).to_bytes(3, "big") + struct.pack(">H", 2) + b"\xDE\xAD" + b"EOF"
        ip2 = exefs_patch.parse_ips(ips24, build_id_hint=bid.ljust(64, "0"))
        self.assertEqual(ip2["records"], [(0x108, b"\xDE\xAD")])

    def test_apply_patches_priority_sha_and_disable_revert(self):
        bid = "8bef68ed1eebc24bf7278040e63b83f6e417e2e2"
        text = bytearray(b"\x00" * 0x40)
        ro = bytearray(b"hello_world" + b"\x00" * 0x20)
        data = bytearray(b"\x55" * 0x10)
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            van = tmp / "exefs"
            out = tmp / "exefs-patched"
            mods = tmp / "mods"
            van.mkdir()
            (van / "main").write_bytes(self._make_nso(bid, bytes(text), bytes(ro), bytes(data)))
            (van / "sdk").write_bytes(self._make_nso("1111" * 10, b"\x11" * 0x20, b"", b""))

            # ModLow uses atmosphere/exefs_patches layout; ModHigh uses exefs/ + .ips overlapping 0x100..0x104.
            low_dir = mods / "ModLow" / "atmosphere" / "exefs_patches" / "patchA"
            high_dir = mods / "ModHigh" / "exefs"
            low_dir.mkdir(parents=True)
            high_dir.mkdir(parents=True)
            (low_dir / "a.pchtxt").write_text(
                f"@nsobid-{bid}\n@enabled\n00000100 11223344\n00000108 55667788\n",
                encoding="utf-8",
            )
            ips32 = b"IPS32" + struct.pack(">IH", 0x100, 4) + b"\x1F\x20\x03\xD5" + b"EEOF"
            (high_dir / f"{bid.upper()}.ips").write_bytes(ips32)

            # Non-matching mod should show in unmatched without failing matching patches
            wrong_dir = mods / "WrongVer" / "exefs"
            wrong_dir.mkdir(parents=True)
            (wrong_dir / "old.pchtxt").write_text("@nsobid-" + ("ab" * 20) + "\n@enabled\n00000100 00000000\n", encoding="utf-8")

            ini = tmp / "nxrt.ini"
            ini.write_text("[mods]\norder = ModHigh, ModLow\ndisabled =\n", encoding="utf-8")

            st = exefs_patch.apply_exefs_patches(van, out, mods, ini_path=ini)
            self.assertTrue(st["active"])
            self.assertEqual(len(st["unmatched"]), 1)
            patched_main = (out / "main").read_bytes()
            self.assertTrue((out / "sdk").is_file())
            self.assertTrue((out / "m3_state.json").is_file())
            # 0x100 gets ModHigh (1F2003D5), 0x108 keeps ModLow (55667788)
            self.assertEqual(patched_main[0x100:0x104], bytes.fromhex("1F2003D5"))
            self.assertEqual(patched_main[0x108:0x10C], bytes.fromhex("55667788"))
            # Segment SHA-256 at 0xA0 matches updated text segment
            import hashlib
            t_len = struct.unpack_from("<I", patched_main, 0x18)[0]
            self.assertEqual(patched_main[0xA0:0xC0], hashlib.sha256(patched_main[0x100 : 0x100 + t_len]).digest())

            # Disabling both matching mods cleans out_exefs back to inactive
            ini.write_text("[mods]\norder =\ndisabled = ModHigh, ModLow, WrongVer\n", encoding="utf-8")
            st2 = exefs_patch.apply_exefs_patches(van, out, mods, ini_path=ini)
            self.assertFalse(st2["active"])
            self.assertFalse(out.exists())

            # Out-of-bounds offset raises cleanly instead of corrupting
            with self.assertRaises(ValueError):
                exefs_patch.patch_nso_bytes((van / "main").read_bytes(), [(0x90000, b"\x00")])

    def test_m4c_static_text_cheat_patches_main_and_skips_heap(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            van = tmp / "exefs"
            mods = tmp / "mods"
            out = tmp / "out"
            van.mkdir()
            bid_main = "aa" * 20
            nso_main = self._make_nso(bid_main, b"\x00" * 64, b"\x11" * 16, b"\x22" * 16)
            (van / "main").write_bytes(nso_main)

            cheat_dir = mods / "CheatPack" / "cheats"
            cheat_dir.mkdir(parents=True)
            # 16-hex short prefix match + both a static .text cheat and a runtime Heap cheat
            (cheat_dir / f"{bid_main[:16].upper()}.txt").write_text(
                "[60 FPS Text]\n"
                "04000000 00000010 14000001\n\n"
                "[Heap Gold]\n"
                "04100000 00000020 05F5E0FF\n",
                encoding="utf-8",
            )
            (mods / ".cheats_enabled.tsv").write_text(
                "CheatPack\t60 FPS Text\nCheatPack\tHeap Gold\n", encoding="utf-8"
            )

            st = exefs_patch.apply_exefs_patches(van, out, mods)
            self.assertTrue(st["active"])
            self.assertEqual(len(st["applied"]), 1)
            self.assertEqual(st["applied"][0]["source"], f"CheatPack:{bid_main[:16].upper()}.txt#60 FPS Text")
            patched = (out / "main").read_bytes()
            self.assertEqual(patched[0x110:0x114], bytes.fromhex("01000014"))


class ModCheckAndIndexTests(unittest.TestCase):
    def test_norm_and_mod_root(self):
        self.assertEqual(mod_check.norm("//Ma61a\\Cache/Texture/RK050"), "ma61a/cache/texture/rk050")
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            ams = tmp / "atmosphere" / "contents" / "0100E95004038000" / "romfs"
            ams.mkdir(parents=True)
            self.assertEqual(mod_check.mod_root(tmp, "0100E95004038000"), ams)

    def test_classify_all_statuses(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            same_f = tmp / "same.txt"
            same_f.write_bytes(b"identical")
            diff_f = tmp / "diff.txt"
            diff_f.write_bytes(b"modified!")
            ard_f = tmp / "ard.bin"
            ard_f.write_bytes(b"xyz")
            abs_f = tmp / "missing.txt"
            abs_f.write_bytes(b"123")

            cat_path = tmp / "assets.tsv"
            same_hash = mod_check.sha256_of(same_f)
            cat_path.write_text(
                "source\tpath\tkind\tsize\tunc\tcomp\toffset\tchecksum\tsha256\n"
                f"loose\tsame.txt\ttext\t9\t\t\t\t\t{same_hash}\n"
                "loose\tdiff.txt\ttext\t9\t\t\t\t\tdeadbeef\n"
                "ard\t//ard.bin\tbinary\t\t3\t2\t4096\t1\t\n",
                encoding="utf-8",
            )
            cat = mod_check.load_catalog(cat_path)
            self.assertEqual(mod_check.classify(same_f, "same.txt", cat)["status"], "same")
            self.assertEqual(mod_check.classify(diff_f, "diff.txt", cat)["status"], "differs")
            self.assertEqual(mod_check.classify(ard_f, "ard.bin", cat)["status"], "only-archive")
            self.assertEqual(mod_check.classify(abs_f, "missing.txt", cat)["status"], "absent")

    def test_asset_index_kind_of(self):
        self.assertEqual(asset_index.kind_of("//ma61a/cache/texture/RK050"), "texture")
        self.assertEqual(asset_index.kind_of("common/bdat/menu.bdat"), "table")
        self.assertEqual(asset_index.kind_of("sound/bgm/title.wem"), "audio")


if __name__ == "__main__":
    unittest.main()
