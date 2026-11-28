// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/cheats.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::cheats {
namespace {

constexpr char kTag[] = "cheats";
constexpr char kAppIdDefault[] = "0100e95004038000";
constexpr uint32_t kMaxLoopIters = 10000;

struct State {
    std::mutex mu;
    RuntimeConfig cfg{};
    std::vector<CheatProgram> programs;
    std::string nso_bid;
    uint64_t nso_text_end = 0;
    uint64_t pending_main_base = 0;
    bool pending_config_load = false;
    bool initialized = false;
    bool warned_unsafe = false;
};

State& state() {
    static State s;
    return s;
}

std::string to_lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool is_hex(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::vector<std::string> split_csv(const std::string& raw) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < raw.size()) {
        const size_t comma = raw.find(',', i);
        const size_t end = comma == std::string::npos ? raw.size() : comma;
        std::string item = trim(raw.substr(i, end - i));
        if (!item.empty()) out.push_back(std::move(item));
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return out;
}

bool list_has(const std::vector<std::string>& v, const std::string& target) {
    return std::find(v.begin(), v.end(), target) != v.end();
}

uint64_t effective_main_base(const RuntimeConfig& cfg) {
    return cfg.main_base ? cfg.main_base : nxrt::mem::kMainBase;
}
uint64_t effective_heap_base(const RuntimeConfig& cfg) {
    return cfg.heap_base ? cfg.heap_base : nxrt::mem::kHeapBase;
}
uint64_t effective_alias_base(const RuntimeConfig& cfg) {
    return cfg.alias_base ? cfg.alias_base : nxrt::mem::alias_base();
}

uint64_t region_base(const RuntimeConfig& cfg, uint8_t region) {
    switch (region) {
        case 0: return effective_main_base(cfg);
        case 1: return effective_heap_base(cfg);
        case 2: return effective_alias_base(cfg);
        case 3: return effective_main_base(cfg);
        default: return 0;
    }
}

bool valid_width(uint32_t w) {
    return w == 1 || w == 2 || w == 4 || w == 8;
}

uint64_t width_mask(uint32_t w) {
    switch (w) {
        case 1: return 0xFFull;
        case 2: return 0xFFFFull;
        case 4: return 0xFFFFFFFFull;
        case 8: return 0xFFFFFFFFFFFFFFFFull;
        default: return 0;
    }
}

bool page_prot_allows_write(DWORD protect) {
    const DWORD base = protect & 0xFF;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool page_prot_allows_read(DWORD protect) {
    const DWORD base = protect & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool safe_read(uint64_t addr, uint32_t width, uint64_t& out) {
    out = 0;
    if (!valid_width(width) || addr < 0x10000) return false;
    if (addr > UINT64_MAX - width) return false;
    if (!nxrt::mem::is_mapped(addr, width)) {
        // VirtualQuery fallback for standalone tests using regular heap/stack/VirtualAlloc buffers outside nxrt::mem sections.
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof mbi) != sizeof mbi) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !page_prot_allows_read(mbi.Protect) || addr + width > end) {
            return false;
        }
    }
    uint64_t tmp = 0;
    std::memcpy(&tmp, reinterpret_cast<const void*>(addr), width);
    out = tmp & width_mask(width);
    return true;
}

bool safe_write(uint64_t addr, uint32_t width, uint64_t val, bool& warned) {
    if (!valid_width(width) || addr < 0x10000 || addr > UINT64_MAX - width) return false;
    // Commit lazy heap pages first if inside nxrt heap view, then verify VirtualQuery write protection.
    (void)nxrt::mem::is_mapped(addr, width);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof mbi) != sizeof mbi) {
        if (!warned) { warned = true; NXLOG_W(kTag, "cheat write skipped unmapped address 0x%llx", static_cast<unsigned long long>(addr)); }
        return false;
    }
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || !page_prot_allows_write(mbi.Protect) || addr + width > end) {
        if (!warned) { warned = true; NXLOG_W(kTag, "cheat write skipped read-only/oob address 0x%llx", static_cast<unsigned long long>(addr)); }
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(addr), &val, width);
    return true;
}

