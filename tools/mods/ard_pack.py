#!/usr/bin/env python3
"""Build the archive records a mod needs, so the runtime never has to compress anything.

The game reads an archive file as: 48 bytes of xbc1 header at the record offset, then "comp" bytes of zlib
payload right after it. An override therefore only has to put the same 48+comp bytes at the same relative
offsets - which is what this tool writes, one file per replaced file:

    mods/<mod>/.ard/<name>.rec

Each .rec is the original header with "unc" set to the mod file size and "comp" kept, followed by the zlib
stream padded to "comp" with zeros. The header keeps the original checksum: it is not crc32 or adler32 of the
data (measured 2026-11-22), so it is treated as the engine's own asset id and left alone.

If the mod file does not compress into the original comp, the record is NOT written and the file is reported -
a silent truncation would corrupt the asset.

    python tools/mods/ard_pack.py --mod "<mod folder>" --catalog work/mods/assets.tsv
"""
import argparse
import csv
import json
import struct
import sys
import zlib
from pathlib import Path

import arh_index

HDR = 0x30


def load_catalog(path):
    """name (lower case) -> list of (offset, unc, comp, checksum) records."""
    out = {}
    with path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if row["source"] != "ard":
                continue
            name = row["path"].replace("\\", "/").lstrip("/").lower()
            try:
                rec = (int(row["offset"] or 0), int(row["unc"] or 0), int(row["comp"] or 0), int(row["checksum"] or 0))
            except ValueError:
                continue
            out.setdefault(name, []).append(rec)
    return out


def build_header(orig48, unc, comp):
    h = bytearray(orig48[:HDR].ljust(HDR, b"\x00"))
    struct.pack_into("<I", h, 0x08, unc)
    struct.pack_into("<I", h, 0x0C, comp)
    return bytes(h)


def build_xbc1_header(name, unc, comp, checksum):
    """Build a 48-byte (0x30) xbc1 record header matching tools/shader/cache/ard_index.py."""
    name_bytes = name.encode("utf-8")[:27]
    return struct.pack("<4sIIII28s", b"xbc1", 1, unc, comp, checksum, name_bytes)


def append_record(out_dir, header_payload, tail_off):
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "append.bin"
    with path.open("ab") as f:
        if tail_off == 0 and path.stat().st_size:
            f.truncate(0)                      # a fresh build: start the tail over
        f.write(header_payload)
    return path


def load_arh_catalog(arh_bytes, cat_records):
    """Map normalized full bf2.arh path -> list of (idx, offset, unc, comp, checksum, flags)."""
    chk_by_off = {}
    for recs in cat_records.values():
        for off, _, _, ck in recs:
            chk_by_off.setdefault(off, ck)
    out = {}
    for ent in arh_index.load_arh_entries(arh_bytes):
        key = ent["path"].replace("\\", "/").lstrip("/").lower()
        chk = chk_by_off.get(ent["offset"], 0)
        out.setdefault(key, []).append((ent["idx"], ent["offset"], ent["unc"], ent["comp"], chk, ent["flags"]))
    return out


def patch_index_at(arh_bytes, entry_idx, new_offset, comp, unc):
    """Repoint a known arh1 entry index directly without ambiguous offset scanning."""
    if len(arh_bytes) < 0x24 or arh_bytes[:4] != b"arh1":
        return None
    ent_off, ent_cnt = struct.unpack_from("<II", arh_bytes, 0x1C)
    if entry_idx < 0 or entry_idx >= ent_cnt:
        return None
    pos = ent_off + entry_idx * 24
    if pos + 24 > len(arh_bytes):
        return None
    out = bytearray(arh_bytes)
    struct.pack_into("<QII", out, pos, new_offset, comp, unc)
    return bytes(out), pos


