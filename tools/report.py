#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Run the game once with every diagnostic on and pack what it said into one zip.

    tools\python\python.exe tools\report.py                  run, window, diagnostics on
    tools\python\python.exe tools\report.py --headless --seconds 30
    tools\python\python.exe tools\report.py --from-log work\logs\play-....log

A bug report about rendering is worth nothing as prose: the facts that decide a diagnosis are the validation
layer's VUIDs, the capability lines the driver reports, whether the device was lost, and the frame times. This
runs the game with all of them on and writes work\reports\report-<stamp>.zip, which can be attached to an issue
as-is. It never kills a game you started yourself; with --seconds it stops the one it started.

Pure standard library, so the Python that ships in the package runs it. No PowerShell, no .NET.
"""
import argparse
import ctypes
import datetime
import json
import os
import re
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # <install>\tools\report.py -> <install>

VUID_RE = re.compile(r"(VUID-[A-Za-z0-9_-]+|Undefined-Value-[A-Za-z]+)")


def read_env(path):
    env = {}
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8", errors="ignore"):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def ini_language():
    """[system] language from nxrt.ini: the game follows the console language, and the launcher hands it to the runtime
    as an environment value because the config lookup answers AmericanEnglish for it a moment after reading the file."""
    try:
        text = open(os.path.join(ROOT, "nxrt.ini"), encoding="utf-8", errors="ignore").read()
    except OSError:
        return ""
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            section = line
            continue
        if section == "[system]" and line.startswith("language") and "=" in line:
            return line.split("=", 1)[1].strip()
    return ""


def run_game(args):
    exe = args.exe or os.path.join(ROOT, "xc2_boot.exe")
    exefs = os.path.join(ROOT, "exefs")
    if not os.path.isfile(exe):
        sys.exit("no game executable at %s (run Set up first)" % exe)
    log_dir = os.path.join(ROOT, "work", "logs")
    os.makedirs(log_dir, exist_ok=True)
    log = os.path.join(log_dir, "report-%s.log" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    csv = os.path.join(ROOT, "work", "reports", "frame-times.csv")
    os.makedirs(os.path.dirname(csv), exist_ok=True)

    env = dict(os.environ)
    env.update(read_env(os.path.join(ROOT, "launch.env")))
    if not args.no_validation:
        env["NXRT_GFX_VALIDATION"] = "1"
    env["NXRT_NVN_DS_DEBUG"] = "1"
    env["NXRT_NVN_FS_DEBUG"] = "1"
    env["NXRT_NVN_FRAME_CSV"] = csv
    lang = ini_language()
    if lang:
        env["NXRT_SYSTEM_LANGUAGE"] = lang
    ini_log = os.path.join(ROOT, "nxrt.log")
    os.makedirs(os.path.dirname(ini_log), exist_ok=True)
    env["NXRT_INI"] = os.path.join(ROOT, "nxrt.ini")
    t0 = time.time()
    argv = [exe, "--exefs", exefs, "--gfx", "headless" if args.headless else "window", "--log", log]
    print("running: %s" % " ".join(argv))
    print("close the game when you have seen enough; this packages the run afterwards.")
    p = subprocess.Popen(argv, cwd=ROOT, env=env)
    # A frozen game never exits, and a report is worth most exactly then: stop when the log has not grown for --stall
    # seconds (a frozen render thread writes nothing), or after --seconds when one is given.
    # Packaged xc2_boot.exe ignores --log and writes to <install>/nxrt.log, so watch both paths to avoid false 60 s kills.
    stall = max(10, args.stall)
    last_size, last_change = -1, time.time()
    try:
        while p.poll() is None:
            if args.seconds and time.time() - t0 >= args.seconds:
                print("stopping after %d s" % args.seconds)
                break
            size = -1
            for cand in (log, ini_log):
                try:
                    if os.path.getmtime(cand) >= t0 - 2.0:
                        size = max(size, os.path.getsize(cand))
                except OSError:
                    pass
            if size != last_size:
                last_size, last_change = size, time.time()
            elif time.time() - last_change >= stall:
                print("the log has not grown for %d s: the game looks frozen, stopping it (the report keeps what it wrote)" % stall)
                break
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("interrupted: packaging what the run wrote")
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    return log, csv


def newest_log(since=0.0):
    """The newest log this run could have written. A release package's xc2_boot.exe is built from xc2_stub.cpp, which
    forwards its arguments to the runtime and ignores --log, so the log lands where nxrt.ini says: <install>/nxrt.log.
    Looking only in work/logs made every report on a package install come out with an empty folder (issue #18)."""
    cands = []
    d = os.path.join(ROOT, "work", "logs")
    if os.path.isdir(d):
        cands += [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".log")]
    ini_log = os.path.join(ROOT, "nxrt.log")
    if os.path.isfile(ini_log):
        cands.append(ini_log)
    if since:
        cands = [c for c in cands if os.path.getmtime(c) >= since]
    return max(cands, key=os.path.getmtime) if cands else ""


