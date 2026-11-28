// SPDX-License-Identifier: GPL-3.0-or-later
//! M4 launcher backend for <game_dir>/mods/:
//! - Scans mod folders (skipping `.dot` metadata files), `mod.json` metadata & options, RomFS/archive/ExeFS caps,
//!   and Atmosphère `cheats/<build_id>.txt` sections (`memory` vs static `.text` `code`).
//! - Reads and updates `nxrt.ini` (`[mods] order / disabled / pending_rebuild`) via `ini::Ini` (preserving comments),
//!   plus `.mod_choices.json`, `.mod_layers.tsv`, and `.cheats_enabled.tsv`.
//! - Detects logical file conflicts across enabled mods and compares desired M3 module fingerprints against
//!   `build/m3_applied.json` so stale code mods stay held off cleanly until Set up rebuilds them.

use crate::dump::EXEFS_FILES;
use crate::ini::Ini;
use crate::settings::{write_atomic, GAME_INI};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};

pub const APP_ID_DEFAULT: &str = "0100E95004038000";
pub const SUPPORTED_GAME_VERSION: &str = "3.0.1";
const CHOICES_FILE: &str = ".mod_choices.json";
const LAYERS_FILE: &str = ".mod_layers.tsv";
const CHEATS_FILE: &str = ".cheats_enabled.tsv";
const NSO_HDR_SIZE: usize = 0x100;

#[derive(Serialize, Clone, Default)]
pub struct ModMeta {
    pub name: String,
    pub version: String,
    pub game: String,
    pub author: String,
    pub description: String,
}

#[derive(Serialize, Clone, Default)]
pub struct ModCaps {
    pub romfs: bool,
    pub archive: bool,
    pub code_patches: usize,
    pub unmatched_patches: usize,
    pub cheats_mem: usize,
    pub cheats_code: usize,
}

#[derive(Serialize, Clone)]
pub struct OptionChoice {
    pub id: String,
    pub label: String,
    pub path: String,
}

#[derive(Serialize, Clone)]
pub struct ModOptionSpec {
    pub key: String,
    pub label: String,
    pub hint: String,
    /// "select" | "toggle"
    pub kind: String,
    /// String for "select", "true"/"false" for "toggle"
    pub value: String,
    pub path: String,
    pub choices: Vec<OptionChoice>,
    pub unsafe_path: bool,
}

#[derive(Serialize, Clone)]
pub struct CheatItem {
    pub title: String,
    /// "memory" | "code"
    pub kind: String,
    pub enabled: bool,
    pub file: String,
}

#[derive(Serialize, Clone)]
pub struct ModConflict {
    pub path: String,
    /// "wins" | "loses"
    pub role: String,
    pub peer: String,
}

#[derive(Serialize, Clone)]
pub struct ConflictGroup {
    pub path: String,
    pub winner: String,
    pub losers: Vec<String>,
}

#[derive(Serialize, Clone)]
pub struct ModCategory {
    /// "bdat" | "ui" | "model" | "map" | "media" | "archive" | "code" | "cheats" | "other"
    pub key: String,
    pub count: usize,
}

#[derive(Serialize, Clone, Default)]
pub struct ModRuntimeStatus {
    pub seen: bool,
    pub files: usize,
    pub replaced: usize,
    pub added: usize,
    pub ard_overrides: usize,
    pub archive_recs: usize,
    pub held_off: bool,
    pub skipped: bool,
}

#[derive(Serialize, Clone)]
pub struct ModItem {
    pub folder: String,
    pub enabled: bool,
    pub rank: usize,
    pub meta: Option<ModMeta>,
    pub meta_error: Option<String>,
    pub version_mismatch: bool,
    pub empty: bool,
    pub bdat_error: Option<String>,
    pub unmatched_cheats: usize,
    pub unmatched_paths_count: usize,
    pub unmatched_paths_sample: Vec<String>,
    /// Traffic-light health indicator: "green" | "yellow" | "red" | "off"
    pub health: String,
    /// Machine-readable reason key for UI tooltip/chip: "active" | "partial" | "pending_rebuild" | "not_verified" | "incompatible" | "empty" | "disabled"
    pub health_reason: String,
    pub runtime_status: ModRuntimeStatus,
    pub categories: Vec<ModCategory>,
    pub caps: ModCaps,
    pub pending_rebuild: bool,
    pub options: Vec<ModOptionSpec>,
    pub cheats: Vec<CheatItem>,
    pub conflict_count: usize,
    pub conflicts: Vec<ModConflict>,
}

#[derive(Serialize, Clone)]
pub struct ModsSnapshot {
    pub mods_dir: String,
    pub main_build_id: String,
    pub has_exefs_patched: bool,
    pub arh_loaded: bool,
    pub game_running: bool,
    pub needs_rebuild: bool,
    pub rebuild_modules: Vec<String>,
    pub mods: Vec<ModItem>,
    pub conflicts: Vec<ConflictGroup>,
}

#[derive(Serialize, Clone)]
pub struct ImportModResult {
    pub folder: String,
    pub warnings: Vec<String>,
    pub has_exefs: bool,
    pub snapshot: ModsSnapshot,
}

#[derive(Serialize, Clone)]
pub struct DeleteModResult {
    pub deleted: String,
    pub had_code_mods: bool,
    pub snapshot: ModsSnapshot,
}

#[derive(Serialize, Clone)]
pub struct ModLogView {
    pub file: String,
    pub lines: Vec<String>,
    pub total_matched: usize,
}

#[derive(Deserialize, Default)]
#[serde(default)]
pub struct SaveModsStateInput {
    pub order: Vec<String>,
    pub disabled: Vec<String>,
    pub choices: HashMap<String, HashMap<String, serde_json::Value>>,
    pub cheats_enabled: HashMap<String, Vec<String>>,
}

#[derive(Clone)]
pub struct ExefsTargetInfo {
    /// module name -> 64-char lowercase hex build_id
    pub build_ids: BTreeMap<String, String>,
    /// upper VA offset bound of main .text section (`text_mem_off + text_size`), 0 if unknown
    pub main_text_end: u64,
}

impl Default for ExefsTargetInfo {
    fn default() -> Self {
        Self { build_ids: BTreeMap::new(), main_text_end: 0 }
    }
}

pub fn exefs_target_info(exefs_dir: Option<&Path>, repo_dir: Option<&Path>) -> ExefsTargetInfo {
    let mut info = ExefsTargetInfo::default();
    if let Some(dir) = exefs_dir {
        for m in EXEFS_FILES {
            let p = dir.join(m);
            if let Some((bid, text_end)) = read_nso_info(&p) {
                info.build_ids.insert(m.to_string(), bid);
                if m == "main" {
                    info.main_text_end = text_end;
                }
            }
        }
    }
    // If vanilla ExeFS hasn't been ingested yet, still match `main` against config.py's 40-char build_id.
    if !info.build_ids.contains_key("main") {
        if let Some(repo) = repo_dir {
            if let Some((bid40, _)) = crate::dump::supported_builds(repo).into_iter().next() {
                let padded = format!("{:0<64}", bid40.to_ascii_lowercase());
                info.build_ids.insert("main".to_string(), padded);
            }
        }
    }
    info
}

fn read_nso_info(nso: &Path) -> Option<(String, u64)> {
    let mut h = [0u8; 0x60];
    fs::File::open(nso).ok()?.read_exact(&mut h).ok()?;
    if &h[..4] != b"NSO0" {
        return None;
    }
    let text_mem_off = u32::from_le_bytes(h[0x14..0x18].try_into().ok()?) as u64;
    let text_size = u32::from_le_bytes(h[0x18..0x1C].try_into().ok()?) as u64;
    let bid: String = h[0x40..0x60].iter().map(|b| format!("{b:02x}")).collect();
    Some((bid, text_mem_off.saturating_add(text_size)))
}

pub fn is_safe_layer_rel(raw: &str) -> bool {
    let s = raw.trim();
    if s.is_empty() || s.starts_with('/') || s.starts_with('\\') || s.contains(':') || s.contains('\t') || s.contains('\n') || s.contains('\r') {
        return false;
    }
    let p = Path::new(s);
    if p.is_absolute() {
        return false;
    }
    let mut has_normal = false;
    for comp in p.components() {
        match comp {
            Component::Normal(os) => {
                let part = os.to_string_lossy();
                if part.starts_with('.') {
                    return false;
                }
                has_normal = true;
            }
            _ => return false,
        }
    }
    has_normal
}

