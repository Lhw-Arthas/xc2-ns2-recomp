// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads and edits the runtime's ini files in the run folder: nxrt.ini (the settings of the XC2 NS2 recomp runtime) and
//! controls.ini (the play-mode file the game writes on its first start). The key schema lives in the frontend; this side moves
//! `section.key` values in and out with the careful line editor in ini.rs, so comments and unknown keys survive.

use crate::ini::Ini;
use crate::setup::Paths;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const GAME_INI: &str = "nxrt.ini";
pub const CONTROLS_INI: &str = "controls.ini";

#[derive(Serialize)]
pub struct SettingsDump {
    pub game_dir: String,
    pub game_exists: bool,
    pub controls_exists: bool,
    pub game: HashMap<String, String>,
    pub controls: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Edit {
    /// "game" (nxrt.ini) or "controls" (controls.ini)
    pub file: String,
    pub section: String,
    pub key: String,
    pub value: String,
}

fn load(path: &Path) -> Option<Ini> {
    let bytes = fs::read(path).ok()?;
    Some(Ini::parse(&String::from_utf8_lossy(&bytes)))
}

fn map_of(ini: &Option<Ini>) -> HashMap<String, String> {
    ini.as_ref().map(|i| i.all().into_iter().collect()).unwrap_or_default()
}

fn file_for(dir: &Path, which: &str) -> Result<PathBuf, String> {
    match which {
        "game" => Ok(dir.join(GAME_INI)),
        "controls" => Ok(dir.join(CONTROLS_INI)),
        other => Err(format!("unknown settings file '{other}'")),
    }
}

pub fn read(dir: &Path) -> SettingsDump {
    let game = load(&dir.join(GAME_INI));
    let controls = load(&dir.join(CONTROLS_INI));
    SettingsDump {
        game_dir: crate::dump::fwd(dir),
        game_exists: game.is_some(),
        controls_exists: controls.is_some(),
        game: map_of(&game),
        controls: map_of(&controls),
    }
}

pub fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(d) = path.parent() {
        fs::create_dir_all(d).map_err(|e| format!("cannot create {}: {}", d.display(), e))?;
    }
    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {}", path.display(), e)
    })
}

