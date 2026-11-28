# SPDX-License-Identifier: GPL-3.0-or-later
r"""Pack ROCm Whisper transcripts into context-aware translation batches and merge results into voice_subtitles/<lang>.json.

Subcommands:
  pack-banter   Group multi-turn post-battle banter (Vo_WinSp / Vo_WinSpIra) by conversation
                so translators preserve full multi-speaker dialogue context (~20 convs/batch).
  pack-combat   Group non-banter battle/exploration lines by speaker_id (~45 lines/batch)
                to maximize Prompt Cache hit rate and character persona consistency.
  merge         Validate & merge translated batch JSONs into voice_subtitles/<lang>.json,
                preserving existing manual entries unless --overwrite is set.
  export-raw    Directly export ja.json / en.json from transcripts.jsonl for immediate testing.
"""
import argparse
import json
import pathlib
from collections import defaultdict

SPEAKER_NAMES: dict[int, str] = {
    1: "莱克斯", 2: "尼娅", 3: "虎", 4: "凡达姆", 5: "梅勒芙", 6: "齐格", 7: "真", 8: "灭", 10: "青龙",
    11: "焰", 12: "光", 13: "灵魂", 14: "白虎", 15: "花JS", 16: "花JK", 17: "花JD",
    18: "朱雀", 19: "迦具土", 20: "彩歌", 21: "尼娅(异刃)",
    22: "斩铁", 23: "义经", 24: "弁庆", 25: "佐田彦", 26: "神威", 27: "罗候", 28: "大锤",
    31: "绵津见", 32: "红莲", 33: "力男", 34: "婆娑罗", 35: "雄黄", 36: "宫比罗", 37: "韦驮天",
    38: "萤", 39: "重音", 40: "水分", 41: "星织", 42: "时叶", 43: "石榴", 44: "蓟", 45: "七冰",
    46: "纽兹", 47: "宇迦", 48: "凛音", 49: "结", 50: "式", 51: "雷鼓", 52: "八重切", 53: "伊吹",
    54: "KOS-MOS Re:", 55: "天威", 56: "鸣", 57: "玛瑙", 58: "月", 59: "T-elos Re:",
    501: "花巴斯塔", 502: "修尔克", 503: "菲奥伦", 504: "神八", 505: "火花", 506: "艾尔玛",
    **{cb: "普通异刃" for cb in range(61, 81)},
    701: "劳拉", 702: "阿德尔", 703: "尤格", 704: "真(伊拉)", 705: "霞", 706: "光(伊拉)",
    707: "米诺奇", 708: "迦具土(伊拉)", 709: "绵津见(伊拉)",
}

# KK categories that belong on the bottom P0 subtitle track (exploration, field skills, level-up, inn, merc, pouch, weather, time, quest, post-battle/victory)
P0_BANTER_KK = {
    1, 41, 42, 43, 44, 48, 49, 51, 52, 53, 54, 55, 57, 59, 60, 61, 62, 64, 65, 67, 68, 69,
    *range(70, 99),
}
# KK categories that are pure grunts/efforts (damage gasp, jump grunt)
SKIP_GRUNT_KK = {6, 38, 50}
# Suggested default SemanticTag by KK category
KK_DEFAULT_TAG = {
    7: "LowHPHelp",
    23: "AggroWarning",
    29: "LowHPHelp",
    30: "HealPotion",
    32: "SpecialReady",
    33: "SpecialReady",
    34: "SpecialReady",
    40: "SpecialReady",
    41: "Banter",
    42: "Banter",
    44: "Banter",
}


def load_transcripts(path: pathlib.Path) -> dict[str, dict]:
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        out[rec["event"]] = rec
    return out


