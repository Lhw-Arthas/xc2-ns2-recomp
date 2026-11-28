// SPDX-License-Identifier: GPL-3.0-or-later
//! Smart mod importer ("wide intake, strict normalized output"):
//! Accepts folders, `.zip` (native pure-Rust via flate2), `.7z` (via Windows `tar.exe`), or single loose files
//! (`.txt` cheats, `.ips`/`.pchtxt` ExeFS patches, `.bdat`/`.wismt` assets via `bf2.arh` lookup),
//! strips wrapper folders, flattens Atmosphere layouts into `<mods>/<name>/{romfs,exefs,cheats}`,
//! blocks empty packages & NS1 `common.bdat` (table count != 926), and returns actionable warnings.

use crate::arh::{inspect_common_bdat, ArhIndex, BdatCheck, NS2_COMMON_BDAT_TABLES};
use crate::mods::{build_id_matches, parse_cheat_text, parse_pchtxt_header_pub, ExefsTargetInfo};
use flate2::read::DeflateDecoder;
use serde::Serialize;
use std::collections::HashSet;
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

const DEFAULT_MAIN_BID_16: &str = "8BEF68ED1EEBC24B";

/// Top-level folder names that belong directly inside `romfs/` if a mod author omitted the `romfs/` wrapper.
const ROMFS_ROOT_MARKERS: &[&str] = &[
    "bdat", "menu", "chr", "map", "model", "stream", "movie", "sound", "effect", "event", "shader", "ui", "bf2.ard",
    "ard",
];

#[derive(Debug, Serialize, Clone)]
pub struct ImportSummary {
    pub folder: String,
    pub warnings: Vec<String>,
    pub has_exefs: bool,
}

fn temp_stage_dir(tag: &str) -> Result<PathBuf, String> {
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
    let p = std::env::temp_dir().join(format!("xc2-mod-{tag}-{}-{stamp}", std::process::id()));
    fs::create_dir_all(&p).map_err(|e| format!("cannot create temp staging dir: {e}"))?;
    Ok(p)
}

fn sanitize_mod_name(raw: &str) -> String {
    let mut s: String = raw
        .trim()
        .chars()
        .map(|c| match c {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' | '\t' | '\r' | '\n' => '_',
            _ => c,
        })
        .collect();
    while s.starts_with('.') {
        s.remove(0);
    }
    s = s.trim().to_string();
    if s.is_empty() {
        "imported_mod".to_string()
    } else {
        s
    }
}

fn safe_rel_path(raw: &str) -> Option<PathBuf> {
    let norm = raw.replace('\\', "/");
    let mut out = PathBuf::new();
    for comp in Path::new(&norm).components() {
        match comp {
            Component::Normal(os) => {
                let s = os.to_string_lossy();
                if s.is_empty() || s.contains(':') {
                    return None;
                }
                out.push(os);
            }
            Component::CurDir => {}
            _ => return None,
        }
    }
    (!out.as_os_str().is_empty()).then_some(out)
}