def patch_index(arh_bytes, orig_offset, new_offset, comp, unc):
    """Find the 24-byte entry by its 8-byte archive offset and repoint it.

    The entry carries offset at +0, comp at +8 and unc at +12 (measured: the font record's entry holds 0xef6fe090,
    96723 and 942194). The rest of the entry - flags and a serial - is left alone.
    """
    if len(arh_bytes) >= 0x24 and arh_bytes[:4] == b"arh1":
        ent_off, ent_count = struct.unpack_from("<II", arh_bytes, 0x1C)
        end = min(len(arh_bytes) - 23, ent_off + ent_count * 24)
        for pos in range(ent_off, end, 24):
            o, c, u = struct.unpack_from("<QII", arh_bytes, pos)
            if o == orig_offset and 0 < c <= u:
                out = bytearray(arh_bytes)
                struct.pack_into("<QII", out, pos, new_offset, comp, unc)
                return bytes(out), pos
        return None
    needle = struct.pack("<Q", orig_offset)
    at = -1
    start = 0
    while True:
        start = arh_bytes.find(needle, start)
        if start < 0:
            break
        # an entry: the next 4 bytes are a comp that must be > 0 and <= the unc that follows
        if start + 24 <= len(arh_bytes):
            c, u = struct.unpack_from("<II", arh_bytes, start + 8)
            if 0 < c <= u:
                at = start
                break
        start += 1
    if at < 0:
        return None
    out = bytearray(arh_bytes)
    struct.pack_into("<Q", out, at, new_offset)
    struct.pack_into("<I", out, at + 8, comp)
    struct.pack_into("<I", out, at + 12, unc)
    return bytes(out), at


