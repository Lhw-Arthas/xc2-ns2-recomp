# SPDX-License-Identifier: GPL-3.0-or-later
r"""Extract authoritative Driver & Blade VoiceID -> Official Name / Element mappings from XC2 bf2.arh + bf2.ard.

Reads /bdat/common.bdat and /bdat/gb/common_ms.bdat (Simplified Chinese) from the user's own
romfs-301 without modifying any game files.

Usage:
    python tools/voice/extract_bdat_speakers.py --romfs <path-to-romfs-301>
"""
import argparse
import pathlib
import struct
import zlib


def load_arh_subset(romfs: pathlib.Path, want_paths: set[str]):
    buf = (romfs / "bf2.arh").read_bytes()
    assert buf[:4] == b"arh1", "Invalid bf2.arh magic"
    str_off, str_sz, trie_off, trie_sz, ent_off, ent_cnt, key = struct.unpack_from("<7I", buf, 0x0C)
    k_trie = 0x0C0CACAC

    def xor4(data: bytes, k32: int) -> bytearray:
        kb = struct.pack("<I", k32 & 0xFFFFFFFF)
        out = bytearray(data)
        for i in range(len(out)):
            out[i] ^= kb[i & 3]
        return out

    dec_str = xor4(buf[str_off : str_off + str_sz], key ^ k_trie ^ 0xFFFFFFFF)
    dec_trie = xor4(buf[trie_off : trie_off + trie_sz], key ^ k_trie)
    trie_n = len(dec_trie) // 8
    trie_a = struct.unpack_from(f"<{trie_n*2}i", dec_trie, 0)

    found = {}
    for node in range(1, trie_n):
        a = trie_a[node * 2]
        b = trie_a[node * 2 + 1]
        if b >= 0:
            continue
        par = -b - 1
        if par < 0 or par >= trie_n:
            continue
        pa = trie_a[par * 2]
        if pa >= 0:
            continue
        edge = (-pa - 1) ^ node
        if edge < 0 or edge >= 256:
            continue
        if a > 0:
            nul = dec_str.find(0, a + 1)
            if nul < 0 or nul + 5 > len(dec_str):
                continue
            tail = dec_str[a + 1 : nul].decode("ascii", "replace")
            entry_idx = struct.unpack_from("<I", dec_str, nul + 1)[0]
        elif edge == 0 and a < 0:
            tail = ""
            entry_idx = -a - 1
        else:
            continue
        if entry_idx >= ent_cnt:
            continue

        rev = bytearray()
        cur = node
        ok = True
        for _ in range(512):
            if cur <= 0:
                break
            cb = trie_a[cur * 2 + 1]
            if cb >= 0:
                ok = False
                break
            p = -cb - 1
            if p < 0 or p >= trie_n:
                ok = False
                break
            pa2 = trie_a[p * 2]
            if pa2 >= 0:
                ok = False
                break
            ch = (-pa2 - 1) ^ cur
            if ch < 0 or ch >= 256:
                ok = False
                break
            if ch != 0:
                rev.append(ch)
            cur = p
        if not ok:
            continue
        rev.reverse()
        full = (rev.decode("ascii", "replace") + tail).lower()
        if full in want_paths:
            off, comp, unc, flags = struct.unpack_from("<QIII", buf, ent_off + entry_idx * 24)
            found[full] = (off, comp, unc, flags)
            if len(found) == len(want_paths):
                break
    return found


def read_ard_file(ard_path: pathlib.Path, entry) -> bytes:
    off, comp, unc, flags = entry
    with ard_path.open("rb") as f:
        f.seek(off)
        raw = f.read(comp + 0x30)
    if raw[:4] == b"xbc1":
        h_unc, h_comp = struct.unpack_from("<II", raw, 8)
        return zlib.decompress(raw[0x30 : 0x30 + h_comp])
    return raw[:unc]


def decrypt_bdat_table(d: bytearray, base: int):
    # BDAT v2 cipher-feedback decryption when flag bit 1 is set (+0x04 u16 flags)
    flags = struct.unpack_from("<H", d, base + 4)[0]
    if not (flags & 2):
        return
    names_off = struct.unpack_from("<H", d, base + 6)[0]
    b_off = struct.unpack_from("<H", d, base + 0x0A)[0]
    str_off = struct.unpack_from("<I", d, base + 0x18)[0]
    str_len = struct.unpack_from("<I", d, base + 0x1C)[0]
    k1 = (~d[base + 0x17]) & 0xFF
    k2 = (~d[base + 0x16]) & 0xFF

    def dec_range(start: int, end: int):
        a, b = k1, k2
        i = start
        while i < end:
            c0 = d[i]
            d[i] = c0 ^ a
            a = (a + c0) & 0xFF
            if i + 1 < end:
                c1 = d[i + 1]
                d[i + 1] = c1 ^ b
                b = (b + c1) & 0xFF
            i += 2

    dec_range(base + names_off, base + b_off)
    if str_off and str_len:
        dec_range(base + str_off, base + str_off + str_len)
    struct.pack_into("<H", d, base + 4, flags & ~2)