/// Pure-Rust ZIP extractor (Stored=0 and Deflate=8 via `flate2`) with path-traversal protection.
pub fn extract_zip(archive: &Path, dst: &Path) -> Result<(), String> {
    let data = fs::read(archive).map_err(|e| format!("cannot read {}: {e}", archive.display()))?;
    if data.len() < 22 {
        return Err("Archive is too small to be a valid ZIP file.".into());
    }
    // Locate End of Central Directory (PK\x05\x06) searching backwards within max comment length (65535 + 22).
    let min_pos = data.len().saturating_sub(65535 + 22);
    let mut eocd = None;
    for i in (min_pos..=data.len() - 22).rev() {
        if &data[i..i + 4] == b"PK\x05\x06" {
            let comment_len = u16::from_le_bytes([data[i + 20], data[i + 21]]) as usize;
            if i + 22 + comment_len == data.len() {
                eocd = Some(i);
                break;
            }
        }
    }
    let eocd_off = eocd.ok_or_else(|| "Invalid or corrupted ZIP file (missing central directory).".to_string())?;
    let mut cd_entries = u16::from_le_bytes([data[eocd_off + 10], data[eocd_off + 11]]) as u64;
    let mut cd_size = u32::from_le_bytes(data[eocd_off + 12..eocd_off + 16].try_into().unwrap()) as u64;
    let mut cd_off = u32::from_le_bytes(data[eocd_off + 16..eocd_off + 20].try_into().unwrap()) as u64;

    // Basic ZIP64 EOCD locator check (PK\x06\x07) 20 bytes before EOCD.
    if (cd_entries == 0xFFFF || cd_size == 0xFFFF_FFFF || cd_off == 0xFFFF_FFFF) && eocd_off >= 20 {
        let loc = eocd_off - 20;
        if &data[loc..loc + 4] == b"PK\x06\x07" {
            let z64_off = u64::from_le_bytes(data[loc + 8..loc + 16].try_into().unwrap()) as usize;
            if z64_off + 56 <= data.len() && &data[z64_off..z64_off + 4] == b"PK\x06\x06" {
                cd_entries = u64::from_le_bytes(data[z64_off + 32..z64_off + 40].try_into().unwrap());
                cd_size = u64::from_le_bytes(data[z64_off + 40..z64_off + 48].try_into().unwrap());
                cd_off = u64::from_le_bytes(data[z64_off + 48..z64_off + 56].try_into().unwrap());
            }
        }
    }

    let cd_end = cd_off.checked_add(cd_size).ok_or("ZIP central directory overflow")? as usize;
    if cd_end > data.len() {
        return Err("ZIP central directory extends past end of file.".into());
    }

    fs::create_dir_all(dst).map_err(|e| format!("cannot create staging dir: {e}"))?;
    let mut pos = cd_off as usize;
    for _ in 0..cd_entries {
        if pos + 46 > cd_end || &data[pos..pos + 4] != b"PK\x01\x02" {
            return Err("Corrupted ZIP central directory entry.".into());
        }
        let method = u16::from_le_bytes([data[pos + 10], data[pos + 11]]);
        let mut comp_sz = u32::from_le_bytes(data[pos + 20..pos + 24].try_into().unwrap()) as u64;
        let mut uncomp_sz = u32::from_le_bytes(data[pos + 24..pos + 28].try_into().unwrap()) as u64;
        let name_len = u16::from_le_bytes([data[pos + 28], data[pos + 29]]) as usize;
        let extra_len = u16::from_le_bytes([data[pos + 30], data[pos + 31]]) as usize;
        let comment_len = u16::from_le_bytes([data[pos + 32], data[pos + 33]]) as usize;
        let mut lfh_off = u32::from_le_bytes(data[pos + 42..pos + 46].try_into().unwrap()) as u64;

        let name_start = pos + 46;
        let extra_start = name_start + name_len;
        let next_pos = extra_start + extra_len + comment_len;
        if next_pos > cd_end {
            return Err("Truncated ZIP entry metadata.".into());
        }
        let raw_name = String::from_utf8_lossy(&data[name_start..extra_start]).into_owned();

        // Parse ZIP64 extra field (header ID 0x0001) if needed.
        if comp_sz == 0xFFFF_FFFF || uncomp_sz == 0xFFFF_FFFF || lfh_off == 0xFFFF_FFFF {
            let mut ep = extra_start;
            let ep_end = extra_start + extra_len;
            while ep + 4 <= ep_end {
                let hid = u16::from_le_bytes([data[ep], data[ep + 1]]);
                let hlen = u16::from_le_bytes([data[ep + 2], data[ep + 3]]) as usize;
                ep += 4;
                if ep + hlen > ep_end {
                    break;
                }
                if hid == 0x0001 {
                    let mut zp = ep;
                    if uncomp_sz == 0xFFFF_FFFF && zp + 8 <= ep + hlen {
                        uncomp_sz = u64::from_le_bytes(data[zp..zp + 8].try_into().unwrap());
                        zp += 8;
                    }
                    if comp_sz == 0xFFFF_FFFF && zp + 8 <= ep + hlen {
                        comp_sz = u64::from_le_bytes(data[zp..zp + 8].try_into().unwrap());
                        zp += 8;
                    }
                    if lfh_off == 0xFFFF_FFFF && zp + 8 <= ep + hlen {
                        lfh_off = u64::from_le_bytes(data[zp..zp + 8].try_into().unwrap());
                    }
                    break;
                }
                ep += hlen;
            }
        }
        pos = next_pos;

        let is_dir = raw_name.ends_with('/') || raw_name.ends_with('\\');
        let Some(rel) = safe_rel_path(&raw_name) else {
            continue;
        };
        let target = dst.join(&rel);
        if is_dir {
            fs::create_dir_all(&target).map_err(|e| format!("cannot create dir {}: {e}", target.display()))?;
            continue;
        }
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("cannot create parent {}: {e}", parent.display()))?;
        }

        let lfh = lfh_off as usize;
        if lfh + 30 > data.len() || &data[lfh..lfh + 4] != b"PK\x03\x04" {
            return Err(format!("Corrupted local file header for '{raw_name}'."));
        }
        let lfh_name_len = u16::from_le_bytes([data[lfh + 26], data[lfh + 27]]) as usize;
        let lfh_extra_len = u16::from_le_bytes([data[lfh + 28], data[lfh + 29]]) as usize;
        let payload_start = lfh + 30 + lfh_name_len + lfh_extra_len;
        let payload_end = payload_start.checked_add(comp_sz as usize).ok_or("ZIP payload overflow")?;
        if payload_end > data.len() {
            return Err(format!("Truncated ZIP payload for '{raw_name}'."));
        }
        let payload = &data[payload_start..payload_end];

        let mut out_file = fs::File::create(&target).map_err(|e| format!("cannot write {}: {e}", target.display()))?;
        match method {
            0 => io::copy(&mut &payload[..], &mut out_file).map_err(|e| format!("write failed: {e}"))?,
            8 => {
                let mut dec = DeflateDecoder::new(payload);
                io::copy(&mut dec, &mut out_file).map_err(|e| format!("deflate failed for '{raw_name}': {e}"))?
            }
            other => {
                return Err(format!(
                    "Unsupported ZIP compression method {other} for '{raw_name}'. Please extract the archive first and import the folder."
                ));
            }
        };
        let _ = uncomp_sz;
    }
    Ok(())
}

