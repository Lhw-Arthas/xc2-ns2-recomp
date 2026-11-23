# tools/mods — the mod path's own tools

Everything the mod work needs lives here, so a future round finds it in one place instead of across
`tools/analysis` and `tools/shader`.

| file | what | state |
|---|---|---|
| `asset_index.py` | **M0**: one table of every file the game can read — romfs, `bf2.ard`, add-on content | works |
| `mod_check.py` | **M0.5**: for each file of a mod — `same` / `differs` / `only-archive` / `absent`, and where the game reads it from | works |
| `ard_pack.py` | **M2**: turns a mod file into the archive record the game expects (`<mod>/.ard/<name>.rec`) | works |
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

## Archive replacements (M2)

Most of this game's assets are inside `bf2.ard`, which the game reads as "48 bytes of xbc1 header at the
record offset, then comp bytes right after it". `ard_pack.py` writes that pair once, per replaced file:

    mods/<mod>/.ard/<name>.rec        original header (unc = the mod file size, comp and checksum kept) + zlib(padded)

The runtime has no zlib, so it never compresses: it slices the .rec at the same relative offsets the game asks for.
The name is the archive's logical name (`font_binary2.txt`), not a romfs path - the runtime learns it from
the header the game reads.

    python tools/mods/ard_pack.py --mod "<mod folder>" --catalog work/mods/assets.tsv

A file whose zlib stream is larger than the original comp does not fit and is reported, not truncated. That is the
v1 limit; large textures (AI upscales) need the append-and-repoint path (M2b).

## The contract the later milestones rely on

* **M1/M2** resolve a mod path by looking it up in `assets.tsv`: a path found under `ard` needs the archive
  override, a path found under `romfs` or `dlc:<id>` is a plain file.
* **mod_check** reports, per file: `found` / `missing` / `size differs` / `sha256 differs` — which is what a
  player needs when an NS1 mod is offered for the NS2 edition.
* The catalog holds **paths, sizes and hashes only** — never file contents, and never anything from the archive's
  payload.