fn is_hex(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn build_id_matches(patch_bid: &str, mod_bid: &str) -> bool {
    let p = patch_bid.trim().to_ascii_lowercase();
    let m = mod_bid.trim().to_ascii_lowercase();
    if p.len() < 16 || !is_hex(&p) || m.len() != 64 || !is_hex(&m) {
        return false;
    }
    if p == m {
        return true;
    }
    if p.len() < 64 && m.starts_with(&p) && m[p.len()..].bytes().all(|b| b == b'0') {
        return true;
    }
    m.len() >= p.len() && m.starts_with(&p) && p.len() >= 40
}

fn cheat_bid_matches(file_stem: &str, mod_bid: &str) -> bool {
    let p = file_stem.trim().to_ascii_lowercase();
    let m = mod_bid.trim().to_ascii_lowercase();
    p.len() >= 16 && is_hex(&p) && m.len() == 64 && m.starts_with(&p)
}

fn split_csv(val: &str) -> Vec<String> {
    val.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect()
}

fn discover_mod_folders(mods_dir: &Path) -> Vec<String> {
    let Ok(rd) = fs::read_dir(mods_dir) else { return Vec::new() };
    let mut names: Vec<String> = rd
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with('.') || !e.path().is_dir() {
                return None;
            }
            Some(name)
        })
        .collect();
    names.sort_by_key(|a| a.to_ascii_lowercase());
    names
}

fn resolve_order(present: &[String], ini_order: &[String]) -> Vec<String> {
    let present_set: HashSet<&str> = present.iter().map(|s| s.as_str()).collect();
    let mut out = Vec::with_capacity(present.len());
    let mut seen = HashSet::new();
    for name in ini_order {
        if present_set.contains(name.as_str()) && seen.insert(name.clone()) {
            out.push(name.clone());
        }
    }
    for name in present {
        if seen.insert(name.clone()) {
            out.push(name.clone());
        }
    }
    out
}

fn read_choices_file(mods_dir: &Path) -> HashMap<String, HashMap<String, serde_json::Value>> {
    let p = mods_dir.join(CHOICES_FILE);
    let Ok(text) = fs::read_to_string(p) else { return HashMap::new() };
    serde_json::from_str(text.trim_start_matches('\u{feff}')).unwrap_or_default()
}

fn read_cheats_enabled_tsv(mods_dir: &Path) -> HashMap<String, HashSet<String>> {
    let p = mods_dir.join(CHEATS_FILE);
    let Ok(text) = fs::read_to_string(p) else { return HashMap::new() };
    let mut out: HashMap<String, HashSet<String>> = HashMap::new();
    for raw in text.lines() {
        let line = raw.trim_end_matches('\r');
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((m, title)) = line.split_once('\t') {
            let m = m.trim();
            let title = title.trim();
            if !m.is_empty() && !title.is_empty() {
                out.entry(m.to_string()).or_default().insert(title.to_string());
            }
        }
    }
    out
}

fn value_as_string(v: &serde_json::Value) -> Option<String> {
    match v {
        serde_json::Value::String(s) => Some(s.clone()),
        serde_json::Value::Bool(b) => Some(if *b { "true" } else { "false" }.to_string()),
        serde_json::Value::Number(n) => Some(n.to_string()),
        _ => None,
    }
}

fn parse_mod_json(
    mod_dir: &Path,
    saved_choices: Option<&HashMap<String, serde_json::Value>>,
) -> (Option<ModMeta>, Option<String>, Vec<ModOptionSpec>, bool) {
    let path = mod_dir.join("mod.json");
    if !path.is_file() {
        return (None, None, Vec::new(), false);
    }
    let text = match fs::read_to_string(&path) {
        Ok(t) => t,
        Err(e) => return (None, Some(format!("Cannot read mod.json: {e}")), Vec::new(), false),
    };
    let root: serde_json::Value = match serde_json::from_str(text.trim_start_matches('\u{feff}')) {
        Ok(v) => v,
        Err(e) => return (None, Some(format!("Invalid mod.json: {e}")), Vec::new(), false),
    };
    let Some(obj) = root.as_object() else {
        return (None, Some("mod.json must be a JSON object".into()), Vec::new(), false);
    };

    let s = |k: &str| obj.get(k).and_then(value_as_string).unwrap_or_default().trim().to_string();
    let meta = ModMeta { name: s("name"), version: s("version"), game: s("game"), author: s("author"), description: s("description") };
    let version_mismatch = !meta.game.is_empty() && !game_version_matches(&meta.game);

    let mut options = Vec::new();
    if let Some(opts) = obj.get("options").and_then(|v| v.as_object()) {
        for (key, raw_spec) in opts {
            let Some(spec) = raw_spec.as_object() else { continue };
            let label = spec.get("label").and_then(value_as_string).filter(|x| !x.is_empty()).unwrap_or_else(|| key.clone());
            let hint = spec.get("hint").and_then(value_as_string).unwrap_or_default();
            let raw_type = spec.get("type").and_then(value_as_string).unwrap_or_default().to_ascii_lowercase();
            let saved = saved_choices.and_then(|m| m.get(key));

            if raw_type == "toggle" || (raw_type.is_empty() && spec.get("values").is_none()) {
                let def_bool = spec.get("default").and_then(|v| v.as_bool()).unwrap_or(false);
                let active = saved.and_then(|v| v.as_bool()).or_else(|| saved.and_then(value_as_string).map(|x| x == "true" || x == "1")).unwrap_or(def_bool);
                let rel_path = spec.get("path").and_then(value_as_string).unwrap_or_default().trim().to_string();
                let unsafe_path = !rel_path.is_empty() && !is_safe_layer_rel(&rel_path);
                options.push(ModOptionSpec {
                    key: key.clone(),
                    label,
                    hint,
                    kind: "toggle".into(),
                    value: if active { "true" } else { "false" }.into(),
                    path: rel_path,
                    choices: Vec::new(),
                    unsafe_path,
                });
                continue;
            }

            let mut choices = Vec::new();
            if let Some(vals) = spec.get("values").and_then(|v| v.as_object()) {
                for (cid, raw_c) in vals {
                    let (clabel, cpath) = if let Some(co) = raw_c.as_object() {
                        (
                            co.get("label").and_then(value_as_string).filter(|x| !x.is_empty()).unwrap_or_else(|| cid.clone()),
                            co.get("path").and_then(value_as_string).unwrap_or_default().trim().to_string(),
                        )
                    } else {
                        (cid.clone(), value_as_string(raw_c).unwrap_or_default().trim().to_string())
                    };
                    choices.push(OptionChoice { id: cid.clone(), label: clabel, path: cpath });
                }
            }
            let def_val = spec.get("default").and_then(value_as_string).unwrap_or_else(|| choices.first().map(|c| c.id.clone()).unwrap_or_default());
            let mut val = saved.and_then(value_as_string).unwrap_or(def_val);
            if !choices.is_empty() && !choices.iter().any(|c| c.id == val) {
                val = choices[0].id.clone();
            }
            let selected_path = choices.iter().find(|c| c.id == val).map(|c| c.path.clone()).unwrap_or_default();
            let unsafe_path = !selected_path.is_empty() && !is_safe_layer_rel(&selected_path);
            options.push(ModOptionSpec {
                key: key.clone(),
                label,
                hint,
                kind: "select".into(),
                value: val,
                path: selected_path,
                choices,
                unsafe_path,
            });
        }
    }

    (Some(meta), None, options, version_mismatch)
}

fn game_version_matches(game: &str) -> bool {
    let g = game.trim();
    g == SUPPORTED_GAME_VERSION || g == "*" || g.eq_ignore_ascii_case("any")
}

fn base_romfs_dir(mod_dir: &Path) -> Option<PathBuf> {
    let mut cands = vec![mod_dir.join("romfs"), mod_dir.join("atmosphere").join("contents").join(APP_ID_DEFAULT).join("romfs"), mod_dir.join("contents").join(APP_ID_DEFAULT).join("romfs"), mod_dir.join(APP_ID_DEFAULT).join("romfs")];
    if let Ok(rd) = fs::read_dir(mod_dir.join("atmosphere").join("contents")) {
        for e in rd.flatten() {
            if e.path().join("romfs").is_dir() {
                cands.push(e.path().join("romfs"));
            }
        }
    }
    cands.into_iter().find(|p| p.is_dir())
}

fn has_archive_content(root: &Path) -> bool {
    if root.join("bf2.ard").is_dir() || root.join("ard").is_dir() || root.join("append.bin").is_file() {
        return true;
    }
    if let Ok(rd) = fs::read_dir(root) {
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() && p.extension().and_then(|s| s.to_str()).map_or(false, |x| x.eq_ignore_ascii_case("ard")) {
                return true;
            }
        }
    }
    false
}

struct DiscoveredPatch {
    source: String,
    sha256: String,
    build_id: String,
}

