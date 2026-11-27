// SPDX-License-Identifier: GPL-3.0-or-later
//! Checks the player's own decrypted dump. Read-only: nothing here writes to the dump.
//!
//! Accepted layouts: a folder with `exefs/` and `romfs/` (one title version, e.g. the update), or a dump root with `base/` and
//! `updates/<version>/`, each holding `exefs/` and `romfs/`. The newest complete candidate whose `main` is a supported build is used.
//! Add-on content (DLC) the player dumped sits in `dlc/<16-hex-digit title id>/romfs` (or `dlc/<id>/<n>/romfs`) of that folder. The
//! runtime is pointed at that `dlc/` folder ([aoc] dir) and serves only what is there.

use serde::Serialize;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

pub const EXEFS_FILES: [&str; 7] = ["main", "sdk", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "rtld"];
/// Switch 2 Edition Upgrade Pack title ID: its RomFS holds the full 3.0.1 NS2 game data (~24 GB)
/// in un-trimmed raw dumps, and must not be mounted as a normal AOC DLC.
pub const NS2_UPGRADE_PACK_ID: &str = "0100E95004039062";

#[derive(Serialize, Clone)]
pub struct Check {
    pub label: String,
    pub ok: bool,
}

#[derive(Serialize, Clone)]
pub struct Candidate {
    /// "folder", "base" or "update"
    pub kind: String,
    pub version: String,
    pub path: String,
    pub exefs: String,
    pub romfs: String,
    pub ok: bool,
    pub checks: Vec<Check>,
}

#[derive(Serialize, Clone)]
pub struct DumpReport {
    pub path: String,
    pub exists: bool,
    pub errors: Vec<String>,
    pub candidates: Vec<Candidate>,
    pub chosen: Option<Candidate>,
    /// the folder's `dlc/`, when it holds at least one add-on
    pub dlc_dir: Option<String>,
    /// title ids of the add-ons found there (upper-case hex), sorted
    pub dlc: Vec<String>,
}