def section(out_dir, name, lines):
    path = os.path.join(out_dir, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n" if lines else "(nothing in this run)\n")
    return path


def collect_flags(lines):
    """5-second triage flags: highlight non-default / external conditions at the top of environment.txt."""
    flags = []
    # 1. CPU affinity check on Windows (catches manual core pinning like #82)
    if sys.platform == "win32":
        try:
            proc_mask = ctypes.c_size_t()
            sys_mask = ctypes.c_size_t()
            h = ctypes.windll.kernel32.GetCurrentProcess()
            if ctypes.windll.kernel32.GetProcessAffinityMask(h, ctypes.byref(proc_mask), ctypes.byref(sys_mask)):
                allowed = bin(proc_mask.value).count("1")
                total = bin(sys_mask.value).count("1")
                if total > 0 and allowed < total:
                    flags.append("FLAG: CPU affinity restricted (%d/%d logical cores)" % (allowed, total))
        except Exception:
            pass
    # 2. Runtime vs link/prefill stamp check (catches mixed builds after upgrade)
    rt_dll = os.path.join(ROOT, "runtime", "xc2_rt.dll")
    if os.path.isfile(rt_dll):
        try:
            st = os.stat(rt_dll)
            cur = {"size": st.st_size, "mtime": int(st.st_mtime)}
            for label, rel in (("link-stamp", "link-stamp.json"), ("prefill-stamp", "prefill-stamp.json")):
                sp = os.path.join(ROOT, "build", rel)
                if os.path.isfile(sp):
                    got = json.loads(open(sp, encoding="utf-8").read())
                    if got.get("size") != cur["size"] or got.get("mtime") != cur["mtime"]:
                        flags.append("FLAG: %s mismatch vs runtime/xc2_rt.dll (mixed build?)" % label)
        except Exception:
            pass
    # 3. Mods / ExeFS patches present
    mods_dir = os.path.join(ROOT, "mods")
    if os.path.isdir(mods_dir):
        mods = sorted(d for d in os.listdir(mods_dir)
                      if not d.startswith(".") and os.path.isdir(os.path.join(mods_dir, d)))
        if mods:
            flags.append("FLAG: %d mod folder(s) present: %s" % (len(mods), ", ".join(mods[:8])))
    if os.path.isdir(os.path.join(ROOT, "build", "exefs-patched")):
        flags.append("FLAG: build/exefs-patched present (ExeFS code patches applied)")
    # 4. Vulkan validation actually active (impacts performance measurements)
    if any("validation on," in l for l in lines):
        flags.append("FLAG: Vulkan validation layer was ACTIVE in this run (perf numbers are lower than normal)")
    return flags


def main():
    ap = argparse.ArgumentParser(description="Collect one diagnostics report into a zip.")
    ap.add_argument("--headless", action="store_true", help="no window")
    ap.add_argument("--seconds", type=int, default=0, help="stop the game this many seconds after it starts")
    ap.add_argument("--stall", type=int, default=60, help="stop when the log has not grown for this many seconds (a frozen game)")
    ap.add_argument("--from-log", default="", help="package this log instead of running the game")
    ap.add_argument("--exe", default="", help="the game executable (default: <install>\\xc2_boot.exe)")
    ap.add_argument("--no-validation", action="store_true", help="do not force NXRT_GFX_VALIDATION=1 (better for perf captures)")
    ap.add_argument("--no-zip", action="store_true")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(ROOT, "work", "reports", "report-%s" % stamp)
    os.makedirs(out_dir, exist_ok=True)

    started = time.time()
    if args.from_log:
        log, csv = args.from_log, ""
        print("packaging %s" % log)
    else:
        log, csv = run_game(args)

    if not os.path.isfile(log):
        alt = newest_log(started)
        if not alt:
            sys.exit("the run wrote no log at all")
        print("the game wrote its log to %s (a packaged xc2_boot.exe ignores --log): packaging that" % alt)
        log = alt
    with open(log, encoding="utf-8", errors="ignore") as f:
        lines = [l.rstrip("\n") for l in f]
    if any("VK_LAYER_KHRONOS_validation is not installed" in l for l in lines):
        print("note: the Vulkan validation layer is not installed on this machine, so vuids.txt will be empty -")
        print("      nothing in this run checked the API usage. The rest of the report is still useful.")

    # 1. the validation layer, which is the whole point
    vuids = [l for l in lines if "VUID-" in l or "Undefined-Value-" in l]
    counts = {}
    for l in vuids:
        m = VUID_RE.search(l)
        if m:
            counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    summary = ["count  id"] + ["%-6d %s" % (n, k) for k, n in sorted(counts.items(), key=lambda kv: -kv[1])]
    section(out_dir, "vuids.txt",
            summary + ["", "--- first 20 full messages ---"] + vuids[:20])

    # 2. what the driver says it can do
    section(out_dir, "features.txt",
            [l for l in lines if "gfx: features:" in l or "gfx: device ready:" in l or "gfx: swapchain" in l])

    # 3. did the device go away
    section(out_dir, "device-lost.txt",
            [l for l in lines if re.search(r"device lost|DEVICE_LOST|svcBreak|HOST EXCEPTION|vkQueueSubmit2 failed|vkWaitForFences", l)])

    # 4. what was running
    env_lines = []
    flags = collect_flags(lines)
    if flags:
        env_lines.append("triage flags:")
        for fl in flags:
            env_lines.append("  " + fl)
    else:
        env_lines.append("triage flags: none")
    exe = args.exe or os.path.join(ROOT, "xc2_boot.exe")
    if os.path.isfile(exe):
        st = os.stat(exe)
        env_lines.append("exe        %s  %.1f MB  %s" % (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
                                                        st.st_size / 1048576.0, exe))
    for ini_name in ("nxrt.ini", "controls.ini"):
        ini = os.path.join(ROOT, ini_name)
        if os.path.isfile(ini):
            env_lines.append("%-10s %s" % (ini_name, ini))
            for l in open(ini, encoding="utf-8", errors="ignore"):
                if l.strip():
                    env_lines.append("  " + l.rstrip())
    env_lines.append("serial     %s" % os.path.join(ROOT, "shader_cache", "spirv"))
    if os.path.isdir(os.path.join(ROOT, "shader_cache", "spirv")):
        env_lines.append("  %d shader(s)" % len([f for f in os.listdir(os.path.join(ROOT, "shader_cache", "spirv")) if f.endswith(".spv")]))
    env_lines.append("python     %s" % sys.version.replace("\n", " "))
    env_lines.append("platform   %s" % sys.platform)
    section(out_dir, "environment.txt", env_lines)

    for name, src in (("run.log", log), ("frame-times.csv", csv)):
        if src and os.path.isfile(src):
            with open(src, "rb") as a, open(os.path.join(out_dir, name), "wb") as b:
                b.write(a.read())

    # Include recent freeze / crash / device-lost dumps written beside the exe or install root.
    dump_dirs = [ROOT]
    exe_dir = os.path.dirname(os.path.abspath(exe))
    if exe_dir and exe_dir != ROOT:
        dump_dirs.append(exe_dir)
    ref_time = os.path.getmtime(log) if os.path.isfile(log) else started
    for dname in ("xc2_boot_freeze.txt", "xc2_boot_crash.txt", "xc2_boot_devicelost.txt"):
        for d in dump_dirs:
            cand = os.path.join(d, dname)
            if not os.path.isfile(cand):
                continue
            if os.path.getmtime(cand) >= min(started, ref_time) - 300.0:
                with open(cand, "rb") as a, open(os.path.join(out_dir, dname), "wb") as b:
                    b.write(a.read())
                break

    readme = """Rendering report, %s

run.log            the whole log of one run
vuids.txt          every VUID by count, then the first 20 in full
features.txt       the capability lines the driver reported
device-lost.txt    anything that says the device went away (empty is good)
environment.txt    the exe, the ini, the shader cache, the toolchain
frame-times.csv    one line per presented frame (only with --headless)
xc2_boot_*.txt     freeze / crash / device-lost dumps from the run (when generated)

Attach the zip to the issue as-is. Nothing here is game data: it is the log, the settings and the timing.
""" % stamp
    with open(os.path.join(out_dir, "README.txt"), "w", encoding="utf-8") as f:
        f.write(readme)

    if args.no_zip:
        print("report folder: %s" % out_dir)
        return
    zip_path = out_dir + ".zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in os.listdir(out_dir):
            z.write(os.path.join(out_dir, name), name)
    print("report zip: %s" % zip_path)
    print("attach that file to the issue.")


if __name__ == "__main__":
    main()