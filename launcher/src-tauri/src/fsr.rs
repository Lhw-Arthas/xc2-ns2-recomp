// SPDX-License-Identifier: GPL-3.0-or-later
//! Downloads AMD's signed FidelityFX SDK Vulkan DLLs and licence texts directly from GPUOpen's official GitHub tags,
//! verifies each file's byte count and SHA-256, and places them into upscalers/fsr3 next to the game.

use std::fs;
use std::ops::Deref;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;

use crate::streamline::{download_verified, DownloadAsset, DownloadManifest, Status};

/// Distinct Tauri state wrapper so FSR download status/cancellation never collides with DLSS `streamline::Shared`.
#[derive(Default)]
pub struct Shared(pub crate::streamline::Shared);

impl Deref for Shared {
    type Target = crate::streamline::Shared;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

struct FsrAsset {
    name: &'static str,
    url: &'static str,
    size: u64,
    sha256: &'static str,
}

/// Official GPUOpen FidelityFX-SDK v2.4.0 (FSR 3.1.6 + FSR 4.1.2 upscaler) and v1.1.4 (FSR 3 frame generation + fallback).
const ASSETS: [FsrAsset; 5] = [
    FsrAsset {
        name: "amd_fidelityfx_loader_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll",
        size: 39_184,
        sha256: "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9",
    },
    FsrAsset {
        name: "amd_fidelityfx_upscaler_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
        size: 36_135_696,
        sha256: "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7",
    },
    FsrAsset {
        name: "license-FidelityFX-v2.4.0.md",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/docs/license.md",
        size: 65_770,
        sha256: "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb",
    },
    FsrAsset {
        name: "amd_fidelityfx_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/PrebuiltSignedDLL/amd_fidelityfx_vk.dll",
        size: 9_332_432,
        sha256: "a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78",
    },
    FsrAsset {
        name: "LICENSE-FidelityFX-v1.1.4.txt",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/LICENSE.txt",
        size: 1_115,
        sha256: "82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a",
    },
];

const TOTAL_BYTES: u64 = 45_574_197;
const WORK: &str = ".download";

pub fn manifest() -> DownloadManifest {
    DownloadManifest {
        kind: "fsr",
        version: "2.4.0 / 1.1.4",
        repo_url: "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK",
        total_bytes: TOTAL_BYTES,
        license: "AMD FidelityFX SDK Binary EULA (v2.4.0) + MIT (v1.1.4)",
        assets: ASSETS
            .iter()
            .map(|a| DownloadAsset { name: a.name, url: a.url, size: a.size, sha256: a.sha256 })
            .collect(),
    }
}

/// upscalers/fsr3 next to the game: the folder temporal_ffx.cpp and framegen_ffx.cpp search by default.
pub fn dest(game_dir: &Path) -> PathBuf {
    game_dir.join("upscalers").join("fsr3")
}

pub struct Run<'a> {
    pub shared: &'a Shared,
    pub game_dir: PathBuf,
}

impl Run<'_> {
    fn set(&self, phase: &str, done: u64, total: u64, message: &str) {
        let mut st = self.shared.status.lock().unwrap();
        st.state = "running".into();
        st.phase = phase.into();
        st.done = done;
        st.total = total;
        st.message = message.into();
    }

    fn cancelled(&self) -> Result<(), String> {
        if self.shared.cancel.load(Ordering::SeqCst) { Err("Download cancelled.".into()) } else { Ok(()) }
    }

    pub fn run(&self) -> Result<Status, String> {
        let dest = dest(&self.game_dir);
        let work = dest.join(WORK);
        fs::create_dir_all(&work).map_err(|e| format!("cannot create {}: {e}", work.display()))?;
        let mb = |b: u64| b as f64 / (1024.0 * 1024.0);
        let mut base_done: u64 = 0;

        let result = (|| -> Result<(), String> {
            for (idx, a) in ASSETS.iter().enumerate() {
                self.cancelled()?;
                let part = work.join(format!("{}.part", a.name));
                let staged = work.join(a.name);
                download_verified(a.url, &part, &staged, a.size, a.sha256, &self.shared.cancel, |got| {
                    let cur = base_done + got;
                    self.set(
                        "download",
                        cur,
                        TOTAL_BYTES,
                        &format!("Downloading AMD FidelityFX ({}/{}: {:.1} of {:.1} MiB)", idx + 1, ASSETS.len(), mb(cur), mb(TOTAL_BYTES)),
                    );
                })?;
                base_done += a.size;
            }
            self.cancelled()?;
            self.set("install", TOTAL_BYTES, TOTAL_BYTES, "Installing");
            for a in ASSETS {
                let from = work.join(a.name);
                fs::copy(&from, dest.join(a.name)).map_err(|e| format!("cannot copy {} to {}: {e}", from.display(), dest.display()))?;
            }
            Ok(())
        })();

        let _ = fs::remove_dir_all(&work);
        result?;
        Ok(Status {
            state: "done".into(),
            phase: "done".into(),
            done: TOTAL_BYTES,
            total: TOTAL_BYTES,
            message: format!("AMD FidelityFX v2.4.0 / v1.1.4 installed in {}.", dest.display()),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn manifest_matches_assets_and_sha256() {
        let m = manifest();
        assert_eq!(m.total_bytes, m.assets.iter().map(|a| a.size).sum::<u64>());
        assert!(m.assets.iter().all(|a| a.url.starts_with("https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/")));
        assert!(m.assets.iter().all(|a| a.sha256.len() == 64 && a.sha256.chars().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())));
    }
}
