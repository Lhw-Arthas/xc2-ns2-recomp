#!/usr/bin/env python3
"""Pull every NVN2 shader block that the resource files inside bf2.ard embed.

    python tools/shader/cache/ard_index.py    --ard <romfs>/bf2.ard --out files.tsv
    python tools/shader/cache/extract_embedded.py --ard <romfs>/bf2.ard --tsv files.tsv --out blocks

The retail 3.0.1 archive does not store shaders in the six .wishp containers alone: the rest are embedded
inside ordinary resource records. Layout of an embedded group, verified against the blocks the runtime
dumped:

    control sections first, contiguous, 0xd8 apart
    code areas follow at  code_base = round_up(last control + 0xd8, 0x1000)
    each block at the previous one rounded up to 0x100

A block's identity is the first 16 hex digits of the SHA-1 of its code, which is the same hash the runtime
uses to look a shader up, so the extraction can be checked against a dump: pass --known with a manifest.tsv
from a -dump run and the summary reports how many blocks the game is known to ask for.

Measured on the retail 3.0.1 archive (75,646 records): 13,059 groups, 78,793 blocks, 12,126 distinct,
85 seconds. Of those, 12,104 are real shaders and 22 are misaligned garbage that the game never requests.

Why it is here: the earlier tool (census.py) only understands the 3.0.0 record layout. On a 3.0.1 archive
it finds almost nothing and reports zero rows as success, which reads as "this game has no shaders".
"""
import argparse
import collections
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
import struct
import sys
import threading
import time
import zlib

CONTROL_MAGIC = b"\x34\x12\x76\x98"
CONTROL_SIZE = 0xd8
CONTROL_ID = 201
NDSR_MAGIC = b"NDSR"   # ndsr.pck: an opcode stream, code before the control table
HEADER = 0x30

STAGE = {0: "vertex", 1: "fragment", 2: "geometry", 3: "tess_control", 4: "tess_eval", 5: "compute"}

MANIFEST_COLUMNS = ("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes"
                    "\tconst_off\tconst_size\tcode_sha1_16\tcode_file\tctrl_file\n")


def load_index(path):
    recs = []
    with open(path, encoding="utf-8") as f:
        f.readline()
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5:
                recs.append((int(p[0]), p[1], int(p[2]), int(p[3])))
    return recs


def load_known(path):
    """Hashes the runtime has dumped, if a dump manifest is available. Optional: it only feeds a statistic."""
    known = set()
    if not path or not os.path.exists(path):
        return known
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f.read().splitlines()[1:]:
            p = line.split("\t")
            if len(p) > 12:
                known.add(p[10])
    return known