def parse_bdat_container(blob: bytes) -> dict[str, tuple[bytearray, int]]:
    d = bytearray(blob)
    n, total = struct.unpack_from("<II", d, 0)
    tables = {}
    for i in range(n):
        base = struct.unpack_from("<I", d, 8 + i * 4)[0]
        if d[base : base + 4] != b"BDAT":
            continue
        decrypt_bdat_table(d, base)
        name_off = struct.unpack_from("<H", d, base + 6)[0]
        end = d.find(0, base + name_off, base + name_off + 128)
        tname = d[base + name_off : end].decode("ascii", "replace")
        tables[tname] = (d, base)
    return tables


def read_table_rows(d: bytearray, base: int):
    row_sz = struct.unpack_from("<H", d, base + 8)[0]
    bkt_off = struct.unpack_from("<H", d, base + 0x0A)[0]
    bkt_n = struct.unpack_from("<H", d, base + 0x0C)[0]
    row_off = struct.unpack_from("<H", d, base + 0x0E)[0]
    cnt = struct.unpack_from("<H", d, base + 0x10)[0]
    top = struct.unpack_from("<H", d, base + 0x12)[0]

    cols = {}
    for i in range(bkt_n):
        e = struct.unpack_from("<H", d, base + bkt_off + 2 * i)[0]
        while e:
            desc, nxt, noff = struct.unpack_from("<HHH", d, base + e)
            end = d.find(0, base + noff, base + noff + 128)
            cname = d[base + noff : end].decode("ascii", "replace")
            tag = d[base + desc]
            typ = d[base + desc + 1]
            voff = struct.unpack_from("<H", d, base + desc + 2)[0]
            cols[cname] = (typ, voff, tag)
            e = nxt

    def get_val(row_idx: int, cname: str):
        typ, voff, tag = cols[cname]
        p = base + row_off + row_idx * row_sz + voff
        if typ == 7:  # string offset
            soff = struct.unpack_from("<I", d, p)[0]
            end = d.find(0, base + soff)
            return d[base + soff : end].decode("utf-8", "replace")
        if typ in (1, 4):
            return d[p]
        if typ in (2, 5):
            return struct.unpack_from("<H", d, p)[0]
        if typ in (3, 6):
            return struct.unpack_from("<I", d, p)[0]
        if typ == 8:
            return struct.unpack_from("<f", d, p)[0]
        return d[p]

    return top, cnt, cols, get_val


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True, help="Path to romfs-301 directory containing bf2.arh and bf2.ard")
    args = ap.parse_args()
    romfs = pathlib.Path(args.romfs)

    want = {"/bdat/common.bdat", "/bdat/gb/common_ms.bdat"}
    idx = load_arh_subset(romfs, want)
    print("Found in bf2.arh:", list(idx.keys()))

    common = parse_bdat_container(read_ard_file(romfs / "bf2.ard", idx["/bdat/common.bdat"]))
    ms = parse_bdat_container(read_ard_file(romfs / "bf2.ard", idx["/bdat/gb/common_ms.bdat"]))

    def load_msg(tname: str) -> dict[int, str]:
        if tname not in ms:
            return {}
        d, b = ms[tname]
        top, cnt, cols, gv = read_table_rows(d, b)
        out = {}
        for i in range(cnt):
            out[top + i] = gv(i, "name") if "name" in cols else ""
        return out

    dr_names = load_msg("chr_dr_ms") or load_msg("chr_dr_name")
    bl_names = load_msg("chr_bl_ms") or load_msg("chr_bl_name")
    print("ms chr tables:", [k for k in ms if "chr_" in k.lower()][:20])
    print(f"Loaded dr_names={len(dr_names)}, bl_names={len(bl_names)}")

    for tname, nmsg in [("CHR_Dr", dr_names), ("CHR_Bl", bl_names)]:
        if tname not in common:
            print(f"Missing {tname}")
            continue
        d, b = common[tname]
        top, cnt, cols, gv = read_table_rows(d, b)
        voice_cols = [c for c in cols if any(w in c.lower() for w in ("voice", "rsc", "name", "atr", "elem", "id"))]
        print(f"=== {tname} (top={top}, cnt={cnt}) cols: {sorted(voice_cols)} ===")
        for i in range(cnt):
            rid = top + i
            nid = gv(i, "Name") if "Name" in cols else 0
            nm = nmsg.get(nid, "")
            vc = gv(i, "Voice") if "Voice" in cols else (gv(i, "VoiceID") if "VoiceID" in cols else "")
            atr = gv(i, "Atr") if "Atr" in cols else 0
            if nm or vc:
                print(f"  row={rid:4d} name_id={nid:4d} name={nm!r:14s} voice={vc!r} atr={atr}")


if __name__ == "__main__":
    main()
