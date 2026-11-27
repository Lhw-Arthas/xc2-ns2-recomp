// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only XC2 `bf2.arh` path-table decoder (matches runtime `fs_romfs.cpp`) and `common.bdat` table-count validator.
//! Zero game data is embedded: everything is derived at runtime from the player's own `romfs-301/bf2.arh`.

use flate2::read::ZlibDecoder;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

pub const NS2_COMMON_BDAT_TABLES: u32 = 926;
const TRIE_XOR: u32 = 0x0C0C_ACAC;
const XBC1_HEADER_LEN: usize = 0x30;

#[derive(Clone, Default)]
pub struct ArhIndex {
    pub paths: HashSet<String>,
    pub by_filename: HashMap<String, Vec<String>>,
}

impl ArhIndex {
    pub fn is_empty(&self) -> bool {
        self.paths.is_empty()
    }

    pub fn from_bytes(arh: &[u8]) -> Self {
        let mut paths = HashSet::new();
        let mut by_filename: HashMap<String, Vec<String>> = HashMap::new();
        for p in decode_arh_paths(arh) {
            if paths.insert(p.clone()) {
                let fname = p.rsplit('/').next().unwrap_or(&p).to_string();
                by_filename.entry(fname).or_default().push(p);
            }
        }
        for list in by_filename.values_mut() {
            list.sort();
            list.dedup();
        }
        Self { paths, by_filename }
    }

    pub fn from_file(path: &Path) -> Option<Self> {
        let data = fs::read(path).ok()?;
        let idx = Self::from_bytes(&data);
        (!idx.is_empty()).then_some(idx)
    }

    /// Returns the unique logical archive path if exactly one `bf2.arh` entry has this filename.
    pub fn lookup_unique_by_filename(&self, filename: &str) -> Option<String> {
        let key = filename.trim().to_ascii_lowercase();
        let list = self.by_filename.get(&key)?;
        (list.len() == 1).then(|| list[0].clone())
    }

    /// Checks whether a logical `romfs/` relative path (lowercase, `/` separated) matches either
    /// `bf2.arh`, a loose file in vanilla `romfs`, or a known archive-mod control file.
    pub fn is_known_romfs_path(&self, logical_rel: &str, vanilla_romfs: Option<&Path>) -> bool {
        let p = logical_rel.trim_start_matches('/').to_ascii_lowercase();
        if p.is_empty() {
            return false;
        }
        // Known archive-level mod files / .ard record directories
        if matches!(p.as_str(), "bf2.arh" | "bf2.ard" | "append.bin") {
            return true;
        }
        if let Some(first) = p.split('/').next() {
            if first.ends_with(".ard") || first == "ard" {
                return true;
            }
        }
        if self.paths.contains(&p) {
            return true;
        }
        if let Some(vroot) = vanilla_romfs {
            if vroot.join(&p).is_file() {
                return true;
            }
        }
        false
    }
}

/// Cached loader for `bf2.arh` keyed by file path so repeated `list_mods` calls stay fast.
pub fn cached_arh_index(vanilla_romfs: Option<&Path>) -> Option<Arc<ArhIndex>> {
    let arh_path = vanilla_romfs?.join("bf2.arh");
    if !arh_path.is_file() {
        return None;
    }
    static CACHE: OnceLock<Mutex<Option<(PathBuf, Arc<ArhIndex>)>>> = OnceLock::new();
    let mu = CACHE.get_or_init(|| Mutex::new(None));
    let mut guard = mu.lock().ok()?;
    if let Some((ref p, ref idx)) = *guard {
        if *p == arh_path {
            return Some(Arc::clone(idx));
        }
    }
    let idx = Arc::new(ArhIndex::from_file(&arh_path)?);
    *guard = Some((arh_path, Arc::clone(&idx)));
    Some(idx)
}

fn xor4(src: &[u8], key: u32) -> Vec<u8> {
    let pad = key.to_le_bytes();
    src.iter().enumerate().map(|(i, &b)| b ^ pad[i & 3]).collect()
}

fn get_u32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