fn strip_pchtxt_comment(line: &str) -> &str {
    let bytes = line.as_bytes();
    let mut in_quote = false;
    let mut i = 0;
    while i < bytes.len() {
        let ch = bytes[i];
        if ch == b'"' && (i == 0 || bytes[i - 1] != b'\\') {
            in_quote = !in_quote;
        } else if !in_quote {
            if ch == b'#' || (ch == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'/') {
                return &line[..i];
            }
        }
        i += 1;
    }
    line
}

pub fn parse_pchtxt_header_pub(text: &str) -> (String, bool) {
    parse_pchtxt_header(text)
}

fn parse_pchtxt_header(text: &str) -> (String, bool) {
    let mut build_id = String::new();
    let mut enabled = false;
    let mut has_records = false;
    for raw in text.lines() {
        let line = strip_pchtxt_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        if let Some(rest) = line.strip_prefix('@') {
            let low = rest.to_ascii_lowercase();
            if let Some(bid) = low.strip_prefix("nsobid-") {
                let b = bid.trim();
                if b.len() >= 16 && is_hex(b) {
                    build_id = b.to_string();
                }
            } else if low == "enabled" {
                enabled = true;
            } else if low == "disabled" {
                enabled = false;
            } else if low == "stop" {
                break;
            }
            continue;
        }
        if enabled && line.split_whitespace().count() >= 2 {
            has_records = true;
        }
    }
    let valid = has_records && !build_id.is_empty();
    (build_id, valid)
}

fn has_valid_ips_records(data: &[u8]) -> bool {
    if data.len() < 8 {
        return false;
    }
    let (off_len, eof): (usize, &[u8]) = if data.starts_with(b"IPS32") {
        (4, b"EEOF")
    } else if data.starts_with(b"PATCH") {
        (3, b"EOF")
    } else {
        return false;
    };
    let mut pos = 5usize;
    let mut count = 0usize;
    while pos < data.len() {
        if data[pos..].starts_with(eof) {
            return count > 0;
        }
        if pos + off_len + 2 > data.len() {
            return false;
        }
        pos += off_len;
        let size = u16::from_be_bytes([data[pos], data[pos + 1]]) as usize;
        pos += 2;
        if size == 0 {
            if pos + 3 > data.len() {
                return false;
            }
            pos += 3;
        } else {
            if pos + size > data.len() {
                return false;
            }
            pos += size;
        }
        count += 1;
    }
    false
}

fn mod_patch_roots(mod_dir: &Path) -> Vec<PathBuf> {
    let mut roots = Vec::new();
    for cand in [
        mod_dir.join("exefs"),
        mod_dir.join("atmosphere").join("contents").join(APP_ID_DEFAULT).join("exefs"),
        mod_dir.join("contents").join(APP_ID_DEFAULT).join("exefs"),
        mod_dir.join(APP_ID_DEFAULT).join("exefs"),
    ] {
        if cand.is_dir() {
            roots.push(cand);
        }
    }
    let atm_ep = mod_dir.join("atmosphere").join("exefs_patches");
    if atm_ep.is_dir() {
        if let Ok(rd) = fs::read_dir(&atm_ep) {
            let mut subs: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
            subs.sort();
            roots.extend(subs);
        }
        roots.push(atm_ep);
    }
    roots
}

fn scan_mod_exefs_patches(mod_name: &str, mod_dir: &Path) -> Vec<DiscoveredPatch> {
    let mut seen: HashSet<PathBuf> = HashSet::new();
    let mut out = Vec::new();
    for root in mod_patch_roots(mod_dir) {
        let Ok(rd) = fs::read_dir(&root) else { continue };
        let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.is_file()).collect();
        files.sort();
        for p in files {
            let canon = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
            if !seen.insert(canon) {
                continue;
            }
            let Some(ext) = p.extension().and_then(|s| s.to_str()).map(|s| s.to_ascii_lowercase()) else {
                continue;
            };
            let Ok(raw) = fs::read(&p) else { continue };
            let file_name = p.file_name().and_then(|s| s.to_str()).unwrap_or_default().to_string();
            let sha = hex_sha256(&raw);
            if ext == "pchtxt" {
                let text = String::from_utf8_lossy(&raw);
                let (bid, valid) = parse_pchtxt_header(&text);
                if valid {
                    out.push(DiscoveredPatch { source: format!("{mod_name}:{file_name}"), sha256: sha, build_id: bid });
                }
            } else if ext == "ips" {
                let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or_default().to_ascii_lowercase();
                if stem.len() >= 16 && is_hex(&stem) && has_valid_ips_records(&raw) {
                    out.push(DiscoveredPatch { source: format!("{mod_name}:{file_name}"), sha256: sha, build_id: stem });
                }
            }
        }
    }
    out
}

fn cheat_roots(mod_dir: &Path) -> Vec<PathBuf> {
    let mut roots = Vec::new();
    for cand in [
        mod_dir.join("cheats"),
        mod_dir.join("atmosphere").join("contents").join(APP_ID_DEFAULT).join("cheats"),
        mod_dir.join("contents").join(APP_ID_DEFAULT).join("cheats"),
        mod_dir.join(APP_ID_DEFAULT).join("cheats"),
    ] {
        if cand.is_dir() {
            roots.push(cand);
        }
    }
    roots
}

/// Parses `[Cheat Title]` sections and classifies each section as `"code"` (pure static Type `0` write(s)
/// into `main .text` below `main_text_end`) vs `"memory"` (heap/data writes, pointer chains, or keypress/loop).
pub fn parse_cheat_text(text: &str, file_name: &str, main_text_end: u64, enabled_titles: Option<&HashSet<String>>) -> Vec<(CheatItem, Vec<(u64, Vec<u8>)>)> {
    let mut out = Vec::new();
    let mut cur_title: Option<String> = None;
    let mut cur_words: Vec<u32> = Vec::new();

    let flush = |title_opt: Option<String>, words: &mut Vec<u32>, out: &mut Vec<(CheatItem, Vec<(u64, Vec<u8>)>)>| {
        let Some(title) = title_opt else {
            words.clear();
            return;
        };
        if words.is_empty() {
            return;
        }
        let (is_code, writes) = classify_dmnt_words(words, main_text_end);
        let enabled = enabled_titles.map_or(false, |s| s.contains(&title));
        out.push((
            CheatItem {
                title,
                kind: if is_code { "code" } else { "memory" }.into(),
                enabled,
                file: file_name.to_string(),
            },
            if is_code { writes } else { Vec::new() },
        ));
        words.clear();
    };

    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with("//") {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') && line.len() >= 2 {
            flush(cur_title.take(), &mut cur_words, &mut out);
            let t = line[1..line.len() - 1].trim().to_string();
            if !t.is_empty() {
                cur_title = Some(t);
            }
            continue;
        }
        if cur_title.is_none() {
            continue;
        }
        let code_part = line.split('#').next().unwrap_or(line).split("//").next().unwrap_or(line);
        for tok in code_part.split_whitespace() {
            if tok.len() == 8 && is_hex(tok) {
                if let Ok(w) = u32::from_str_radix(tok, 16) {
                    cur_words.push(w);
                }
            }
        }
    }
    flush(cur_title.take(), &mut cur_words, &mut out);
    out
}

fn classify_dmnt_words(words: &[u32], main_text_end: u64) -> (bool, Vec<(u64, Vec<u8>)>) {
    let mut i = 0usize;
    let mut has_runtime_op = false;
    let mut has_text_write = false;
    let mut writes = Vec::new();

    while i < words.len() {
        let w0 = words[i];
        let op = (w0 >> 28) & 0xF;
        match op {
            0 => {
                // 0TMR00AA AAAAAAAA VVVVVVVV [VVVVVVVV]
                let width = ((w0 >> 24) & 0xF) as usize;
                let region = (w0 >> 20) & 0xF;
                let need = if width == 8 { 4 } else { 3 };
                if i + need > words.len() || !matches!(width, 1 | 2 | 4 | 8) {
                    has_runtime_op = true;
                    break;
                }
                let addr = (((w0 & 0xFF) as u64) << 32) | (words[i + 1] as u64);
                if region == 0 && main_text_end > 0 && addr < main_text_end {
                    has_text_write = true;
                    let mut payload = Vec::with_capacity(width);
                    match width {
                        1 => payload.push((words[i + 2] & 0xFF) as u8),
                        2 => payload.extend_from_slice(&(words[i + 2] as u16).to_le_bytes()),
                        4 => payload.extend_from_slice(&words[i + 2].to_le_bytes()),
                        8 => {
                            payload.extend_from_slice(&words[i + 3].to_le_bytes());
                            payload.extend_from_slice(&words[i + 2].to_le_bytes());
                        }
                        _ => {}
                    }
                    writes.push((NSO_HDR_SIZE as u64 + addr, payload));
                } else {
                    has_runtime_op = true;
                }
                i += need;
            }
            1 => {
                has_runtime_op = true;
                let width = (w0 >> 24) & 0xF;
                i += if width == 8 { 4 } else { 3 };
            }
            2 | 8 => {
                has_runtime_op = true;
                i += 1;
            }
            3 => {
                has_runtime_op = true;
                let sub = (w0 >> 24) & 0xF;
                i += if sub == 0 { 2 } else { 1 };
            }
            4 | 6 => {
                has_runtime_op = true;
                i += 3;
            }
            5 | 7 => {
                has_runtime_op = true;
                i += 2;
            }
            9 => {
                has_runtime_op = true;
                let width = (w0 >> 24) & 0xF;
                let has_imm = ((w0 >> 8) & 0xF) != 0;
                i += if !has_imm {
                    1
                } else if width == 8 {
                    3
                } else {
                    2
                };
            }
            0xA => {
                has_runtime_op = true;
                let off_type = (w0 >> 12) & 0xF;
                i += if matches!(off_type, 2 | 4 | 5) { 3 } else { 2 };
            }
            _ => {
                has_runtime_op = true;
                i += 1;
            }
        }
    }
    let is_code = has_text_write && !has_runtime_op && !writes.is_empty();
    (is_code, if is_code { writes } else { Vec::new() })
}

