# SPDX-License-Identifier: GPL-3.0-or-later
r"""Offline Wwise Voice_voice.bnk / jp.pck (or en.pck) Event-to-WEM inspector & extractor for XC2 (3.0.1).

Reads the user's own local romfs-301/sound/{jp,en}.pck (AKPK), parses Voice_voice.bnk (BKHD v154,
19,125 HIRC Event -> Action -> Sound entries), and resolves any Wwise voice event name
(e.g. voice_10440111_Play) to its exact streamed .wem offset, size, and raw .wem file for
offline Whisper/ASR or manual subtitle authoring.

Examples:
    python tools/voice/extract_wwise_voices.py --pck <romfs>/sound/jp.pck \
        --from-capture voice_capture.jsonl --out out_wavs
"""
import argparse
import json
import pathlib
import struct
import subprocess


# Standard Ogg CRC32 (poly 0x04C11DB7, non-reflected, init 0)
_OGG_CRC_TABLE = []
for _i in range(256):
    _r = _i << 24
    for _ in range(8):
        _r = ((_r << 1) ^ 0x04C11DB7) if (_r & 0x80000000) else (_r << 1)
    _OGG_CRC_TABLE.append(_r & 0xFFFFFFFF)


def _ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) ^ _OGG_CRC_TABLE[((crc >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return crc


def _ogg_page(serial: int, seq: int, header_type: int, granule: int, packets: list[bytes]) -> bytes:
    lacing = bytearray()
    body = bytearray()
    for pkt in packets:
        n = len(pkt)
        while n >= 255:
            lacing.append(255)
            n -= 255
        lacing.append(n)
        body.extend(pkt)
    hdr = bytearray(struct.pack("<4sBBqIIIB", b"OggS", 0, header_type, granule, serial, seq, 0, len(lacing)))
    page = bytes(hdr) + bytes(lacing) + bytes(body)
    crc = _ogg_crc(page)
    return page[:22] + struct.pack("<I", crc) + page[26:]


def wem_opus_to_ogg(wem: bytes) -> tuple[bytes, float]:
    """Convert Wwise Opus (0x3041) RIFF/WEM to standard RFC 7845 Ogg Opus bytes and return (ogg_bytes, duration_sec)."""
    if len(wem) < 12 or wem[:4] != b"RIFF" or wem[8:12] != b"WAVE":
        raise ValueError("Not a RIFF/WAVE WEM")
    fmt_body = seek_body = data_body = None
    pos = 12
    while pos + 8 <= len(wem):
        tag = wem[pos : pos + 4]
        csz = struct.unpack_from("<I", wem, pos + 4)[0]
        chunk = wem[pos + 8 : pos + 8 + csz]
        if tag == b"fmt ":
            fmt_body = chunk
        elif tag == b"seek":
            seek_body = chunk
        elif tag == b"data":
            data_body = chunk
        pos += 8 + csz + (csz & 1)

    if not fmt_body or not seek_body or not data_body:
        raise ValueError("Missing fmt/seek/data chunk in WEM")
    wfmt, ch, sr = struct.unpack_from("<HHI", fmt_body, 0)
    if wfmt != 0x3041:
        raise ValueError(f"Unsupported WEM codec 0x{wfmt:04x} (expected 0x3041 Wwise Opus)")
    frame_samples = struct.unpack_from("<H", fmt_body, 18)[0] if len(fmt_body) >= 20 else 960
    total_samples = struct.unpack_from("<I", fmt_body, 24)[0] if len(fmt_body) >= 28 else 0
    pre_skip = struct.unpack_from("<H", fmt_body, 32)[0] if len(fmt_body) >= 34 else 312

    n_pkts = len(seek_body) // 2
    pkt_sizes = struct.unpack_from(f"<{n_pkts}H", seek_body, 0)
    pkts = []
    off = 0
    for psz in pkt_sizes:
        pkts.append(data_body[off : off + psz])
        off += psz

    dur_sec = (total_samples / float(sr)) if (total_samples and sr) else (n_pkts * frame_samples / 48000.0)
    serial = 0x58433256  # 'XC2V'
    opus_head = struct.pack("<8sBBHIhB", b"OpusHead", 1, ch, pre_skip, sr, 0, 0)
    vendor = b"xc2-ns2-recomp"
    opus_tags = struct.pack("<8sI", b"OpusTags", len(vendor)) + vendor + struct.pack("<I", 0)

    out = bytearray()
    out.extend(_ogg_page(serial, 0, 0x02, 0, [opus_head]))
    out.extend(_ogg_page(serial, 1, 0x00, 0, [opus_tags]))

    seq = 2
    granule = 0
    batch: list[bytes] = []
    for i, pkt in enumerate(pkts):
        batch.append(pkt)
        granule += frame_samples
        is_last = (i == len(pkts) - 1)
        if len(batch) >= 25 or is_last:
            g = (total_samples + pre_skip) if (is_last and total_samples > 0) else granule
            out.extend(_ogg_page(serial, seq, 0x04 if is_last else 0x00, g, batch))
            seq += 1
            batch = []

    return bytes(out), dur_sec


def fnv1_32(s: str) -> int:
    h = 2166136261
    for b in s.lower().encode("ascii"):
        h = ((h * 16777619) ^ b) & 0xFFFFFFFF
    return h


def parse_akpk(pck: bytes):
    magic, hdr_sz, ver, lang_sz, bnk_sz, strm_sz = struct.unpack_from("<4sIIIII", pck, 0)
    if magic != b"AKPK":
        raise SystemExit(f"Not an AKPK file (magic={magic!r})")

    # Banks
    off = 0x1C + lang_sz
    bnk_count = struct.unpack_from("<I", pck, off)[0]
    banks = {}
    for i in range(bnk_count):
        bid, mult, sz, boff, lang = struct.unpack_from("<IIIII", pck, off + 4 + i * 20)
        banks[bid] = (boff * mult, sz, lang)

    # Streamed WEMs
    soff = off + bnk_sz
    strm_count = struct.unpack_from("<I", pck, soff)[0]
    wems = {}
    for i in range(strm_count):
        wid, mult, sz, woff, lang = struct.unpack_from("<IIIII", pck, soff + 4 + i * 20)
        wems[wid] = (woff * mult, sz, lang)

    return banks, wems


def parse_voice_bnk(bnk: bytes, wems: dict):
    """Parse BKHD v154 HIRC Event(4) -> Action(3) -> Sound(2) -> WEM ID."""
    pos = 0
    hirc_off = hirc_sz = 0
    ver = 0
    while pos + 8 <= len(bnk):
        tag = bnk[pos : pos + 4]
        csz = struct.unpack_from("<I", bnk, pos + 4)[0]
        if tag == b"BKHD":
            ver = struct.unpack_from("<I", bnk, pos + 8)[0]
        elif tag == b"HIRC":
            hirc_off, hirc_sz = pos + 8, csz
        pos += 8 + csz

    if not hirc_off:
        raise SystemExit("HIRC chunk not found in bank")

    cnt = struct.unpack_from("<I", bnk, hirc_off)[0]
    p = hirc_off + 4
    events = {}
    actions = {}
    sounds = {}
    for _ in range(cnt):
        otype = bnk[p]
        osz = struct.unpack_from("<I", bnk, p + 1)[0]
        oid = struct.unpack_from("<I", bnk, p + 5)[0]
        body = bnk[p + 9 : p + 5 + osz]
        if otype == 4:  # Event
            acnt = body[0] if ver >= 122 else struct.unpack_from("<I", body, 0)[0]
            aoff = 1 if ver >= 122 else 4
            events[oid] = [struct.unpack_from("<I", body, aoff + i * 4)[0] for i in range(min(acnt, 8))]
        elif otype == 3:  # Action
            actions[oid] = struct.unpack_from("<I", body, 2)[0]
        elif otype == 2:  # Sound SFX/Voice: locate WEM source_id matching streamed table
            # Wwise v154 AkBankSourceData: plugin_id(u32 @ 0), stream_type(u8 @ 4), source_id(u32 @ 5)
            src_id = struct.unpack_from("<I", body, 5)[0] if len(body) >= 9 else 0
            if src_id not in wems and len(body) >= 13:
                # Fallback scan first 24 bytes for a known WEM ID
                for scan in range(0, min(20, len(body) - 4)):
                    cand = struct.unpack_from("<I", body, scan)[0]
                    if cand in wems:
                        src_id = cand
                        break
            sounds[oid] = src_id
        p += 5 + osz

    return ver, events, actions, sounds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pck", required=True, help="Path to romfs-301/sound/jp.pck or en.pck")
    ap.add_argument("--bank", default="Voice_voice", help="Bank name (default: Voice_voice)")
    ap.add_argument("--from-capture", help="Optional voice_capture.jsonl to extract")
    ap.add_argument("--events", nargs="*", default=[], help="Explicit event names to resolve/extract")
    ap.add_argument("--out", help="Output directory to write extracted audio files")
    ap.add_argument("--format", choices=["wem", "opus", "wav"], default="opus",
                    help="Output format when --out is set (default: opus; wav uses ffmpeg)")
    ap.add_argument("--ffmpeg", default="ffmpeg",
                    help="Path to ffmpeg executable (only needed for --format wav)")
    args = ap.parse_args()

    pck_path = pathlib.Path(args.pck)
    pck = pck_path.read_bytes()
    banks, wems = parse_akpk(pck)
    bid = fnv1_32(args.bank)
    if bid not in banks:
        raise SystemExit(f"Bank {args.bank} (0x{bid:08x}) not found in {pck_path}")
    boff, bsz, _ = banks[bid]
    ver, events, actions, sounds = parse_voice_bnk(pck[boff : boff + bsz], wems)
    print(f"Loaded {args.bank}.bnk (BKHD v{ver}): {len(events)} events, {len(wems)} streamed WEMs in {pck_path.name}")

    wanted = list(args.events)
    if args.from_capture:
        seen = set(wanted)
        for line in pathlib.Path(args.from_capture).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            ev = json.loads(line).get("event", "")
            if ev and ev not in seen:
                seen.add(ev)
                wanted.append(ev)

    out_dir = pathlib.Path(args.out) if args.out else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    resolved = 0
    for ev in wanted:
        h = fnv1_32(ev)
        aids = events.get(h, [])
        wem_id = 0
        for aid in aids:
            tgt = actions.get(aid, 0)
            if tgt in sounds and sounds[tgt] in wems:
                wem_id = sounds[tgt]
                break
        if wem_id:
            woff, wsz, _ = wems[wem_id]
            raw = pck[woff : woff + wsz]
            magic = raw[:4]
            dur_str = ""
            ogg_bytes = None
            if magic == b"RIFF":
                try:
                    ogg_bytes, dur = wem_opus_to_ogg(raw)
                    dur_str = f", dur={dur:.2f}s"
                except Exception:
                    pass
            resolved += 1
            print(f"  OK   {ev:24s} -> wem {wem_id} ({wsz:,} B, magic={magic!r}{dur_str})")
            if out_dir:
                if args.format == "wem" or ogg_bytes is None:
                    (out_dir / f"{ev}__{wem_id}.wem").write_bytes(raw)
                elif args.format == "opus":
                    (out_dir / f"{ev}__{wem_id}.opus").write_bytes(ogg_bytes)
                elif args.format == "wav":
                    opus_tmp = out_dir / f"{ev}__{wem_id}.opus"
                    wav_out = out_dir / f"{ev}__{wem_id}.wav"
                    opus_tmp.write_bytes(ogg_bytes)
                    subprocess.run([args.ffmpeg, "-y", "-v", "error", "-i", str(opus_tmp),
                                    "-ar", "16000", "-ac", "1", str(wav_out)], check=True)
                    opus_tmp.unlink(missing_ok=True)
        else:
            print(f"  MISS {ev:24s} (hash=0x{h:08x}, actions={len(aids)})")

    if wanted:
        print(f"Resolved {resolved}/{len(wanted)} event(s).")


if __name__ == "__main__":
    main()