size_t instr_words(const std::vector<uint32_t>& words, size_t pc) {
    const uint32_t w0 = words[pc];
    const uint32_t op = (w0 >> 28) & 0xF;
    switch (op) {
        case 0: return ((w0 >> 24) & 0xF) == 8 ? 4 : 3;
        case 1: return ((w0 >> 24) & 0xF) == 8 ? 4 : 3;
        case 2:
        case 8: return 1;
        case 3: return ((w0 >> 24) & 0xF) == 0 ? 2 : 1;
        case 4:
        case 6: return 3;
        case 5:
        case 7: return 2;
        case 9: {
            const uint32_t has_imm = (w0 >> 8) & 0xF;
            const uint32_t width = (w0 >> 24) & 0xF;
            return has_imm ? (width == 8 ? 3 : 2) : 1;
        }
        case 0xA: {
            const uint32_t off_type = (w0 >> 12) & 0xF;
            return (off_type == 2 || off_type == 4 || off_type == 5) ? 3 : 2;
        }
        default: return 1;
    }
}

bool is_pure_static_text_code(const std::vector<uint32_t>& words, uint64_t main_text_end) {
    if (words.empty() || main_text_end == 0) return false;
    size_t pc = 0;
    bool has_text_write = false;
    while (pc < words.size()) {
        const uint32_t w0 = words[pc];
        const uint32_t op = (w0 >> 28) & 0xF;
        if (op != 0) return false;
        const uint32_t width = (w0 >> 24) & 0xF;
        const uint32_t region = (w0 >> 20) & 0xF;
        const size_t len = (width == 8) ? 4 : 3;
        if (!valid_width(width) || pc + len > words.size()) return false;
        const uint64_t off = (static_cast<uint64_t>(w0 & 0xFF) << 32) | words[pc + 1];
        if (region != 0 || off >= main_text_end) return false;
        has_text_write = true;
        pc += len;
    }
    return has_text_write;
}

bool eval_cond(uint32_t cond, uint64_t mem_val, uint64_t cmp_val) {
    switch (cond) {
        case 1: return mem_val > cmp_val;
        case 2: return mem_val >= cmp_val;
        case 3: return mem_val < cmp_val;
        case 4: return mem_val <= cmp_val;
        case 5: return mem_val == cmp_val;
        case 6: return mem_val != cmp_val;
        default: return false;
    }
}

uint64_t apply_arith(uint32_t op, uint32_t width, uint64_t lhs, uint64_t rhs) {
    const uint64_t mask = width_mask(width);
    lhs &= mask;
    rhs &= mask;
    uint64_t out = lhs;
    switch (op) {
        case 0: out = lhs + rhs; break;
        case 1: out = lhs - rhs; break;
        case 2: out = lhs * rhs; break;
        case 3: out = lhs << (rhs & 63); break;
        case 4: out = lhs >> (rhs & 63); break;
        case 5: out = lhs & rhs; break;
        case 6: out = lhs | rhs; break;
        case 7: out = (~lhs); break;
        case 8: out = lhs ^ rhs; break;
        case 9: out = lhs; break;
        default: break;
    }
    return out & mask;
}

std::pair<std::string, uint64_t> read_main_exefs_info(const std::filesystem::path& exefs_dir) {
    std::error_code ec;
    const std::filesystem::path main_p = exefs_dir / "main";
    if (!std::filesystem::is_regular_file(main_p, ec)) return {"", 0};
    std::ifstream in(main_p, std::ios::binary);
    if (!in) return {"", 0};
    std::array<uint8_t, 0x60> h{};
    in.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()));
    if (in.gcount() != static_cast<std::streamsize>(h.size())) return {"", 0};
    if (std::memcmp(h.data(), "NSO0", 4) != 0) return {"", 0};
    uint32_t text_mem_off = 0, text_size = 0;
    std::memcpy(&text_mem_off, h.data() + 0x14, 4);
    std::memcpy(&text_size, h.data() + 0x18, 4);
    char hex[65]{};
    for (size_t i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", h[0x40 + i]);
    }
    return {std::string(hex), static_cast<uint64_t>(text_mem_off) + text_size};
}

