// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nxrt::cheats {

enum class RegionMode : uint8_t {
    LiveMemory = 0, // Resolve Main/Heap/Alias against guest bases + VirtualQuery/mem::is_mapped
};

struct RuntimeConfig {
    uint64_t main_base = 0;      // 0 -> nxrt::mem::kMainBase
    uint64_t heap_base = 0;      // 0 -> nxrt::mem::kHeapBase
    uint64_t alias_base = 0;     // 0 -> nxrt::mem::alias_base()
    uint64_t main_text_end = 0;  // offsets < main_text_end in Type 0 Main are handled statically by M3 (skipped at runtime)
};

struct CheatProgram {
    std::string mod_name;
    std::string title;
    std::vector<uint32_t> words;
};

// Parses active cheat sections matching `enabled_titles` from one Atmosphere `<bid>.txt` file.
// Pure static Type-0 writes targeting Main `.text` (`< main_text_end`) are excluded (handled by M3 NSO patching).
std::vector<CheatProgram> parse_txt_filtered(
    const std::string& mod_name,
    const std::string& text,
    uint64_t main_text_end,
    const std::vector<std::string>& enabled_titles);

// Loads enabled cheats from `mods_dir/.cheats_enabled.tsv` matching `main_build_id` (64 hex chars lowercase, or empty to match any hex filename),
// respecting `[mods] order / disabled / pending_rebuild` just like fs_romfs.
void load_from_mods_dir(
    const std::filesystem::path& mods_dir,
    const std::string& main_build_id,
    const RuntimeConfig& cfg,
    const std::vector<std::string>& disabled_mods,
    const std::vector<std::string>& pending_rebuild_mods);

// Reads `[fs] mods`, `[fs] exefs`, `[mods] disabled`, `[mods] pending_rebuild` from nxrt config and loads cheats once.
void init_from_config(uint64_t main_base = 0);

// Records the mapped `main` NSO build_id and `.text` end offset directly from nso.cpp (used when `--exefs` is passed on CLI without `[fs] exefs`).
void register_main_nso(const std::string& build_id, uint64_t text_end);

// Executes all loaded cheat programs once using `held_buttons` (nn::hid::NpadButton bits) and active region bases.
void tick_with_buttons(uint64_t held_buttons);

// Convenience frame tick: polls nxrt::hid::input_source() for held NpadButton bits and runs all loaded cheats.
void tick();

// Executes a single decoded word stream directly (used by unit tests).
void exec_program(const CheatProgram& prog, const RuntimeConfig& cfg, uint64_t held_buttons);

size_t loaded_count();
void clear();

}  // namespace nxrt::cheats
