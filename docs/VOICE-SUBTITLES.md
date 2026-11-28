<a id="english"></a>

# Voice Subtitle Pack Authoring Guide

**English** · [中文](#中文)

> [!IMPORTANT]
> **`xc2-ns2-recomp` only provides the open-source voice overlay renderer and a generic JSON loader.**
> This repository does **not** ship, host, or link to any game dialogue transcripts, voice audio, or subtitle packs. The subtitle interface is intentionally open so anyone in the community can create, translate, customize, and share their own pure-JSON packs (in any language, accessibility style, or custom format) independently.

---

## 1. How the Open Interface Works

When `[xc2] voice_subtitles = true`, the runtime listens for in-game Wwise voice trigger events (`voice_CCKKNNVV_Play`) and looks up matching text entries in an external JSON file located next to `xc2_boot.exe`:

```text
<install_dir>/
  xc2_boot.exe
  voice_subtitles/
    en.json
    ja.json
    zh_hans.json
    zh_hant.json
    <any_custom_lang>.json
```

- **Pure text JSON only**: Subtitle packs never contain audio, textures, or binaries.
- **Automatic runtime portraits**: Character portraits are read on the fly from the player's own local game archives by the engine — pack authors do not need to bundle any art assets.
- **Audio track agnostic**: Japanese and English voice tracks emit the same Wwise event names, so one pack works regardless of the player's voice audio language.

---

## 2. File Naming & Launcher Import

| Language / Code | Recommended Filename | `nxrt.ini` (`[xc2] voice_lang`) |
|---|---|---|
| English | `en.json` | `en` |
| Japanese | `ja.json` | `ja` |
| Simplified Chinese | `zh_hans.json` | `zh_hans` |
| Traditional Chinese | `zh_hant.json` | `zh_hant` |
| Any other language (e.g. Korean, French, German, Spanish, Italian, custom) | `<code >.json` (e.g. `ko.json`, `fr.json`) | `<code >` |

Players can install a pack in two ways:
1. **Via XC2 Launcher (v0.3.0+)**: **Play → In-Game Features → Voice Subtitles → Import Subtitle Pack**, selecting either a `.json` file or a `.zip` containing one or more `.json` files (at archive root or inside a `voice_subtitles/` folder). The launcher validates that each JSON has a non-empty `cues` object before copying it into `voice_subtitles/`.
2. **Manual**: Copy `<lang>.json` directly into `<install_dir>/voice_subtitles/`.

---

## 3. JSON Schema Specification

Each `.json` file must be valid UTF-8 (BOM optional) with a top-level object containing `speakers` (optional — falls back to built-in speaker metadata) and `cues` (required):

```json
{
  "speakers": {
    "1": {
      "name": "Example Driver",
      "role": "driver",
      "partner_id": 11,
      "element": "fire",
      "color_hex": "#3B82F6"
    },
    "11": {
      "name": "Example Blade",
      "role": "blade",
      "partner_id": 1,
      "element": "fire",
      "color_hex": "#EF4444"
    }
  },
  "cues": {
    "voice_01050101_Play": {
      "speaker_id": 1,
      "priority": 1,
      "tag": "Break",
      "duration_sec": 2.4,
      "text_full": "Example tactical battle callout.",
      "text_compact": "Example short summary."
    },
    "voice_11440101_Play": {
      "speaker_id": 11,
      "priority": 0,
      "tag": "Banter",
      "duration_sec": 3.0,
      "text_full": "Example post-battle or exploration line."
    }
  }
}
```

### `speakers.<voice_id>` Fields

| Field | Type | Required | Description |
|---|---|---|---|
| `name` | string | Yes | Display name shown on the card / banter bar (localized for your pack's language). |
| `role` | string | No | `"driver"` or `"blade"`. |
| `partner_id` | integer | No | Partner's numeric `VoiceID` (`0` if none). |
| `element` | string | No | One of `fire`, `water`, `wind`, `earth`, `elec` (or `electric`), `ice`, `light`, `dark`, or empty. |
| `color_hex` | string | No | Accent color `#RRGGBB` or `#RRGGBBAA` (defaults to `#38BDF8`). |

### `cues.<event_name>` Fields

Keys are Wwise event names (e.g. `voice_01050101_Play`) or decimal cue ID strings.

| Field | Type | Required | Description |
|---|---|---|---|
| `speaker_id` | integer | Yes | Numeric `VoiceID` matching `speakers` (e.g. `1`–`28` main cast, `31`–`59` Rare Blades, `61`–`80` Common Blades, `501`–`506` DLC Blades, `701`–`709` Torna cast). |
| `priority` | integer | No | `0` = Story / post-battle banter & exploration (bottom subtitle bar); `1` = High-priority tactical combat (left cards); `2` = Ambient combat chatter (left cards, shown when `voice_density = full`); `3` = Suppressed non-verbal grunts. Default `2`. |
| `tag` | string | No | Optional tactical badge: `Break`, `Topple`, `Launch`, `Smash`, `SpecialReady`, `AggroWarning`, `LowHPHelp`, `HealPotion`, `Banter`, or `""`. |
| `duration_sec` | number | No | Optional display duration hint in seconds (`0` = auto-calculated from text length). |
| `text_full` | string | Yes | Spoken line displayed on the subtitle bar or combat card. |
| `text_compact` | string | No | Optional short tactical summary (used as fallback if `text_full` is empty). |

---

## 4. Discovering Event Keys & Testing Your Pack

- **Live in-game inspection**: Set `[xc2] voice_debug = true` in `nxrt.ini` to log triggered Wwise event names in real time while playing, so you can map or refine specific lines directly.
- **Open-source local pipeline scripts**: Developers working from their own legally dumped game files can also inspect the reference helper scripts in [`tools/voice/`](../tools/voice/) for manifest indexing and formatting.
- **Packaging for distribution**: Zip one or more `<lang>.json` files together with a `README.txt` (keep the `.json` files at the zip root or under `voice_subtitles/`). Any community mod platform (GameBanana, GBAtemp, personal repos, etc.) works cleanly with the launcher's local importer.

---

<a id="中文"></a>

# 语音字幕包制作与开放接口指南

[English](#english) · **中文**

> [!IMPORTANT]
> **`xc2-ns2-recomp` 仅提供开源的语音可视化渲染框架与通用 JSON 加载器。**
> 本仓库**不包含、不托管、也不直链**任何游戏台词文本、语音音频或字幕包。字幕接口完全开放，欢迎任何社区作者或玩家基于此开放格式，自由制作、翻译、定制并分享任意语言或风格的纯 JSON 字幕包。

---

## 1. 开放接口原理

当 `[xc2] voice_subtitles = true` 时，运行时会监听游戏内触发的 Wwise 语音事件名（如 `voice_CCKKNNVV_Play`），并从 `xc2_boot.exe` 同级的外部 `voice_subtitles/` 目录读取对应语言的纯 JSON 文件：

```text
<安装目录>/
  xc2_boot.exe
  voice_subtitles/
    zh_hans.json
    zh_hant.json
    en.json
    ja.json
    <任意自定义语言码>.json
```

- **纯文本 JSON，零版权素材**：字幕包无需也不应包含任何音频、贴图或二进制文件。
- **头像由运行时自动读取**：角色头像由重编译引擎在运行时直接读取玩家本地游戏归档，字幕包作者只需关注文本。
- **日/英配音通用**：日语与英语音轨触发的 Wwise 事件名完全一致，同一份字幕包在两种配音下均可正常匹配。

---

## 2. 文件命名与启动器导入

| 语言 | 推荐文件名 | `nxrt.ini`（`[xc2] voice_lang`） |
|---|---|---|
| 简体中文 | `zh_hans.json` | `zh_hans` |
| 繁體中文 | `zh_hant.json` | `zh_hant` |
| English | `en.json` | `en` |
| 日本語 | `ja.json` | `ja` |
| 其他语言（韩、法、德、西、意或自定义梗包等） | `<语言码>.json`（如 `ko.json`、`fr.json`） | `<语言码>` |

玩家有两种安装方式：
1. **启动器一键导入（v0.3.0+）**：在 **游玩页 → 游戏内增强 → 语音字幕 → 导入字幕包** 中直接选择 `.json` 或 `.zip` 压缩包（压缩包根目录或 `voice_subtitles/` 子目录均可），启动器会自动校验非空 `cues` 字段并解压导入。
2. **手动放入**：将 `<lang>.json` 直接放到 `<安装目录>/voice_subtitles/` 中即可。

---

## 3. JSON 格式规范

文件必须为合法的 UTF-8 JSON（允许带 BOM），顶层包含 `speakers`（可选，缺省时回退内置角色元数据）与 `cues`（必填）：

```json
{
  "speakers": {
    "1": {
      "name": "示例御刃者",
      "role": "driver",
      "partner_id": 11,
      "element": "fire",
      "color_hex": "#3B82F6"
    },
    "11": {
      "name": "示例异刃",
      "role": "blade",
      "partner_id": 1,
      "element": "fire",
      "color_hex": "#EF4444"
    }
  },
  "cues": {
    "voice_01050101_Play": {
      "speaker_id": 1,
      "priority": 1,
      "tag": "Break",
      "duration_sec": 2.4,
      "text_full": "示例战斗战术语音台词。",
      "text_compact": "示例精简战术提示。"
    },
    "voice_11440101_Play": {
      "speaker_id": 11,
      "priority": 0,
      "tag": "Banter",
      "duration_sec": 3.0,
      "text_full": "示例战后对话或原野探索台词。"
    }
  }
}
```

### `speakers.<voice_id>` 字段说明

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | string | 是 | 显示在语音卡片 / 字幕条上的角色名（按字幕包语言本地化）。 |
| `role` | string | 否 | `"driver"`（御刃者）或 `"blade"`（异刃）。 |
| `partner_id` | integer | 否 | 搭档的数字 `VoiceID`（无搭档填 `0`）。 |
| `element` | string | 否 | 属性：`fire`、`water`、`wind`、`earth`、`elec`（或 `electric`）、`ice`、`light`、`dark` 或留空。 |
| `color_hex` | string | 否 | 主题强调色 `#RRGGBB` 或 `#RRGGBBAA`（默认 `#38BDF8`）。 |

### `cues.<event_name>` 字段说明

键名为 Wwise 事件名（如 `voice_01050101_Play`）或十进制 cue ID 字符串。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `speaker_id` | integer | 是 | 对应 `speakers` 的数字 `VoiceID`（`1`–`28` 主线角色、`31`–`59` 稀有异刃、`61`–`80` 普通异刃、`501`–`506` DLC 异刃、`701`–`709` 黄金国主角）。 |
| `priority` | integer | 否 | `0` = 战后相声与探索语音（底部字幕带）；`1` = 高优先级战术语音（左侧卡片）；`2` = 战斗氛围喊话（左侧卡片，`voice_density = full` 时显示）；`3` = 忽略的纯拟声吼叫。默认 `2`。 |
| `tag` | string | 否 | 可选战术徽章：`Break`、`Topple`、`Launch`、`Smash`、`SpecialReady`、`AggroWarning`、`LowHPHelp`、`HealPotion`、`Banter` 或留空。 |
| `duration_sec` | number | 否 | 可选停留时长提示（秒）；填 `0` 则按字数自动计算。 |
| `text_full` | string | 是 | 实际显示在字幕带或左侧卡片上的台词文本。 |
| `text_compact` | string | 否 | 可选精简摘要（仅在 `text_full` 为空时兜底使用）。 |

---

## 4. 如何查询事件名、调试与打包分享

- **游戏内实时查看事件名**：在 `nxrt.ini` 中开启 `[xc2] voice_debug = true`，游玩时日志会输出实时触发的 Wwise 事件名，方便逐条核对或润色翻译。
- **开源辅助脚本参考**：基于自己合法提取的本地游戏文件制作词表时，可参考 [`tools/voice/`](../tools/voice/) 下的开源索引与格式化脚本。
- **打包分发**：将一个或多个 `<lang>.json`（放在压缩包根目录或 `voice_subtitles/` 下）与自述 `README.txt` 打成 `.zip` 即可分享给其他玩家，通过启动器「导入字幕包」一键安装。