def check_pair(ard, recs, sample=24):
    """The index and the archive have to describe each other, or every offset below is nonsense."""
    if not recs:
        return 0, 0
    step = max(1, len(recs) // sample)
    ok = bad = 0
    with open(ard, "rb") as f:
        for off, name, unc, comp in recs[::step][:sample]:
            f.seek(off + HEADER)
            try:
                if len(zlib.decompress(f.read(comp))) == unc:
                    ok += 1
                else:
                    bad += 1
            except Exception:
                bad += 1
    return ok, bad


def parse_record(data):
    """Return (groups, ndsr_record, ndsr_blocks, entries) for one inflated archive record.

    Each entry is (code_bytes, ctrl_bytes, sha1_16, row_str).
    """
    ctrls = []
    j = data.find(CONTROL_MAGIC)
    while j >= 0:
        if j + CONTROL_SIZE <= len(data) and struct.unpack_from("<I", data, j + 4)[0] == CONTROL_ID:
            ctrls.append(j)
        j = data.find(CONTROL_MAGIC, j + 4)
    if not ctrls:
        return 0, 0, 0, []

    runs, cur = [], [ctrls[0]]
    for c in ctrls[1:]:
        if c == cur[-1] + CONTROL_SIZE:
            cur.append(c)
        else:
            runs.append(cur)
            cur = [c]
    runs.append(cur)

    groups = 0
    ndsr_rec = 0
    ndsr_blk = 0
    entries = []

    def build_entry(code_off, size, ctrl_off):
        code = data[code_off:code_off + size]
        ctrl = data[ctrl_off:ctrl_off + CONTROL_SIZE]
        h = hashlib.sha1(code).hexdigest()[:16]
        st = struct.unpack_from("<H", ctrl, 0x56)[0]
        prog_end = struct.unpack_from("<I", ctrl, 0x38)[0]
        const_size = struct.unpack_from("<I", ctrl, 0x3c)[0]
        const_off = struct.unpack_from("<I", ctrl, 0x40)[0]
        row = ("run_%s\t%s\t0x0\t0x%x\t0x0\t0xd8\t0x%x\t0x%x\t0x%x\t0x%x\t%s\tcode_%s.bin\tctrl_%s.bin\n"
               % (h, STAGE.get(st, "unknown"), size, prog_end, prog_end, const_off, const_size, h, h, h))
        return (code, ctrl, h, row)

    for run in runs:
        end = run[-1] + CONTROL_SIZE
        pos = (end + 0xfff) & ~0xfff
        if pos >= len(data):
            continue
        groups += 1
        for c in run:
            cs = struct.unpack_from("<I", data, c + 0x44)[0]
            if cs == 0 or pos + cs > len(data):
                break
            entries.append(build_entry(pos, cs, c))
            pos += (cs + 0xff) & ~0xff

    # The "NDSR" containers (ndsr.pck) put the code BEFORE the control table and describe each program
    # with a 16-byte entry in an opcode stream:  [control][?][code][(size << 4) | flags]
    # The control section holds no pointer to its code, so the stream is the only way in.  Measured on the
    # retail archive: 40 entries, 28 distinct blocks, and all 28 of the blocks the runtime asked for were
    # found this way - they are exactly the ones the group rule above misses.
    if data[:4] == NDSR_MAGIC:
        lo, hi = ctrls[0], ctrls[-1] + CONTROL_SIZE
        ndsr_rec = 1
        for k in range(0, len(data) - 16, 4):
            ctrl, _unused, code_off, field = struct.unpack_from("<IIII", data, k)
            if not (lo <= ctrl < hi) or not (0 < code_off < lo):
                continue
            size = field >> 4
            if size == 0 or code_off + size > len(data):
                continue
            ndsr_blk += 1
            entries.append(build_entry(code_off, size, ctrl))
    return groups, ndsr_rec, ndsr_blk, entries


def main(argv):
    ap = argparse.ArgumentParser(description="Extract the NVN2 shader blocks embedded in bf2.ard.")
    ap.add_argument("--ard", required=True)
    ap.add_argument("--tsv", required=True, help="the record index from ard_index.py")
    ap.add_argument("--out", required=True, help="folder for the code/ctrl pairs and manifest.tsv")
    ap.add_argument("--known", default="", help="optional manifest.tsv from a -dump run, for validation")
    ap.add_argument("--jobs", type=int, default=0, help="worker threads for parallel zlib inflation (0 = auto)")
    a = ap.parse_args(argv)

    recs = load_index(a.tsv)
    if not recs:
        print("ERROR: the index is empty: %s" % a.tsv, file=sys.stderr)
        return 1
    ok, bad = check_pair(a.ard, recs)
    if bad:
        print("ERROR: %d of %d sampled records do not inflate: the index does not describe this archive."
              % (bad, ok + bad), file=sys.stderr)
        print("       regenerate it with ard_index.py --ard %s" % a.ard, file=sys.stderr)
        return 1

    known = load_known(a.known)
    print("records: %d   known blocks: %d%s" % (len(recs), len(known), "" if known else "  (no --known: validation off)"))

    os.makedirs(a.out, exist_ok=True)
    stat = collections.Counter()
    seen = set()
    manifest_rows = []
    lock = threading.Lock()
    tls = threading.local()
    handles = []
    progress_state = {"done": 0, "next_mark": 10000}
    t0 = time.time()

    def process_batch(batch_items):
        f = getattr(tls, "f", None)
        if f is None:
            f = open(a.ard, "rb")
            tls.f = f
            with lock:
                handles.append(f)
        l_groups = 0
        l_inflate_fail = 0
        l_ndsr_rec = 0
        l_ndsr_blk = 0
        l_blocks = 0
        l_known_hit = 0
        l_entries = []
        for idx, off, _name, _unc, comp in batch_items:
            f.seek(off + HEADER)
            try:
                data = zlib.decompress(f.read(comp))
            except Exception:
                l_inflate_fail += 1
                continue
            groups, ndsr_rec, ndsr_blk, entries = parse_record(data)
            l_groups += groups
            l_ndsr_rec += ndsr_rec
            l_ndsr_blk += ndsr_blk
            for seq, (code, ctrl, h, row) in enumerate(entries):
                l_blocks += 1
                if h in known:
                    l_known_hit += 1
                l_entries.append((idx, seq, code, ctrl, h, row))

        to_write = []
        new_rows = []
        with lock:
            stat["groups"] += l_groups
            stat["inflate_fail"] += l_inflate_fail
            stat["ndsr_records"] += l_ndsr_rec
            stat["ndsr_blocks"] += l_ndsr_blk
            stat["blocks"] += l_blocks
            stat["known_hit"] += l_known_hit
            for idx, seq, code, ctrl, h, row in l_entries:
                if h in seen:
                    continue
                seen.add(h)
                to_write.append((h, code, ctrl))
                new_rows.append(((idx, seq), row))
            manifest_rows.extend(new_rows)
            progress_state["done"] += len(batch_items)
            if progress_state["done"] >= progress_state["next_mark"]:
                print("  %d/%d %.0fs groups=%d blocks=%d known_hit=%d"
                      % (progress_state["done"], len(recs), time.time() - t0,
                         stat["groups"], stat["blocks"], stat["known_hit"]), flush=True)
                progress_state["next_mark"] += 10000

        for h, code, ctrl in to_write:
            with open(os.path.join(a.out, "code_%s.bin" % h), "wb") as g:
                g.write(code)
            with open(os.path.join(a.out, "ctrl_%s.bin" % h), "wb") as g:
                g.write(ctrl)

    workers = a.jobs if a.jobs > 0 else min(12, max(2, os.cpu_count() or 4))
    batch_size = 400
    indexed = [(i, off, name, unc, comp) for i, (off, name, unc, comp) in enumerate(recs)]
    batches = [indexed[i:i + batch_size] for i in range(0, len(indexed), batch_size)]
    try:
        if workers <= 1 or len(batches) <= 1:
            for b in batches:
                process_batch(b)
        else:
            with ThreadPoolExecutor(max_workers=workers) as ex:
                list(ex.map(process_batch, batches))
    finally:
        for f in handles:
            try:
                f.close()
            except OSError:
                pass

    manifest_rows.sort(key=lambda item: item[0])
    with open(os.path.join(a.out, "manifest.tsv"), "w", encoding="utf-8", newline="\n") as man:
        man.write(MANIFEST_COLUMNS)
        for _, row in manifest_rows:
            man.write(row)

    print("done %.0fs" % (time.time() - t0))
    print("  groups  : %d" % stat["groups"])
    print("  blocks  : %d   (inflate failures: %d)" % (stat["blocks"], stat["inflate_fail"]))
    if stat["ndsr_records"]:
        print("  ndsr    : %d record(s), %d program entries" % (stat["ndsr_records"], stat["ndsr_blocks"]))
    print("  distinct: %d   -> %s" % (len(seen), a.out))
    if known:
        print("  known   : %d of the distinct blocks are hashes the runtime has asked for" % stat["known_hit"])
    if not seen:
        print("ERROR: no embedded block found. Is this a 3.0.1 archive?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