bool cheat_bid_matches(const std::string& file_stem, const std::string& main_bid) {
    const std::string stem = to_lower(trim(file_stem));
    if (stem.size() < 16 || !is_hex(stem)) return false;
    if (main_bid.empty()) return true;
    const std::string bid = to_lower(trim(main_bid));
    return bid.size() == 64 && bid.rfind(stem, 0) == 0;
}

std::vector<std::filesystem::path> cheat_roots_for_mod(const std::filesystem::path& mod_dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& cand : {
             mod_dir / "cheats",
             mod_dir / "atmosphere" / "contents" / kAppIdDefault / "cheats",
             mod_dir / "contents" / kAppIdDefault / "cheats",
             mod_dir / kAppIdDefault / "cheats",
         }) {
        if (std::filesystem::is_directory(cand, ec)) out.push_back(cand);
    }
    return out;
}

std::map<std::string, std::vector<std::string>> read_cheats_enabled_tsv(const std::filesystem::path& mods_dir) {
    std::map<std::string, std::vector<std::string>> out;
    std::error_code ec;
    const std::filesystem::path p = mods_dir / ".cheats_enabled.tsv";
    if (!std::filesystem::is_regular_file(p, ec)) return out;
    std::ifstream in(p, std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        std::string mod = trim(line.substr(0, tab));
        std::string title = trim(line.substr(tab + 1));
        if (!mod.empty() && !title.empty()) out[mod].push_back(title);
    }
    return out;
}

}  // namespace

std::vector<CheatProgram> parse_txt_filtered(
    const std::string& mod_name,
    const std::string& text,
    uint64_t main_text_end,
    const std::vector<std::string>& enabled_titles) {
    std::vector<CheatProgram> out;
    std::set<std::string> wanted(enabled_titles.begin(), enabled_titles.end());
    std::string cur_title;
    std::vector<uint32_t> cur_words;

    auto flush = [&]() {
        if (!cur_title.empty() && !cur_words.empty() && wanted.count(cur_title)) {
            if (!is_pure_static_text_code(cur_words, main_text_end)) {
                out.push_back(CheatProgram{mod_name, cur_title, cur_words});
            }
        }
        cur_title.clear();
        cur_words.clear();
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = trim(text.substr(pos, end - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
        if (line.front() == '[' && line.back() == ']' && line.size() >= 2) {
            flush();
            cur_title = trim(line.substr(1, line.size() - 2));
            continue;
        }
        if (cur_title.empty()) continue;
        const size_t cpos = std::min(line.find('#'), line.find("//"));
        if (cpos != std::string::npos) line.resize(cpos);
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= line.size()) break;
            size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            const std::string tok = line.substr(i, j - i);
            if (tok.size() == 8 && is_hex(tok)) {
                cur_words.push_back(static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16)));
            }
            i = j;
        }
    }
    flush();
    return out;
}

