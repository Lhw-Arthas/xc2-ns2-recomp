#!/usr/bin/env python3
"""The install template's values must be the documented defaults.

config/nxrt.ini.in is what a fresh install starts from; config/keys.tsv documents the default of every
key. They drifted: the template shipped [media] movie_pts_shift_ms = 110 long after the default became 0
and the code stopped needing it, and nothing noticed - a new install behaved differently from a documented
one, and the difference only showed up when a clean install was tested.

This gate compares the two. Values that are install paths or filled in by setup are skipped, and so is
anything the template deliberately overrides, as long as it says why in OVERRIDE.

  python tools/ci/check_install_template.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "config/nxrt.ini.in"
KEYS = ROOT / "config/keys.tsv"
BT = chr(96)

# Template values that are intentionally not the default, with the reason.
OVERRIDE = {
    # The package ships the 60 fps arrangement: `[xc2] fps = 0` would keep the console's 30 Hz step, and
    # `fps_interval = 2` (the default) is what turns 60 into one present per vsync.
    ("xc2", "fps"): "the package ships 60 fps (fps_interval = 2), not the console's 30 Hz step",
    # Baseline quality preset: lock dynamic resolution to the top rung alongside [upscale] render_quality = native
    # so the portable install serves as a crisp 1080p native TAAU baseline out of the box.
    ("upscale", "lock_render_size"): "the package baseline locks dynamic resolution alongside 1080p native TAAU",
    ("hdr", "mode"): "match launcher default (enable HDR only when Windows reports HDR active on the display)",
}

SECTION_RE = re.compile(r"^\[([^\]]+)\]\s*$")
KV_RE = re.compile(r"^([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")


def read_template():
    out, sec = {}, None
    for line in TEMPLATE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        m = SECTION_RE.match(line)
        if m:
            sec = m.group(1)
            continue
        m = KV_RE.match(line)
        if m and sec:
            out[(sec, m.group(1))] = m.group(2)
    return out


def read_defaults():
    out = {}
    for line in KEYS.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) < 3:
            continue
        out[(cols[0].strip(BT), cols[1].strip(BT))] = cols[2].strip(BT).strip()
    return out


def normalise(default):
    """A comparable value, or None when the documented default is prose (a path, 'empty', a range)."""
    d = default.strip()
    if not d:
        return None
    low = d.lower()
    if low.startswith("empty") or "/" in d or "\\" in d or "[" in d:
        return None
    head = re.split(r"\s+\(", d)[0].strip()
    if head.lower() in ("true", "false", "on", "off"):
        return head.lower().replace("off", "false").replace("on", "true")
    if re.fullmatch(r"-?\d+", head):
        return head
    if re.fullmatch(r"[A-Za-z0-9_.-]+", head):
        return head.lower()
    return None


def main() -> int:
    template, defaults = read_template(), read_defaults()
    bad = 0
    for (sec, key), value in sorted(template.items()):
        if value.startswith("E:/") or value.startswith("C:/"):
            continue
        if "{" in value:
            continue                      # a setup placeholder (exefs, romfs, shader_cache, ...)
        if (sec, key) in OVERRIDE:
            continue
        doc = defaults.get((sec, key))
        if doc is None:
            print(f"  FAIL  [{sec}] {key} is in the install template but not in config/keys.tsv")
            bad += 1
            continue
        want = normalise(doc)
        if want is None:
            continue
        have = value.strip().lower()
        if have != want:
            print(f"  FAIL  [{sec}] {key} = {value!r} in the template, the documented default is {doc!r}")
            bad += 1
    if bad:
        print(f"\n{bad} template value(s) differ from the documented defaults. Fix the template, or say why in OVERRIDE.")
        return 1
    print(f"  ok    the {len(template)} template values are the documented defaults")
    return 0


if __name__ == "__main__":
    sys.exit(main())