struct ScannedCheats {
    items: Vec<CheatItem>,
    code_patches: Vec<DiscoveredPatch>,
}

fn scan_mod_cheats(
    mod_name: &str,
    mod_dir: &Path,
    main_bid: Option<&str>,
    main_text_end: u64,
    enabled_set: Option<&HashSet<String>>,
) -> ScannedCheats {
    let mut out = ScannedCheats { items: Vec::new(), code_patches: Vec::new() };
    let Some(bid) = main_bid else {
        // Still surface cheat files even before build_id is known? Require matching stem if bid known; if unknown, skip matching check.
        return scan_mod_cheats_unfiltered(mod_name, mod_dir, main_text_end, enabled_set);
    };
    let mut seen: HashSet<PathBuf> = HashSet::new();
    for root in cheat_roots(mod_dir) {
        let Ok(rd) = fs::read_dir(&root) else { continue };
        let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.is_file()).collect();
        files.sort();
        for p in files {
            if !p.extension().and_then(|s| s.to_str()).map_or(false, |e| e.eq_ignore_ascii_case("txt")) {
                continue;
            }
            let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or_default();
            if !cheat_bid_matches(stem, bid) {
                continue;
            }
            let canon = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
            if !seen.insert(canon) {
                continue;
            }
            let Ok(raw) = fs::read(&p) else { continue };
            let file_name = p.file_name().and_then(|s| s.to_str()).unwrap_or_default().to_string();
            let text = String::from_utf8_lossy(&raw);
            for (item, writes) in parse_cheat_text(&text, &file_name, main_text_end, enabled_set) {
                if item.enabled && item.kind == "code" {
                    let canonical = canonical_code_cheat_digest(&writes);
                    out.code_patches.push(DiscoveredPatch {
                        source: format!("{mod_name}:{file_name}#{}", item.title),
                        sha256: canonical,
                        build_id: bid.to_string(),
                    });
                }
                out.items.push(item);
            }
        }
    }
    out
}

fn scan_mod_cheats_unfiltered(
    _mod_name: &str,
    mod_dir: &Path,
    main_text_end: u64,
    enabled_set: Option<&HashSet<String>>,
) -> ScannedCheats {
    let mut out = ScannedCheats { items: Vec::new(), code_patches: Vec::new() };
    let mut seen: HashSet<PathBuf> = HashSet::new();
    for root in cheat_roots(mod_dir) {
        let Ok(rd) = fs::read_dir(&root) else { continue };
        let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.is_file()).collect();
        files.sort();
        for p in files {
            if !p.extension().and_then(|s| s.to_str()).map_or(false, |e| e.eq_ignore_ascii_case("txt")) {
                continue;
            }
            let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or_default();
            if stem.len() < 16 || !is_hex(stem) {
                continue;
            }
            let canon = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
            if !seen.insert(canon) {
                continue;
            }
            let Ok(raw) = fs::read(&p) else { continue };
            let file_name = p.file_name().and_then(|s| s.to_str()).unwrap_or_default().to_string();
            let text = String::from_utf8_lossy(&raw);
            for (item, _) in parse_cheat_text(&text, &file_name, main_text_end, enabled_set) {
                out.items.push(item);
            }
        }
    }
    out
}

fn canonical_code_cheat_digest(writes: &[(u64, Vec<u8>)]) -> String {
    let mut s = String::new();
    for (off, bytes) in writes {
        s.push_str(&format!("{off:x}:"));
        for b in bytes {
            s.push_str(&format!("{b:02x}"));
        }
        s.push('\n');
    }
    hex_sha256(s.as_bytes())
}

fn hex_sha256(data: &[u8]) -> String {
    let digest = Sha256::digest(data);
    digest.iter().map(|b| format!("{b:02x}")).collect()
}

fn collect_tree_logical_paths(root: &Path, into: &mut HashSet<String>) {
    fn walk(base: &Path, cur: &Path, out: &mut HashSet<String>) {
        let Ok(rd) = fs::read_dir(cur) else { return };
        for e in rd.flatten() {
            let p = e.path();
            let name = e.file_name().to_string_lossy().to_string();
            if name.starts_with('.') {
                continue;
            }
            if p.is_dir() {
                walk(base, &p, out);
            } else if p.is_file() {
                if let Ok(rel) = p.strip_prefix(base) {
                    let logical = rel.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
                    if !logical.is_empty() {
                        out.insert(logical);
                    }
                }
            }
        }
    }
    if root.is_dir() {
        walk(root, root, into);
    }
}

fn read_applied_modules(game_dir: &Path) -> HashMap<String, String> {
    let p = game_dir.join("build").join("m3_applied.json");
    let Ok(text) = fs::read_to_string(p) else { return HashMap::new() };
    let Ok(val) = serde_json::from_str::<serde_json::Value>(&text) else { return HashMap::new() };
    let mut out = HashMap::new();
    if let Some(mods) = val.get("modules").and_then(|v| v.as_object()) {
        for (k, v) in mods {
            if let Some(s) = v.as_str() {
                out.insert(k.clone(), s.to_string());
            }
        }
    }
    out
}

fn count_unmatched_cheat_files(mod_dir: &Path, main_bid: Option<&str>) -> usize {
    let Some(bid) = main_bid else { return 0 };
    let mut seen: HashSet<PathBuf> = HashSet::new();
    let mut unmatched = 0usize;
    for root in cheat_roots(mod_dir) {
        let Ok(rd) = fs::read_dir(&root) else { continue };
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_file() || !p.extension().and_then(|s| s.to_str()).map_or(false, |x| x.eq_ignore_ascii_case("txt")) {
                continue;
            }
            let canon = fs::canonicalize(&p).unwrap_or_else(|_| p.clone());
            if !seen.insert(canon) {
                continue;
            }
            let stem = p.file_stem().and_then(|s| s.to_str()).unwrap_or_default();
            if !cheat_bid_matches(stem, bid) {
                unmatched += 1;
            }
        }
    }
    unmatched
}

fn check_layers_common_bdat(layers: &[PathBuf]) -> Option<String> {
    fn find_bdat(dir: &Path) -> Option<PathBuf> {
        let Ok(rd) = fs::read_dir(dir) else { return None };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                if let Some(hit) = find_bdat(&p) {
                    return Some(hit);
                }
            } else if p.is_file()
                && e.file_name().to_string_lossy().eq_ignore_ascii_case("common.bdat")
            {
                return Some(p);
            }
        }
        None
    }
    for layer in layers {
        if let Some(bdat) = find_bdat(layer) {
            match crate::arh::inspect_common_bdat(&bdat) {
                crate::arh::BdatCheck::Ok { .. } => {}
                crate::arh::BdatCheck::Mismatch { tables, .. } => {
                    return Some(format!(
                        "common.bdat has {tables} tables (NS2 3.0.1 requires {}; NS1 has 844)",
                        crate::arh::NS2_COMMON_BDAT_TABLES
                    ));
                }
                crate::arh::BdatCheck::Invalid(msg) => {
                    return Some(msg);
                }
            }
        }
    }
    None
}

