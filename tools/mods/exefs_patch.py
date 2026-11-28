#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M3: apply Switch .pchtxt (IPSwitch) and .ips (IPS/IPS32) code patches to a copy of ExeFS NSO0 modules.

Why on a copy, never in place: the player's dump (and install/exefs junction) stays untouched. Active patches
from enabled mods are applied to uncompressed NSO0 segments, segment SHA-256 hashes at 0xA0..0x100 are updated,
and the patched module is written to an output ExeFS folder before lifting and linking.

Supported layouts inside each enabled mod folder (matching docs/MODS.md):
    mods/<mod>/exefs/*.{pchtxt,ips}
    mods/<mod>/atmosphere/exefs_patches/<name>/*.{pchtxt,ips}
    mods/<mod>/atmosphere/contents/<app_id>/exefs/*.{pchtxt,ips}
    mods/<mod>/contents/<app_id>/exefs/*.{pchtxt,ips}
    mods/<mod>/<app_id>/exefs/*.{pchtxt,ips}

Patch priority follows [mods] order (highest priority first; unnamed mods follow alphabetically) and skips
mods listed in [mods] disabled. Higher-priority patches overwrite lower-priority patches on overlapping bytes.
"""
import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import struct
import sys
from pathlib import Path

NSO_MAGIC = b"NSO0"
NSO_HDR_SIZE = 0x100
APP_ID_DEFAULT = "0100E95004038000"
MODULES = ("main", "sdk", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "rtld")
_HEX_RE = re.compile(r"^[0-9a-fA-F]+$")


def _lz4_block_pure(src: bytes, usize: int) -> bytes:
    """Decode one raw LZ4 block (fallback when the lz4 package is not installed)."""
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i : i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0:
            raise ValueError(f"bad LZ4 offset 0 at {i}")
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if start < 0:
            raise ValueError("LZ4 back-reference before start")
        for k in range(ml):
            dst.append(dst[start + k])
    if len(dst) != usize:
        raise ValueError(f"LZ4 size mismatch: {len(dst)} != {usize}")
    return bytes(dst)


def lz4_decompress(src: bytes, usize: int) -> bytes:
    if len(src) == usize and usize == 0:
        return b""
    try:
        import lz4.block  # type: ignore

        return lz4.block.decompress(src, uncompressed_size=usize)
    except ImportError:
        return _lz4_block_pure(src, usize)


def read_nso_info(nso_path: Path) -> tuple[str, int]:
    """Return (lowercase 64-char hex build_id, text_mem_end) from an NSO0 header, else ('', 0)."""
    with nso_path.open("rb") as f:
        hdr = f.read(0x60)
    if len(hdr) < 0x60 or hdr[:4] != NSO_MAGIC:
        return "", 0
    text_mem_off, text_size = struct.unpack_from("<II", hdr, 0x14)
    return hdr[0x40:0x60].hex(), text_mem_off + text_size


def read_nso_build_id(nso_path: Path) -> str:
    """Return lowercase 64-char hex build_id from an NSO0 header (offset 0x40..0x60), else ''."""
    return read_nso_info(nso_path)[0]


def build_id_matches(patch_bid: str, mod_bid: str) -> bool:
    """Match Switch build IDs case-insensitively; allow 40-char NSOBID prefixes (trailing zeros omitted)."""
    p = patch_bid.strip().lower()
    m = mod_bid.strip().lower()
    if len(p) < 16 or not _HEX_RE.match(p) or len(m) != 64:
        return False
    if p == m:
        return True
    if len(p) < 64 and m.startswith(p) and set(m[len(p) :]) <= {"0"}:
        return True
    if len(m) >= len(p) and m.startswith(p) and len(p) >= 40:
        return True
    return False


def _strip_comment(line: str) -> str:
    in_quote = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == '"' and (i == 0 or line[i - 1] != "\\"):
            in_quote = not in_quote
        elif not in_quote:
            if ch == "#":
                return line[:i]
            if ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
                return line[:i]
        i += 1
    return line


def _parse_value_bytes(spec: str) -> bytes:
    s = spec.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return bytes(s[1:-1], "utf-8").decode("unicode_escape").encode("utf-8")
    compact = re.sub(r"\s+", "", s)
    if not compact or len(compact) % 2 != 0 or not _HEX_RE.match(compact):
        raise ValueError(f"invalid patch value: {spec!r}")
    return bytes.fromhex(compact)


def parse_pchtxt(text: str, source: str = "<pchtxt>") -> dict:
    """Parse an IPSwitch .pchtxt file -> {build_id, records: [(nso_offset, bytes), ...], source}."""
    build_id = ""
    offset_shift = 0
    enabled = False
    records: list[tuple[int, bytes]] = []

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("@"):
            low = line.lower()
            if low.startswith("@nsobid-"):
                bid = line[8:].strip().lower()
                if not _HEX_RE.match(bid) or len(bid) < 16:
                    raise ValueError(f"{source}:{lineno}: invalid @nsobid: {line!r}")
                build_id = bid
            elif low.startswith("@flag "):
                parts = line[6:].strip().split()
                if len(parts) >= 2 and parts[0].lower() == "offset_shift":
                    offset_shift = int(parts[1], 0)
            elif low == "@enabled":
                enabled = True
            elif low == "@disabled":
                enabled = False
            elif low == "@stop":
                break
            continue

        if not enabled:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise ValueError(f"{source}:{lineno}: expected '<hex_offset> <hex_bytes>', got {line!r}")
        off_s, val_s = parts
        try:
            raw_off = int(off_s, 16)
        except ValueError as e:
            raise ValueError(f"{source}:{lineno}: bad hex offset {off_s!r}") from e
        nso_off = raw_off + offset_shift
        payload = _parse_value_bytes(val_s)
        records.append((nso_off, payload))

    if not build_id:
        raise ValueError(f"{source}: missing @nsobid-<hex> header")
    return {"build_id": build_id, "records": records, "source": source}


def parse_ips(data: bytes, build_id_hint: str = "", source: str = "<ips>") -> dict:
    """Parse IPS ('PATCH'..'EOF') or IPS32 ('IPS32'..'EEOF') -> {build_id, records, source}."""
    bid = build_id_hint.strip().lower()
    if not _HEX_RE.match(bid) or len(bid) < 16:
        raise ValueError(f"{source}: .ips filename must carry target build id hex (got {build_id_hint!r})")

    if data[:5] == b"IPS32":
        off_len, eof = 4, b"EEOF"
        pos = 5
    elif data[:5] == b"PATCH":
        off_len, eof = 3, b"EOF"
        pos = 5
    else:
        raise ValueError(f"{source}: unknown IPS magic {data[:5]!r}")

    records: list[tuple[int, bytes]] = []
    n = len(data)
    while pos < n:
        if data[pos : pos + len(eof)] == eof:
            return {"build_id": bid, "records": records, "source": source}
        if pos + off_len + 2 > n:
            raise ValueError(f"{source}: truncated IPS record at {pos}")
        offset = int.from_bytes(data[pos : pos + off_len], "big")
        pos += off_len
        size = struct.unpack_from(">H", data, pos)[0]
        pos += 2
        if size == 0:
            if pos + 3 > n:
                raise ValueError(f"{source}: truncated IPS RLE record at {pos}")
            rle_len, fill = struct.unpack_from(">HB", data, pos)
            pos += 3
            records.append((offset, bytes([fill]) * rle_len))
        else:
            if pos + size > n:
                raise ValueError(f"{source}: truncated IPS payload at {pos}")
            records.append((offset, data[pos : pos + size]))
            pos += size
    raise ValueError(f"{source}: missing {eof.decode()} footer")


def read_mods_ini(ini_path: Path | None) -> tuple[list[str], set[str]]:
    """Read [mods] order and disabled lists from nxrt.ini (preserving priority order)."""
    if not ini_path or not ini_path.is_file():
        return [], set()
    cp = configparser.ConfigParser(interpolation=None)
    try:
        cp.read(ini_path, encoding="utf-8")
    except Exception:
        return [], set()
    if not cp.has_section("mods"):
        return [], set()

    def split_csv(val: str) -> list[str]:
        return [x.strip() for x in val.split(",") if x.strip()]

    order = split_csv(cp.get("mods", "order", fallback=""))
    disabled = set(split_csv(cp.get("mods", "disabled", fallback="")))
    return order, disabled


def _mod_patch_roots(mod_dir: Path, app_id: str) -> list[Path]:
    roots: list[Path] = []
    for cand in (
        mod_dir / "exefs",
        mod_dir / "atmosphere" / "contents" / app_id / "exefs",
        mod_dir / "contents" / app_id / "exefs",
        mod_dir / app_id / "exefs",
    ):
        if cand.is_dir():
            roots.append(cand)
    atm_ep = mod_dir / "atmosphere" / "exefs_patches"
    if atm_ep.is_dir():
        for sub in sorted(atm_ep.iterdir()):
            if sub.is_dir():
                roots.append(sub)
        roots.append(atm_ep)
    return roots


def read_cheats_enabled_tsv(mods_dir: Path) -> dict[str, set[str]]:
    """Read mods/.cheats_enabled.tsv (<mod>\\t<cheat_title> lines) written by launcher M4."""
    tsv = mods_dir / ".cheats_enabled.tsv"
    if not tsv.is_file():
        return {}
    out: dict[str, set[str]] = {}
    for raw in tsv.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.rstrip("\r")
        if not line or line.startswith("#") or "\t" not in line:
            continue
        mod_name, title = line.split("\t", 1)
        m, t = mod_name.strip(), title.strip()
        if m and t:
            out.setdefault(m, set()).add(t)
    return out


def _mod_cheat_roots(mod_dir: Path, app_id: str) -> list[Path]:
    roots: list[Path] = []
    for cand in (
        mod_dir / "cheats",
        mod_dir / "atmosphere" / "contents" / app_id / "cheats",
        mod_dir / "contents" / app_id / "cheats",
        mod_dir / app_id / "cheats",
    ):
        if cand.is_dir():
            roots.append(cand)
    return roots


def _extract_pure_text_records(words: list[int], main_text_end: int) -> list[tuple[int, bytes]]:
    if not words or main_text_end <= 0:
        return []
    i = 0
    recs: list[tuple[int, bytes]] = []
    while i < len(words):
        w0 = words[i]
        op = (w0 >> 28) & 0xF
        if op != 0:
            return []
        width = (w0 >> 24) & 0xF
        region = (w0 >> 20) & 0xF
        need = 4 if width == 8 else 3
        if width not in (1, 2, 4, 8) or i + need > len(words):
            return []
        addr = ((w0 & 0xFF) << 32) | words[i + 1]
        if region != 0 or addr >= main_text_end:
            return []
        if width == 1:
            payload = bytes([words[i + 2] & 0xFF])
        elif width == 2:
            payload = struct.pack("<H", words[i + 2] & 0xFFFF)
        elif width == 4:
            payload = struct.pack("<I", words[i + 2] & 0xFFFFFFFF)
        else:
            payload = struct.pack("<Q", ((words[i + 2] & 0xFFFFFFFF) << 32) | (words[i + 3] & 0xFFFFFFFF))
        recs.append((NSO_HDR_SIZE + addr, payload))
        i += need
    return recs


def parse_cheat_code_sections(
    text: str,
    mod_name: str,
    file_name: str,
    main_bid: str,
    main_text_end: int,
    enabled_titles: set[str],
) -> list[dict]:
    """Return M3 patch dicts for enabled sections in one <bid>.txt that write purely to Main .text."""
    if not enabled_titles or not main_bid or main_text_end <= 0:
        return []
    out: list[dict] = []
    cur_title = ""
    cur_words: list[int] = []

    def flush():
        nonlocal cur_title, cur_words
        if cur_title and cur_title in enabled_titles and cur_words:
            recs = _extract_pure_text_records(cur_words, main_text_end)
            if recs:
                canon = "".join(f"{off:x}:{pl.hex()}\n" for off, pl in recs).encode("utf-8")
                out.append({
                    "build_id": main_bid,
                    "records": recs,
                    "source": f"{mod_name}:{file_name}#{cur_title}",
                    "mod": mod_name,
                    "sha256": hashlib.sha256(canon).hexdigest(),
                })
        cur_title = ""
        cur_words = []

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if line.startswith("[") and line.endswith("]") and len(line) >= 2:
            flush()
            cur_title = line[1:-1].strip()
            continue
        if not cur_title:
            continue
        code_part = line.split("#", 1)[0].split("//", 1)[0]
        for tok in code_part.split():
            if len(tok) == 8 and _HEX_RE.match(tok):
                cur_words.append(int(tok, 16))
    flush()
    return out


def discover_mod_patches(
    mods_dir: Path,
    ini_path: Path | None = None,
    app_id: str = APP_ID_DEFAULT,
    main_bid: str = "",
    main_text_end: int = 0,
) -> list[dict]:
    """Return parsed patches ordered lowest -> highest priority so last writer wins."""
    if not mods_dir.is_dir():
        return []
    present = sorted(p.name for p in mods_dir.iterdir() if p.is_dir() and not p.name.startswith("."))
    if not present:
        return []
    order, disabled = read_mods_ini(ini_path)
    cheats_enabled = read_cheats_enabled_tsv(mods_dir)
    seq: list[str] = [n for n in order if n in present]
    seq.extend(n for n in present if n not in seq)
    seq.reverse()  # lowest priority first

    patches: list[dict] = []
    for mod_name in seq:
        if mod_name in disabled:
            continue
        mod_dir = mods_dir / mod_name
        seen_files: set[Path] = set()
        for root in _mod_patch_roots(mod_dir, app_id):
            for p in sorted(x for x in root.iterdir() if x.is_file()):
                rp = p.resolve()
                if rp in seen_files:
                    continue
                seen_files.add(rp)
                suf = p.suffix.lower()
                if suf == ".pchtxt":
                    parsed = parse_pchtxt(p.read_text(encoding="utf-8", errors="replace"), source=f"{mod_name}:{p.name}")
                elif suf == ".ips":
                    parsed = parse_ips(p.read_bytes(), build_id_hint=p.stem, source=f"{mod_name}:{p.name}")
                else:
                    continue
                parsed["mod"] = mod_name
                parsed["file"] = str(p)
                parsed["sha256"] = hashlib.sha256(p.read_bytes()).hexdigest()
                if parsed["records"]:
                    patches.append(parsed)

        enabled_titles = cheats_enabled.get(mod_name)
        if enabled_titles and main_bid and main_text_end > 0:
            seen_cheats: set[Path] = set()
            mb_low = main_bid.lower()
            for root in _mod_cheat_roots(mod_dir, app_id):
                for p in sorted(x for x in root.iterdir() if x.is_file() and x.suffix.lower() == ".txt"):
                    stem = p.stem.strip().lower()
                    if len(stem) < 16 or not _HEX_RE.match(stem) or not mb_low.startswith(stem):
                        continue
                    rp = p.resolve()
                    if rp in seen_cheats:
                        continue
                    seen_cheats.add(rp)
                    patches.extend(
                        parse_cheat_code_sections(
                            p.read_text(encoding="utf-8", errors="replace"),
                            mod_name=mod_name,
                            file_name=p.name,
                            main_bid=mb_low,
                            main_text_end=main_text_end,
                            enabled_titles=enabled_titles,
                        )
                    )
    return patches


def patch_nso_bytes(nso_bytes: bytes, records: list[tuple[int, bytes]], mod_name: str = "<nso>") -> bytes:
    """Decompress NSO0 segments, apply (nso_offset, bytes) patches, and serialize uncompressed NSO0 with fresh SHA-256."""
    if len(nso_bytes) < NSO_HDR_SIZE or nso_bytes[:4] != NSO_MAGIC:
        raise ValueError(f"{mod_name}: not an NSO0 module")
    hdr = bytearray(nso_bytes[:NSO_HDR_SIZE])
    flags = struct.unpack_from("<I", hdr, 0x0C)[0]
    segs = []
    for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, moff, dsize = struct.unpack_from("<III", hdr, ho)
        csize = struct.unpack_from("<I", hdr, cso)[0]
        raw = nso_bytes[foff : foff + csize]
        if flags & (1 << i):
            raw = lz4_decompress(raw, dsize)
        elif len(raw) < dsize:
            raise ValueError(f"{mod_name}: segment {i} truncated ({len(raw)} < {dsize})")
        else:
            raw = raw[:dsize]
        segs.append([moff, dsize, bytearray(raw)])

    for nso_off, payload in records:
        if nso_off < NSO_HDR_SIZE:
            raise ValueError(f"{mod_name}: patch offset 0x{nso_off:x} targets NSO header (< 0x100)")
        mem_off = nso_off - NSO_HDR_SIZE
        end_off = mem_off + len(payload)
        placed = False
        for moff, dsize, buf in segs:
            if moff <= mem_off and end_off <= moff + dsize:
                rel = mem_off - moff
                buf[rel : rel + len(payload)] = payload
                placed = True
                break
        if not placed:
            raise ValueError(f"{mod_name}: patch [0x{nso_off:x}..0x{nso_off + len(payload):x}) does not fit any NSO segment")

    # Rebuild uncompressed NSO0 (clear LZ4 flags bits 0..2, keep segment hash check flags bits 3..5 + updated digests).
    struct.pack_into("<I", hdr, 0x0C, flags & ~0x7)
    out = bytearray(hdr)
    cursor = NSO_HDR_SIZE
    for i, ((ho, cso), (moff, dsize, buf)) in enumerate(zip(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68)), segs)):
        data = bytes(buf)
        struct.pack_into("<III", out, ho, cursor, moff, dsize)
        struct.pack_into("<I", out, cso, dsize)
        out[0xA0 + i * 0x20 : 0xC0 + i * 0x20] = hashlib.sha256(data).digest()
        out.extend(data)
        cursor += dsize
    return bytes(out)


def _mirror_vanilla(src: Path, dst: Path):
    try:
        if dst.exists():
            dst.unlink()
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def apply_exefs_patches(
    vanilla_exefs: Path,
    out_exefs: Path,
    mods_dir: Path,
    ini_path: Path | None = None,
    app_id: str = APP_ID_DEFAULT,
    dry_run: bool = False,
) -> dict:
    """Apply all active matching patches from `mods_dir` onto a copy at `out_exefs`.

    If zero patches match any module in `vanilla_exefs`, cleans `out_exefs` (when not dry_run) so callers fall back
    to `vanilla_exefs` without lifting or loading duplicate files. Returns a JSON-serializable state dict.
    """
    mod_bids: dict[str, str] = {}
    main_text_end = 0
    for m in MODULES:
        mp = vanilla_exefs / m
        if mp.is_file():
            bid, text_end = read_nso_info(mp)
            mod_bids[m] = bid
            if m == "main":
                main_text_end = text_end

    patches = discover_mod_patches(
        mods_dir,
        ini_path=ini_path,
        app_id=app_id,
        main_bid=mod_bids.get("main", ""),
        main_text_end=main_text_end,
    )

    by_mod: dict[str, list[dict]] = {m: [] for m in mod_bids}
    unmatched: list[dict] = []
    for pat in patches:
        matched_mod = next((m for m, bid in mod_bids.items() if bid and build_id_matches(pat["build_id"], bid)), None)
        if matched_mod is None:
            unmatched.append({"source": pat["source"], "build_id": pat["build_id"]})
            continue
        by_mod[matched_mod].append(pat)

    active_mods = {m: pats for m, pats in by_mod.items() if pats}
    state = {
        "active": bool(active_mods),
        "modules": {
            m: hashlib.sha256(
                "".join(f"{p['source']}\t{p['sha256']}\n" for p in by_mod[m]).encode("utf-8")
            ).hexdigest()
            if by_mod[m]
            else ""
            for m in mod_bids
        },
        "applied": [
            {"module": m, "source": p["source"], "records": len(p["records"]), "build_id": mod_bids[m]}
            for m, pats in active_mods.items()
            for p in pats
        ],
        "unmatched": unmatched,
    }

    if dry_run:
        return state

    if not active_mods:
        if out_exefs.exists():
            shutil.rmtree(out_exefs, ignore_errors=True)
        return state

    out_exefs.mkdir(parents=True, exist_ok=True)
    for item in sorted(vanilla_exefs.iterdir()):
        if not item.is_file():
            continue
        dst = out_exefs / item.name
        pats = active_mods.get(item.name)
        if pats:
            flat_recs: list[tuple[int, bytes]] = []
            for p in pats:
                flat_recs.extend(p["records"])
            patched = patch_nso_bytes(item.read_bytes(), flat_recs, mod_name=item.name)
            dst.write_bytes(patched)
        else:
            _mirror_vanilla(item, dst)
    (out_exefs / "m3_state.json").write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return state


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Apply .pchtxt/.ips ExeFS code patches from enabled mods onto an ExeFS copy (M3).")
    ap.add_argument("--exefs", required=True, help="vanilla ExeFS directory (read-only)")
    ap.add_argument("--mods", required=True, help="mods root directory")
    ap.add_argument("--out", required=True, help="output directory for patched ExeFS copy")
    ap.add_argument("--ini", default="", help="nxrt.ini ([mods] order and disabled)")
    ap.add_argument("--app-id", default=APP_ID_DEFAULT)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)

    state = apply_exefs_patches(
        Path(a.exefs),
        Path(a.out),
        Path(a.mods),
        ini_path=Path(a.ini) if a.ini else None,
        app_id=a.app_id,
        dry_run=a.dry_run,
    )
    print(json.dumps(state, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
