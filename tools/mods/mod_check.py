#!/usr/bin/env python3
"""M0.5: does this mod fit this dump? One line per file.

    python tools/mods/mod_check.py --mod "<mod folder>" --catalog work/mods/assets.tsv

Walks the mod's files (a `romfs/` inside it is treated as the root, so both a raw overlay and an emulator
layout work), looks each path up in the asset catalog from M0, and says where the game reads that path from and
whether the bytes match.

Why it exists: most XC2 mods were made for the Switch 1 release, and the Switch 2 edition's romfs is not the
same tree. A player who drops an NS1 mod in deserves a list - this file exists / this one does not / this one
lives inside bf2.ard and needs M2 - instead of a silent no-op.

Statuses, per file:

    same            the path exists and size and sha256 match the dump (the mod does not change it)
    differs         the path exists, content differs (what a working replacement looks like)
    only-archive    the path exists only inside bf2.ard; without M2 an overlay cannot replace it
    absent          the path does not exist in this dump (an NS1 layout, a different version, or a typo)
    no-hash         the dump side cannot be hashed (above the catalog's --hash-limit-mb)

Exit code: 0 when every file is same/differs/only-archive, 1 when any file is absent, 2 on a usage error.
"""
import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def mod_root(mod: Path, app_id: str) -> Path:
    """The same layouts the runtime accepts (fs_romfs.cpp), so the check and the game agree on what a mod is."""
    for cand in (mod / "romfs", mod / "atmosphere" / "contents" / app_id / "romfs",
                 mod / "contents" / app_id / "romfs", mod / app_id / "romfs"):
        if cand.is_dir():
            return cand
    return mod


def norm(rel: str) -> str:
    """One spelling per path: '/' separators, no leading slash, lower case.

    The archive stores names with a leading '//' ("//ma61a/cache/texture/RK050"), a mod folder cannot, and the
    console is case-sensitive while Windows is not. Normalising lets the two meet; the report keeps the original.
    """
    return rel.replace("\\", "/").lstrip("/").lower()


def load_catalog(path: Path) -> dict:
    """normalised path -> list of rows (an archive path can have several records; a loose file is unique)."""
    out: dict = {}
    with path.open(encoding="utf-8") as f:
        rd = csv.DictReader(f, delimiter="\t")
        for row in rd:
            out.setdefault(norm(row["path"]), []).append(row)
    return out


def classify(mod_file: Path, rel: str, catalog: dict) -> dict:
    size = mod_file.stat().st_size
    rows = catalog.get(norm(rel))
    rec = {"path": rel, "mod_size": size, "status": "", "where": "", "dump_size": "", "note": ""}
    if not rows:
        rec["status"] = "absent"
        rec["note"] = "no such path in this dump"
        return rec
    sources = sorted({r["source"] for r in rows})
    rec["where"] = ",".join(sources)
    loose = [r for r in rows if r["source"] != "ard"]
    if not loose:
        rec["status"] = "only-archive"
        rec["dump_size"] = rows[0]["unc"] or rows[0]["size"]
        rec["note"] = f"{len(rows)} archive record(s); an overlay alone cannot replace it"
        return rec
    r = loose[0]
    rec["dump_size"] = r["size"]
    if r["size"] and int(r["size"]) != size:
        rec["status"] = "differs"
        rec["note"] = "size differs from the dump"
        return rec
    if r.get("sha256"):
        try:
            digest = sha256_of(mod_file)
        except OSError as e:
            rec["status"] = "no-hash"
            rec["note"] = str(e)
            return rec
        rec["status"] = "same" if digest == r["sha256"] else "differs"
        if rec["status"] == "differs":
            rec["note"] = "same size, different bytes"
        return rec
    rec["status"] = "no-hash"
    rec["note"] = "the catalog did not hash this file"
    return rec


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Check a mod folder against a dump's asset catalog (M0.5).")
    ap.add_argument("--mod", required=True, help="the mod folder (it may hold a romfs/ subdirectory)")
    ap.add_argument("--catalog", default=str(ROOT / "work" / "mods" / "assets.tsv"))
    ap.add_argument("--out", default=str(ROOT / "work" / "mods" / "mods-report.json"))
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    ap.add_argument("--app-id", default="0100E95004038000", help="the application id the emulator layouts are filed under")
    a = ap.parse_args(argv)

    mod = Path(a.mod)
    cat_path = Path(a.catalog)
    if not mod.is_dir():
        raise SystemExit(f"not a directory: {mod}")
    if not cat_path.is_file():
        raise SystemExit(f"no catalog at {cat_path} - run tools/mods/asset_index.py first")

    root = mod_root(mod, a.app_id.lower())
    catalog = load_catalog(cat_path)
    files = sorted(p for p in root.rglob("*") if p.is_file())
    if not files:
        raise SystemExit(f"no files under {root}")

    results = [classify(p, p.relative_to(root).as_posix(), catalog) for p in files]
    counts: dict = {}
    where: dict = {}
    for r in results:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
        if r["where"]:
            where[r["where"]] = where.get(r["where"], 0) + 1

    report = {
        "mod": str(mod),
        "root": str(root),
        "catalog": str(cat_path),
        "files": len(results),
        "status": dict(sorted(counts.items(), key=lambda kv: -kv[1])),
        "where": dict(sorted(where.items(), key=lambda kv: -kv[1])),
        "absent": [r["path"] for r in results if r["status"] == "absent"][:50],
        "only_archive": [r["path"] for r in results if r["status"] == "only-archive"][:50],
        "files_detail": results,
    }
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")

    if a.json:
        print(json.dumps({k: report[k] for k in ("mod", "files", "status", "where", "absent", "only_archive")}, indent=1))
    else:
        print(f"mod: {mod}  ({len(results)} file(s))")
        print("  status: " + ", ".join(f"{k} {v}" for k, v in report["status"].items()))
        print("  found in: " + (", ".join(f"{k} {v}" for k, v in report["where"].items()) or "(nothing)"))
        for p in report["absent"][:8]:
            print(f"  absent       : {p}")
        if len(report["absent"]) > 8:
            print(f"  ... {len(report['absent']) - 8} more absent")
        for p in report["only_archive"][:5]:
            print(f"  only-archive : {p}")
        print(f"  report: {out}")
    return 1 if report["absent"] else 0


if __name__ == "__main__":
    sys.exit(main())