fn classify_mod_categories(
    romfs_paths: &HashSet<String>,
    has_archive: bool,
    code_patches: usize,
    cheats_total: usize,
) -> Vec<ModCategory> {
    let mut counts: BTreeMap<&'static str, usize> = BTreeMap::new();
    for lp in romfs_paths {
        let p = lp.trim_start_matches('/');
        let first = p.split('/').next().unwrap_or("");
        let cat = if first == "bdat" || p.ends_with(".bdat") {
            "bdat"
        } else if matches!(first, "menu" | "ui") || p.ends_with(".wilay") {
            "ui"
        } else if matches!(first, "chr" | "model" | "effect" | "event") || p.ends_with(".wimdo") {
            "model"
        } else if matches!(first, "map" | "shader") {
            "map"
        } else if matches!(first, "sound" | "stream" | "movie")
            || p.ends_with(".bnk")
            || p.ends_with(".wem")
            || p.ends_with(".mp4")
            || p.ends_with(".webm")
        {
            "media"
        } else if matches!(p, "bf2.arh" | "bf2.ard" | "append.bin") || first.ends_with(".ard") || first == "ard" {
            "archive"
        } else {
            "other"
        };
        *counts.entry(cat).or_insert(0) += 1;
    }
    if has_archive && !counts.contains_key("archive") {
        counts.insert("archive", 1);
    }
    if code_patches > 0 {
        counts.insert("code", code_patches);
    }
    if cheats_total > 0 {
        counts.insert("cheats", cheats_total);
    }
    let order = ["bdat", "ui", "model", "map", "media", "archive", "code", "cheats", "other"];
    let mut out = Vec::new();
    for k in order {
        if let Some(&c) = counts.get(k) {
            out.push(ModCategory { key: k.to_string(), count: c });
        }
    }
    out
}

fn find_latest_log_path(game_dir: &Path) -> Option<PathBuf> {
    let mut candidates: Vec<(std::time::SystemTime, PathBuf)> = Vec::new();
    let logs_dir = game_dir.join("work").join("logs");
    if let Ok(rd) = fs::read_dir(&logs_dir) {
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_file() {
                continue;
            }
            let fname = e.file_name().to_string_lossy().to_string();
            if !(fname.starts_with("play-") && fname.ends_with(".log")) {
                continue;
            }
            let mtime = e.metadata().and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH);
            candidates.push((mtime, p));
        }
    }
    for name in ["nxrt.log", "fefw.log"] {
        let p = game_dir.join(name);
        if p.is_file() {
            let mtime = fs::metadata(&p).and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH);
            candidates.push((mtime, p));
        }
    }
    candidates.sort_by_key(|(t, _)| *t);
    candidates.pop().map(|(_, p)| p)
}

fn parse_first_usize(s: &str) -> Option<usize> {
    s.trim().split_whitespace().next()?.parse::<usize>().ok()
}

/// Parses the latest launch log to extract per-mod runtime effectiveness (`mods:` / `cheats:` lines).
fn parse_runtime_log_statuses(game_dir: &Path) -> (HashMap<String, ModRuntimeStatus>, usize) {
    let mut map: HashMap<String, ModRuntimeStatus> = HashMap::new();
    let mut active_cheats = 0usize;
    let Some(log_path) = find_latest_log_path(game_dir) else {
        return (map, 0);
    };
    let Ok(text) = fs::read_to_string(&log_path) else {
        return (map, 0);
    };
    for line in text.lines() {
        // e.g. "[...] cheats: loaded 3 active runtime cheat(s) from ..."
        if let Some(idx) = line.find("loaded ") {
            if line.contains("active runtime cheat(s)") {
                if let Some(n) = parse_first_usize(&line[idx + 7..]) {
                    active_cheats = active_cheats.max(n);
                }
            }
        }
        // Look for "mods: <mod_name>..."
        let Some(pos) = line.find("mods: ") else { continue };
        let body = &line[pos + 6..];
        if let Some(rest) = body.strip_suffix(" held off until Set up rebuilds code patches") {
            let name = rest.trim();
            if !name.is_empty() {
                let st = map.entry(name.to_string()).or_default();
                st.seen = true;
                st.held_off = true;
            }
            continue;
        }
        if let Some(rest) = body.strip_suffix(" has no romfs folder: skipped") {
            let name = rest.trim();
            if !name.is_empty() {
                let st = map.entry(name.to_string()).or_default();
                st.seen = true;
                st.skipped = true;
            }
            continue;
        }
        if let Some((name_raw, detail)) = body.split_once(": ") {
            let name = name_raw.trim();
            if name.is_empty() || name.contains(' ') && !detail.contains("file(s)") && !detail.starts_with("loose romfs/") && !detail.contains("archive record(s)") {
                // Skip generic lines like "mods: 1 enabled, 0 disabled..." or "mods: ard: ..."
                if matches!(name, "ard" | "no folder at" | "bf2.arh") {
                    continue;
                }
            }
            if detail.starts_with("loose romfs/") && detail.contains("-> bf2.ard") {
                let st = map.entry(name.to_string()).or_default();
                st.seen = true;
                st.ard_overrides += 1;
            } else if detail.contains(" file(s), ") && detail.contains(" replaced, ") {
                let st = map.entry(name.to_string()).or_default();
                st.seen = true;
                if let Some(f) = parse_first_usize(detail) {
                    st.files = f;
                }
                if let Some(r_idx) = detail.find("file(s), ") {
                    if let Some(r) = parse_first_usize(&detail[r_idx + 9..]) {
                        st.replaced = r;
                    }
                }
                if let Some(a_idx) = detail.find("replaced, ") {
                    if let Some(a) = parse_first_usize(&detail[a_idx + 10..]) {
                        st.added = a;
                    }
                }
            } else if detail.contains(" archive record(s)") {
                let st = map.entry(name.to_string()).or_default();
                st.seen = true;
                if let Some(n) = parse_first_usize(detail) {
                    st.archive_recs += n;
                }
            }
        }
    }
    (map, active_cheats)
}