fn extract_7z_via_tar(archive: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("cannot create staging dir: {e}"))?;
    let mut cmd = Command::new("tar");
    cmd.arg("-xf").arg(archive).arg("-C").arg(dst);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);
    }
    let out = cmd.output().map_err(|e| {
        format!("Cannot run system tar to extract .7z ({e}). Please extract the .7z archive manually and import the folder.")
    })?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr).trim().to_string();
        return Err(format!(
            "Could not extract .7z archive ({}). Please extract it with 7-Zip and import the folder.",
            if stderr.is_empty() { "unsupported format".into() } else { stderr }
        ));
    }
    Ok(())
}

fn copy_dir_merge(src: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("cannot create {}: {e}", dst.display()))?;
    for entry in fs::read_dir(src).map_err(|e| format!("cannot read {}: {e}", src.display()))? {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name();
        let name_s = name.to_string_lossy();
        if name_s == "__MACOSX" || name_s == ".DS_Store" || name_s == "Thumbs.db" {
            continue;
        }
        let path = entry.path();
        let target = dst.join(&name);
        if path.is_dir() {
            copy_dir_merge(&path, &target)?;
        } else if path.is_file() {
            fs::copy(&path, &target).map_err(|e| format!("cannot copy {}: {e}", path.display()))?;
        }
    }
    Ok(())
}

fn is_hex(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_hexdigit())
}

fn looks_like_title_id(name: &str) -> bool {
    name.len() == 16 && is_hex(name)
}

fn is_recognized_mod_root(dir: &Path) -> bool {
    let Ok(rd) = fs::read_dir(dir) else { return false };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_ascii_lowercase();
        let p = e.path();
        if p.is_file() && name == "mod.json" {
            return true;
        }
        if p.is_dir()
            && (matches!(name.as_str(), "romfs" | "exefs" | "cheats" | "atmosphere" | "contents" | "exefs_patches")
                || looks_like_title_id(&name)
                || ROMFS_ROOT_MARKERS.contains(&name.as_str()))
        {
            return true;
        }
    }
    false
}

/// Unwraps redundant single-subdirectory wrappers (e.g. `ModZip/ModName/romfs` -> `ModName`).
fn unwrap_nested_root(start: &Path) -> PathBuf {
    let mut cur = start.to_path_buf();
    for _ in 0..6 {
        if is_recognized_mod_root(&cur) {
            break;
        }
        let Ok(rd) = fs::read_dir(&cur) else { break };
        let entries: Vec<PathBuf> = rd
            .flatten()
            .filter(|e| {
                let n = e.file_name().to_string_lossy().to_string();
                n != "__MACOSX" && n != ".DS_Store" && n != "Thumbs.db"
            })
            .map(|e| e.path())
            .collect();
        if entries.len() == 1 && entries[0].is_dir() {
            cur = entries[0].clone();
        } else {
            break;
        }
    }
    cur
}

/// Converts a Switch-Cheats-DB `.json` (`{ "<16-hex-bid>": { "[Title]": "body", ... } }`) into `cheats/<BID>.txt` files.
/// Returns number of cheat files written.
fn import_switch_cheats_db_json(src: &Path, dst_cheats: &Path) -> Result<usize, String> {
    let text = fs::read_to_string(src).map_err(|e| format!("cannot read {}: {e}", src.display()))?;
    let val: serde_json::Value = serde_json::from_str(text.trim_start_matches('\u{feff}'))
        .map_err(|e| format!("invalid JSON in {}: {e}", src.display()))?;
    let Some(root) = val.as_object() else {
        return Ok(0);
    };
    let mut written = 0usize;
    for (bid_key, entries) in root {
        let bid = bid_key.trim();
        if bid.len() < 16 || !is_hex(bid) {
            continue;
        }
        let Some(map) = entries.as_object() else { continue };
        let mut out_txt = String::new();
        for (title, body_val) in map {
            let Some(body) = body_val.as_str() else { continue };
            let trimmed = body.trim();
            if trimmed.is_empty() {
                continue;
            }
            if !trimmed.starts_with('[') {
                out_txt.push_str(title);
                out_txt.push('\n');
            }
            out_txt.push_str(trimmed);
            out_txt.push_str("\n\n");
        }
        if !parse_cheat_text(&out_txt, bid, 0, None).is_empty() {
            fs::create_dir_all(dst_cheats).map_err(|e| format!("cannot create cheats/: {e}"))?;
            let fname = format!("{}.txt", bid.to_ascii_uppercase());
            fs::write(dst_cheats.join(fname), out_txt).map_err(|e| format!("cannot write cheat txt: {e}"))?;
            written += 1;
        }
    }
    Ok(written)
}

fn copy_cheats_files(src_dir: &Path, dst_cheats: &Path) -> Result<usize, String> {
    let mut count = 0usize;
    let Ok(rd) = fs::read_dir(src_dir) else { return Ok(0) };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_file() && p.extension().and_then(|s| s.to_str()).map_or(false, |x| x.eq_ignore_ascii_case("txt")) {
            fs::create_dir_all(dst_cheats).map_err(|err| format!("cannot create cheats/: {err}"))?;
            fs::copy(&p, dst_cheats.join(e.file_name())).map_err(|err| format!("cannot copy cheat file: {err}"))?;
            count += 1;
        }
    }
    Ok(count)
}