pub fn fwd(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// A path as fefw.exe sees it. On Windows that is the path itself. On Linux fefw.exe runs under Wine/Proton, where the host's `/`
/// is drive Z: (every Wine and Proton prefix maps it), so an absolute path becomes `Z:/...`; a relative one stays as it is.
pub fn game_path(p: &Path) -> String {
    let s = fwd(p);
    if cfg!(windows) || !s.starts_with('/') {
        s
    } else {
        format!("Z:{s}")
    }
}

/// The build of main this recomp supports: `build_id = '<40 hex>'` in xc2/recomp/xc2ns2/config.py.
pub fn supported_builds(repo: &Path) -> Vec<(String, String)> {
    let text = fs::read_to_string(repo.join("xc2").join("recomp").join("xc2ns2").join("config.py")).unwrap_or_default();
    let mut out = Vec::new();
    for line in text.lines() {
        let Some(rest) = line.trim().strip_prefix("build_id =") else { continue };
        let Some(open) = rest.find('\'') else { continue };
        let tail = &rest[open + 1..];
        let Some(close) = tail.find('\'') else { continue };
        let id = &tail[..close];
        if id.len() == 40 && id.chars().all(|c| c.is_ascii_hexdigit()) {
            out.push((id.to_uppercase(), "3.0.1".to_string()));
        }
    }
    out
}

/// Build id of an NSO (20 bytes at 0x40), upper-case hex; None if the file is missing, short or not a decrypted NSO.
fn build_id(main: &Path) -> Option<String> {
    let mut h = [0u8; 0x54];
    fs::File::open(main).ok()?.read_exact(&mut h).ok()?;
    if &h[..4] != b"NSO0" {
        return None;
    }
    Some(h[0x40..0x54].iter().map(|b| format!("{:02X}", b)).collect())
}

fn check_candidate(kind: &str, version: &str, dir: &Path, romfs_override: Option<&Path>, root_romfs: Option<&Path>, builds: &[(String, String)]) -> Candidate {
    let exefs = dir.join("exefs");
    let romfs = if let Some(ov) = romfs_override {
        ov.to_path_buf()
    } else if dir.join("romfs").is_dir() {
        dir.join("romfs")
    } else if let Some(rr) = root_romfs {
        rr.to_path_buf()
    } else {
        dir.join("romfs")
    };
    let mut checks: Vec<Check> = EXEFS_FILES.iter().map(|f| Check { label: format!("exefs/{f}"), ok: exefs.join(f).is_file() }).collect();
    checks.push(Check { label: "romfs/".into(), ok: romfs.is_dir() });
    if exefs.join("main").is_file() {
        let id = build_id(&exefs.join("main"));
        checks.push(Check { label: "exefs/main is decrypted".into(), ok: id.is_some() });
        if let Some(id) = id {
            if !builds.is_empty() {
                let want = builds.iter().map(|(_, v)| v.as_str()).collect::<Vec<_>>().join(" or ");
                checks.push(Check { label: format!("supported version ({want})"), ok: builds.iter().any(|(b, _)| *b == id) });
            }
        }
    }
    let ok = checks.iter().all(|c| c.ok);
    Candidate { kind: kind.into(), version: version.into(), path: fwd(dir), exefs: fwd(&exefs), romfs: fwd(&romfs), ok, checks }
}

fn is_title_id(s: &str) -> bool {
    s.len() == 16 && s.chars().all(|c| c.is_ascii_hexdigit())
}

fn is_ns2_upgrade_pack(s: &str) -> bool {
    s.eq_ignore_ascii_case(NS2_UPGRADE_PACK_ID)
}

/// Extracts the highest numeric segment immediately following an `updates` component in `rel`, if any.
fn update_version_in_path(path: &Path) -> u64 {
    let comps: Vec<String> = path.components().map(|c| c.as_os_str().to_string_lossy().to_lowercase()).collect();
    let mut best = 0u64;
    for w in comps.windows(2) {
        if w[0] == "updates" {
            if let Ok(v) = w[1].parse::<u64>() {
                best = best.max(v);
            }
        }
    }
    best
}

/// Locates the best `romfs/` directory inside a title-ID folder (supporting both flat `<id>/romfs`
/// and raw nested layouts like `<id>/0/updates/65536/program/0/romfs` or `<id>/0/data/0/romfs`).
/// Never descends into any `romfs/` directory itself.
pub fn find_addon_romfs(addon: &Path) -> Option<PathBuf> {
    let direct = addon.join("romfs");
    if direct.is_dir() {
        return Some(direct);
    }
    let mut found: Vec<(PathBuf, usize)> = Vec::new();
    let mut stack: Vec<(PathBuf, usize)> = vec![(addon.to_path_buf(), 0)];
    while let Some((dir, depth)) = stack.pop() {
        if depth >= 6 {
            continue;
        }
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_dir() {
                continue;
            }
            let name = e.file_name().to_string_lossy().to_string();
            if name.eq_ignore_ascii_case("romfs") {
                found.push((p, depth + 1));
            } else if !name.eq_ignore_ascii_case("exefs") {
                stack.push((p, depth + 1));
            }
        }
    }
    found.into_iter().max_by_key(|(p, depth)| {
        let ard = p.join("bf2.ard");
        let ard_size = fs::metadata(&ard).map(|m| m.len()).unwrap_or(0);
        let has_ard = ard.is_file();
        let ver = update_version_in_path(p);
        (has_ard, ard_size, ver, std::cmp::Reverse(*depth))
    }).map(|(p, _)| p)
}

/// In an un-trimmed Switch 2 dump, the full ~24 GB NS2 3.0.1 RomFS lives inside the Switch 2 Edition
/// Upgrade Pack (`0100e95004039062`, e.g. `0100e95004039062/0/updates/65536/program/0/romfs`).
fn find_ns2_upgrade_romfs(root: &Path) -> Option<PathBuf> {
    for parent in [root.join("dlc"), root.to_path_buf()] {
        let Ok(rd) = fs::read_dir(&parent) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_dir() {
                continue;
            }
            if is_ns2_upgrade_pack(&e.file_name().to_string_lossy()) {
                if let Some(romfs) = find_addon_romfs(&p) {
                    if romfs.join("bf2.ard").is_file() {
                        return Some(romfs);
                    }
                }
            }
        }
    }
    None
}

/// Finds nested `exefs/` parent directories up to depth 6 (skipping `romfs/` trees) for un-trimmed raw dumps.
fn find_nested_exefs_parents(root: &Path, seen: &std::collections::HashSet<PathBuf>) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let mut stack: Vec<(PathBuf, usize)> = vec![(root.to_path_buf(), 0)];
    while let Some((dir, depth)) = stack.pop() {
        if depth >= 6 {
            continue;
        }
        let Ok(rd) = fs::read_dir(&dir) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_dir() {
                continue;
            }
            let name = e.file_name().to_string_lossy().to_string();
            if name.eq_ignore_ascii_case("exefs") {
                if !seen.contains(&dir) {
                    out.push(dir.clone());
                }
            } else if !name.eq_ignore_ascii_case("romfs") {
                stack.push((p, depth + 1));
            }
        }
    }
    out.sort_by(|a, b| update_version_in_path(b).cmp(&update_version_in_path(a)).then_with(|| a.cmp(b)));
    out.dedup();
    out
}

