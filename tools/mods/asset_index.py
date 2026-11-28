#!/usr/bin/env python3
"""M0: one table of every file the game can read - romfs, the bf2.ard archive, and the add-on content.

    python tools/mods/asset_index.py --romfs "<dump>/base/romfs" --out work/mods

Writes (never committed - they are derived from the player's own dump):

    work/mods/assets.tsv        source, path, kind, size, unc, comp, offset, checksum, sha256
    work/mods/assets-summary.json   counts, bytes and per-kind totals, plus what was skipped and why

Why it exists: a mod that replaces a file has to know that the file exists, where it lives (loose on the
romfs, inside bf2.ard, or in an add-on pack), and what it looked like before the mod touched it. Nothing in
the runtime answers that today: the archive is scanned for shaders only, and a mod folder is an overlay the
game reads without any list of what it overrides.

The archive is read with tools/shader/cache/ard_index.py (the same scanner the shader cache uses): records
are validated by inflating a sample, so a false "xbc1" match inside compressed data is dropped.
"""
import argparse
import hashlib
import importlib.util
import json
import sys
import time
from pathlib import Path

import arh_index

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ARD_INDEX = ROOT / "tools" / "shader" / "cache" / "ard_index.py"

KIND_BY_SUFFIX = {
    ".bdat": "table",
    ".ard": "archive",
    ".arh": "archive-index",
    ".mp4": "video",
    ".usm": "video",
    ".adx": "audio",
    ".wav": "audio",
    ".wem": "audio",
    ".ogg": "audio",
    ".pcm": "audio",
    ".dds": "texture",
    ".ktx": "texture",
    ".anm": "animation",
    ".mot": "animation",
    ".moe": "animation",
    ".eff": "effect",
    ".spt": "effect",
    ".cam": "camera",
    ".map": "map",
    ".sgp": "script",
    ".bin": "binary",
    ".txt": "text",
    ".json": "text",
    ".xml": "text",
    ".csv": "text",
}

COLUMNS = ["source", "path", "kind", "size", "unc", "comp", "offset", "checksum", "sha256"]


# The archive's records are mostly named by path, not by file name: "//ma61a/cache/texture/RK050" carries no
# suffix at all, and one path can hold several records (mips, blocks). Classify by path segment first, suffix second.
KIND_BY_PATH = [
    ("/texture/", "texture"),
    ("/model/", "model"),
    ("/mdl/", "model"),
    ("/map/", "map"),
    ("/eff/", "effect"),
    ("/event/", "event"),
    ("/sound/", "audio"),
    ("/movie/", "video"),
    ("/ui/", "ui"),
    ("/menu/", "ui"),
    ("/script/", "script"),
    ("/bdat/", "table"),
]


def kind_of(name: str) -> str:
    low = name.lower()
    for needle, kind in KIND_BY_PATH:
        if needle in low:
            return kind
    return KIND_BY_SUFFIX.get(Path(name).suffix.lower(), "other")