void exec_program(const CheatProgram& prog, const RuntimeConfig& cfg, uint64_t held_buttons) {
    State& st = state();
    std::array<uint64_t, 16> regs{};
    std::array<uint32_t, 16> loop_rem{};
    std::array<size_t, 16> loop_pc{};
    std::array<bool, 16> loop_active{};

    const auto& words = prog.words;
    const size_t n = words.size();
    size_t pc = 0;
    size_t steps = 0;
    uint32_t skip_depth = 0;

    auto skip_to_matching_end = [&](size_t start_pc) -> size_t {
        size_t p = start_pc;
        uint32_t depth = 1;
        while (p < n) {
            const uint32_t w0 = words[p];
            const uint32_t op = (w0 >> 28) & 0xF;
            const size_t adv = instr_words(words, p);
            if (op == 1 || op == 8) {
                ++depth;
            } else if (op == 2) {
                const uint32_t sub = (w0 >> 24) & 0xF;
                if (sub == 1 && depth == 1) {
                    return p + adv; // Else branch matches outer skipped block
                }
                if (--depth == 0) return p + adv;
            }
            p += adv;
        }
        return n;
    };

    while (pc < n && steps < 65536) {
        ++steps;
        const size_t len = instr_words(words, pc);
        if (pc + len > n) break;
        const uint32_t w0 = words[pc];
        const uint32_t op = (w0 >> 28) & 0xF;

        switch (op) {
            case 0: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t region = (w0 >> 20) & 0xF;
                const uint32_t r = (w0 >> 16) & 0xF;
                const uint64_t off = (static_cast<uint64_t>(w0 & 0xFF) << 32) | words[pc + 1];
                const uint64_t val = (width == 8) ? ((static_cast<uint64_t>(words[pc + 2]) << 32) | words[pc + 3]) : words[pc + 2];
                const uint64_t base = region_base(cfg, static_cast<uint8_t>(region));
                if (base) (void)safe_write(base + off + regs[r], width, val, st.warned_unsafe);
                pc += len;
                break;
            }
            case 1: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t region = (w0 >> 20) & 0xF;
                const uint32_t cond = (w0 >> 16) & 0xF;
                const uint64_t off = (static_cast<uint64_t>(w0 & 0xFF) << 32) | words[pc + 1];
                const uint64_t cmp = (width == 8) ? ((static_cast<uint64_t>(words[pc + 2]) << 32) | words[pc + 3]) : words[pc + 2];
                const uint64_t base = region_base(cfg, static_cast<uint8_t>(region));
                uint64_t mem_v = 0;
                const bool ok = base && safe_read(base + off, width, mem_v) && eval_cond(cond, mem_v, cmp & width_mask(width));
                pc = ok ? (pc + len) : skip_to_matching_end(pc + len);
                break;
            }
            case 2: {
                const uint32_t sub = (w0 >> 24) & 0xF;
                if (sub == 1) {
                    // Entered `Else` at end of a true `If` branch -> skip until matching `EndIf`
                    pc = skip_to_matching_end(pc + len);
                } else {
                    if (skip_depth > 0) --skip_depth;
                    pc += len;
                }
                break;
            }
            case 3: {
                const uint32_t sub = (w0 >> 24) & 0xF;
                const uint32_t r = (w0 >> 16) & 0xF;
                if (sub == 0) {
                    uint32_t count = words[pc + 1];
                    if (count > kMaxLoopIters) count = kMaxLoopIters;
                    if (count == 0) {
                        loop_active[r] = false;
                        // Advance to matching loop-end `310R0000`
                        size_t p = pc + len;
                        while (p < n) {
                            const size_t a = instr_words(words, p);
                            if (((words[p] >> 28) & 0xF) == 3 && ((words[p] >> 24) & 0xF) == 1 && ((words[p] >> 16) & 0xF) == r) {
                                p += a;
                                break;
                            }
                            p += a;
                        }
                        pc = p;
                    } else {
                        loop_rem[r] = count;
                        loop_pc[r] = pc + len;
                        loop_active[r] = true;
                        pc += len;
                    }
                } else {
                    if (loop_active[r] && loop_rem[r] > 1) {
                        --loop_rem[r];
                        pc = loop_pc[r];
                    } else {
                        loop_active[r] = false;
                        pc += len;
                    }
                }
                break;
            }
            case 4: {
                const uint32_t r = (w0 >> 16) & 0xF;
                regs[r] = (static_cast<uint64_t>(words[pc + 1]) << 32) | words[pc + 2];
                pc += len;
                break;
            }
            case 5: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t region = (w0 >> 20) & 0xF;
                const uint32_t r = (w0 >> 16) & 0xF;
                const uint32_t load_reg_rel = (w0 >> 12) & 0xF;
                const uint64_t off = (static_cast<uint64_t>(w0 & 0xFF) << 32) | words[pc + 1];
                const uint64_t addr = load_reg_rel ? (regs[r] + off) : (region_base(cfg, static_cast<uint8_t>(region)) + off);
                uint64_t val = 0;
                if (!safe_read(addr, width, val)) {
                    return; // broken pointer chain -> abort this cheat safely for this frame
                }
                regs[r] = val;
                pc += len;
                break;
            }
            case 6: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t r = (w0 >> 16) & 0xF;
                const uint32_t inc = (w0 >> 12) & 0xF;
                const uint32_t off_en = (w0 >> 8) & 0xF;
                const uint32_t r_off = (w0 >> 4) & 0xF;
                const uint64_t val = (static_cast<uint64_t>(words[pc + 1]) << 32) | words[pc + 2];
                const uint64_t addr = regs[r] + (off_en ? regs[r_off] : 0ull);
                if (valid_width(width)) {
                    (void)safe_write(addr, width, val, st.warned_unsafe);
                    if (inc) regs[r] += width;
                }
                pc += len;
                break;
            }
            case 7: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t r = (w0 >> 16) & 0xF;
                const uint32_t arith_op = (w0 >> 12) & 0xF;
                if (valid_width(width)) regs[r] = apply_arith(arith_op, width, regs[r], words[pc + 1]);
                pc += len;
                break;
            }
            case 8: {
                const uint64_t mask = static_cast<uint64_t>(w0 & 0x0FFFFFFFull);
                const bool ok = (held_buttons & mask) == mask;
                pc = ok ? (pc + len) : skip_to_matching_end(pc + len);
                break;
            }
            case 9: {
                const uint32_t width = (w0 >> 24) & 0xF;
                const uint32_t arith_op = (w0 >> 20) & 0xF;
                const uint32_t r_dst = (w0 >> 16) & 0xF;
                const uint32_t r_lhs = (w0 >> 12) & 0xF;
                const uint32_t use_imm = (w0 >> 8) & 0xF;
                const uint32_t r_rhs = (w0 >> 4) & 0xF;
                uint64_t rhs = regs[r_rhs];
                if (use_imm) {
                    rhs = (width == 8) ? ((static_cast<uint64_t>(words[pc + 1]) << 32) | words[pc + 2]) : words[pc + 1];
                }
                if (valid_width(width)) regs[r_dst] = apply_arith(arith_op, width, regs[r_lhs], rhs);
                pc += len;
                break;
            }
            default:
                pc += len;
                break;
        }
    }
}

