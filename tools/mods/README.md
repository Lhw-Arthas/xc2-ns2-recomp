# tools/mods — the mod path's own tools

Everything the mod work needs lives here, so a future round finds it in one place instead of across
`tools/analysis` and `tools/shader`.

| file | what | state |
|---|---|---|
| `asset_index.py` | **M0**: one table of every file the game can read — romfs, `bf2.ard`, add-on content | works |
| `mod_check.py` | **M0.5**: for each file of a mod — `same` / `differs` / `only-archive` / `absent`, and where the game reads it from | works |
| `ard_pack.py` | **M2**: turns a mod file into the archive record the game expects (`<mod>/.ard/<name>.rec`) | works |
| `arh_index.py` | **M2b**: decodes full untruncated `bf2.arh` paths (`arh1` trie) for `ard_pack.py` & append deltas | works |
| `exefs_patch.py` | **M3 / M4c**: applies `.pchtxt`, `.ips`, and enabled `.text` DMNT cheats onto `build/exefs-patched` | works |
| `mod_pack.py` | validates and packs a mod folder (manifest, path escapes, no game data) | not started |

## What it reads

```
<dump>/base/romfs/            bf2.ard, bf2.arh, stream/, sound/
<dump>/dlc/<id>/romfs/        or <id>/0/romfs — both layouts the runtime accepts
```

## What it writes (never committed — derived from the player's own dump)

```
work/mods/assets.tsv           source, path, kind, size, unc, comp, offset, checksum, sha256
work/mods/assets-summary.json  counts, bytes, per-kind totals, what was skipped and why, timings
```

`source` is `romfs`, `ard` (inside the archive) or `dlc:<16-hex id>`. A loose file's `sha256` is filled up to
`--hash-limit-mb`; an archive record carries the archive's own `checksum` and its offsets instead.

```powershell
tools\python\python.exe tools\mods\asset_index.py --romfs "<dump>\base\romfs" --out work\mods
tools\python\python.exe tools\mods\asset_index.py --romfs "<dump>\base\romfs" --no-archive   # fast, loose files only
```

## Checking a mod (M0.5)

```powershell
tools\python\python.exe tools\mods\mod_check.py --mod "<mod folder>" --catalog work\mods\assets.tsv
```

A mod may be the overlay itself or a folder holding `romfs/`. Paths are compared with one spelling: `/`
separators, no leading slash, lower case — the archive stores `//ma61a/cache/texture/RK050`, which no Windows
folder can hold, and the console is case-sensitive while Windows is not.

| status | meaning |
|---|---|
| `same` | the path exists and size and sha256 match the dump (the mod does not change it) |
| `differs` | the path exists, content differs — what a working replacement looks like |
| `only-archive` | the path exists only inside `bf2.ard`; until M2 an overlay cannot replace it |
| `absent` | no such path in this dump — an NS1 layout, a different version, or a typo (exit code 1) |
| `no-hash` | the dump side was not hashed (above the catalog's `--hash-limit-mb`) |

Report: `work/mods/mods-report.json` (the 50 first absent and only-archive paths, plus every file's record).

## Archive replacements (M2 / M2b)

Most of this game's assets are inside `bf2.ard`, which the game reads either raw (`flags = 0`) or as "48 bytes of `xbc1`
header at the record offset, then `comp` bytes right after it" (`flags = 2`). `ard_pack.py` handles both:

    mods/<mod>/.ard/<name>.rec        original header (unc = the mod file size, comp and checksum kept) + zlib(padded)
    mods/<mod>/.ard/append.bin        M2b tail records (xbc1 or raw) for files larger than original comp / flags=0 files
    mods/<mod>/romfs/bf2.arh          repointed arh1 index entries for appended files

    python tools/mods/ard_pack.py --mod "<mod folder>" --catalog work/mods/assets.tsv --arh "<romfs>/bf2.arh" --ard-size <bf2.ard bytes>

Passing `--arh` lets `arh_index.py` decode the full untruncated `arh1` path trie (eliminating false `diff_unc` collisions
on localized `.bdat` and truncated 27-byte `xbc1` headers) and append uncompressed `flags = 0` entries as raw slices. When
multiple active mods ship `append.bin` + `romfs/bf2.arh`, `fs_romfs.cpp` concatenates each mod's tail in `[mods] order`
and composes their `bf2.arh` entry deltas in memory with relocated offsets.

## The contract the later milestones rely on

* **M1/M2** resolve a mod path by looking it up in `assets.tsv`: a path found under `ard` needs the archive
  override, a path found under `romfs` or `dlc:<id>` is a plain file.
* **mod_check** reports, per file: `found` / `missing` / `size differs` / `sha256 differs` — which is what a
  player needs when an NS1 mod is offered for the NS2 edition.
* The catalog holds **paths, sizes and hashes only** — never file contents, and never anything from the archive's
  payload.