fn copy_exefs_files(src_dir: &Path, dst_exefs: &Path) -> Result<usize, String> {
    let mut count = 0usize;
    let Ok(rd) = fs::read_dir(src_dir) else { return Ok(0) };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            count += copy_exefs_files(&p, dst_exefs)?;
        } else if p.is_file() {
            let ext = p.extension().and_then(|s| s.to_str()).unwrap_or("").to_ascii_lowercase();
            if matches!(ext.as_str(), "ips" | "pchtxt") {
                fs::create_dir_all(dst_exefs).map_err(|err| format!("cannot create exefs/: {err}"))?;
                fs::copy(&p, dst_exefs.join(e.file_name())).map_err(|err| format!("cannot copy exefs patch: {err}"))?;
                count += 1;
            }
        }
    }
    Ok(count)
}

fn copy_content_bucket(bucket: &Path, dst: &Path) -> Result<(), String> {
    if bucket.join("romfs").is_dir() {
        copy_dir_merge(&bucket.join("romfs"), &dst.join("romfs"))?;
    }
    if bucket.join("exefs").is_dir() {
        copy_exefs_files(&bucket.join("exefs"), &dst.join("exefs"))?;
    }
    if bucket.join("cheats").is_dir() {
        copy_cheats_files(&bucket.join("cheats"), &dst.join("cheats"))?;
    }
    Ok(())
}

