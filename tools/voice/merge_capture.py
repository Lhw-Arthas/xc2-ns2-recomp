# SPDX-License-Identifier: GPL-3.0-or-later
r"""Merge live voice_capture.jsonl dumps into a deduplicated voice_subtitles/<lang>.json template.

Usage:
    python tools/voice/merge_capture.py --capture voice_capture.jsonl \
                                        --json    voice_subtitles/zh_cn.json
"""
import argparse
import json
import pathlib
import re

# Official common.bdat VoiceIDs are used directly as speaker_id (2-digit main/rare, 3-digit DLC 501..506 & Torna 701..709)
RX_VOICE = re.compile(r"^voice_(\d{2,3})(\d{2})\d{4}_Play$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True, help="Path to voice_capture.jsonl")
    ap.add_argument("--json", required=True, help="Path to voice_subtitles/<lang>.json to update")
    args = ap.parse_args()

    cap_path = pathlib.Path(args.capture)
    json_path = pathlib.Path(args.json)
    data = json.loads(json_path.read_text(encoding="utf-8")) if json_path.exists() else {"speakers": {}, "cues": {}}
    cues = data.setdefault("cues", {})

    added = 0
    for line in cap_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        ev = rec.get("event", "")
        if not ev or ev in cues:
            continue
        m = RX_VOICE.match(ev)
        sp_id = 0
        prio = 1
        tag = "None"
        if m:
            cc, kk = int(m.group(1)), int(m.group(2))
            sp_id = cc
            if kk == 44:
                prio, tag = 0, "Banter"
            elif kk in (38, 39, 49, 50):
                prio, tag = 3, "None"
        cues[ev] = {
            "speaker_id": sp_id,
            "priority": prio,
            "tag": tag,
            "duration_sec": round(float(rec.get("duration_sec", 0.0)), 2),
            "text_full": "" if prio == 3 else f"[TODO] {ev}",
            "text_compact": "",
        }
        added += 1

    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Merged {added} new cue(s) into {json_path} (total cues: {len(cues)})")


if __name__ == "__main__":
    main()
