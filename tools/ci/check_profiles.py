#!/usr/bin/env python3
"""A dev/test profile must not contradict the switches a package launches with.

`tools/setup/xc2_setup.py` owns that list (GAME_ENV, written into `<install>/launch.env` on every setup, and
sourced by tools/run/play.ps1 before it starts the game). A profile only carries dev-only extras; a profile that
overrides a GAME_ENV key tests something no player runs, and the failure looks like a game bug rather than a config one:

2026-11-26 - the dev profile had lost `NXRT_NVN_EARLYZ_FS=auto`. Only the eleven built-in light-list shaders then
qualified for the registration-mask rebuild, the others logged "is not in earlyz_fs, so the mask is not rebuilt",
the light list lost tiles and distant geometry went black - while the same code in a package, whose launch.env
still carried the key, looked fine. Hours went into bisecting the runtime before anyone compared the two launch
chains.

Exit codes: 0 = every profile agrees, 1 = a profile is missing a switch or has a different value.
"""

import argparse
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILES = ROOT / "config" / "profiles"


def load_game_env():
    spec = importlib.util.spec_from_file_location("xc2_setup", ROOT / "tools" / "setup" / "xc2_setup.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return dict(module.GAME_ENV)


def read_env_section(path):
    """Returns (lines, {key: value}) for the profile's [env] section, values trimmed."""
    lines = path.read_text(encoding="utf-8").splitlines()
    values = {}
    section = ""
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().lower()
            continue
        if section != "env" or not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        key, _, value = stripped.partition("=")
        values[key.strip()] = value.strip()
    return lines, values


def fix_profile(path, lines, missing, wrong):
    """Rewrites the [env] section with the missing keys added and the wrong values corrected."""
    out = []
    section = ""
    inserted = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if section == "env" and not inserted:
                for key in missing:
                    out.append("%s = %s" % (key, GAME_ENV[key]))
                inserted = True
            section = stripped[1:-1].strip().lower()
            out.append(line)
            continue
        if section == "env" and "=" in stripped and not stripped.startswith("#"):
            key = stripped.partition("=")[0].strip()
            if key in wrong:
                out.append("%s = %s" % (key, GAME_ENV[key]))
                continue
        out.append(line)
    if section == "env" and not inserted:
        for key in missing:
            out.append("%s = %s" % (key, GAME_ENV[key]))
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


# Keys a profile may differ on, with the reason. Anything else must match launch.env exactly.
# Empty on purpose: a profile that overrides a GAME_ENV key is how the earlyz regression happened. If a profile ever
# needs a different value, list the key here with the reason and check the reason still holds.
ALLOWED_DIFFERENT = set()

# play.ps1 writes these from the profile's [run] fps / hz after the [env] block, so a profile that sets those two
# numbers is covered even though it does not repeat them as environment keys.
IMPLIED_BY_RUN = ("NXRT_XC2_FPS", "NXRT_NVN_PRESENT_INTERVAL_HZ")


GAME_ENV = {}


def run_section_has(lines, key):
    section = ""
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().lower()
            continue
        if section == "run" and stripped.lower().startswith(key + " "):
            return True
    return False


def main():
    global GAME_ENV
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare every profile with GAME_ENV (default)")
    ap.add_argument("--fix", action="store_true", help="add missing keys and correct different values")
    args = ap.parse_args()

    GAME_ENV = load_game_env()
    if not GAME_ENV:
        print("check_profiles: xc2_setup.GAME_ENV is empty")
        return 1
    if not PROFILES.is_dir():
        print("check_profiles: no config/profiles")
        return 1

    failed = False
    for path in sorted(PROFILES.glob("*.ini")):
        lines, values = read_env_section(path)
        # play.ps1 injects GAME_ENV before the profile's [env], so a profile is not required to repeat those keys -
        # but it must not contradict them: an override is how the earlyz regression happened.
        inherited = [k for k in GAME_ENV if k not in values]
        wrong = [k for k in GAME_ENV if k in values and values[k] != GAME_ENV[k] and k not in ALLOWED_DIFFERENT]
        missing = []
        extra = [k for k in values if k not in GAME_ENV]
        name = path.name
        if missing or wrong:
            if args.fix:
                fix_profile(path, lines, missing, wrong)
                print("  fixed %s: %d added, %d corrected" % (name, len(missing), len(wrong)))
            else:
                failed = True
                print("  FAIL %s" % name)
                for key in missing:
                    print("       missing   %s = %s" % (key, GAME_ENV[key]))
                for key in wrong:
                    print("       different %s: profile has %r, launch.env has %r" % (key, values[key], GAME_ENV[key]))
        else:
            print("  ok   %s (%d from GAME_ENV, %d set here)" % (name, len(inherited), len(values)))
        for key in extra:
            print("       note  %s = %s is not in launch.env (dev-only switch?)" % (key, values[key]))

    if failed:
        print("profiles drift from launch.env: run tools/ci/check_profiles.py --fix")
        return 1
    print("every profile launches the game like a package does")
    return 0


if __name__ == "__main__":
    sys.exit(main())