def load_ard_index():
    spec = importlib.util.spec_from_file_location("xc2_ard_index", ARD_INDEX)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {ARD_INDEX}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def walk_loose(root: Path, source: str, rows: list, hash_limit: int, skipped: list):
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        size = p.stat().st_size
        digest = ""
        if size <= hash_limit:
            try:
                digest = sha256_of(p)
            except OSError as e:
                skipped.append([source, rel, f"unreadable: {e}"])
        else:
            skipped.append([source, rel, f"not hashed: {size} bytes > {hash_limit}"])
        rows.append({
            "source": source, "path": rel, "kind": kind_of(rel), "size": size,
            "unc": "", "comp": "", "offset": "", "checksum": "", "sha256": digest,
        })


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Build the asset catalog for a dump (M0).")
    ap.add_argument("--romfs", required=True, help="the base romfs directory (holds bf2.ard, stream/, sound/)")
    ap.add_argument("--dlc", default="", help="add-on content directory; default <romfs>/../dlc when it exists")
    ap.add_argument("--out", default=str(ROOT / "work" / "mods"), help="output directory")
    ap.add_argument("--hash-limit-mb", type=int, default=64, help="loose files up to this size get a sha256 (0 = none)")
    ap.add_argument("--no-archive", action="store_true", help="skip bf2.ard (fast run, loose files only)")
    ap.add_argument("--no-dlc", action="store_true", help="skip the add-on content")
    a = ap.parse_args(argv)

    romfs = Path(a.romfs)
    if not romfs.is_dir():
        raise SystemExit(f"not a directory: {romfs}")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    rows: list = []
    skipped: list = []
    hash_limit = a.hash_limit_mb * (1 << 20)

    walk_loose(romfs, "romfs", rows, hash_limit, skipped)
    loose_done = time.time()

    archive_rows = 0
    archive_bytes = 0
    ard = romfs / "bf2.ard"
    if not a.no_archive and ard.is_file():
        arh_by_off: dict[int, str] = {}
        arh_path = romfs / "bf2.arh"
        if arh_path.is_file():
            for ent in arh_index.load_arh_entries(arh_path):
                arh_by_off.setdefault(ent["offset"], ent["path"])
        ard_index = load_ard_index()
        records, rejected, size = ard_index.scan(str(ard))
        for off, name, unc, comp, checksum in records:
            full = arh_by_off.get(off, name)
            rows.append({
                "source": "ard", "path": full, "kind": kind_of(full), "size": comp,
                "unc": unc, "comp": comp, "offset": off, "checksum": checksum, "sha256": "",
            })
        archive_rows = len(records)
        archive_bytes = size
        if rejected:
            skipped.append(["ard", "*", f"{rejected} candidate records failed validation"])
    archive_done = time.time()

    if a.dlc:
        dlc_root = Path(a.dlc)
    else:
        dlc_root = next((c for c in (romfs / "dlc", romfs.parent / "dlc", romfs.parent.parent / "dlc") if c.is_dir()),
                        romfs.parent / "dlc")
    dlc_rows = 0
    if not a.no_dlc and dlc_root.is_dir():
        for pack in sorted(p for p in dlc_root.iterdir() if p.is_dir()):
            sub = pack / "romfs"
            if not sub.is_dir():
                sub = pack / "0" / "romfs"
            if not sub.is_dir():
                skipped.append(["dlc:" + pack.name, "*", "no romfs (tried <id>/romfs and <id>/0/romfs)"])
                continue
            before = len(rows)
            walk_loose(sub, "dlc:" + pack.name, rows, hash_limit, skipped)
            dlc_rows += len(rows) - before
    dlc_done = time.time()

    rows.sort(key=lambda r: (r["source"], r["path"].lower()))
    tsv = out / "assets.tsv"
    with tsv.open("w", encoding="utf-8", newline="") as f:
        f.write("\t".join(COLUMNS) + "\n")
        for r in rows:
            f.write("\t".join(str(r[c]) for c in COLUMNS) + "\n")

    by_source: dict = {}
    by_kind: dict = {}
    by_ext: dict = {}
    paths_by_source: dict = {}
    for r in rows:
        s = by_source.setdefault(r["source"], {"records": 0, "bytes": 0})
        s["records"] += 1
        s["bytes"] += int(r["size"] or 0)
        by_kind[r["kind"]] = by_kind.get(r["kind"], 0) + 1
        ext = Path(r["path"]).suffix.lower() or "(none)"
        by_ext[ext] = by_ext.get(ext, 0) + 1
        paths_by_source.setdefault(r["source"], set()).add(r["path"])
    for src, paths in paths_by_source.items():
        by_source[src]["paths"] = len(paths)

    summary = {
        "romfs": str(romfs),
        "dlc": str(dlc_root) if dlc_root.is_dir() and not a.no_dlc else "",
        "archive": {"file": str(ard), "records": archive_rows, "bytes": archive_bytes} if archive_rows else {},
        "files": len(rows),
        "by_source": by_source,
        "by_kind": dict(sorted(by_kind.items(), key=lambda kv: -kv[1])),
        "by_extension": dict(sorted(by_ext.items(), key=lambda kv: -kv[1])[:30]),
        "skipped": skipped[:200],
        "skipped_count": len(skipped),
        "seconds": {
            "loose": round(loose_done - t0, 1),
            "archive": round(archive_done - loose_done, 1),
            "dlc": round(dlc_done - archive_done, 1),
            "total": round(dlc_done - t0, 1),
        },
        "hash_limit_mb": a.hash_limit_mb,
        "python": sys.version.split()[0],
    }
    (out / "assets-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"catalog -> {tsv}")
    print("  rows: " + str(summary["files"]) + "  (" + ", ".join(f"{k}: {v['records']} records / {v['paths']} paths" for k, v in by_source.items()) + ")")
    print(f"  kinds: {', '.join(f'{k} {v}' for k, v in list(summary['by_kind'].items())[:8])}")
    if archive_rows:
        print(f"  bf2.ard: {archive_rows} records, {archive_bytes / (1 << 30):.1f} GiB, {summary['seconds']['archive']} s")
    print(f"  skipped: {len(skipped)} (see assets-summary.json)")
    print(f"  total: {summary['seconds']['total']} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