def cmd_pack_banter(args):
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
    tr = load_transcripts(pathlib.Path(args.transcripts))
    existing_cues = {}
    if args.existing_json and pathlib.Path(args.existing_json).exists():
        existing_cues = json.loads(pathlib.Path(args.existing_json).read_text(encoding="utf-8")).get("cues", {})

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    ready_groups = []
    skipped_incomplete = 0
    for grp in manifest["banter_groups"]:
        turns_out = []
        complete = True
        for t in grp["turns"]:
            ev = t["event"]
            rec = tr.get(ev)
            if not rec:
                complete = False
                break
            sp = t["speaker_id"]
            item = {
                "turn": t["turn"],
                "event": ev,
                "speaker_id": sp,
                "speaker": SPEAKER_NAMES.get(sp, f"ID:{sp}"),
                "jp": rec.get("jp_text", ""),
                "en": rec.get("en_text", ""),
                "dur": rec.get("jp_dur", t.get("jp_dur", 0.0)),
            }
            if rec.get("needs_audio_review"):
                item["needs_audio_review"] = True
            if ev in existing_cues:
                item["existing_zh"] = existing_cues[ev].get("text_full", "")
            turns_out.append(item)
        if not complete:
            skipped_incomplete += 1
            continue
        # Skip group only if every turn is already translated and --only-missing is set
        if args.only_missing and all("existing_zh" in x for x in turns_out):
            continue
        ready_groups.append({
            "group_id": grp["group_id"],
            "pack": grp["pack"],
            "turns": turns_out,
        })

    n_per = args.groups_per_batch
    written = 0
    for idx in range(0, len(ready_groups), n_per):
        batch = ready_groups[idx : idx + n_per]
        bnum = idx // n_per + 1
        bpath = out_dir / f"banter_{bnum:03d}.json"
        bpath.write_text(
            json.dumps({"batch": f"banter_{bnum:03d}", "conversations": batch}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        written += 1

    total_turns = sum(len(g["turns"]) for g in ready_groups)
    print(f"Packed {len(ready_groups)} banter conversations ({total_turns} turns) into {written} batch file(s) in {out_dir}")
    if skipped_incomplete:
        print(f"  (skipped {skipped_incomplete} conversations not yet transcribed)")


def cmd_pack_combat(args):
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
    tr = load_transcripts(pathlib.Path(args.transcripts))
    existing_cues = {}
    if args.existing_json and pathlib.Path(args.existing_json).exists():
        existing_cues = json.loads(pathlib.Path(args.existing_json).read_text(encoding="utf-8")).get("cues", {})

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    by_sp: dict[int, list[dict]] = defaultdict(list)
    for e in manifest["entries"]:
        if e["tier"] != "high" or e["kk"] in SKIP_GRUNT_KK:
            continue
        ev = e["event"]
        rec = tr.get(ev)
        if not rec:
            continue
        if args.only_missing and ev in existing_cues:
            continue
        sp = e["speaker_id"]
        item = {
            "event": ev,
            "kk": e["kk"],
            "pack": e["pack"],
            "jp": rec.get("jp_text", ""),
            "en": rec.get("en_text", ""),
            "dur": rec.get("jp_dur", e.get("jp_dur", 0.0)),
        }
        if rec.get("needs_audio_review"):
            item["needs_audio_review"] = True
        by_sp[sp].append(item)

    n_per = args.lines_per_batch
    written = 0
    total_lines = 0
    for sp in sorted(by_sp.keys()):
        lines = sorted(by_sp[sp], key=lambda x: (x["kk"], x["event"]))
        sp_name = SPEAKER_NAMES.get(sp, f"ID:{sp}")
        for idx in range(0, len(lines), n_per):
            chunk = lines[idx : idx + n_per]
            bnum = idx // n_per + 1
            bpath = out_dir / f"combat_sp{sp:03d}_{bnum:02d}.json"
            payload = {
                "batch": f"combat_sp{sp:03d}_{bnum:02d}",
                "speaker_id": sp,
                "speaker": sp_name,
                "lines": chunk,
            }
            bpath.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            written += 1
            total_lines += len(chunk)

    print(f"Packed {total_lines} combat/field lines across {len(by_sp)} speakers into {written} batch file(s) in {out_dir}")


def default_priority_and_tag(kk: int) -> tuple[int, str]:
    if kk in SKIP_GRUNT_KK:
        return 3, "None"
    if kk in P0_BANTER_KK:
        return 0, KK_DEFAULT_TAG.get(kk, "Banter")
    return 1, KK_DEFAULT_TAG.get(kk, "None")


def cmd_merge(args):
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
    meta = {e["event"]: e for e in manifest["entries"]}

    target_path = pathlib.Path(args.target_json)
    data = json.loads(target_path.read_text(encoding="utf-8")) if target_path.exists() else {"speakers": {}, "cues": {}}
    cues = data.setdefault("cues", {})

    in_paths = []
    for p in args.inputs:
        pp = pathlib.Path(p)
        if pp.is_dir():
            in_paths.extend(sorted(pp.glob("*.json")))
        else:
            in_paths.append(pp)

    added = updated = skipped = compact_warns = 0
    for ip in in_paths:
        doc = json.loads(ip.read_text(encoding="utf-8"))
        # Accept either {"cues": {ev: {...}}} or {"translations": [{event, text_full, text_compact, tag}]}
        items = []
        if "cues" in doc and isinstance(doc["cues"], dict):
            for ev, obj in doc["cues"].items():
                obj = dict(obj)
                obj["event"] = ev
                items.append(obj)
        elif "translations" in doc and isinstance(doc["translations"], list):
            items.extend(doc["translations"])

        for obj in items:
            ev = obj.get("event", "")
            text_full = (obj.get("text_full") or "").strip()
            m = meta.get(ev, {})
            kk = int(m.get("kk") or 0)
            def_prio, def_tag = default_priority_and_tag(kk)
            prio = int(obj.get("priority", def_prio))
            if not ev or (not text_full and prio != 3):
                continue
            if ev in cues and not args.overwrite:
                skipped += 1
                continue
            sp = int(obj.get("speaker_id") or m.get("speaker_id") or 0)
            tag = obj.get("tag") or def_tag
            dur = round(float(obj.get("duration_sec") or m.get("jp_dur") or 0.0), 2)
            text_compact = (obj.get("text_compact") or "").strip()
            if prio == 1 and len(text_compact) > 16:
                compact_warns += 1
            entry = {
                "speaker_id": sp,
                "priority": prio,
                "tag": tag,
                "duration_sec": dur,
                "text_full": text_full,
            }
            if text_compact:
                entry["text_compact"] = text_compact
            if ev in cues:
                updated += 1
            else:
                added += 1
            cues[ev] = entry

    # Keep cues deterministically sorted by event key
    data["cues"] = dict(sorted(cues.items()))
    target_path.parent.mkdir(parents=True, exist_ok=True)
    target_path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Merged into {target_path}: +{added} added, ~{updated} updated, ={skipped} kept (total={len(data['cues'])}, compact>16 warns={compact_warns})")


def cmd_export_raw(args):
    tr = load_transcripts(pathlib.Path(args.transcripts))
    out_path = pathlib.Path(args.target_json)
    field = "jp_text" if args.lang == "ja" else "en_text"
    dur_field = "jp_dur" if args.lang == "ja" else "en_dur"

    cues = {}
    for ev, rec in sorted(tr.items()):
        txt = (rec.get(field) or "").strip()
        if not txt:
            continue
        prio, tag = default_priority_and_tag(int(rec.get("kk", 0)))
        cues[ev] = {
            "speaker_id": int(rec.get("speaker_id", 0)),
            "priority": prio,
            "tag": tag,
            "duration_sec": round(float(rec.get(dur_field, 0.0)), 2),
            "text_full": txt,
        }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({"speakers": {}, "cues": cues}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(cues)} {args.lang} cues to {out_path}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p1 = sub.add_parser("pack-banter")
    p1.add_argument("--manifest", required=True)
    p1.add_argument("--transcripts", required=True)
    p1.add_argument("--out-dir", required=True)
    p1.add_argument("--existing-json", default="")
    p1.add_argument("--groups-per-batch", type=int, default=20)
    p1.add_argument("--only-missing", action="store_true")
    p1.set_defaults(func=cmd_pack_banter)

    p2 = sub.add_parser("pack-combat")
    p2.add_argument("--manifest", required=True)
    p2.add_argument("--transcripts", required=True)
    p2.add_argument("--out-dir", required=True)
    p2.add_argument("--existing-json", default="")
    p2.add_argument("--lines-per-batch", type=int, default=45)
    p2.add_argument("--only-missing", action="store_true")
    p2.set_defaults(func=cmd_pack_combat)

    p3 = sub.add_parser("merge")
    p3.add_argument("--manifest", required=True)
    p3.add_argument("--target-json", required=True)
    p3.add_argument("--inputs", nargs="+", required=True)
    p3.add_argument("--overwrite", action="store_true")
    p3.set_defaults(func=cmd_merge)

    p4 = sub.add_parser("export-raw")
    p4.add_argument("--transcripts", required=True)
    p4.add_argument("--lang", choices=["ja", "en"], required=True)
    p4.add_argument("--target-json", required=True)
    p4.set_defaults(func=cmd_export_raw)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