/// Decodes all lowercase logical paths (without leading `/`) from a Xenoblade `arh1` header.
pub fn decode_arh_paths(arh: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    if arh.len() < 0x28 || &arh[..4] != b"arh1" {
        return out;
    }
    let str_off = get_u32(arh, 0x0C) as usize;
    let str_sz = get_u32(arh, 0x10) as usize;
    let trie_off = get_u32(arh, 0x14) as usize;
    let trie_sz = get_u32(arh, 0x18) as usize;
    let ent_off = get_u32(arh, 0x1C) as usize;
    let ent_cnt = get_u32(arh, 0x20);
    let key = get_u32(arh, 0x24);

    if str_off < 0x28
        || str_sz < 4
        || trie_off < 0x28
        || trie_sz < 8
        || str_off.saturating_add(str_sz) > arh.len()
        || trie_off.saturating_add(trie_sz) > arh.len()
        || (ent_off as u64).saturating_add((ent_cnt as u64).saturating_mul(24)) > arh.len() as u64
    {
        return out;
    }

    let dec_str = xor4(&arh[str_off..str_off + str_sz], key ^ TRIE_XOR ^ 0xFFFF_FFFF);
    let dec_trie = xor4(&arh[trie_off..trie_off + trie_sz], key ^ TRIE_XOR);
    let n_nodes = dec_trie.len() / 8;

    let trie_i32 = |node: usize, field: usize| -> i32 {
        let p = node * 8 + field * 4;
        get_u32(&dec_trie, p) as i32
    };

    for node in 1..n_nodes {
        let a = trie_i32(node, 0);
        let b = trie_i32(node, 1);
        if b >= 0 {
            continue;
        }
        let par = -(b as i64) - 1;
        if par < 0 || (par as usize) >= n_nodes {
            continue;
        }
        let pa = trie_i32(par as usize, 0);
        if pa >= 0 {
            continue;
        }
        let edge = (-(pa as i64) - 1) ^ (node as i64);
        if !(0..256).contains(&edge) {
            continue;
        }

        let (tail, idx) = if a > 0 {
            let pos = (a as usize) + 1;
            let mut nul = pos;
            while nul < dec_str.len() && dec_str[nul] != 0 {
                nul += 1;
            }
            if nul + 5 > dec_str.len() {
                continue;
            }
            let tail_s = String::from_utf8_lossy(&dec_str[pos..nul]).into_owned();
            let idx = get_u32(&dec_str, nul + 1);
            (tail_s, idx)
        } else if edge == 0 && a < 0 {
            (String::new(), (-(a as i64) - 1) as u32)
        } else {
            continue;
        };

        if idx >= ent_cnt {
            continue;
        }

        let mut cur = node;
        let mut prefix_bytes = Vec::new();
        let mut ok = true;
        for _ in 0..1024 {
            if cur == 0 {
                break;
            }
            let cb = trie_i32(cur, 1);
            if cb >= 0 {
                ok = false;
                break;
            }
            let p = -(cb as i64) - 1;
            if p < 0 || (p as usize) >= n_nodes {
                ok = false;
                break;
            }
            let pa2 = trie_i32(p as usize, 0);
            if pa2 >= 0 {
                ok = false;
                break;
            }
            let ch = (-(pa2 as i64) - 1) ^ (cur as i64);
            if !(0..256).contains(&ch) {
                ok = false;
                break;
            }
            if ch != 0 {
                prefix_bytes.push(ch as u8);
            }
            cur = p as usize;
        }
        if !ok || cur != 0 {
            continue;
        }
        prefix_bytes.reverse();
        let mut full = String::from_utf8_lossy(&prefix_bytes).into_owned();
        full.push_str(&tail);
        let full = full.trim_start_matches('/').to_ascii_lowercase();
        if !full.is_empty() {
            out.push(full);
        }
    }
    out
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BdatCheck {
    Ok { tables: u32, compressed: bool },
    Mismatch { tables: u32, compressed: bool },
    Invalid(String),
}

/// Validates a `common.bdat` file (plain BDAT or `xbc1`-wrapped zlib BDAT).
/// NS2 3.0.1 has exactly 926 tables; NS1 2.1.0 has 844 and will crash on boot.
pub fn inspect_common_bdat(path: &Path) -> BdatCheck {
    let mut f = match fs::File::open(path) {
        Ok(f) => f,
        Err(e) => return BdatCheck::Invalid(format!("cannot open common.bdat: {e}")),
    };
    let mut head = [0u8; XBC1_HEADER_LEN];
    let n = match f.read(&mut head) {
        Ok(n) => n,
        Err(e) => return BdatCheck::Invalid(format!("cannot read common.bdat: {e}")),
    };
    if n < 8 {
        return BdatCheck::Invalid("common.bdat is too small (< 8 bytes)".into());
    }

    if &head[..4] == b"xbc1" {
        if n < XBC1_HEADER_LEN {
            return BdatCheck::Invalid("truncated xbc1 header in common.bdat".into());
        }
        // Only decompress the first 8 bytes from the zlib stream after the 0x30-byte xbc1 header.
        let mut dec = ZlibDecoder::new(f);
        let mut raw = [0u8; 8];
        if let Err(e) = dec.read_exact(&mut raw) {
            return BdatCheck::Invalid(format!("failed to decompress xbc1 common.bdat: {e}"));
        }
        let tables = u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]);
        if tables == NS2_COMMON_BDAT_TABLES {
            BdatCheck::Ok { tables, compressed: true }
        } else {
            BdatCheck::Mismatch { tables, compressed: true }
        }
    } else {
        let tables = u32::from_le_bytes([head[0], head[1], head[2], head[3]]);
        if tables == NS2_COMMON_BDAT_TABLES {
            BdatCheck::Ok { tables, compressed: false }
        } else {
            BdatCheck::Mismatch { tables, compressed: false }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::ZlibEncoder;
    use flate2::Compression;
    use std::io::Write;

    fn build_minimal_arh(path_str: &str) -> Vec<u8> {
        // Build a tiny 2-node trie for "/<first_char>" + tail "<rest>" -> entry 0.
        let bytes = path_str.as_bytes();
        let first = bytes[0] as i32;
        let tail = &bytes[1..];

        // String table: [0x00, tail..., 0x00, idx(u32 LE)]
        let mut raw_str = vec![0u8];
        raw_str.extend_from_slice(tail);
        raw_str.push(0);
        raw_str.extend_from_slice(&0u32.to_le_bytes());

        // Trie: node 0 has a = -(base + 1) where base = 0 => a = -1, b = 0.
        // Node `node_id = first ^ 0 = first`: a = 0 (offset into str_table -> pos = a + 1 = 1? Wait: a > 0 required!)
        // So put 1 dummy byte at index 0, 1 dummy byte at index 1, tail starts at index 2 => a = 1.
        let mut str_buf = vec![0u8, 0u8];
        str_buf.extend_from_slice(tail);
        str_buf.push(0);
        str_buf.extend_from_slice(&0u32.to_le_bytes());

        let node_id = first as usize;
        let n_nodes = node_id + 1;
        let mut trie_buf = vec![0u8; n_nodes * 8];
        // node 0: a = -1 (base = 0), b = 0
        trie_buf[0..4].copy_from_slice(&(-1i32).to_le_bytes());
        // node `node_id`: a = 1 (tail starts at pos 2), b = -1 (parent = 0)
        trie_buf[node_id * 8..node_id * 8 + 4].copy_from_slice(&1i32.to_le_bytes());
        trie_buf[node_id * 8 + 4..node_id * 8 + 8].copy_from_slice(&(-1i32).to_le_bytes());

        let key: u32 = 0x1234_5678;
        let enc_str = xor4(&str_buf, key ^ TRIE_XOR ^ 0xFFFF_FFFF);
        let enc_trie = xor4(&trie_buf, key ^ TRIE_XOR);

        let str_off = 0x28u32;
        let trie_off = str_off + enc_str.len() as u32;
        let ent_off = trie_off + enc_trie.len() as u32;

        let mut arh = vec![0u8; ent_off as usize + 24];
        arh[0..4].copy_from_slice(b"arh1");
        arh[0x0C..0x10].copy_from_slice(&str_off.to_le_bytes());
        arh[0x10..0x14].copy_from_slice(&(enc_str.len() as u32).to_le_bytes());
        arh[0x14..0x18].copy_from_slice(&trie_off.to_le_bytes());
        arh[0x18..0x1C].copy_from_slice(&(enc_trie.len() as u32).to_le_bytes());
        arh[0x1C..0x20].copy_from_slice(&ent_off.to_le_bytes());
        arh[0x20..0x24].copy_from_slice(&1u32.to_le_bytes());
        arh[0x24..0x28].copy_from_slice(&key.to_le_bytes());
        arh[str_off as usize..trie_off as usize].copy_from_slice(&enc_str);
        arh[trie_off as usize..ent_off as usize].copy_from_slice(&enc_trie);
        arh
    }

    #[test]
    fn decodes_minimal_arh_and_finds_unique_filename() {
        let arh = build_minimal_arh("/bdat/common.bdat");
        let idx = ArhIndex::from_bytes(&arh);
        assert!(idx.paths.contains("bdat/common.bdat"));
        assert_eq!(idx.lookup_unique_by_filename("COMMON.BDAT").as_deref(), Some("bdat/common.bdat"));
        assert!(idx.is_known_romfs_path("bdat/common.bdat", None));
        assert!(!idx.is_known_romfs_path("wrong/common.bdat", None));
    }

    #[test]
    fn validates_plain_and_xbc1_common_bdat() {
        let dir = std::env::temp_dir().join(format!("xc2-arh-bdat-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        let write_plain = |name: &str, tables: u32| -> PathBuf {
            let p = dir.join(name);
            let mut b = vec![0u8; 16];
            b[0..4].copy_from_slice(&tables.to_le_bytes());
            fs::write(&p, b).unwrap();
            p
        };
        let write_xbc1 = |name: &str, tables: u32| -> PathBuf {
            let p = dir.join(name);
            let mut raw = vec![0u8; 32];
            raw[0..4].copy_from_slice(&tables.to_le_bytes());
            let mut enc = ZlibEncoder::new(Vec::new(), Compression::default());
            enc.write_all(&raw).unwrap();
            let comp = enc.finish().unwrap();
            let mut out = vec![0u8; XBC1_HEADER_LEN];
            out[0..4].copy_from_slice(b"xbc1");
            out.extend_from_slice(&comp);
            fs::write(&p, out).unwrap();
            p
        };

        assert_eq!(inspect_common_bdat(&write_plain("ok.bdat", 926)), BdatCheck::Ok { tables: 926, compressed: false });
        assert_eq!(inspect_common_bdat(&write_plain("ns1.bdat", 844)), BdatCheck::Mismatch { tables: 844, compressed: false });
        assert_eq!(inspect_common_bdat(&write_xbc1("ok_xbc1.bdat", 926)), BdatCheck::Ok { tables: 926, compressed: true });
        assert_eq!(inspect_common_bdat(&write_xbc1("ns1_xbc1.bdat", 844)), BdatCheck::Mismatch { tables: 844, compressed: true });

        let _ = fs::remove_dir_all(&dir);
    }
}
