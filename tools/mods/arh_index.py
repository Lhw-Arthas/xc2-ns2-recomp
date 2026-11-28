#!/usr/bin/env python3
"""Decode the Monolith Soft 'arh1' archive index (bf2.arh) full path table.

An arh1 file stores:
    0x00  "arh1"     magic
    0x04  u32        tail / string buffer size
    0x0C  u32        string buffer offset
    0x10  u32        string buffer size
    0x14  u32        radix trie offset
    0x18  u32        radix trie size
    0x1C  u32        entry table offset
    0x20  u32        entry count (24 bytes each: <QIIII = offset, comp, unc, flags, serial)
    0x24  u32        key (used with 0x0C0CACAC to XOR-decrypt trie and string tables)

The trie + string table hold full untruncated logical paths (unlike xbc1 record headers inside
bf2.ard, which truncate names at 27 bytes and omit directories on localized files).
"""
import struct
from pathlib import Path

ARH_MAGIC = b"arh1"
TRIE_XOR = 0x0C0CACAC
ENTRY_FMT = "<QIIII"
ENTRY_SIZE = 24


def _xor4(data: bytes, key32: int) -> bytes:
    pad = struct.pack("<I", key32 & 0xFFFFFFFF)
    out = bytearray(data)
    for mod in range(4):
        k = pad[mod]
        if k:
            out[mod::4] = bytes(b ^ k for b in out[mod::4])
    return bytes(out)


def decode_arh_paths(arh_bytes: bytes) -> dict[int, str]:
    """Return {entry_idx: full_path} from an arh1 file, or {} if no trie/string table is present."""
    if len(arh_bytes) < 0x28 or arh_bytes[:4] != ARH_MAGIC:
        return {}
    _, _, _, str_off, str_sz, trie_off, trie_sz, ent_off, ent_cnt, key = struct.unpack_from("<10I", arh_bytes, 0)
    if (
        str_off < 0x28
        or str_sz < 4
        or trie_off < 0x28
        or trie_sz < 8
        or str_off + str_sz > len(arh_bytes)
        or trie_off + trie_sz > len(arh_bytes)
        or ent_off + ent_cnt * ENTRY_SIZE > len(arh_bytes)
    ):
        return {}

    dec_str = _xor4(arh_bytes[str_off : str_off + str_sz], key ^ TRIE_XOR ^ 0xFFFFFFFF)
    dec_trie = _xor4(arh_bytes[trie_off : trie_off + trie_sz], key ^ TRIE_XOR)
    trie = list(struct.iter_unpack("<ii", dec_trie[: (len(dec_trie) // 8) * 8]))

    paths: dict[int, str] = {}
    for node, (a, b) in enumerate(trie):
        if node == 0 or b >= 0:
            continue
        par = -b - 1
        if not (0 <= par < len(trie)):
            continue
        pa, _ = trie[par]
        if pa >= 0:
            continue
        edge = (-pa - 1) ^ node
        if not (0 <= edge < 256):
            continue

        if a > 0:
            nul = dec_str.find(b"\x00", a + 1)
            if nul < 0 or nul + 5 > len(dec_str):
                continue
            tail = dec_str[a + 1 : nul]
            idx = struct.unpack_from("<I", dec_str, nul + 1)[0]
        elif edge == 0 and a < 0:
            tail = b""
            idx = -a - 1
        else:
            continue

        if idx >= ent_cnt:
            continue

        cur = node
        prefix = bytearray()
        ok = True
        steps = 0
        while cur > 0:
            steps += 1
            if steps > 1024:
                ok = False
                break
            _, cb = trie[cur]
            if cb >= 0:
                ok = False
                break
            p = -cb - 1
            if not (0 <= p < len(trie)):
                ok = False
                break
            pa2, _ = trie[p]
            if pa2 >= 0:
                ok = False
                break
            ch = (-pa2 - 1) ^ cur
            if not (0 <= ch < 256):
                ok = False
                break
            if ch != 0:
                prefix.append(ch)
            cur = p
        if not ok:
            continue
        prefix.reverse()
        paths[idx] = (bytes(prefix) + tail).decode("utf-8", "replace")
    return paths


def load_arh_entries(arh_source) -> list[dict]:
    """Decode all arh1 index entries with full paths: [{idx, path, offset, comp, unc, flags, serial}, ...]."""
    data = arh_source if isinstance(arh_source, (bytes, bytearray)) else Path(arh_source).read_bytes()
    if len(data) < 0x24 or data[:4] != ARH_MAGIC:
        return []
    ent_off, ent_cnt = struct.unpack_from("<II", data, 0x1C)
    if ent_off + ent_cnt * ENTRY_SIZE > len(data):
        return []
    paths = decode_arh_paths(data)
    if not paths:
        return []
    out: list[dict] = []
    for idx in range(ent_cnt):
        path = paths.get(idx)
        if not path:
            continue
        off, comp, unc, flags, serial = struct.unpack_from(ENTRY_FMT, data, ent_off + idx * ENTRY_SIZE)
        out.append({
            "idx": idx,
            "path": path,
            "offset": off,
            "comp": comp,
            "unc": unc,
            "flags": flags,
            "serial": serial,
        })
    return out