/// Applies edits grouped per file. controls.ini is never created here: the game writes it with its factory bindings on first run.
pub fn apply(dir: &Path, edits: &[Edit]) -> Result<(), String> {
    for which in ["game", "controls"] {
        let mine: Vec<&Edit> = edits.iter().filter(|e| e.file == which).collect();
        if mine.is_empty() {
            continue;
        }
        let path = file_for(dir, which)?;
        let mut ini = match load(&path) {
            Some(i) => i,
            None if which == "game" => Ini::empty(),
            None => return Err(format!("{CONTROLS_INI} does not exist yet. Start the game once so it creates it.")),
        };
        for e in mine {
            ini.set(&e.section, &e.key, &e.value);
            // "Resolution" also has to size the window. [am] resolution is only what the console reports to the game
            // (am.cpp), while the window is created before any settings file is read, from [gfx] window_width/height
            // (xc2_boot.cpp). Measured 2026-11-18: with [am] alone the window stayed 1280x720.
            if which == "game" && e.section == "am" && e.key == "resolution" {
                let v = e.value.trim();
                let (w, h) = match v.split_once(['x', 'X']) {
                    Some((w, h)) => (w.trim().parse::<u32>().ok(), h.trim().parse::<u32>().ok()),
                    None => (None, None),
                };
                match (w, h) {
                    (Some(w), Some(h)) if w >= 320 && h >= 240 => {
                        ini.set("gfx", "window_width", &w.to_string());
                        ini.set("gfx", "window_height", &h.to_string());
                    }
                    _ => {
                        // Empty (auto) or unparsable: 0 means "the display's own size" to the runtime (xc2_boot.cpp),
                        // which is what the picture wants: the game composes at 2160p docked whatever the window is, so a
                        // fixed 1280x720 window here presented that composite at 720p (HANDOVER 220).
                        ini.set("gfx", "window_width", "0");
                        ini.set("gfx", "window_height", "0");
                    }
                }
            }
        }
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// The ini values setup owns: where the game code, the romfs, the dumped add-on content, the mods, the shader cache and the texture
/// cache are. With no dlc/ in the dump, [aoc] dir is empty: no add-on content.
fn owned(p: &Paths) -> Vec<(&'static str, &'static str, String)> {
    let norm_dlc = p.game_dir.join("game").join("dlc-301");
    let aoc_dir = if norm_dlc.is_dir() {
        Some(norm_dlc)
    } else {
        p.dlc.clone()
    };
    vec![
        ("fs", "exefs", crate::dump::game_path(&p.plain_exefs())),
        ("fs", "main", String::new()),
        ("fs", "romfs", crate::dump::game_path(&p.romfs)),
        ("fs", "mods", crate::dump::game_path(&p.mods())),
        ("aoc", "dir", aoc_dir.as_deref().map(crate::dump::game_path).unwrap_or_default()),
        ("nvn", "shader_cache", crate::dump::game_path(&p.spirv())),
        ("nvn", "bc7_cache", crate::dump::game_path(&p.bc7())),
    ]
}

pub fn play_paths_ok(dir: &Path, p: &Paths) -> bool {
    let Some(ini) = load(&dir.join(GAME_INI)) else { return false };
    owned(p).iter().all(|(s, k, v)| ini.get(s, k).unwrap_or_default().replace('\\', "/") == *v)
}

/// The shipped language tables (`locales\*.ini`), from the package next to the launcher or from the checkout.
fn locale_sources(p: &Paths) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Some(tools) = &p.package {
        if let Some(root) = tools.parent() {
            out.push(root.join("locales"));
        }
    }
    out.push(p.repo.join("locales"));
    out
}

/// Copies the language tables the package ships into the run folder, where fefw_rt.dll reads them (`locales\<lang>.ini` next to
/// the DLL). A table already there is left alone: it may be the player's own translation of that language. Only `.ini` files are
/// copied, and only the ones the run folder does not have yet.
pub fn install_locales(dir: &Path, p: &Paths) {
    let target = dir.join("locales");
    for src in locale_sources(p) {
        let Ok(rd) = fs::read_dir(&src) else { continue };
        for e in rd.flatten() {
            let from = e.path();
            let is_ini = from.extension().and_then(|x| x.to_str()).is_some_and(|x| x.eq_ignore_ascii_case("ini"));
            if !is_ini || !from.is_file() {
                continue;
            }
            let Some(name) = from.file_name() else { continue };
            let to = target.join(name);
            if to.exists() {
                continue; // the player's copy, or one this already put there
            }
            let _ = fs::create_dir_all(&target);
            if let Err(e) = fs::copy(&from, &to) {
                eprintln!("[locales] could not copy {} to {}: {e}", from.display(), to.display());
            }
        }
    }
}

/// Writes nxrt.ini's paths (kept: every other key, and a save folder the player chose). Used by setup and before every Play.
pub fn ensure_play_paths(dir: &Path, p: &Paths) -> Result<(), String> {
    let path = dir.join(GAME_INI);
    let new = !path.exists();
    let mut ini = load(&path).unwrap_or_else(|| {
        Ini::parse("; nxrt.ini: settings of the XC2 NS2 recomp runtime. The launcher writes the [fs] and [aoc] paths and the keys the settings page owns; every other line is left as it is.s.\n")
    });
    let mut changed = new;
    let _ = fs::create_dir_all(p.mods());
    install_locales(dir, p);
    for (s, k, v) in owned(p) {
        if ini.get(s, k).unwrap_or_default().replace('\\', "/") != v {
            ini.set(s, k, &v);
            changed = true;
        }
    }
    if ini.get("fs", "save_dir").unwrap_or_default().trim().is_empty() {
        let _ = fs::create_dir_all(dir.join("save"));
        ini.set("fs", "save_dir", "save"); // relative to the run folder
        changed = true;
    }
    // The game follows the console's language, so this must be a Switch code (or "auto" to follow Windows).
    // Written whenever it is missing; the settings page edits it afterwards.
    if ini.get("system", "language").unwrap_or_default().trim().is_empty() {
        ini.set("system", "language", "auto");
        changed = true;
    }
    if new {
        // Portable baseline: 1080p docked output, built-in TAAU at 100% native render quality with dynamic-res locked,
        // VSync on, Frame Generation off, 60 fps native step, and logs next to the executable.
        ini.set("log", "path", "nxrt.log");
        ini.set("am", "operation_mode", "docked");
        ini.set("am", "resolution", "1920x1080");
        ini.set("gfx", "window_width", "1920");
        ini.set("gfx", "window_height", "1080");
        ini.set("gfx", "present_mode", "fifo");
        ini.set("dlss", "upscaler", "taau");
        ini.set("dlss", "frame_gen", "0");
        ini.set("upscale", "render_quality", "native");
        ini.set("upscale", "lock_render_size", "1");
        ini.set("hdr", "mode", "auto");
        ini.set("xc2", "fps", "60");
    }
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("fefw-loc-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn paths_with(repo: &Path, game: &Path) -> Paths {
        Paths {
            repo: repo.to_path_buf(),
            exefs: repo.join("exefs"),
            romfs: repo.join("romfs"),
            dlc: None,
            game_dir: game.to_path_buf(),
            package: None,
        }
    }

    #[test]
    fn the_resolution_edit_also_sizes_the_window() {
        let dir = tmp("res");
        fs::write(dir.join(GAME_INI), "[fs]\nromfs = romfs\n").unwrap();
        let edit = |v: &str| Edit {
            file: "game".into(),
            section: "am".into(),
            key: "resolution".into(),
            value: v.into(),
        };
        apply(&dir, &[edit("1920x1080")]).unwrap();
        let ini = load(&dir.join(GAME_INI)).unwrap();
        // the console reports this to the game, and the window is created at this size before any file is read
        assert_eq!(ini.get("am", "resolution").as_deref(), Some("1920x1080"));
        assert_eq!(ini.get("gfx", "window_width").as_deref(), Some("1920"));
        assert_eq!(ini.get("gfx", "window_height").as_deref(), Some("1080"));

        // empty (auto) hands the window to the runtime's "the display's own size" (0), not to a fixed 720p window
        apply(&dir, &[edit("")]).unwrap();
        let ini = load(&dir.join(GAME_INI)).unwrap();
        assert_eq!(ini.get("gfx", "window_width").as_deref(), Some("0"));
        assert_eq!(ini.get("gfx", "window_height").as_deref(), Some("0"));
    }

    #[test]
    fn install_locales_copies_the_shipped_tables_only_once() {
        let repo = tmp("repo");
        let game = tmp("game");
        fs::create_dir_all(repo.join("locales")).unwrap();
        fs::write(repo.join("locales").join("zh.ini"), "shipped zh").unwrap();
        fs::write(repo.join("locales").join("README.md"), "docs").unwrap();
        let p = paths_with(&repo, &game);

        install_locales(&game, &p);
        assert_eq!(fs::read_to_string(game.join("locales").join("zh.ini")).unwrap(), "shipped zh");
        assert!(!game.join("locales").join("README.md").exists(), "only the tables are copied");

        // the player's own copy is never replaced by a later run
        fs::write(game.join("locales").join("zh.ini"), "the player's own").unwrap();
        install_locales(&game, &p);
        assert_eq!(fs::read_to_string(game.join("locales").join("zh.ini")).unwrap(), "the player's own");
    }

    #[test]
    fn ensure_play_paths_writes_1080p_native_taau_baseline_on_new_ini() {
        let repo = tmp("base-repo");
        let game = tmp("base-game");
        let p = paths_with(&repo, &game);
        ensure_play_paths(&game, &p).unwrap();
        let ini = load(&game.join(GAME_INI)).unwrap();
        assert_eq!(ini.get("am", "operation_mode").as_deref(), Some("docked"));
        assert_eq!(ini.get("am", "resolution").as_deref(), Some("1920x1080"));
        assert_eq!(ini.get("gfx", "window_width").as_deref(), Some("1920"));
        assert_eq!(ini.get("gfx", "window_height").as_deref(), Some("1080"));
        assert_eq!(ini.get("dlss", "upscaler").as_deref(), Some("taau"));
        assert_eq!(ini.get("dlss", "frame_gen").as_deref(), Some("0"));
        assert_eq!(ini.get("upscale", "render_quality").as_deref(), Some("native"));
        assert_eq!(ini.get("upscale", "lock_render_size").as_deref(), Some("1"));
        assert_eq!(ini.get("xc2", "fps").as_deref(), Some("60"));
        assert_eq!(ini.get("system", "language").as_deref(), Some("auto"));
    }
}
