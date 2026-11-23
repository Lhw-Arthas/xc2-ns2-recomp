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

HDR = 0x30


def load_catalog(path):
    """name (lower case) -> the smallest record: (offset, unc, comp, checksum)."""
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
            if name not in out or rec[2] < out[name][2]:
                out[name] = rec
    return out


def build_header(orig48, unc, comp):
    h = bytearray(orig48[:HDR])
    struct.pack_into("<I", h, 0x08, unc)
    struct.pack_into("<I", h, 0x0C, comp)
    return bytes(h)


def append_record(out_dir, header_payload, tail_off):
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "append.bin"
    with path.open("ab") as f:
        if tail_off == 0 and path.stat().st_size:
            f.truncate(0)                      # a fresh build: start the tail over
        f.write(header_payload)
    return path


def patch_index(arh_bytes, orig_offset, new_offset, comp, unc):
    """Find the 24-byte entry by its 8-byte archive offset and repoint it.

    The entry carries offset at +0, comp at +8 and unc at +12 (measured: the font record's entry holds 0xef6fe090,
    96723 and 942194). The rest of the entry - flags and a serial - is left alone.
    """
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
    arh = None
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
        rec = catalog.get(key)
        if rec is None:
            missing += 1
            report.append({"file": rel, "status": "not-in-archive"})
            continue
        offset, unc, comp, checksum = rec
        data = p.read_bytes()
        if len(data) == unc:
            same += 1
        z = zlib.compress(data, a.level)
        if len(z) > comp or a.force_append:
            if not a.arh or not a.ard_size:
                too_big += 1
                report.append({"file": rel, "status": "does-not-fit", "mod": len(data), "zlib": len(z), "room": comp})
                continue
            # M2b: the record is too big for the original slot, so it goes after the archive and its index entry is
            # repointed. The entry read from the arh is patched in a copy the mod ships as romfs/bf2.arh.
            if arh is None:
                arh = Path(a.arh).read_bytes()
            tail_off = append_pos
            new_off = a.ard_size + tail_off
            if len(z) > len(data):
                too_big += 1
                report.append({"file": rel, "status": "comp-gt-unc", "zlib": len(z), "unc": len(data)})
                continue
            header = build_header(struct.pack("<4sIIIII", b"xbc1", 1, len(data), len(z), checksum, 0), len(data), len(z))
            record = header + z
            append_record(out_dir, record, tail_off)
            append_pos += len(record)
            patched = patch_index(arh, offset, new_off, len(z), len(data))
            if patched is None:
                too_big += 1
                report.append({"file": rel, "status": "index-entry-not-found", "offset": offset})
                continue
            arh = patched[0]
            written += 1
            report.append({"file": rel, "status": "appended", "zlib": len(z), "at": new_off, "entry": patched[1]})
            continue
        if a.dry_run:
            written += 1
            report.append({"file": rel, "status": "dry-run-ok", "zlib": len(z), "room": comp})
            continue
        header = build_header(struct.pack("<4sIIIII", b"xbc1", 1, unc, comp, checksum, 0), len(data), comp)
        payload = z + bytes(comp - len(z))
        out = out_dir / (key + ".rec")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(header + payload)
        written += 1
        report.append({"file": rel, "status": "packed", "zlib": len(z), "room": comp})

    if arh is not None:
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