pub fn compute_snapshot(game_dir: &Path, exefs: &ExefsTargetInfo, vanilla_romfs: Option<&Path>, game_running: bool) -> ModsSnapshot {
    let mods_dir = game_dir.join("mods");
    let present = discover_mod_folders(&mods_dir);
    let ini = fs::read_to_string(game_dir.join(GAME_INI)).map(|t| Ini::parse(&t)).unwrap_or_else(|_| Ini::empty());
    let ini_order = split_csv(&ini.get("mods", "order").unwrap_or_default());
    let disabled_set: HashSet<String> = split_csv(&ini.get("mods", "disabled").unwrap_or_default()).into_iter().collect();
    let ordered = resolve_order(&present, &ini_order);

    let saved_choices = read_choices_file(&mods_dir);
    let cheats_enabled = read_cheats_enabled_tsv(&mods_dir);
    let arh_index = crate::arh::cached_arh_index(vanilla_romfs);
    let (runtime_statuses, runtime_cheats_loaded) = parse_runtime_log_statuses(game_dir);

    let mut items = Vec::with_capacity(ordered.len());
    // Accumulate M3 patches per module in lowest -> highest priority order (so SHA-256 matches exefs_patch.py).
    let mut m3_by_mod: BTreeMap<String, Vec<DiscoveredPatch>> = exefs.build_ids.keys().map(|m| (m.clone(), Vec::new())).collect();
    // Map each enabled mod -> its contributed module names & logical RomFS paths for conflict checks.
    let mut mod_touched_modules: HashMap<String, Vec<String>> = HashMap::new();
    let mut path_owners: BTreeMap<String, Vec<String>> = BTreeMap::new();

    for (idx, folder) in ordered.iter().enumerate() {
        let mod_dir = mods_dir.join(folder);
        let enabled = !disabled_set.contains(folder);
        let (meta, meta_error, options, version_mismatch) = parse_mod_json(&mod_dir, saved_choices.get(folder));

        let base_r = base_romfs_dir(&mod_dir);
        let mut active_layers: Vec<PathBuf> = Vec::new();
        if let Some(br) = &base_r {
            active_layers.push(br.clone());
        }
        for opt in &options {
            if opt.kind == "toggle" && opt.value != "true" {
                continue;
            }
            if !opt.path.is_empty() && !opt.unsafe_path {
                let candidate = mod_dir.join(&opt.path);
                if candidate.is_dir() {
                    active_layers.push(candidate);
                }
            }
        }
        let mut my_paths: HashSet<String> = HashSet::new();
        for layer in &active_layers {
            collect_tree_logical_paths(layer, &mut my_paths);
        }
        let has_romfs = !my_paths.is_empty();
        let has_archive = active_layers.iter().any(|p| has_archive_content(p));
        let bdat_error = check_layers_common_bdat(&active_layers);

        let mut unmatched_paths_count = 0usize;
        let mut unmatched_paths_sample = Vec::new();
        if let Some(ref idx) = arh_index {
            let mut sorted_paths: Vec<&String> = my_paths.iter().collect();
            sorted_paths.sort();
            for lp in sorted_paths {
                if !idx.is_known_romfs_path(lp, vanilla_romfs) {
                    unmatched_paths_count += 1;
                    if unmatched_paths_sample.len() < 10 {
                        unmatched_paths_sample.push(lp.clone());
                    }
                }
            }
        }

        let exefs_patches = scan_mod_exefs_patches(folder, &mod_dir);
        let main_bid = exefs.build_ids.get("main").map(|s| s.as_str());
        let scanned_cheats = scan_mod_cheats(folder, &mod_dir, main_bid, exefs.main_text_end, cheats_enabled.get(folder));
        let unmatched_cheats = count_unmatched_cheat_files(&mod_dir, main_bid);

        let mut matched_count = 0usize;
        let mut unmatched_count = 0usize;
        let mut touched_mods: Vec<String> = Vec::new();
        let mut matched_patches_for_hash: Vec<(String, DiscoveredPatch)> = Vec::new();

        for pat in exefs_patches {
            if let Some((target_m, _)) = exefs.build_ids.iter().find(|(_, bid)| build_id_matches(&pat.build_id, bid)) {
                matched_count += 1;
                touched_mods.push(target_m.clone());
                matched_patches_for_hash.push((target_m.clone(), pat));
            } else {
                unmatched_count += 1;
            }
        }
        for cp in scanned_cheats.code_patches {
            touched_mods.push("main".to_string());
            matched_patches_for_hash.push(("main".to_string(), cp));
        }
        mod_touched_modules.insert(folder.clone(), touched_mods);

        let cheats_mem = scanned_cheats.items.iter().filter(|c| c.kind == "memory").count();
        let cheats_code = scanned_cheats.items.iter().filter(|c| c.kind == "code").count();
        let categories = classify_mod_categories(&my_paths, has_archive, matched_count, scanned_cheats.items.len());
        let rt_status = runtime_statuses.get(folder).cloned().unwrap_or_default();
        let empty = !has_romfs
            && matched_count == 0
            && unmatched_count == 0
            && scanned_cheats.items.is_empty()
            && unmatched_cheats == 0;

        if enabled {
            for lp in my_paths {
                path_owners.entry(lp).or_default().push(folder.clone());
            }
            for (m, _) in &matched_patches_for_hash {
                path_owners.entry(format!("exefs/{m}")).or_default().push(folder.clone());
            }
        }

        items.push((
            matched_patches_for_hash,
            ModItem {
                folder: folder.clone(),
                enabled,
                rank: idx + 1,
                meta,
                meta_error,
                version_mismatch,
                empty,
                bdat_error,
                unmatched_cheats,
                unmatched_paths_count,
                unmatched_paths_sample,
                health: "off".into(),
                health_reason: "disabled".into(),
                runtime_status: rt_status,
                categories,
                caps: ModCaps {
                    romfs: has_romfs,
                    archive: has_archive,
                    code_patches: matched_count,
                    unmatched_patches: unmatched_count,
                    cheats_mem,
                    cheats_code,
                },
                pending_rebuild: false,
                options,
                cheats: scanned_cheats.items,
                conflict_count: 0,
                conflicts: Vec::new(),
            },
        ));
    }

    // Iterate reverse (lowest priority -> highest priority) so m3_by_mod matches exefs_patch.py hash order.
    for (matched_list, item) in items.iter().rev() {
        if !item.enabled {
            continue;
        }
        for (m, pat) in matched_list {
            if let Some(bucket) = m3_by_mod.get_mut(m) {
                bucket.push(DiscoveredPatch { source: pat.source.clone(), sha256: pat.sha256.clone(), build_id: pat.build_id.clone() });
            }
        }
    }

    let applied = read_applied_modules(game_dir);
    let mut rebuild_modules = Vec::new();
    for (m, pats) in &m3_by_mod {
        let desired_hash = if pats.is_empty() {
            String::new()
        } else {
            let mut buf = String::new();
            for p in pats {
                buf.push_str(&p.source);
                buf.push('\t');
                buf.push_str(&p.sha256);
                buf.push('\n');
            }
            hex_sha256(buf.as_bytes())
        };
        let prev = applied.get(m).map(|s| s.as_str()).unwrap_or("");
        if desired_hash != prev {
            rebuild_modules.push(m.clone());
        }
    }
    let dirty_mod_set: HashSet<&str> = rebuild_modules.iter().map(|s| s.as_str()).collect();
    let needs_rebuild = !rebuild_modules.is_empty();

    // Build conflict list and per-mod views.
    let mut conflict_groups = Vec::new();
    let mut mod_conflicts: HashMap<String, Vec<ModConflict>> = HashMap::new();
    for (lp, owners) in path_owners {
        if owners.len() < 2 {
            continue;
        }
        let winner = owners[0].clone();
        let losers = owners[1..].to_vec();
        for l in &losers {
            mod_conflicts.entry(winner.clone()).or_default().push(ModConflict { path: lp.clone(), role: "wins".into(), peer: l.clone() });
            mod_conflicts.entry(l.clone()).or_default().push(ModConflict { path: lp.clone(), role: "loses".into(), peer: winner.clone() });
        }
        conflict_groups.push(ConflictGroup { path: lp, winner, losers });
    }

    let final_mods = items
        .into_iter()
        .map(|(_, mut item)| {
            if item.enabled && !dirty_mod_set.is_empty() {
                if let Some(touched) = mod_touched_modules.get(&item.folder) {
                    item.pending_rebuild = touched.iter().any(|m| dirty_mod_set.contains(m.as_str()));
                }
            }
            if let Some(mut cs) = mod_conflicts.remove(&item.folder) {
                cs.truncate(64);
                item.conflict_count = cs.len();
                item.conflicts = cs;
            }
            // Compute MO2-style traffic light (green / yellow / red / off)
            let has_effective_payload = item.caps.romfs
                || item.caps.archive
                || item.caps.code_patches > 0
                || !item.cheats.is_empty();
            let has_enabled_mem_cheat = item.cheats.iter().any(|c| c.kind == "memory" && c.enabled);
            let rt_applied = item.runtime_status.files > 0
                || item.runtime_status.ard_overrides > 0
                || item.runtime_status.archive_recs > 0
                || (has_enabled_mem_cheat && runtime_cheats_loaded > 0)
                || (item.caps.code_patches > 0 && !item.pending_rebuild);
            let has_warnings = item.unmatched_paths_count > 0
                || item.caps.unmatched_patches > 0
                || item.unmatched_cheats > 0
                || item.version_mismatch
                || item.conflicts.iter().any(|c| c.role == "loses");

            let (health, reason) = if !item.enabled {
                ("off", "disabled")
            } else if item.empty {
                ("red", "empty")
            } else if item.bdat_error.is_some() || !has_effective_payload {
                ("red", "incompatible")
            } else if item.pending_rebuild || item.runtime_status.held_off {
                ("yellow", "pending_rebuild")
            } else if rt_applied && !has_warnings {
                ("green", "active")
            } else if rt_applied && has_warnings {
                ("yellow", "partial")
            } else if item.runtime_status.skipped && !has_enabled_mem_cheat && item.caps.code_patches == 0 {
                ("red", "incompatible")
            } else {
                ("yellow", "not_verified")
            };
            item.health = health.into();
            item.health_reason = reason.into();
            item
        })
        .collect();

    let main_build_id = exefs
        .build_ids
        .get("main")
        .map(|b| b.chars().take(16).collect::<String>().to_ascii_uppercase())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "8BEF68ED1EEBC24B".to_string());
    let has_exefs_patched = game_dir.join("build").join("exefs-patched").join("m3_state.json").is_file();

    ModsSnapshot {
        mods_dir: crate::dump::fwd(&mods_dir),
        main_build_id,
        has_exefs_patched,
        arh_loaded: arh_index.is_some(),
        game_running,
        needs_rebuild,
        rebuild_modules,
        mods: final_mods,
        conflicts: conflict_groups,
    }
}

