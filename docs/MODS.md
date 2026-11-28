<a id="english"></a>

# Mods & Cheats Guide (Human & AI-Agent Ready)

**English** · [中文](#中文)

> [!IMPORTANT]
> **Legal & Zero-Game-Data Policy:** This repository and its releases ship **no** game code, game assets, pre-built cheats, or generated asset tables. Every tool (`tools/mods/*.py`, launcher `bf2.arh` path lookup) reads **only your own legally dumped 3.0.1 files locally**; generated catalogs under `work/mods/` are git-ignored and never distributed.

---

## 1. NS2 3.0.1 Target Contract (Read First)

| Item | Value / Rule |
|---|---|
| **Target Version** | *Xenoblade Chronicles 2 – Nintendo Switch 2 Edition* **Retail 3.0.1** |
| **Application ID** | `0100E95004038000` (same base Title ID as NS1; do **not** treat this ID alone as NS1) |
| **`main` Build ID (16-hex)** | `8BEF68ED1EEBC24B` (full 40-hex: `8BEF68ED1EEBC24BF7278040E63B83F6E417E2E2`) |
| **`common.bdat` Table Count** | **926** tables (NS1 2.0.2/2.1.0 has **844** tables → crashes on boot; launcher blocks it) |
| **Archive Override (`bf2.ard`)** | Loose files in `mods/<mod>/romfs/<logical_path>` **automatically override** matching entries inside `bf2.ard` at runtime (no manual `bf2.ard` repack needed) |
| **Forbidden Legacy Patches** | **Do NOT port NS1 60 FPS / RRS / Dynamic Resolution patches.** Native 30–240 FPS and resolution scaling are built into Settings; patching dynres records causes a black screen |
| **Load Order** | **Top = highest priority (`#1` wins).** Higher mods override lower mods per file (no automatic file merge) |

---

## 2. Porting NS1 Mods & Cheats to NS2 3.0.1 (Human + AI Agent Playbook)

Most community mods and cheats were built for Switch 1 (2.0.2 / 2.1.0). Use this decision matrix before installing or porting:

| Mod / Cheat Type | Works As-Is on 3.0.1? | What To Do |
|---|---|---|
| **Loose Textures / UI / Audio / Models** (`.wilay`, `.wismt`, `.wimdo`, `.bnk`, `.wem`, etc.) | **Often yes** | Drop into the launcher **Mods** page. The launcher checks every relative path against your own `bf2.arh` / `romfs-301` and flags unmatched paths. |
| **NS1 `common.bdat` Mods** (844 tables) | **No (crashes)** | Never drop an NS1 `common.bdat` directly. Export the mod's row/field edits and re-apply them onto **your own 3.0.1 `common.bdat` (926 tables)**. |
| **NS1 Memory Cheats** (DMNT `.txt` / Switch-Cheats-DB `.json`) | **No (wrong Build ID & base pointers)** | Do **not** just rename the `.txt` to `8BEF68ED1EEBC24B.txt`. Re-resolve the `main + offset` global pointer(s) against **your own 3.0.1 `main` NSO** (see workflow below). |
| **NS1 Code Cheats / `.ips` / `.pchtxt`** | **No (different ARM64 code layout)** | Must be re-targeted to 3.0.1 `main` instructions and rebuilt via **Setup**. |
| **NS1 60 FPS / RRS / Resolution Cheats** | **Discard** | Already built into launcher **Settings** (FPS cap, resolution scale, FSR/DLSS, frame generation). |

### Step-by-Step: Porting NS1 Memory Cheats to 3.0.1 (Using Your Own Dump)

1. **Inspect the NS1 cheat instructions** (Atmosphère DMNT format):
   - Type `5` (`580F0000 <offset>`) loads a 64-bit global pointer from `main + <offset>` into register `R15`.
   - Type `7` (`780F0000 <field_off>`) adds a struct field offset to `R15`.
   - Type `6` (`640F0000 00000000 <val>`) writes a 1/2/4/8-byte value to `[R15]`.
   - Type `0` (`04000000 <text_off> <insn>`) writes directly into `main .text` (**code cheat** → requires Setup rebuild; keep separate from runtime memory cheats).
2. **Locate the matching global pointer in your own 3.0.1 `main` NSO**:
   - Decompress `.text` from your local `exefs/main` (verify NSO header Build ID starts with `8BEF68ED1EEBC24B`).
   - Search for distinctive constants or clamps used by the target subsystem (for example: max value caps like `999,999,999` / `9,999,999`, known struct strides, or string xrefs) and trace back to the `ADRP + ADD/LDR` instruction loading the `.bss`/`.data` global pointer.
   - Cross-check that the struct field offsets used by the cheat match the disassembly (because NS1 saves load directly on NS2 3.0.1, save-backed struct layouts typically keep the same internal field offsets once the base pointer is updated).
3. **Emit a clean 3.0.1 mod folder**:
   ```text
   <ModName>/
     mod.json                      # "game": "3.0.1"
     cheats/
       8BEF68ED1EEBC24B.txt        # Only verified 3.0.1 entries; omit unverified & FPS/RRS cheats
   ```
4. **Verify in the launcher & in-game**:
   - Drag the folder onto the launcher **Mods** page → confirm **0 warnings** → expand the card and tick one safe cheat (e.g. currency/stats) → **Save** → launch the game → confirm the effect in-game → exit and click **Refresh** in Mods → the card's status light should turn **green (`Active`)**.

### Copy-Paste Prompt for AI Coding Agents (Gemini / Claude / Codex)

Players who want an AI agent to help migrate an old NS1 mod or cheat locally can paste this prompt:

```text
Read docs/MODS.md in this repository first and follow all 3.0.1 rules strictly.
Task: Port my local NS1 mod/cheat at <PATH_TO_OLD_MOD> to XC2 NS2 3.0.1 (Build ID 8BEF68ED1EEBC24B)
using ONLY my own local 3.0.1 dump at <PATH_TO_MY_301_DUMP>.

Hard rules:
1. Never copy an NS1 common.bdat (844 tables) into 3.0.1 (926 tables); only re-apply field edits onto my 3.0.1 common.bdat.
2. Never rename NS1 cheat files without verifying every `main + offset` pointer against my 3.0.1 `exefs/main` NSO. Omit any cheat whose pointer cannot be verified.
3. Discard all NS1 60 FPS / RRS / dynamic-resolution patches.
4. Output a normalized mod folder (`mod.json` with `"game": "3.0.1"`, plus `romfs/` / `cheats/8BEF68ED1EEBC24B.txt` / `exefs/`) outside the git repository.
5. Report which entries were verified and ported, which were skipped, and how I can verify them in-game.
```

---

## 3. Using the Launcher Mods Page

- **Wide Intake (Drag & Drop or Buttons):**
  - Drop or pick a **folder**, **`.zip` / `.7z` archive**, **Switch-Cheats-DB `.json`**, or a single loose file (`.txt`, `.ips`, `.pchtxt`, `.bdat`, `.wismt`).
  - The importer automatically strips wrapper folders, flattens Atmosphère paths (`atmosphere/contents/0100E95004038000/{romfs,exefs,cheats}` and `exefs_patches/`), auto-places unique `.bdat`/`.wismt` files via your local `bf2.arh`, preserves any existing `mod.json` (including author `options`), and blocks empty packages or NS1 `common.bdat`.
- **MO2-Style Traffic Light Indicators:**
  - **Green (`Active`):** Confirmed loaded in your latest `nxrt.log` / `play-*.log` with no warnings (shows replaced file / archive override counts).
  - **Yellow (`Active with warnings` / `Not verified yet` / `Rebuild needed`):** Loaded with unmatched paths or lower-priority conflicts, newly installed and not yet run in-game, or waiting for **Setup** to rebuild ExeFS code patches.
  - **Red (`Inactive`):** Empty folder, incompatible `common.bdat`, or unmatched Build ID with no usable 3.0.1 content.
  - **Gray (`Disabled`):** Mod is unchecked.
- **Category Badges:** Automatically summarizes touched top-level areas (`Data / BDAT`, `UI / Menu`, `Models / Textures`, `Maps / Shaders`, `Audio / Video`, `Archive`, `Code patch`, `Cheats`, `Other assets`) with file counts.
- **Troubleshooting Tools:**
  - **Load log:** Filters the latest `nxrt.log` / `play-*.log` for `[mods]` and `[cheats]` lines.
  - **Launch without mods (red button):** Temporarily skips all RomFS mods and runtime cheats via `NXRT_FS_MODS` without modifying `nxrt.ini`. *(Note: ExeFS code patches already compiled into `xc2_boot.exe` still require deleting/disabling the mod and running **Setup**.)*
  - **Clean Delete:** Removes the mod folder and cleans up `nxrt.ini`, `.mod_choices.json`, `.cheats_enabled.tsv`, and `.mod_layers.tsv` (and reminds you to run **Setup** if code patches were removed).

---

## 4. Standard Mod Structure & Technical Reference

### Standard Folder Layout

```text
mods/
  <ModName>/
    mod.json                    # Optional metadata & declarative options
    romfs/                      # Logical RomFS & bf2.ard overrides
    exefs/                      # .ips / .pchtxt code patches (triggers Setup rebuild)
    cheats/
      8BEF68ED1EEBC24B.txt      # Atmosphère DMNT cheats (memory = instant; .text = Setup rebuild)
```

Emulator layouts (`atmosphere/contents/0100E95004038000/...`, `contents/0100E95004038000/...`, `0100E95004038000/...`, `atmosphere/exefs_patches/...`) are also accepted directly if copied manually into `mods/`.

### Optional `mod.json` Schema

```json
{
  "name": "Better UI",
  "version": "1.0",
  "game": "3.0.1",
  "author": "AuthorName",
  "description": "Short description shown in the mod drawer.",
  "options": {
    "style": {
      "label": "HUD Style",
      "type": "select",
      "default": "clean",
      "values": {
        "clean": { "label": "Clean", "path": "opts/clean" },
        "classic": { "label": "Classic", "path": "opts/classic" }
      }
    }
  }
}
```

### Runtime & Architecture Notes (M0–M4)

- **Config keys (`nxrt.ini`):** `[fs] mods` (`NXRT_FS_MODS`), `[mods] order` (highest priority first), `[mods] disabled`, `[mods] pending_rebuild` (holds off unbuilt code mods until Setup completes).
- **RomFS & `bf2.ard` overlay (`fs_romfs.cpp`):** Built into the in-memory RomFS table at boot; loose `romfs/` files matching a `bf2.arh` entry automatically override `bf2.ard` via virtual tail slices (`flags=0` raw or `flags=2` stored-zlib `xbc1`) without copying the 15 GB archive.
- **No separate `ResourceSizeTable`:** Verified against the 3.0.1 catalog — archive sizes live in each 24-byte `bf2.arh` entry (`comp`/`unc`), loose sizes come from the synthesized RomFS tables.
- **Local CLI tools (`tools/mods/`):** `asset_index.py` (local catalog to git-ignored `work/mods/`), `mod_check.py`, `ard_pack.py`, `exefs_patch.py`. See [`tools/mods/README.md`](../tools/mods/README.md).
- **Not supported on purpose:** Arbitrary runtime DLL/engine hooks, mid-session mod swapping, or automatic binary conversion of NS1 `common.bdat` / code offsets.

---

<a id="中文"></a>

# 模组与金手指指南（人类 & AI Agent 通用）

[English](#english) · **中文**

> [!IMPORTANT]
> **合规与零游戏数据声明：** 本仓库与发行包**不包含**任何游戏代码、美术资源、现成金手指包或导出的资产索引表。所有工具（`tools/mods/*.py`、启动器 `bf2.arh` 路径反查）均只在本机读取**你自己的正版 3.0.1 副本**；生成的本地清单（`work/mods/`）已被 `.gitignore` 忽略，永不提交或分发。

---

## 1. NS2 3.0.1 目标硬性规格（先看这一张表）

| 项目 | 规格 / 规则 |
|---|---|
| **目标版本** | 《异度之刃 2 – Nintendo Switch 2 Edition》**零售版 3.0.1** |
| **Application ID** | `0100E95004038000`（与 NS1 共用基础 Title ID，**不能**仅凭此目录名判为 NS1） |
| **`main` Build ID（前 16 位）** | `8BEF68ED1EEBC24B`（完整 40 位：`8BEF68ED1EEBC24BF7278040E63B83F6E417E2E2`） |
| **`common.bdat` 表数量** | **926 张表**（NS1 2.0.2/2.1.0 为 **844 张表**，直接放入会启动崩溃，启动器会自动拦截） |
| **归档覆写（`bf2.ard`）** | 放在 `mods/<模组名>/romfs/<逻辑路径>` 的松散文件会在运行期**自动覆盖** `bf2.ard` 内的同名原版资源，无需手工重打包 15 GB 归档 |
| **禁止迁移的旧补丁** | **不要装旧 NS1 60 FPS / RRS 锁分辨率补丁。** 启动器「设置」已原生支持 30–240 FPS 与分辨率缩放，强改 dynres 记录会导致黑屏 |
| **加载优先级** | **列表越靠上优先级越高（`#1 最高`）。** 同路径文件由高优先级整文件覆盖低优先级 |

---

## 2. 将 NS1 旧模组 / 金手指自行迁移到 3.0.1（人类 + AI Agent 实操）

社区现存模组和金手指大多面向 Switch 1（2.0.2 / 2.1.0）。动手前先对照下表判断：

| 类型 | 3.0.1 能否直接用？ | 正确做法 |
|---|---|---|
| **贴图 / UI / 音频 / 部分模型**（`.wilay`、`.wismt`、`.wimdo`、`.bnk` 等） | **很多可以直接用** | 直接拖进启动器「模组」页。启动器会对照你本机的 `bf2.arh` / `romfs-301` 检查路径，未命中会给出黄牌提示。 |
| **NS1 `common.bdat` 数值模组**（844 表） | **不能（必崩）** | 切勿直接放入。提取旧模组改了哪张表/哪个字段，再基于**你自己的 3.0.1 `common.bdat`（926 表）**重新修改。 |
| **NS1 内存型金手指**（DMNT `.txt` / Switch-Cheats-DB `.json`） | **不能（Build ID 与基址不同）** | **不要**只把文件名改成 `8BEF68ED1EEBC24B.txt`。必须对照**你自己的 3.0.1 `main` NSO** 重新定位 `main + offset` 全局指针（见下方步骤）。 |
| **NS1 代码型金手指 / `.ips` / `.pchtxt`** | **不能（ARM64 指令偏移变化）** | 需对照 3.0.1 `main` 重新适配指令地址，并在启用后重跑一次「构建（Set up）」。 |
| **NS1 60 FPS / RRS 分辨率补丁** | **直接丢弃** | 直接在启动器「设置」里调帧率、分辨率缩放、FSR/DLSS 与帧生成。 |

### 分步流程：把旧版 NS1 内存金手指迁移到 3.0.1（基于你自己的 Dump）

1. **读懂原金手指指令**（Atmosphère DMNT 格式）：
   - `5` 型（`580F0000 <偏移>`）：从 `main + <偏移>` 读取 64 位全局指针到寄存器 `R15`。
   - `7` 型（`780F0000 <字段偏移>`）：给 `R15` 加上结构体内部偏移。
   - `6` 型（`640F0000 00000000 <数值>`）：向 `[R15]` 写入 1/2/4/8 字节数值。
   - `0` 型（`04000000 <代码偏移> <指令>`）：直接改写 `main .text`（属于**代码型金手指**，需重跑 Set up，请与内存型分开处理）。
2. **在自己的 3.0.1 `main` NSO 里定位对应全局指针**：
   - 解压你本地 `exefs/main` 的 `.text` 段（确认 NSO 头 Build ID 为 `8BEF68ED1EEBC24B`）。
   - 搜索目标子系统的特征常量（例如上限钳制 `999,999,999` / `9,999,999`、结构体步长或字符串引用），向上回溯到 `ADRP + ADD/LDR` 指令即可拿到 3.0.1 的 `.bss`/`.data` 全局指针偏移。
   - 核对反汇编里的结构体字段偏移（由于 NS1 存档可直接加载进 NS2 3.0.1，存档结构体内部偏移在换完基址后通常保持一致）。
3. **输出标准 3.0.1 模组目录**：
   ```text
   <模组名>/
     mod.json                      # 注明 "game": "3.0.1"
     cheats/
       8BEF68ED1EEBC24B.txt        # 仅保留已验证基址的条目；剔除未验证项与 60fps/RRS
   ```
4. **启动器 + 实机验证**：
   - 把目录拖进启动器「模组」页 → 确认 **0 告警** → 展开勾选 1 条最易验证的条目（如金币/HP）→ 点「保存」→ 启动游戏实测生效 → 退出后在模组页点「刷新」，卡片左侧自动亮起 **🟢 绿灯（已生效）**。

### 直接给 AI Agent（Gemini / Claude / Codex）复制使用的迁移提示词

如果你打算让 AI 帮你把手里旧的 NS1 模组或金手指迁移到 3.0.1，直接把下面这段话发给它：

```text
请先阅读本仓库的 docs/MODS.md，并严格遵守其中的 3.0.1 规范。
任务：把我本地的 NS1 模组/金手指（路径：<旧模组路径>）迁移为 XC2 NS2 3.0.1 可用版本（Build ID: 8BEF68ED1EEBC24B），
且仅允许读取我本机合法的 3.0.1 Dump（路径：<我的 3.0.1 Dump 路径>）。

硬性要求：
1. 绝不直接复制 NS1 的 common.bdat（844 表）到 3.0.1（926 表）；如涉及 BDAT，必须基于我本地的 3.0.1 common.bdat 重新应用改动。
2. 绝不通过单纯改文件名来伪造 3.0.1 金手指；必须对照我本地 3.0.1 exefs/main 验证每一个 main+offset 基址，未验证的条目直接跳过。
3. 剔除所有 NS1 60 FPS / RRS / 动态分辨率补丁。
4. 在 git 仓库外部输出标准目录（含 "game": "3.0.1" 的 mod.json，以及 romfs/、cheats/8BEF68ED1EEBC24B.txt 或 exefs/）。
5. 汇报已验证并迁移的条目、跳过的条目，以及我进游戏后的验证方法。
```

---

## 3. 启动器「模组」页使用说明

- **宽进导入（拖拽或按钮）**：
  - 支持直接拖入或选择**文件夹**、**`.zip` / `.7z` 压缩包**、**Switch-Cheats-DB `.json`**，以及单个散文件（`.txt` / `.ips` / `.pchtxt` / `.bdat` / `.wismt`）。
  - 自动剥离多余套娃目录、展平 Atmosphère 路径、通过本机 `bf2.arh` 自动归位唯一命中的 `.bdat`/`.wismt`、保留原作者 `mod.json`（含 `options`），并自动拦截空包和 NS1 `common.bdat`。
- **MO2 风格红绿灯状态**：
  - **🟢 绿灯（已生效）**：上次启动日志（`nxrt.log` / `play-*.log`）已确认加载且无异常告警（附替换文件数 / `bf2.ard` 归档覆写数）。
  - **🟡 黄灯（部分生效 / 待启动验证 / 待重建）**：已加载但存在未命中路径或被高优先级覆盖；或刚装好尚未启动过游戏；或含 ExeFS 代码补丁等待在「构建」页重跑 Set up。
  - **🔴 红灯（未生效）**：空模组、`common.bdat` 不兼容，或纯 NS1 Build ID 不匹配。
  - **⚪ 灰灯**：已禁用。
- **改动大项自动归类**：卡片直接标出改了哪些大项（`数据/BDAT`、`界面/UI`、`角色/模型`、`场景/地图`、`音频/过场`、`归档覆写`、`代码补丁`、`金手指`、`其他资源`）与文件数。
- **排障工具**：
  - **加载日志**：一键过滤上次 `nxrt.log` / `play-*.log` 的 `[mods]` / `[cheats]` 行。
  - **无模组安全启动（红色按钮）**：通过临时环境变量跳过全部 RomFS 模组与内存金手指，**不修改 `nxrt.ini`**。（*注意：已编译进 `xc2_boot.exe` 的 ExeFS 代码补丁仍需卸载后重跑 Set up 还原。*）
  - **干净删除**：删除模组目录并同步清理 `nxrt.ini`、`.mod_choices.json`、`.cheats_enabled.tsv`、`.mod_layers.tsv`。

---

## 4. 标准目录结构与底层机制速查

- **标准目录**：`mods/<模组名>/{mod.json, romfs/, exefs/, cheats/8BEF68ED1EEBC24B.txt}`；手工直接拷进 `mods/` 的 Atmosphère 嵌套目录（`atmosphere/contents/0100E95004038000/...`）同样兼容。
- **配置键（`nxrt.ini`）**：`[fs] mods`（`NXRT_FS_MODS`）、`[mods] order`（靠前优先）、`[mods] disabled`、`[mods] pending_rebuild`（未重编代码补丁前暂缓生效）。
- **无独立 `ResourceSizeTable`**：3.0.1 归档大小直接存于 `bf2.arh` 每条 24 字节记录中，松散文件大小由运行时合成 RomFS 表自动计算。
- **本地 CLI 工具（`tools/mods/`）**：`asset_index.py`（输出仅存本机忽略目录 `work/mods/`）、`mod_check.py`、`ard_pack.py`、`exefs_patch.py`，详见 [`tools/mods/README.md`](../tools/mods/README.md)。
