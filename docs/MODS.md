# Mods (interface draft)

Status: **draft, not implemented.** The launcher creates `mods\` and the setup engine writes its path into
`nxrt.ini`, but nothing in the runtime reads it yet. This file is the contract we will build against; treat
everything below as the design, not as behaviour you can rely on today.

The conventions are deliberately the ones the Switch emulators and the closest port to this one already use
([TotK Recomp's mods guide](https://github.com/valentderah/totk-pc-recomp/blob/main/docs/mods.md)), so a mod made
for Ryujinx or yuzu can be dropped in without being repacked.

## A mod is a folder

```
mods\
  Better textures\
    romfs\
      ...                    ← replaces files of the game's romfs, by the same path
  Frame rate unlock\
    exefs\
      patch.pchtxt         ← patches the game's own code (see "Code patches")
  My cheats\
    cheats\
      8BEF68ED1EEBC24BF7278040E63B83F6E417E2E2.txt
```

One folder per mod, named by the player. Inside it, these three may appear at the top level:

| folder | what it replaces |
|---|---|
| `romfs/` | game files, by their logical path (inside `bf2.ard` or loose, see "Where files live") |
| `exefs/` | code patches (`.pchtxt`, `.ips`) applied when the game is built |
| `cheats/` | one text file per build id, in Atmosphère's format |

### Emulator layouts are accepted as they are

Inside a mod's folder the following are found in addition to the plain layout, in this order:

```
<mod>\romfs
<mod>\atmosphere\contents\0100E95004038000\romfs
<mod>\contents\0100E95004038000\romfs
<mod>\0100E95004038000\romfs
```

and the same places for `exefs` and `cheats`; `atmosphere\exefs_patches\<name>\` is accepted for code patches.

**Which id goes in that folder.** `0100E95004038000` is the application (the same title as the Switch 1 release,
update 1114112); its Switch 2 program is `0400E95004038001`, and that is what the lift recompiles. An emulator
layout names the **application id** for base-game content, which is why the three lines above use it. The add-on
packs are their own titles — `0100E95004039001` … `063` (the retail set is not contiguous) — and a mod that
replaces a file *inside a pack* is placed under that pack's id instead.

Files under any other title id are ignored, so one mod folder can hold content for several games without us
serving the wrong one.

## Order and switches

The mod list is every folder in `mods\`, in this order:

1. the order named in `nxrt.ini` under `[mods] order` (a comma-separated list of folder names),
2. then any folder that list does not name, alphabetically — a new or renamed folder is simply appended.

A mod listed in `[mods] disabled` is not loaded. Both lists key on the **folder name**: renaming a folder makes it
a new mod with its switches reset.

Changes take effect at the next start; nothing is swapped mid-session.

## Conflicts

Two mods that provide the same file **do not merge**: the one higher in the order wins that whole file, and the
lower mod's report counts it as covered. Merging two texture packs is the player's job, with the tools the
community already has (TKMM and friends), and the merged result is dropped in as one folder.

The runtime reports every mod it loaded:

```
mods: Better textures: 412 files replaced, 3 not found in this game version (first: romfs\monolib\ui\old.x)
mods: Frame rate unlock: 0 files, 1 code patch, not applied (build id mismatch)
```

"not found in this game version" is the one check an emulator cannot make and we can: the archive index tells us
whether a path exists at all, so a mod that was extracted from a different revision says so instead of silently
doing half its job.

## Where files live

The game's romfs is served from a folder, but most assets are inside `bf2.ard` (about 15 GB) and the game parses
that archive itself. A mod therefore names files by their **logical romfs path**, whether the file is loose
(`romfs\stream\...`) or inside the archive (`romfs\monolib\...`); the runtime maps the path to an archive offset
and serves the mod's bytes there. Cache entries that the archive keeps compressed or tiled are supported by serving
the mod's file as-is; a mod that ships an archive-internal file uncompressed where the game expects compression is
not.

**Size table.** A file that a mod makes larger must have its size entry raised, or the game fails when it first
loads it, long after the start. The size table for this game has not been mapped yet; until it is, a mod that grows
a file may need the entry patched as well. If the table turns out to work like TotK's, the runtime will merge it at
start the same way.

## Code patches

The game's code is lifted from the player's own dump and compiled ahead of time, so a code patch cannot be applied
at run time. A mod's `.pchtxt` / `.ips` files are applied to a **copy of the module** at Set up, before lifting:

1. a mod with code patches turns on only after Set up has run again (the launcher offers it),
2. until then the whole mod stays off, its romfs files included,
3. after that it switches on and off like any other mod, with no further rebuild.

A patch is for one build id: `.ips` is named after it, `.pchtxt` names it on its `@nsobid` line.

## Cheats

`mods\<mod>\cheats\<build id>.txt`, Atmosphère's format, switched cheat by cheat. A cheat that changes code needs
the same re-setup as a code patch; a cheat that only writes values does not.

## Metadata and settings (optional)

A `mod.json` beside the folders is optional. Without it the mod works; with it the launcher can show a name, a
version, the game version it targets, and **settings the mod itself declares**:

```json
{
  "name": "Better textures",
  "version": "1.2",
  "game": "3.0.1",
  "author": "...",
  "options": {
    "quality": { "type": "select", "default": "high",
                 "values": { "high": "romfs_high/...", "low": "romfs_low/..." } }
  }
}
```

The launcher renders `options` with the same declarative fields its own settings page uses, and the chosen values
select which sub-tree of the mod is served. This is the "adjust" half of the interface: a mod author ships the
schema, not an installer.

## Not supported (on purpose)

- **Engine hooks.** No plugin may run inside the game's logic; a mod changes files, code bytes and memory values.
  (The port's own bring-up hooks are not a public interface.)
- **Mid-session switching.** Mods are applied at start.
- **Game versions other than the one installed.** A mod that names a version we do not run is reported, not guessed at.

## Legal

No mod, cheat or extracted asset ships with this project or its releases. Mods are the player's own files, exactly
like the dump the game is built from; the project only provides the loader.

## How the overlay is applied (M1, built)

The game parses the RomFS tables itself, and this runtime synthesises those tables from a host folder. The overlay
is therefore folded in **when the image is built**, before the tables are laid out: a mod's file replaces the host
path of the entry at the same logical path, and nothing is looked up at run time.

```
[fs] mods       = mods          the folder of mod folders
[mods] order    = A,B           highest priority first; unnamed folders follow alphabetically
[mods] disabled = C             not loaded; logged as disabled
```

Only the **game's own** romfs gets the overlay - every add-on pack builds its own image through the same code, and
laying the mods over those as well would put each file in seven storages. The build log reports each mod:

```
mods: TestMod: 1 file(s), 1 replaced, 0 added
mods: 1 enabled, 0 disabled, 0 named in [mods] order
```

## Files inside the archive (M2, built)

The game reads an archive file as 48 bytes of header at the record offset, then the comp bytes the header states.
A mod therefore ships a pre-built record instead of a plain file:

    mods/<mod>/.ard/<name>.rec

`tools/mods/ard_pack.py` builds it: the original header with `unc` set to the mod file's size, `comp` and the
checksum left alone, followed by the zlib stream padded to `comp`. Keeping comp means the game's read lengths
stay valid; keeping the checksum means an id that the engine may key on does not change.

Two limits of this first version:

* the mod file must compress into the original comp - a 512-to-2048 texture swap will not, and is reported rather
  than truncated;
* only the record the game asks for by that name is replaced, one .rec per file.

The next step (M2b) appends oversized records after the archive and repoints an in-memory copy of `bf2.arh`, which
lifts the size limit.

## The asset catalog (M0, built)

Where a mod can put a file depends on where the game reads it from, and most of this game's assets are inside
`bf2.ard` rather than loose on the romfs. `tools/mods/asset_index.py` writes one table of every file the game can
read — loose romfs files, the archive's records, and each add-on pack:

```
work/mods/assets.tsv        source, path, kind, size, unc, comp, offset, checksum, sha256
work/mods/assets-summary.json
```

`source` is `romfs`, `ard` or `dlc:<id>`. The tooling and its contract live in
[`tools/mods/README.md`](../tools/mods/README.md); the output is derived from the player's dump and is never
committed.

## Implementation order

| step | what | state |
|---|---|---|
| M0 | the asset catalog: romfs + `bf2.ard` + add-on content → `work/mods/assets.tsv` | **built** (`tools/mods/asset_index.py`) |
| M0.5 | the porting check: `tools/mods/mod_check.py` says, per file, `same` / `differs` / `only-archive` / `absent` | **built** |
| M1 | a mod folder's `romfs` laid over the game's, load report, `[mods] order` / `[mods] disabled` | **built** (folded into the image tables, `fs_romfs.cpp`) |
| M1.5 | the emulator layouts, filtered by application id | **built** (`mod_romfs_root()`) |
| M2 | files inside `bf2.ard`: a mod file becomes a pre-built record (`tools/mods/ard_pack.py`), served at read time | **built (v1)** |
| M3 | `.pchtxt` / `.ips` applied at Set up | not started |
| M4 | the size table, cheats, `mod.json` options | not started |