pub fn save_state(
    game_dir: &Path,
    exefs: &ExefsTargetInfo,
    vanilla_romfs: Option<&Path>,
    input: SaveModsStateInput,
    game_running: bool,
) -> Result<ModsSnapshot, String> {
    let mods_dir = game_dir.join("mods");
    fs::create_dir_all(&mods_dir).map_err(|e| format!("cannot create {}: {e}", mods_dir.display()))?;
    let present = discover_mod_folders(&mods_dir);
    let present_set: HashSet<&str> = present.iter().map(|s| s.as_str()).collect();

    let clean_order: Vec<String> = resolve_order(&present, &input.order);
    let mut clean_disabled: Vec<String> = Vec::new();
    let mut seen_dis = HashSet::new();
    for d in &input.disabled {
        if present_set.contains(d.as_str()) && seen_dis.insert(d.clone()) {
            clean_disabled.push(d.clone());
        }
    }

    // 1. Write .mod_choices.json (filtering to present mods).
    let mut clean_choices: BTreeMap<String, BTreeMap<String, serde_json::Value>> = BTreeMap::new();
    for (m, kvs) in &input.choices {
        if present_set.contains(m.as_str()) {
            let mut inner = BTreeMap::new();
            for (k, v) in kvs {
                inner.insert(k.clone(), v.clone());
            }
            if !inner.is_empty() {
                clean_choices.insert(m.clone(), inner);
            }
        }
    }
    let choices_json = serde_json::to_string_pretty(&clean_choices).map_err(|e| e.to_string())? + "\n";
    write_atomic(&mods_dir.join(CHOICES_FILE), &choices_json)?;

    // 2. Write .cheats_enabled.tsv cleanly before scanning so snapshot picks it up immediately.
    let mut cheats_tsv = String::from("# Auto-generated by XC2 NS2 Launcher. Format: <mod_folder>\\t<cheat_title>\n");
    for m in &clean_order {
        if let Some(titles) = input.cheats_enabled.get(m) {
            let mut seen_t = HashSet::new();
            for t in titles {
                let title = t.replace(['\t', '\r', '\n'], " ").trim().to_string();
                if !title.is_empty() && seen_t.insert(title.clone()) {
                    cheats_tsv.push_str(&format!("{m}\t{title}\n"));
                }
            }
        }
    }
    write_atomic(&mods_dir.join(CHEATS_FILE), &cheats_tsv)?;

    // 3. Recompute options + validate safe option layers and write .mod_layers.tsv.
    let mut layers_tsv = String::from("# Auto-generated by XC2 NS2 Launcher. Format: <mod_folder>\\t<relative_subdir>\n");
    for m in &clean_order {
        let mod_dir = mods_dir.join(m);
        let saved_for_m = input.choices.get(m);
        let (_, _, opts, _) = parse_mod_json(&mod_dir, saved_for_m);
        for opt in opts {
            if opt.kind == "toggle" && opt.value != "true" {
                continue;
            }
            if !opt.path.is_empty() && is_safe_layer_rel(&opt.path) {
                let clean_rel = opt.path.replace('\\', "/");
                layers_tsv.push_str(&format!("{m}\t{clean_rel}\n"));
            }
        }
    }
    write_atomic(&mods_dir.join(LAYERS_FILE), &layers_tsv)?;

    // 4. Write [mods] order & disabled into nxrt.ini first (so snapshot matches cleanly), then update pending_rebuild.
    let ini_path = game_dir.join(GAME_INI);
    let mut ini = fs::read_to_string(&ini_path).map(|t| Ini::parse(&t)).unwrap_or_else(|_| Ini::empty());
    ini.set("mods", "order", &clean_order.join(","));
    ini.set("mods", "disabled", &clean_disabled.join(","));
    write_atomic(&ini_path, &ini.to_text())?;

    let snap = compute_snapshot(game_dir, exefs, vanilla_romfs, game_running);
    let pending_list: Vec<String> = snap.mods.iter().filter(|m| m.pending_rebuild).map(|m| m.folder.clone()).collect();
    ini.set("mods", "pending_rebuild", &pending_list.join(","));
    write_atomic(&ini_path, &ini.to_text())?;

    Ok(snap)
}

/// Cleanly deletes a mod folder and removes all stale references from `nxrt.ini`,
/// `.mod_choices.json`, `.cheats_enabled.tsv`, and `.mod_layers.tsv`.
pub fn delete_mod(
    game_dir: &Path,
    exefs: &ExefsTargetInfo,
    vanilla_romfs: Option<&Path>,
    folder: &str,
    game_running: bool,
) -> Result<DeleteModResult, String> {
    if game_running {
        return Err("Close the game before deleting a mod.".into());
    }
    let name = folder.trim();
    if name.is_empty() || name.starts_with('.') || name.contains(['/', '\\', ':']) || name == ".." {
        return Err(format!("Invalid mod folder name: '{name}'"));
    }
    let mods_dir = game_dir.join("mods");
    let target = mods_dir.join(name);
    if !target.is_dir() {
        return Err(format!("Mod folder '{name}' does not exist."));
    }

    let before = compute_snapshot(game_dir, exefs, vanilla_romfs, game_running);
    let had_code_mods = before
        .mods
        .iter()
        .find(|m| m.folder == name)
        .map_or(false, |m| m.caps.code_patches > 0 || m.cheats.iter().any(|c| c.kind == "code" && c.enabled));

    fs::remove_dir_all(&target).map_err(|e| format!("cannot delete {}: {e}", target.display()))?;

    let ini = fs::read_to_string(game_dir.join(GAME_INI)).map(|t| Ini::parse(&t)).unwrap_or_else(|_| Ini::empty());
    let order: Vec<String> = split_csv(&ini.get("mods", "order").unwrap_or_default())
        .into_iter()
        .filter(|x| x != name)
        .collect();
    let disabled: Vec<String> = split_csv(&ini.get("mods", "disabled").unwrap_or_default())
        .into_iter()
        .filter(|x| x != name)
        .collect();
    let mut choices = read_choices_file(&mods_dir);
    choices.remove(name);
    let cheats_enabled: HashMap<String, Vec<String>> = read_cheats_enabled_tsv(&mods_dir)
        .into_iter()
        .filter(|(k, _)| k != name)
        .map(|(k, set)| {
            let mut v: Vec<String> = set.into_iter().collect();
            v.sort();
            (k, v)
        })
        .collect();

    let snapshot = save_state(
        game_dir,
        exefs,
        vanilla_romfs,
        SaveModsStateInput { order, disabled, choices, cheats_enabled },
        game_running,
    )?;

    Ok(DeleteModResult { deleted: name.to_string(), had_code_mods, snapshot })
}

/// Reads the latest launch log (`work/logs/play-*.log`, `nxrt.log`, or `fefw.log`) and filters mod & cheat lines.
pub fn read_last_mod_log(game_dir: &Path) -> Result<ModLogView, String> {
    let log_path = find_latest_log_path(game_dir)
        .ok_or_else(|| "No game launch log found yet. Start the game once first.".to_string())?;

    let text = fs::read_to_string(&log_path).map_err(|e| format!("cannot read {}: {e}", log_path.display()))?;
    let mut matched = Vec::new();
    let mut total_matched = 0usize;
    for line in text.lines() {
        let low = line.to_ascii_lowercase();
        if low.contains("[mods]") || low.contains("[cheats]") || low.contains("mods:") || low.contains("cheats:") {
            total_matched += 1;
            if matched.len() < 500 {
                matched.push(line.to_string());
            }
        }
    }
    Ok(ModLogView {
        file: crate::dump::fwd(&log_path),
        lines: matched,
        total_matched,
    })
}

#[cfg(test)]
pub fn install_folder(mods_dir: &Path, src: &Path) -> Result<String, String> {
    if !src.is_dir() {
        return Err(format!("Selected path is not a folder: {}", src.display()));
    }
    let name = src.file_name().and_then(|s| s.to_str()).unwrap_or("").trim().to_string();
    if name.is_empty() || name.starts_with('.') || name.contains(['/', '\\', ':']) {
        return Err(format!("Invalid mod folder name: '{name}'"));
    }
    fs::create_dir_all(mods_dir).map_err(|e| format!("cannot create {}: {e}", mods_dir.display()))?;
    let dst = mods_dir.join(&name);
    if dst.exists() {
        return Err(format!("A mod named '{name}' already exists in {}.", mods_dir.display()));
    }
    copy_dir_recursive(src, &dst).map_err(|e| {
        let _ = fs::remove_dir_all(&dst);
        e
    })?;
    Ok(name)
}