void load_from_mods_dir(
    const std::filesystem::path& mods_dir,
    const std::string& main_build_id,
    const RuntimeConfig& cfg,
    const std::vector<std::string>& disabled_mods,
    const std::vector<std::string>& pending_rebuild_mods) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.cfg = cfg;
    st.programs.clear();
    st.initialized = true;

    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) return;
    const auto enabled_map = read_cheats_enabled_tsv(mods_dir);
    if (enabled_map.empty()) return;

    for (const auto& [mod_name, titles] : enabled_map) {
        if (list_has(disabled_mods, mod_name)) continue;
        if (list_has(pending_rebuild_mods, mod_name)) {
            NXLOG_W(kTag, "cheats: %s held off until Set up rebuilds code patches", mod_name.c_str());
            continue;
        }
        const std::filesystem::path mod_dir = mods_dir / mod_name;
        if (!std::filesystem::is_directory(mod_dir, ec)) continue;
        std::set<std::filesystem::path> seen;
        for (const auto& root : cheat_roots_for_mod(mod_dir)) {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (e.is_regular_file(ec) && to_lower(e.path().extension().string()) == ".txt") {
                    files.push_back(e.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& fp : files) {
                const std::string stem = fp.stem().string();
                if (!cheat_bid_matches(stem, main_build_id)) continue;
                const auto canon = std::filesystem::weakly_canonical(fp, ec);
                if (!seen.insert(canon).second) continue;
                std::ifstream in(fp, std::ios::binary);
                if (!in) continue;
                const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto progs = parse_txt_filtered(mod_name, content, cfg.main_text_end, titles);
                st.programs.insert(st.programs.end(), std::make_move_iterator(progs.begin()), std::make_move_iterator(progs.end()));
            }
        }
    }
    if (!st.programs.empty()) {
        NXLOG_I(kTag, "loaded %zu active runtime cheat(s) from %s", st.programs.size(), mods_dir.string().c_str());
    }
}

void register_main_nso(const std::string& build_id, uint64_t text_end) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.nso_bid = to_lower(build_id);
    st.nso_text_end = text_end;
}