/// Normalizes an extracted/source directory into standard `<dst>/{romfs,exefs,cheats,mod.json}`.
fn normalize_folder_into(src_root: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("cannot create {}: {e}", dst.display()))?;
    let root = unwrap_nested_root(src_root);

    // 1. Preserve mod.json and any option subdirectories referenced by it.
    let mod_json_src = root.join("mod.json");
    let mut preserved_opt_roots: HashSet<String> = HashSet::new();
    if mod_json_src.is_file() {
        fs::copy(&mod_json_src, dst.join("mod.json")).map_err(|e| format!("cannot copy mod.json: {e}"))?;
        if let Ok(text) = fs::read_to_string(&mod_json_src) {
            if let Ok(val) = serde_json::from_str::<serde_json::Value>(text.trim_start_matches('\u{feff}')) {
                if let Some(opts) = val.get("options").and_then(|v| v.as_object()) {
                    for (_, spec) in opts {
                        if let Some(p) = spec.get("path").and_then(|v| v.as_str()) {
                            if let Some(first) = p.replace('\\', "/").split('/').next() {
                                if !first.is_empty() {
                                    preserved_opt_roots.insert(first.to_ascii_lowercase());
                                }
                            }
                        }
                        if let Some(vals) = spec.get("values").and_then(|v| v.as_object()) {
                            for (_, c) in vals {
                                let p = c.get("path").and_then(|v| v.as_str()).or_else(|| c.as_str()).unwrap_or("");
                                if let Some(first) = p.replace('\\', "/").split('/').next() {
                                    if !first.is_empty() {
                                        preserved_opt_roots.insert(first.to_ascii_lowercase());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 2. Direct standard directories (`romfs/`, `exefs/`, `cheats/`, `exefs_patches/`).
    copy_content_bucket(&root, dst)?;
    if root.join("exefs_patches").is_dir() {
        copy_exefs_files(&root.join("exefs_patches"), &dst.join("exefs"))?;
    }

    // 3. Atmosphere layouts (`atmosphere/contents/<tid>/*`, `atmosphere/exefs_patches/*`, `contents/<tid>/*`, `<tid>/*`).
    let atm = root.join("atmosphere");
    if atm.is_dir() {
        if atm.join("exefs_patches").is_dir() {
            copy_exefs_files(&atm.join("exefs_patches"), &dst.join("exefs"))?;
        }
        let atm_contents = atm.join("contents");
        if let Ok(rd) = fs::read_dir(&atm_contents) {
            for e in rd.flatten() {
                if e.path().is_dir() {
                    copy_content_bucket(&e.path(), dst)?;
                }
            }
        }
    }
    let contents = root.join("contents");
    if let Ok(rd) = fs::read_dir(&contents) {
        for e in rd.flatten() {
            if e.path().is_dir() {
                copy_content_bucket(&e.path(), dst)?;
            }
        }
    }

    // 4. Inspect remaining entries at `root`: Title ID folders, mod.json option dirs, direct romfs subfolders, or loose files.
    if let Ok(rd) = fs::read_dir(&root) {
        for e in rd.flatten() {
            let p = e.path();
            let name = e.file_name().to_string_lossy().to_string();
            let low = name.to_ascii_lowercase();
            if matches!(
                low.as_str(),
                "romfs" | "exefs" | "cheats" | "atmosphere" | "contents" | "exefs_patches" | "mod.json" | "__macosx" | ".ds_store" | "thumbs.db"
            ) {
                continue;
            }
            if p.is_dir() {
                if looks_like_title_id(&low) {
                    copy_content_bucket(&p, dst)?;
                } else if preserved_opt_roots.contains(&low) {
                    copy_dir_merge(&p, &dst.join(&name))?;
                } else if ROMFS_ROOT_MARKERS.contains(&low.as_str()) {
                    copy_dir_merge(&p, &dst.join("romfs").join(&name))?;
                }
            } else if p.is_file() {
                let ext = p.extension().and_then(|s| s.to_str()).unwrap_or("").to_ascii_lowercase();
                if matches!(ext.as_str(), "ips" | "pchtxt") {
                    fs::create_dir_all(dst.join("exefs")).map_err(|err| err.to_string())?;
                    fs::copy(&p, dst.join("exefs").join(&name)).map_err(|err| err.to_string())?;
                } else if ext == "txt" {
                    if let Ok(txt) = fs::read_to_string(&p) {
                        if !parse_cheat_text(&txt, &name, 0, None).is_empty() {
                            let target_name = if p
                                .file_stem()
                                .and_then(|s| s.to_str())
                                .map_or(false, |st| st.len() >= 16 && is_hex(st))
                            {
                                name.clone()
                            } else {
                                format!("{DEFAULT_MAIN_BID_16}.txt")
                            };
                            fs::create_dir_all(dst.join("cheats")).map_err(|err| err.to_string())?;
                            fs::copy(&p, dst.join("cheats").join(target_name)).map_err(|err| err.to_string())?;
                        }
                    }
                } else if ext == "json" && low != "mod.json" {
                    let _ = import_switch_cheats_db_json(&p, &dst.join("cheats"))?;
                } else if matches!(low.as_str(), "bf2.arh" | "bf2.ard" | "append.bin") {
                    fs::create_dir_all(dst.join("romfs")).map_err(|err| err.to_string())?;
                    fs::copy(&p, dst.join("romfs").join(&name)).map_err(|err| err.to_string())?;
                }
            }
        }
    }

    Ok(())
}

/// Normalizes a single loose file (`.txt` cheat, `.ips`/`.pchtxt`, or `.bdat`/`.wismt`) into `<dst>`.
fn normalize_single_file_into(src: &Path, dst: &Path, arh: Option<&ArhIndex>) -> Result<(), String> {
    let fname = src
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or("Invalid file name")?
        .to_string();
    let ext = src
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    fs::create_dir_all(dst).map_err(|e| format!("cannot create {}: {e}", dst.display()))?;

    match ext.as_str() {
        "txt" => {
            let text = fs::read_to_string(src).map_err(|e| format!("cannot read {fname}: {e}"))?;
            if parse_cheat_text(&text, &fname, 0, None).is_empty() {
                return Err(format!(
                    "'{fname}' does not contain any valid [Cheat Name] sections with 8-digit hex codes."
                ));
            }
            let stem = src.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            let target_name = if stem.len() >= 16 && is_hex(stem) {
                fname
            } else {
                format!("{DEFAULT_MAIN_BID_16}.txt")
            };
            fs::create_dir_all(dst.join("cheats")).map_err(|e| e.to_string())?;
            fs::copy(src, dst.join("cheats").join(target_name)).map_err(|e| e.to_string())?;
            Ok(())
        }
        "json" => {
            let n = import_switch_cheats_db_json(src, &dst.join("cheats"))?;
            if n == 0 {
                return Err(format!("'{fname}' does not contain any valid Switch-Cheats-DB Build ID cheat sections."));
            }
            Ok(())
        }
        "ips" | "pchtxt" => {
            fs::create_dir_all(dst.join("exefs")).map_err(|e| e.to_string())?;
            fs::copy(src, dst.join("exefs").join(fname)).map_err(|e| e.to_string())?;
            Ok(())
        }
        "bdat" | "wismt" => {
            let rel_path = if let Some(idx) = arh {
                idx.lookup_unique_by_filename(&fname)
            } else if fname.eq_ignore_ascii_case("common.bdat") {
                Some("bdat/common.bdat".to_string())
            } else {
                None
            };
            let Some(rel) = rel_path else {
                return Err(format!(
                    "Cannot auto-place '{fname}': no unique path match in bf2.arh. Please pack it under romfs/<subpath>/{fname} and import the folder."
                ));
            };
            let target = dst.join("romfs").join(&rel);
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::copy(src, &target).map_err(|e| e.to_string())?;
            Ok(())
        }
        _ => Err(format!(
            "Unsupported single file '{fname}'. Import a folder, .zip/.7z archive, or a .txt / .ips / .pchtxt / .bdat / .wismt file."
        )),
    }
}

fn walk_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            walk_files(&p, out);
        } else if p.is_file() {
            out.push(p);
        }
    }
}

/// Runs pre-commit checks on normalized mod folder:
/// - Hard blocks empty packages (no romfs/exefs/cheats files).
/// - Hard blocks incompatible `common.bdat` (table count != 926, e.g. NS1 844).
/// - Collects non-blocking warnings (unmatched Build ID, unmatched romfs paths, ExeFS rebuild reminder).
fn validate_normalized_mod(
    dst: &Path,
    exefs: &ExefsTargetInfo,
    vanilla_romfs: Option<&Path>,
    arh: Option<&ArhIndex>,
) -> Result<(Vec<String>, bool), String> {
    let mut romfs_files = Vec::new();
    let mut exefs_files = Vec::new();
    let mut cheat_files = Vec::new();
    if dst.join("romfs").is_dir() {
        walk_files(&dst.join("romfs"), &mut romfs_files);
    }
    if dst.join("exefs").is_dir() {
        walk_files(&dst.join("exefs"), &mut exefs_files);
    }
    if dst.join("cheats").is_dir() {
        walk_files(&dst.join("cheats"), &mut cheat_files);
    }
    if romfs_files.is_empty() && exefs_files.is_empty() && cheat_files.is_empty() {
        return Err(
            "Empty or unrecognized mod package: no romfs/, exefs/, or cheats/ files were found. Check the folder structure."
                .into(),
        );
    }

    // Hard check: NS1 common.bdat (844 vs 926 tables)
    for f in &romfs_files {
        if f.file_name().and_then(|s| s.to_str()).map_or(false, |n| n.eq_ignore_ascii_case("common.bdat")) {
            match inspect_common_bdat(f) {
                BdatCheck::Ok { .. } => {}
                BdatCheck::Mismatch { tables, .. } => {
                    return Err(format!(
                        "Incompatible common.bdat detected: found {tables} tables (NS2 3.0.1 requires {NS2_COMMON_BDAT_TABLES}; NS1 has 844). Importing an NS1 common.bdat will crash the game."
                    ));
                }
                BdatCheck::Invalid(msg) => {
                    return Err(format!("Invalid common.bdat: {msg}"));
                }
            }
        }
    }

    let mut warnings = Vec::new();
    let main_bid = exefs.build_ids.get("main").map(|s| s.as_str());

    // Check Build IDs on cheats
    for cf in &cheat_files {
        let stem = cf.file_stem().and_then(|s| s.to_str()).unwrap_or("");
        if let Some(bid) = main_bid {
            if !(stem.len() >= 16 && is_hex(stem) && bid.starts_with(&stem.to_ascii_lowercase())) {
                warnings.push(format!(
                    "Cheat file '{}' does not match 3.0.1 Build ID ({DEFAULT_MAIN_BID_16}): NS1 addresses will not work and may crash.",
                    cf.file_name().and_then(|s| s.to_str()).unwrap_or("")
                ));
            }
        }
    }

    // Check Build IDs on exefs patches
    for ef in &exefs_files {
        let ext = ef.extension().and_then(|s| s.to_str()).unwrap_or("").to_ascii_lowercase();
        let fname = ef.file_name().and_then(|s| s.to_str()).unwrap_or("");
        let patch_bid = if ext == "pchtxt" {
            fs::read_to_string(ef).ok().map(|t| parse_pchtxt_header_pub(&t).0).unwrap_or_default()
        } else {
            ef.file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string()
        };
        let matched = exefs.build_ids.values().any(|bid| build_id_matches(&patch_bid, bid));
        if !matched {
            warnings.push(format!(
                "ExeFS patch '{fname}' does not match any 3.0.1 module Build ID ({DEFAULT_MAIN_BID_16})."
            ));
        }
    }

    // Soft check: romfs paths not found in bf2.arh or vanilla romfs
    if let Some(idx) = arh {
        let romfs_root = dst.join("romfs");
        let mut unmatched = 0usize;
        let mut sample = Vec::new();
        for f in &romfs_files {
            if let Ok(rel) = f.strip_prefix(&romfs_root) {
                let logical = rel.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
                if !idx.is_known_romfs_path(&logical, vanilla_romfs) {
                    unmatched += 1;
                    if sample.len() < 3 {
                        sample.push(logical);
                    }
                }
            }
        }
        if unmatched > 0 {
            warnings.push(format!(
                "{unmatched} romfs file(s) do not match vanilla romfs or bf2.arh (e.g. {}). Verify folder depth if the mod has no effect.",
                sample.join(", ")
            ));
        }
    }

    let has_exefs = !exefs_files.is_empty();
    if has_exefs {
        warnings.push("This mod includes ExeFS code patches: run Setup to rebuild before launching.".into());
    }

    Ok((warnings, has_exefs))
}

fn ensure_mod_json(dst: &Path, display_name: &str, source_label: &str) {
    let p = dst.join("mod.json");
    if p.is_file() {
        return; // Preserve author's mod.json (including options) untouched.
    }
    let ts = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let obj = serde_json::json!({
        "name": display_name,
        "version": "",
        "game": "3.0.1",
        "source": source_label,
        "installed_at": ts,
    });
    if let Ok(text) = serde_json::to_string_pretty(&obj) {
        let _ = fs::write(p, text + "\n");
    }
}

/// Imports a folder, `.zip`/`.7z` archive, or single loose file into `mods_dir/<name>/`.
pub fn import_mod_source(
    mods_dir: &Path,
    src: &Path,
    exefs: &ExefsTargetInfo,
    vanilla_romfs: Option<&Path>,
    arh: Option<&ArhIndex>,
) -> Result<ImportSummary, String> {
    if !src.exists() {
        return Err(format!("Path does not exist: {}", src.display()));
    }
    let raw_stem = if src.is_dir() {
        src.file_name().and_then(|s| s.to_str()).unwrap_or("imported_mod")
    } else {
        src.file_stem().and_then(|s| s.to_str()).unwrap_or("imported_mod")
    };
    let mod_name = sanitize_mod_name(raw_stem);
    fs::create_dir_all(mods_dir).map_err(|e| format!("cannot create {}: {e}", mods_dir.display()))?;
    let final_dst = mods_dir.join(&mod_name);
    if final_dst.exists() {
        return Err(format!(
            "A mod named '{mod_name}' already exists. Delete it first or rename the source."
        ));
    }

    let stage_out = temp_stage_dir("norm")?;
    let mut extract_tmp: Option<PathBuf> = None;

    let res = (|| -> Result<ImportSummary, String> {
        let source_label = src.file_name().and_then(|s| s.to_str()).unwrap_or(&mod_name).to_string();
        if src.is_dir() {
            normalize_folder_into(src, &stage_out)?;
        } else {
            let ext = src.extension().and_then(|s| s.to_str()).unwrap_or("").to_ascii_lowercase();
            match ext.as_str() {
                "zip" => {
                    let tmp = temp_stage_dir("zip")?;
                    extract_tmp = Some(tmp.clone());
                    extract_zip(src, &tmp)?;
                    normalize_folder_into(&tmp, &stage_out)?;
                }
                "7z" => {
                    let tmp = temp_stage_dir("7z")?;
                    extract_tmp = Some(tmp.clone());
                    extract_7z_via_tar(src, &tmp)?;
                    normalize_folder_into(&tmp, &stage_out)?;
                }
                _ => {
                    normalize_single_file_into(src, &stage_out, arh)?;
                }
            }
        }

        let (warnings, has_exefs) = validate_normalized_mod(&stage_out, exefs, vanilla_romfs, arh)?;
        ensure_mod_json(&stage_out, raw_stem.trim(), &source_label);

        // Move or copy from temp dir (temp may be on another drive than mods_dir, so fallback to recursive copy).
        if fs::rename(&stage_out, &final_dst).is_err() {
            copy_dir_merge(&stage_out, &final_dst).map_err(|e| {
                let _ = fs::remove_dir_all(&final_dst);
                e
            })?;
        }
        Ok(ImportSummary { folder: mod_name, warnings, has_exefs })
    })();

    let _ = fs::remove_dir_all(&stage_out);
    if let Some(t) = extract_tmp {
        let _ = fs::remove_dir_all(&t);
    }
    res
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::DeflateEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn make_test_zip(path: &Path, entries: &[(&str, &[u8])]) {
        let mut buf = Vec::new();
        let mut cd = Vec::new();
        for (name, content) in entries {
            let lfh_off = buf.len() as u32;
            let mut enc = DeflateEncoder::new(Vec::new(), Compression::default());
            enc.write_all(content).unwrap();
            let comp = enc.finish().unwrap();

            // Local file header
            buf.extend_from_slice(b"PK\x03\x04");
            buf.extend_from_slice(&20u16.to_le_bytes()); // version
            buf.extend_from_slice(&0u16.to_le_bytes()); // flags
            buf.extend_from_slice(&8u16.to_le_bytes()); // deflate
            buf.extend_from_slice(&0u32.to_le_bytes()); // mod time/date
            buf.extend_from_slice(&0u32.to_le_bytes()); // crc32 (unchecked)
            buf.extend_from_slice(&(comp.len() as u32).to_le_bytes());
            buf.extend_from_slice(&(content.len() as u32).to_le_bytes());
            buf.extend_from_slice(&(name.len() as u16).to_le_bytes());
            buf.extend_from_slice(&0u16.to_le_bytes()); // extra len
            buf.extend_from_slice(name.as_bytes());
            buf.extend_from_slice(&comp);

            // Central directory entry
            cd.extend_from_slice(b"PK\x01\x02");
            cd.extend_from_slice(&20u16.to_le_bytes());
            cd.extend_from_slice(&20u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&8u16.to_le_bytes());
            cd.extend_from_slice(&0u32.to_le_bytes());
            cd.extend_from_slice(&0u32.to_le_bytes());
            cd.extend_from_slice(&(comp.len() as u32).to_le_bytes());
            cd.extend_from_slice(&(content.len() as u32).to_le_bytes());
            cd.extend_from_slice(&(name.len() as u16).to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u16.to_le_bytes());
            cd.extend_from_slice(&0u32.to_le_bytes());
            cd.extend_from_slice(&lfh_off.to_le_bytes());
            cd.extend_from_slice(name.as_bytes());
        }
        let cd_off = buf.len() as u32;
        buf.extend_from_slice(&cd);
        // EOCD
        buf.extend_from_slice(b"PK\x05\x06");
        buf.extend_from_slice(&0u16.to_le_bytes());
        buf.extend_from_slice(&0u16.to_le_bytes());
        buf.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        buf.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        buf.extend_from_slice(&(cd.len() as u32).to_le_bytes());
        buf.extend_from_slice(&cd_off.to_le_bytes());
        buf.extend_from_slice(&0u16.to_le_bytes());
        fs::write(path, buf).unwrap();
    }

    #[test]
    fn imports_nested_atmosphere_zip_and_blocks_ns1_bdat_and_empty() {
        let root = temp_stage_dir("imp-test").unwrap();
        let mods = root.join("mods");
        let mut exefs = ExefsTargetInfo::default();
        exefs.build_ids.insert(
            "main".into(),
            "8bef68ed1eebc24bf7278040e63b83f6e417e2e2000000000000000000000000".into(),
        );

        // 1. Nested Atmosphere zip with wrapper dir + romfs + cheat
        let zip_ok = root.join("CoolPack.zip");
        make_test_zip(
            &zip_ok,
            &[
                ("Wrapper/atmosphere/contents/0100E95004038000/romfs/ui/test.wilay", b"hello"),
                ("Wrapper/atmosphere/contents/0100E95004038000/cheats/8BEF68ED1EEBC24B.txt", b"[Max Gold]\n04100000 00001234 0000FFFF\n"),
            ],
        );
        let summary = import_mod_source(&mods, &zip_ok, &exefs, None, None).unwrap();
        assert_eq!(summary.folder, "CoolPack");
        assert!(mods.join("CoolPack/romfs/ui/test.wilay").is_file());
        assert!(mods.join("CoolPack/cheats/8BEF68ED1EEBC24B.txt").is_file());
        assert!(mods.join("CoolPack/mod.json").is_file());

        // 2. NS1 common.bdat (844 tables) is hard-blocked
        let ns1_dir = root.join("BadBdat");
        fs::create_dir_all(ns1_dir.join("romfs/bdat")).unwrap();
        let mut bad = vec![0u8; 16];
        bad[0..4].copy_from_slice(&844u32.to_le_bytes());
        fs::write(ns1_dir.join("romfs/bdat/common.bdat"), bad).unwrap();
        let err = import_mod_source(&mods, &ns1_dir, &exefs, None, None).unwrap_err();
        assert!(err.contains("844"), "expected 844 table error, got: {err}");
        assert!(!mods.join("BadBdat").exists());

        // 3. Empty folder is hard-blocked
        let empty_dir = root.join("EmptyMod");
        fs::create_dir_all(&empty_dir).unwrap();
        fs::write(empty_dir.join("readme.txt"), b"just notes").unwrap();
        assert!(import_mod_source(&mods, &empty_dir, &exefs, None, None).is_err());

        // 4. Single loose cheat .txt auto-placed into cheats/8BEF68ED1EEBC24B.txt
        let loose_cheat = root.join("MyCheats.txt");
        fs::write(&loose_cheat, "[Inf HP]\n04100000 00000100 0000270F\n").unwrap();
        let s2 = import_mod_source(&mods, &loose_cheat, &exefs, None, None).unwrap();
        assert_eq!(s2.folder, "MyCheats");
        assert!(mods.join("MyCheats/cheats/8BEF68ED1EEBC24B.txt").is_file());

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    #[ignore]
    fn verify_real_cheat_samples_and_bf2_arh() {
        let samples_env = std::env::var("XC2_TEST_SAMPLES_DIR").expect("set XC2_TEST_SAMPLES_DIR to run");
        let romfs_env = std::env::var("XC2_TEST_ROMFS_DIR").expect("set XC2_TEST_ROMFS_DIR to run");
        let samples_dir = Path::new(&samples_env);
        let romfs_dir = Path::new(&romfs_env);
        let arh_path = romfs_dir.join("bf2.arh");
        assert!(samples_dir.is_dir(), "samples dir missing");
        assert!(arh_path.is_file(), "bf2.arh missing");

        let arh = ArhIndex::from_file(&arh_path).expect("failed to decode real bf2.arh");
        println!("REAL bf2.arh decoded: {} paths", arh.paths.len());
        assert!(arh.paths.len() > 60_000, "unexpectedly small bf2.arh: {}", arh.paths.len());
        assert_eq!(arh.lookup_unique_by_filename("common.bdat").as_deref(), Some("bdat/common.bdat"));

        let mut exefs = ExefsTargetInfo::default();
        exefs.build_ids.insert(
            "main".into(),
            "8bef68ed1eebc24bf7278040e63b83f6e417e2e2000000000000000000000000".into(),
        );

        let work = temp_stage_dir("real-samples").unwrap();
        let mods_dir = work.join("mods");
        let mut entries: Vec<PathBuf> = fs::read_dir(samples_dir)
            .unwrap()
            .flatten()
            .map(|e| e.path())
            .collect();
        entries.sort();

        for sample in &entries {
            let name = sample.file_name().unwrap().to_string_lossy();
            let res = import_mod_source(&mods_dir, sample, &exefs, Some(romfs_dir), Some(&arh));
            match res {
                Ok(sum) => {
                    println!("IMPORT OK [{}]: folder={} warnings={}", name, sum.folder, sum.warnings.len());
                    for w in &sum.warnings {
                        println!("  WARN: {w}");
                    }
                }
                Err(e) => {
                    println!("IMPORT ERR [{}]: {e}", name);
                }
            }
        }

        let snap = crate::mods::compute_snapshot(&work, &exefs, Some(romfs_dir), false);
        println!("SNAPSHOT: {} mods, arh_loaded={}, main_bid={}", snap.mods.len(), snap.arh_loaded, snap.main_build_id);
        for m in &snap.mods {
            println!(
                "  MOD {:26} empty={} matched_cheats={} unmatched_cheats={} bdat_err={:?}",
                m.folder, m.empty, m.cheats.len(), m.unmatched_cheats, m.bdat_error
            );
        }

        assert_eq!(snap.mods.len(), 5, "all 5 sample folders should import with NS1 Build ID warnings");
        for m in &snap.mods {
            assert!(!m.empty, "{} should not be marked empty", m.folder);
            assert!(m.unmatched_cheats > 0, "{} is NS1 and must flag unmatched_cheats", m.folder);
        }

        let _ = fs::remove_dir_all(&work);
    }
}