#[cfg(test)]
fn copy_dir_recursive(src: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("cannot create {}: {e}", dst.display()))?;
    for entry in fs::read_dir(src).map_err(|e| format!("cannot read {}: {e}", src.display()))? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        let target = dst.join(entry.file_name());
        if path.is_dir() {
            copy_dir_recursive(&path, &target)?;
        } else {
            fs::copy(&path, &target).map_err(|e| format!("cannot copy {}: {e}", path.display()))?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("xc2-mods-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    fn write_minimal_main_nso(exefs: &Path, bid_byte: u8, text_size: u32) -> String {
        fs::create_dir_all(exefs).unwrap();
        let mut h = vec![0u8; NSO_HDR_SIZE];
        h[..4].copy_from_slice(b"NSO0");
        h[0x18..0x1C].copy_from_slice(&text_size.to_le_bytes());
        for i in 0..20 {
            h[0x40 + i] = bid_byte;
        }
        fs::write(exefs.join("main"), &h).unwrap();
        h[0x40..0x60].iter().map(|b| format!("{b:02x}")).collect()
    }

    #[test]
    fn safe_layer_rel_rejects_traversal_and_dotfiles() {
        assert!(is_safe_layer_rel("romfs_high"));
        assert!(is_safe_layer_rel("opts/4k"));
        assert!(!is_safe_layer_rel(""));
        assert!(!is_safe_layer_rel("../escape"));
        assert!(!is_safe_layer_rel("a/../../b"));
        assert!(!is_safe_layer_rel(".hidden"));
        assert!(!is_safe_layer_rel("C:/abs"));
        assert!(!is_safe_layer_rel("bad\tname"));
    }

    #[test]
    fn options_layers_conflicts_and_m3_pending_rebuild_work_end_to_end() {
        let root = temp_root("e2e");
        let exefs = root.join("exefs");
        let bid = write_minimal_main_nso(&exefs, 0xAA, 0x10000);
        let short16 = bid[..16].to_ascii_uppercase();
        let game = root.join("game");
        let mods = game.join("mods");

        // Mod A: higher priority, with select + toggle options + overlapping romfs file + memory & code cheats
        let ma = mods.join("AlphaMod");
        fs::create_dir_all(ma.join("romfs").join("ui")).unwrap();
        fs::write(ma.join("romfs").join("ui").join("logo.wilay"), b"alpha").unwrap();
        fs::create_dir_all(ma.join("romfs_high").join("ui")).unwrap();
        fs::write(ma.join("romfs_high").join("ui").join("hi.bin"), b"hi").unwrap();
        fs::write(
            ma.join("mod.json"),
            r#"{
              "name": "Alpha Pack",
              "version": "1.0",
              "game": "3.0.1",
              "options": {
                "quality": { "label": "Quality", "type": "select", "default": "high", "values": { "high": { "label": "High", "path": "romfs_high" }, "off": "" } },
                "bad": { "label": "Bad", "type": "toggle", "default": true, "path": "../escape" }
              }
            }"#,
        )
        .unwrap();
        fs::create_dir_all(ma.join("cheats")).unwrap();
        // Memory cheat (Heap write + keypress) and pure static .text Code cheat (main + 0x100 < 0x10000)
        fs::write(
            ma.join("cheats").join(format!("{short16}.txt")),
            "[Max Gold]\n04100000 00001234 05F5E0FF\n\n[60 FPS Code]\n04000000 00000100 14000001\n",
        )
        .unwrap();

        // Mod B: lower priority, shares ui/logo.wilay + unmatched .pchtxt
        let mb = mods.join("BetaMod");
        fs::create_dir_all(mb.join("romfs").join("ui")).unwrap();
        fs::write(mb.join("romfs").join("ui").join("logo.wilay"), b"beta").unwrap();
        fs::create_dir_all(mb.join("exefs")).unwrap();
        fs::write(mb.join("exefs").join("other.pchtxt"), "@nsobid-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n@enabled\n00000100 D503201F\n").unwrap();

        fs::write(game.join(GAME_INI), "; keep comment\r\n[display]\r\nvsync = 1\r\n").unwrap();
        let target = exefs_target_info(Some(&exefs), None);

        let mut choices = HashMap::new();
        choices.insert("AlphaMod".into(), HashMap::from([("quality".into(), serde_json::json!("high"))]));
        let mut cheats = HashMap::new();
        cheats.insert("AlphaMod".into(), vec!["Max Gold".into(), "60 FPS Code".into()]);

        let snap = save_state(
            &game,
            &target,
            None,
            SaveModsStateInput {
                order: vec!["AlphaMod".into(), "BetaMod".into()],
                disabled: vec![],
                choices,
                cheats_enabled: cheats,
            },
            false,
        )
        .unwrap();

        assert!(snap.needs_rebuild);
        assert_eq!(snap.rebuild_modules, vec!["main".to_string()]);
        assert_eq!(snap.mods.len(), 2);
        assert_eq!(snap.mods[0].folder, "AlphaMod");
        assert!(snap.mods[0].pending_rebuild);
        assert_eq!(snap.mods[0].caps.cheats_mem, 1);
        assert_eq!(snap.mods[0].caps.cheats_code, 1);
        assert_eq!(snap.mods[0].conflict_count, 1);
        assert_eq!(snap.mods[0].conflicts[0].role, "wins");
        assert_eq!(snap.mods[1].folder, "BetaMod");
        assert_eq!(snap.mods[1].caps.unmatched_patches, 1);
        assert!(!snap.mods[1].pending_rebuild);
        assert_eq!(snap.mods[1].conflicts[0].role, "loses");

        // Safe layers emitted; ../escape suppressed
        let layers = fs::read_to_string(mods.join(LAYERS_FILE)).unwrap();
        assert!(layers.contains("AlphaMod\tromfs_high\n"));
        assert!(!layers.contains("../escape"));

        // Comments in nxrt.ini preserved & pending_rebuild populated
        let ini_text = fs::read_to_string(game.join(GAME_INI)).unwrap();
        assert!(ini_text.contains("; keep comment"));
        assert!(ini_text.contains("pending_rebuild = AlphaMod"));

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn install_folder_rejects_duplicate_and_copies_cleanly() {
        let root = temp_root("install");
        let src = root.join("NewMod");
        let mods = root.join("mods");
        fs::create_dir_all(src.join("romfs")).unwrap();
        fs::write(src.join("romfs").join("a.bin"), b"123").unwrap();
        let name = install_folder(&mods, &src).unwrap();
        assert_eq!(name, "NewMod");
        assert!(mods.join("NewMod").join("romfs").join("a.bin").is_file());
        assert!(install_folder(&mods, &src).is_err());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn delete_mod_cleans_state_and_read_last_mod_log_filters_lines() {
        let root = temp_root("del-log");
        let exefs = root.join("exefs");
        let _ = write_minimal_main_nso(&exefs, 0xBB, 0x8000);
        let target = exefs_target_info(Some(&exefs), None);
        let game = root.join("game");
        let mods = game.join("mods");
        fs::create_dir_all(mods.join("ModOne").join("romfs")).unwrap();
        fs::write(mods.join("ModOne").join("romfs").join("x.bin"), b"1").unwrap();
        fs::create_dir_all(mods.join("ModTwo").join("romfs")).unwrap();
        fs::write(mods.join("ModTwo").join("romfs").join("y.bin"), b"2").unwrap();

        let _ = save_state(
            &game,
            &target,
            None,
            SaveModsStateInput {
                order: vec!["ModOne".into(), "ModTwo".into()],
                disabled: vec!["ModOne".into()],
                choices: HashMap::new(),
                cheats_enabled: HashMap::from([("ModOne".into(), vec!["Inf HP".into()])]),
            },
            false,
        )
        .unwrap();

        // Deleting while game is running is blocked
        assert!(delete_mod(&game, &target, None, "ModOne", true).is_err());

        let res = delete_mod(&game, &target, None, "ModOne", false).unwrap();
        assert_eq!(res.deleted, "ModOne");
        assert!(!mods.join("ModOne").exists());
        assert_eq!(res.snapshot.mods.len(), 1);
        let ini = fs::read_to_string(game.join(GAME_INI)).unwrap();
        assert!(!ini.contains("ModOne"));
        let cheats_tsv = fs::read_to_string(mods.join(CHEATS_FILE)).unwrap();
        assert!(!cheats_tsv.contains("ModOne"));

        // Log filtering picks latest play-*.log and returns [mods] / [cheats] lines
        let logs = game.join("work").join("logs");
        fs::create_dir_all(&logs).unwrap();
        fs::write(
            logs.join("play-100.log"),
            "[info] boot ok\n[mods] mods: ModTwo: 1 file(s), 1 replaced, 0 added\n[cheats] loaded 2 active runtime cheat(s)\n[gfx] swapchain ready\n",
        )
        .unwrap();
        let view = read_last_mod_log(&game).unwrap();
        assert_eq!(view.total_matched, 2);
        assert_eq!(view.lines.len(), 2);

        // Snapshot picks up green traffic light from log & classifies categories
        let snap_after = compute_snapshot(&game, &target, None, false);
        let m2 = snap_after.mods.iter().find(|m| m.folder == "ModTwo").unwrap();
        assert_eq!(m2.health, "green");
        assert_eq!(m2.health_reason, "active");
        assert_eq!(m2.runtime_status.files, 1);
        assert_eq!(m2.runtime_status.replaced, 1);
        assert!(!m2.categories.is_empty());

        let _ = fs::remove_dir_all(&root);
    }
}