fn has_romfs(addon: &Path) -> bool {
    find_addon_romfs(addon).is_some()
}

/// The add-ons in `dlc`: folders named by a title id that hold a `romfs/` (excluding the NS2 upgrade pack). Upper-case ids, sorted.
pub fn scan_dlc(dlc: &Path) -> Vec<String> {
    let mut out: Vec<String> = fs::read_dir(dlc)
        .map(|rd| {
            rd.filter_map(|e| e.ok())
                .filter(|e| e.path().is_dir())
                .filter_map(|e| {
                    let name = e.file_name().to_string_lossy().to_string();
                    (is_title_id(&name) && !is_ns2_upgrade_pack(&name) && has_romfs(&e.path())).then(|| name.to_uppercase())
                })
                .collect()
        })
        .unwrap_or_default();
    out.sort();
    out
}

fn version_order(a: &str, b: &str) -> std::cmp::Ordering {
    match (a.parse::<u64>(), b.parse::<u64>()) {
        (Ok(x), Ok(y)) => y.cmp(&x),
        (Ok(_), Err(_)) => std::cmp::Ordering::Less,
        (Err(_), Ok(_)) => std::cmp::Ordering::Greater,
        (Err(_), Err(_)) => b.cmp(a),
    }
}

pub fn scan(path: &str, builds: &[(String, String)]) -> DumpReport {
    let root = PathBuf::from(path.trim());
    let mut report =
        DumpReport { path: fwd(&root), exists: root.is_dir(), errors: vec![], candidates: vec![], chosen: None, dlc_dir: None, dlc: vec![] };
    if path.trim().is_empty() {
        report.errors.push("No folder chosen yet.".into());
        return report;
    }
    if !report.exists {
        report.errors.push("That folder does not exist or cannot be read.".into());
        return report;
    }
    let ns2_romfs = find_ns2_upgrade_romfs(&root);
    let root_romfs = root.join("romfs").is_dir().then(|| root.join("romfs"));
    let mut seen_dirs = std::collections::HashSet::new();
    if root.join("exefs").is_dir() {
        seen_dirs.insert(root.clone());
        report.candidates.push(check_candidate("folder", "", &root, ns2_romfs.as_deref(), root_romfs.as_deref(), builds));
    }
    let updates = root.join("updates");
    if updates.is_dir() {
        let mut versions: Vec<String> = fs::read_dir(&updates)
            .map(|rd| rd.filter_map(|e| e.ok()).filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().to_string()).collect())
            .unwrap_or_default();
        versions.sort_by(|a, b| version_order(a, b));
        for v in versions {
            let d = updates.join(&v);
            seen_dirs.insert(d.clone());
            report.candidates.push(check_candidate("update", &v, &d, ns2_romfs.as_deref(), root_romfs.as_deref(), builds));
        }
    }
    if root.join("base").is_dir() {
        let d = root.join("base");
        seen_dirs.insert(d.clone());
        report.candidates.push(check_candidate("base", "", &d, ns2_romfs.as_deref(), root_romfs.as_deref(), builds));
    }
    if !report.candidates.iter().any(|c| c.ok) {
        for d in find_nested_exefs_parents(&root, &seen_dirs) {
            let ver = update_version_in_path(&d);
            let ver_s = if ver > 0 { ver.to_string() } else { String::new() };
            report.candidates.push(check_candidate("folder", &ver_s, &d, ns2_romfs.as_deref(), root_romfs.as_deref(), builds));
        }
    }
    if report.candidates.is_empty() {
        report.errors.push("This folder has no exefs/ folder, and no base/ or updates/ folder holding one. Pick the folder of your dump that contains exefs/ and romfs/.".into());
        return report;
    }
    report.chosen = report.candidates.iter().find(|c| c.ok).cloned();
    let dlc = root.join("dlc");
    report.dlc = scan_dlc(&dlc);
    if !report.dlc.is_empty() {
        report.dlc_dir = Some(fwd(&dlc));
    } else {
        let root_dlc = scan_dlc(&root);
        if !root_dlc.is_empty() {
            report.dlc = root_dlc;
            report.dlc_dir = Some(fwd(&root));
        }
    }
    if report.chosen.is_none() {
        let wrong = report.candidates.iter().any(|c| c.checks.iter().any(|k| k.label.starts_with("supported version") && !k.ok));
        report.errors.push(if wrong {
            "These game files are a different version from the one this recomp supports. Dump the supported update.".into()
        } else {
            "No usable game files found. The folder needs exefs/ (main, sdk, subsdk0, subsdk1, subsdk2, rtld, decrypted) and romfs/.".into()
        });
    }
    report
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn game_paths_for_wine() {
        assert_eq!(game_path(Path::new("save")), "save");
        if cfg!(windows) {
            assert_eq!(game_path(Path::new(r"C:\games\fefw\exefs")), "C:/games/fefw/exefs");
        } else {
            assert_eq!(game_path(Path::new("/home/deck/fefw/exefs")), "Z:/home/deck/fefw/exefs");
        }
    }

    #[test]
    fn build_id_of_an_nso_header() {
        let dir = std::env::temp_dir().join(format!("fefw_dump_test_{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let mut h = vec![0u8; 0x60];
        h[..4].copy_from_slice(b"NSO0");
        h[0x40] = 0xdb;
        h[0x53] = 0x0e;
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(build_id(&dir.join("main")).unwrap(), format!("DB{}0E", "00".repeat(18)));
        h[0] = b'X';
        fs::write(dir.join("main"), &h).unwrap();
        assert_eq!(build_id(&dir.join("main")), None);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn dlc_folders_need_a_title_id_and_a_romfs() {
        let dir = std::env::temp_dir().join(format!("fefw_dlc_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("04007ef01f8af002").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("04007EF01F8AF001").join("0").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("0100e95004039063").join("0").join("updates").join("65536").join("data").join("0").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("0100e95004039062").join("0").join("updates").join("65536").join("program").join("0").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("04007EF01F8AF003")).unwrap();
        fs::create_dir_all(dir.join("notes").join("romfs")).unwrap();
        assert_eq!(
            scan_dlc(&dir),
            vec!["0100E95004039063".to_string(), "04007EF01F8AF001".to_string(), "04007EF01F8AF002".to_string()]
        );
        assert!(scan_dlc(&dir.join("missing")).is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn scan_untrimmed_raw_ns1_ns2_dump_prefers_upgrade_romfs_and_nested_dlc() {
        let dir = std::env::temp_dir().join(format!("xc2_raw_dump_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let ns2_bid = format!("DB{}0E", "00".repeat(18));
        let mut main_nso = vec![0u8; 0x60];
        main_nso[..4].copy_from_slice(b"NSO0");
        main_nso[0x40] = 0xdb;
        main_nso[0x53] = 0x0e;

        // NS2 exefs with an NS1-sized sibling romfs
        let ns2_exefs = dir.join("exefs");
        fs::create_dir_all(&ns2_exefs).unwrap();
        for f in EXEFS_FILES {
            fs::write(ns2_exefs.join(f), if f == "main" { &main_nso[..] } else { b"NSO0" }).unwrap();
        }
        fs::create_dir_all(dir.join("romfs")).unwrap();
        fs::write(dir.join("romfs").join("bf2.ard"), b"ns1").unwrap();

        // Switch 2 Edition Upgrade Pack holding the full NS2 romfs
        let upgrade_romfs = dir.join("0100e95004039062").join("0").join("updates").join("65536").join("program").join("0").join("romfs");
        fs::create_dir_all(&upgrade_romfs).unwrap();
        fs::write(upgrade_romfs.join("bf2.ard"), b"ns2-full-romfs").unwrap();

        // Nested DLCs placed directly under dump root
        fs::create_dir_all(dir.join("0100e95004039001").join("0").join("data").join("0").join("romfs")).unwrap();
        fs::create_dir_all(dir.join("0100e95004039063").join("0").join("updates").join("65536").join("data").join("0").join("romfs")).unwrap();

        let report = scan(&dir.to_string_lossy(), &[(ns2_bid, "3.0.1".into())]);
        let chosen = report.chosen.expect("raw dump should be accepted");
        assert_eq!(chosen.romfs, fwd(&upgrade_romfs));
        assert_eq!(report.dlc, vec!["0100E95004039001".to_string(), "0100E95004039063".to_string()]);
        assert_eq!(report.dlc_dir.as_deref(), Some(fwd(&dir).as_str()));
        let _ = fs::remove_dir_all(&dir);
    }
}