static void load_from_config_locked(uint64_t main_base) {
    const std::string mods_str = config_get_str("fs", "mods", "");
    if (mods_str.empty()) return;
    const std::string exefs_str = config_get_str("fs", "exefs", "");
    auto [bid, text_end] = exefs_str.empty() ? std::pair<std::string, uint64_t>{"", 0} : read_main_exefs_info(exefs_str);

    State& st = state();
    if (bid.empty()) bid = st.nso_bid;
    if (text_end == 0) text_end = st.nso_text_end;

    RuntimeConfig cfg;
    cfg.main_base = main_base ? main_base : nxrt::mem::kMainBase;
    cfg.heap_base = nxrt::mem::kHeapBase;
    cfg.alias_base = nxrt::mem::alias_base();
    cfg.main_text_end = text_end;

    const auto disabled = split_csv(config_get_str("mods", "disabled", ""));
    const auto pending = split_csv(config_get_str("mods", "pending_rebuild", ""));
    st.cfg = cfg;
    st.programs.clear();
    st.initialized = true;
    st.pending_config_load = false;

    std::error_code ec;
    const std::filesystem::path mods_dir(mods_str);
    if (!std::filesystem::is_directory(mods_dir, ec)) return;
    const auto enabled_map = read_cheats_enabled_tsv(mods_dir);
    if (enabled_map.empty()) return;

    for (const auto& [mod_name, titles] : enabled_map) {
        if (list_has(disabled, mod_name)) continue;
        if (list_has(pending, mod_name)) {
            NXLOG_W(kTag, "cheats: %s held off until Set up rebuilds code patches", mod_name.c_str());
            continue;
        }
        const std::filesystem::path mod_dir = mods_dir / mod_name;
        if (!std::filesystem::is_directory(mod_dir, ec)) continue;
        std::set<std::filesystem::path> seen;
        for (const auto& root : cheat_roots_for_mod(mod_dir)) {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (e.is_regular_file(ec) && to_lower(e.path().extension().string()) == ".txt") {
                    files.push_back(e.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& fp : files) {
                const std::string stem = fp.stem().string();
                if (!cheat_bid_matches(stem, bid)) continue;
                const auto canon = std::filesystem::weakly_canonical(fp, ec);
                if (!seen.insert(canon).second) continue;
                std::ifstream in(fp, std::ios::binary);
                if (!in) continue;
                const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto progs = parse_txt_filtered(mod_name, content, cfg.main_text_end, titles);
                st.programs.insert(st.programs.end(), std::make_move_iterator(progs.begin()), std::make_move_iterator(progs.end()));
            }
        }
    }
    if (!st.programs.empty()) {
        NXLOG_I(kTag, "loaded %zu active runtime cheat(s) from %s (text_end=0x%llx)",
                st.programs.size(), mods_str.c_str(), static_cast<unsigned long long>(text_end));
    }
}

void init_from_config(uint64_t main_base) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.pending_main_base = main_base;
    st.pending_config_load = true;
    st.initialized = false;
    st.programs.clear();
    if (!st.nso_bid.empty() && st.nso_text_end > 0) {
        load_from_config_locked(main_base);
    }
}

void tick_with_buttons(uint64_t held_buttons) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    for (const auto& prog : st.programs) {
        exec_program(prog, st.cfg, held_buttons);
    }
}

void tick() {
    {
        State& st = state();
        std::lock_guard<std::mutex> lk(st.mu);
        if (st.pending_config_load && !st.initialized) {
            load_from_config_locked(st.pending_main_base);
        }
    }
    uint64_t held = 0;
    if (auto src = nxrt::hid::input_source()) {
        nxrt::hid::InputSnapshot snap{};
        src->poll(snap);
        for (uint32_t i = 0; i < nxrt::hid::kNpadSlots; ++i) {
            if (snap.npad[i].connected) held |= snap.npad[i].buttons;
        }
    }
    tick_with_buttons(held);
}

size_t loaded_count() {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.programs.size();
}

void clear() {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    st.programs.clear();
    st.initialized = false;
}

}  // namespace nxrt::cheats