def main(argv=None):
    ap = argparse.ArgumentParser(description="Pack a mod's archive replacements into .ard records (M2).")
    ap.add_argument("--mod", required=True, help="the mod folder (the one holding romfs/)")
    ap.add_argument("--catalog", default=str(Path(__file__).resolve().parents[2] / "work" / "mods" / "assets.tsv"))
    ap.add_argument("--level", type=int, default=9)
    ap.add_argument("--force-append", action="store_true", help="append even when the record would fit (testing)");
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--arh", default="", help="bf2.arh; required when a file has to be appended past the archive")
    ap.add_argument("--ard-size", type=int, default=0, help="the real bf2.ard size; appended records are placed after it")
    a = ap.parse_args(argv)

    mod = Path(a.mod)
    cat = Path(a.catalog)
    if not mod.is_dir():
        raise SystemExit("not a directory: " + str(mod))
    if not cat.is_file():
        raise SystemExit("no catalog at " + str(cat) + " - run tools/mods/asset_index.py first")
    catalog = load_catalog(cat)

    root = mod / "romfs"
    if not root.is_dir():
        root = mod
    out_dir = mod / ".ard"
    written = 0
    arh = Path(a.arh).read_bytes() if a.arh else None
    arh_catalog = load_arh_catalog(arh, catalog) if arh is not None else {}
    arh_dirty = False
    append_pos = 0
    if not a.dry_run:
        for stale in ((mod / ".ard" / "append.bin"), (mod / "romfs" / "bf2.arh")):
            try:
                stale.unlink()
            except FileNotFoundError:
                pass
    too_big = 0
    missing = 0
    same = 0
    report = []
    for p in sorted(x for x in root.rglob("*") if x.is_file()):
        rel = p.relative_to(root).as_posix()
        key = rel.lower()
        if key == "bf2.arh":
            continue
        arh_recs = arh_catalog.get(key)
        if arh_recs:
            data = p.read_bytes()
            # Prefer arh1 full-path match: separates localized/same-tail files (e.g. /bdat/cn vs /bdat/gb).
            # Raw uncompressed archive entries (flags == 0, unc == 0) always append cleanly as raw tail slices.
            raw_mode = all(r[5] == 0 and r[2] == 0 for r in arh_recs)
            if raw_mode:
                if not a.ard_size or arh is None:
                    too_big += 1
                    report.append({"file": rel, "status": "requires-ard-size"})
                    continue
                if len(data) == arh_recs[0][3]:
                    same += 1
                tail_off = append_pos
                new_off = a.ard_size + tail_off
                entries = []
                patched_arh = arh
                for e_idx, _, _, _, _, _ in arh_recs:
                    patched = patch_index_at(patched_arh, e_idx, new_off, len(data), 0)
                    if patched is None:
                        entries = []
                        break
                    patched_arh = patched[0]
                    entries.append(patched[1])
                if not entries:
                    too_big += 1
                    report.append({"file": rel, "status": "index-entry-not-found"})
                    continue
                append_record(out_dir, data, tail_off)
                append_pos += len(data)
                arh = patched_arh
                arh_dirty = True
                written += 1
                report.append({"file": rel, "status": "appended-raw", "size": len(data), "at": new_off, "entries": entries})
                continue
            uncs = {r[2] for r in arh_recs}
            if len(uncs) > 1:
                too_big += 1
                report.append({"file": rel, "status": "multi-part", "parts": len(arh_recs), "sizes": sorted(uncs)})
                continue
            _, offset, unc, comp, checksum, _ = min(arh_recs, key=lambda r: r[3])
            recs = [(r[1], r[2], r[3], r[4]) for r in arh_recs]
            idx_list = [r[0] for r in arh_recs]
        else:
            recs = catalog.get(key)
            if not recs:
                missing += 1
                report.append({"file": rel, "status": "not-in-archive"})
                continue
            uncs = {r[1] for r in recs}
            if len(uncs) > 1:
                too_big += 1
                report.append({"file": rel, "status": "multi-part", "parts": len(recs), "sizes": sorted(uncs)})
                continue
            offset, unc, comp, checksum = min(recs, key=lambda r: r[2])
            data = p.read_bytes()
            idx_list = None
        if len(data) == unc:
            same += 1
        z = zlib.compress(data, a.level)
        if len(z) > comp or a.force_append:
            if arh is None or not a.ard_size:
                too_big += 1
                report.append({"file": rel, "status": "does-not-fit", "mod": len(data), "zlib": len(z), "room": comp})
                continue
            # M2b: the record is too big for the original slot, so it goes after the archive and its index entry is
            # repointed. The entry read from the arh is patched in a copy the mod ships as romfs/bf2.arh.
            tail_off = append_pos
            new_off = a.ard_size + tail_off
            if len(z) > len(data):
                too_big += 1
                report.append({"file": rel, "status": "comp-gt-unc", "zlib": len(z), "unc": len(data)})
                continue
            header = build_xbc1_header(rel, len(data), len(z), checksum)
            record = header + z
            entries = []
            patched_arh = arh
            if idx_list is not None:
                for e_idx in idx_list:
                    patched = patch_index_at(patched_arh, e_idx, new_off, len(z), len(data))
                    if patched is None:
                        entries = []
                        break
                    patched_arh = patched[0]
                    entries.append(patched[1])
            else:
                for r_off, _, _, _ in recs:
                    patched = patch_index(patched_arh, r_off, new_off, len(z), len(data))
                    if patched is None:
                        entries = []
                        break
                    patched_arh = patched[0]
                    entries.append(patched[1])
            if not entries:
                too_big += 1
                report.append({"file": rel, "status": "index-entry-not-found", "offset": offset})
                continue
            append_record(out_dir, record, tail_off)
            append_pos += len(record)
            arh = patched_arh
            arh_dirty = True
            written += 1
            report.append({"file": rel, "status": "appended", "zlib": len(z), "at": new_off, "entries": entries})
            continue
        if a.dry_run:
            written += 1
            report.append({"file": rel, "status": "dry-run-ok", "zlib": len(z), "room": comp})
            continue
        header = build_xbc1_header(rel, len(data), comp, checksum)
        payload = z + bytes(comp - len(z))
        out = out_dir / (key + ".rec")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(header + payload)
        written += 1
        report.append({"file": rel, "status": "packed", "zlib": len(z), "room": comp})

    if arh_dirty and arh is not None:
        romfs = mod / "romfs"
        romfs.mkdir(parents=True, exist_ok=True)
        (romfs / "bf2.arh").write_bytes(arh)
        print("  index: romfs/bf2.arh patched (" + str(len(arh)) + " bytes)")

    print("mod: " + str(mod))
    print("  packed %d, does not fit %d, not in the archive %d, unchanged %d" % (written, too_big, missing, same))
    for r in report:
        if r["status"] == "does-not-fit":
            print("  DOES NOT FIT " + r["file"] + ": zlib %d > room %d (mod file %d)" % (r["zlib"], r["room"], r["mod"]))
    results = Path(__file__).resolve().parents[2] / "work" / "mods" / "ard-pack.json"
    results.parent.mkdir(parents=True, exist_ok=True)
    results.write_text(json.dumps({"mod": str(mod), "report": report}, indent=1) + "\n", encoding="utf-8")
    return 1 if too_big else 0


if __name__ == "__main__":
    sys.exit(main())
