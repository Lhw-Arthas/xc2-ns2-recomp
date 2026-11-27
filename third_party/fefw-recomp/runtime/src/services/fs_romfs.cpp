// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from yuzu src/core/file_sys/fsmitm_romfsbuild.cpp (GPL-2.0-or-later; the table layout, entry sizes, the name hash and the
// bucket count rule) and ScarletNX src/nx/hle/filesystem/fs.c (GPL-3.0-or-later; the same rules, with unsigned name bytes).
// The image is laid out in memory: the header, the four tables and a map of segments. File data is read from the host files on demand.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fs_romfs.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/nup.h"
#include "nxrt/write_watch.h"

namespace nxrt::services::fs {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;     // "no entry" in every link field
constexpr uint64_t kHeaderSize = 0x50;      // ten u64 fields
constexpr uint64_t kPartitionOfs = 0x200;   // file data starts here; the header is padded to it
constexpr uint64_t kDataAlign = 0x10;       // each file starts at a multiple of this, counted from the partition start
constexpr uint64_t kDirFixed = 0x18;        // directory entry without its name
constexpr uint64_t kFileFixed = 0x20;       // file entry without its name
constexpr size_t kMaxName = 255;            // bytes of UTF-8 in one name
constexpr size_t kMaxPath = 0x300;          // SDK FS_MAX_PATH (0x301 with the terminator); the path counts its leading '/'
constexpr size_t kOpenFiles = 16;           // host files kept open by the cache
constexpr size_t kLongPath = 240;           // host paths this long or longer get the \\?\ prefix

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
uint64_t pad4(size_t n) { return align_up(n, 4); }

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Bucket hash of an entry named `name` under the directory entry at table offset `parent`. The name bytes count unsigned, as the
// console's char does (yuzu's signed char on x86 differs for non-ASCII names only).
uint32_t name_hash(uint32_t parent, const std::string& name) {
    uint32_t h = parent ^ 123456789u;
    for (unsigned char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= c;
    }
    return h;
}

// Bucket count for n entries: 3 for fewer than 3, n|1 below 19, else the first number from n that no prime up to 17 divides.
uint64_t bucket_count(uint64_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint64_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

bool to_utf8(const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr) == n;
}

bool to_wide(const std::string& s, std::wstring& out) {
    out.clear();
    if (s.empty()) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n) == n;
}

// Host paths that may exceed MAX_PATH get the extended-length prefix. The input must already use backslashes.
std::wstring long_path(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() < kLongPath || p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

// One folder or file of the host tree. Layout fields are filled in once the tree is complete.
struct Node {
    std::string name;            // UTF-8; empty for the root
    std::string path;            // image path: "" for the root, "/a/b" otherwise
    std::wstring host;           // host path to open
    bool is_dir = false;
    bool is_blob = false;
    uint64_t size = 0;           // files only
    uint64_t src = 0;            // files: offset of the data in the host file (0, or the extent in a .nup)
    std::vector<Node> kids;      // sorted by name bytes
    uint32_t entry = 0;          // offset of this entry in its table
    uint32_t parent = 0;         // entry offset of the parent directory (the root's is 0)
    uint32_t sibling = kNone;    // next entry in the parent's chain of the same kind
    uint32_t first_child = kNone;  // directories: first subdirectory entry
    uint32_t first_file = kNone;   // directories: first file entry
    uint64_t data = 0;           // files: offset of the data from the partition start
};

bool read_whole_host_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (128ll << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t pos = 0;
    while (pos < out.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - pos, 1u << 24));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + pos, chunk, &got, nullptr) || got == 0) {
            CloseHandle(h);
            return false;
        }
        pos += got;
    }
    CloseHandle(h);
    return true;
}

std::string shown(const Node& n) { return n.path.empty() ? std::string("/") : n.path; }

// ---- mods: a layered romfs (M1) -----------------------------------------------------------------------------------
// A mod is a folder whose romfs tree is laid over the game's romfs by the same path. The image the game parses is one
// tree either way, so the game cannot tell a modded file from a vanilla one, and no lookup happens at run time: the
// overlay is folded into the tables this file builds (the emulator LayeredFS arrangement).
//
// [mods] order names mods from highest priority to lowest, anything unnamed follows alphabetically, and [mods]
// disabled lists folders that are not loaded. Applying from the lowest to the highest lets the last writer win, which
// is the rule docs/MODS.md states.

struct ModStats {
    std::string name;
    uint32_t files = 0, replaced = 0, added = 0;
};

std::vector<std::string> config_name_list(const char* key) {
    std::vector<std::string> out;
    const std::string s = config_get_str("mods", key, "");
    size_t i = 0;
    while (i < s.size()) {
        const size_t comma = s.find(',', i);
        const size_t end = comma == std::string::npos ? s.size() : comma;
        const std::string item = s.substr(i, end - i);
        const size_t a = item.find_first_not_of(" \t");
        const size_t b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
        i = end + 1;
    }
    return out;
}

bool list_has(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

// Validate a relative subdirectory path from mods/.mod_layers.tsv (no traversal, drive, or absolute prefix).
bool is_safe_layer_rel(const std::string& rel) {
    if (rel.empty() || rel.front() == '/' || rel.front() == '\\') return false;
    if (rel.find(':') != std::string::npos || rel.find('\0') != std::string::npos) return false;
    size_t i = 0;
    while (i < rel.size()) {
        const size_t sep = rel.find_first_of("/\\", i);
        const size_t end = sep == std::string::npos ? rel.size() : sep;
        const std::string part = rel.substr(i, end - i);
        if (part.empty() || part == "." || part == "..") return false;
        if (sep == std::string::npos) break;
        i = sep + 1;
    }
    return true;
}

// Reads mods/.mod_layers.tsv (<mod_name>\t<rel_subdir> lines) written by the launcher for mod.json option layers.
std::map<std::string, std::vector<std::string>> read_mod_layers(const std::filesystem::path& mods_dir) {
    std::map<std::string, std::vector<std::string>> out;
    std::error_code ec;
    const std::filesystem::path p = mods_dir / ".mod_layers.tsv";
    if (!std::filesystem::is_regular_file(p, ec)) return out;
    std::ifstream in(long_path(p), std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::string mod_name = line.substr(0, tab);
        const std::string rel = line.substr(tab + 1);
        if (!is_safe_layer_rel(rel)) {
            NXLOG_W("mods", "mods: ignoring unsafe option layer path %s for %s", rel.c_str(), mod_name.c_str());
            continue;
        }
        out[mod_name].push_back(rel);
    }
    return out;
}

// The application id a mod's emulator layout is filed under. The game layer sets [process] program_id (xc2_boot:
// 0x0100E95004038000); [title] program_id is the older key. 0 means nobody said, and then no id-specific layout is
// accepted - better to skip a mod than to serve another game's files.
uint64_t mod_app_id() {
    if (config_has("process", "program_id")) return static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    return static_cast<uint64_t>(config_get_int("title", "program_id", 0));
}

std::string hex16(uint64_t id) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(id));
    return std::string(b);
}

// Where a mod keeps its files, in the order docs/MODS.md lists the layouts:
//   <mod>\romfs
//   <mod>\atmosphere\contents\<app-id>\romfs
//   <mod>\contents\<app-id>\romfs
//   <mod>\<app-id>\romfs
// Also accepts Atmosphère Switch 1 / DLC XC2 title IDs (0100e9500403.... / 04007ef01f8a....) or a sole
// <16-hex-id>\romfs folder under atmosphere/contents or contents so launcher & runtime agree (issue #71).
bool is_hex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::filesystem::path mod_romfs_root(const std::filesystem::path& mod) {
    std::error_code ec;
    auto dir = [&ec](const std::filesystem::path& p) {
        return std::filesystem::is_directory(p, ec) ? p : std::filesystem::path();
    };
    if (std::filesystem::path plain = dir(mod / "romfs"); !plain.empty()) return plain;
    const uint64_t app = mod_app_id();
    if (app != 0) {
        const std::string id = hex16(app);
        for (const char* base : {"atmosphere/contents", "contents"}) {
            if (std::filesystem::path q = dir(mod / base / id / "romfs"); !q.empty()) return q;
        }
        if (std::filesystem::path q = dir(mod / id / "romfs"); !q.empty()) return q;
    }
    // Fallback for Atmosphère mods packaged with an XC2 NS1/DLC title id (e.g. 0100E95004039001) or a single title id.
    for (const char* base : {"atmosphere/contents", "contents"}) {
        const std::filesystem::path root = mod / base;
        if (!std::filesystem::is_directory(root, ec)) continue;
        std::filesystem::path xc2_hit;
        std::vector<std::filesystem::path> any_hits;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            std::string tid;
            if (!to_utf8(it->path().filename().wstring(), tid) || !is_hex16(tid)) continue;
            const std::filesystem::path cand = dir(it->path() / "romfs");
            if (cand.empty()) continue;
            const std::string low = lower_ascii(tid);
            if (low.rfind("04007ef01f8a", 0) == 0 || low.rfind("0100e9500403", 0) == 0) {
                if (xc2_hit.empty()) xc2_hit = cand;
            }
            any_hits.push_back(cand);
        }
        if (!xc2_hit.empty()) return xc2_hit;
        if (any_hits.size() == 1) return any_hits[0];
    }
    return {};
}

Node* find_kid(Node& dir, const std::string& name) {
    for (Node& k : dir.kids)
        if (k.name == name) return &k;
    return nullptr;
}

struct LooseModFile {
    std::wstring path;
    uint64_t size = 0;
    std::string owner;
};

// Lays one mod's tree over dir. A file the game already has replaces it; a file it does not have is added - mods may
// ship new assets, and tools/mods/mod_check.py is where a wrong game version is diagnosed.
void overlay_dir(Node& dir, const std::filesystem::path& host, ModStats& st,
                 std::map<std::string, LooseModFile>* loose_out = nullptr) {
    std::error_code ec;
    std::filesystem::directory_iterator it(host, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::string name;
        if (!to_utf8(it->path().filename().wstring(), name) || name.empty()) {
            NXLOG_W("mods", "mod %s: cannot use a file name under %s", st.name.c_str(), shown(dir).c_str());
            continue;
        }
        const std::filesystem::file_status stat = it->symlink_status(ec);
        if (ec || std::filesystem::is_symlink(stat)) {
            NXLOG_W("mods", "mod %s: skipping %s (a symbolic link or unreadable)", st.name.c_str(), (dir.path + "/" + name).c_str());
            continue;
        }
        Node* kid = find_kid(dir, name);
        if (std::filesystem::is_directory(stat)) {
            if (kid && !kid->is_dir) {
                NXLOG_W("mods", "mod %s: %s is a file in the game and a folder in the mod: skipped", st.name.c_str(),
                        (dir.path + "/" + name).c_str());
                continue;
            }
            if (!kid) {
                Node k;
                k.name = name;
                k.path = dir.path + "/" + name;
                k.is_dir = true;
                k.host = long_path(it->path().wstring());
                dir.kids.push_back(std::move(k));
                kid = &dir.kids.back();
            }
            overlay_dir(*kid, it->path(), st, loose_out);
            continue;
        }
        if (!std::filesystem::is_regular_file(stat)) continue;
        if (kid && kid->is_dir) {
            NXLOG_W("mods", "mod %s: %s is a folder in the game and a file in the mod: skipped", st.name.c_str(),
                    (dir.path + "/" + name).c_str());
            continue;
        }
        const uint64_t size = it->file_size(ec);
        if (ec) {
            NXLOG_W("mods", "mod %s: cannot read the size of %s", st.name.c_str(), (dir.path + "/" + name).c_str());
            continue;
        }
        const std::wstring wfile = long_path(it->path().wstring());
        const std::string full_rel = dir.path + "/" + name;
        if (loose_out) {
            std::string key = lower_ascii(full_rel);
            while (!key.empty() && key[0] == '/') key.erase(key.begin());
            if (key != "bf2.arh" && key != "bf2.ard") {
                (*loose_out)[key] = LooseModFile{wfile, size, st.name};
            }
        }
        if (kid) {
            kid->host = wfile;
            kid->size = size;
            kid->src = 0;   // the mod file starts at 0: src is the extent inside a .nup, and mods never apply to one
            st.replaced++;
        } else {
            Node k;
            k.name = name;
            k.path = full_rel;
            k.host = wfile;
            k.size = size;
            dir.kids.push_back(std::move(k));
            st.added++;
        }
        st.files++;
    }
    if (ec) NXLOG_W("mods", "mod %s: cannot read %s", st.name.c_str(), host.string().c_str());
}

// Applies the mods of [fs] mods, if the folder exists. Never fails the build: a broken mod loses its own files, not
// the game.
struct TailSlice {
    enum class Kind : uint8_t { RawFile = 0, StoredXbc1 = 1 };
    Kind kind = Kind::RawFile;
    std::wstring path;              // <mod>/.ard/append.bin OR loose mod file
    uint64_t offset = 0;            // relative byte offset within the virtual concatenated tail [0, tail_size)
    uint64_t size = 0;              // virtual byte size served by this slice
    uint64_t raw_size = 0;          // StoredXbc1: uncompressed loose file size
    uint32_t adler32 = 1;           // StoredXbc1: zlib Adler-32 trailer
    std::array<uint8_t, 0x30> xbc1_hdr{};   // StoredXbc1: 48-byte xbc1 header
    std::string owner;
};

struct ArhLayer {
    std::wstring path;    // <mod>/romfs/bf2.arh
    uint64_t tail_base = 0;
    bool has_tail = false;
    std::string owner;
};

struct ModsResult {
    std::map<std::string, std::wstring> recs;       // lower-case normalized path (forward slashes) -> <mod>/.ard/<name>.rec
    std::map<std::string, LooseModFile> loose;      // lower-case normalized romfs path -> loose file (for bf2.ard auto-override)
    std::vector<TailSlice> tails;                   // lowest -> highest priority, concatenated at ard_real_size
    std::vector<ArhLayer> arh_layers;               // lowest -> highest priority bf2.arh overrides
    uint64_t tail_size = 0;
};

// Decodes the Monolith Soft 'arh1' radix trie + string buffer (matching tools/mods/arh_index.py) so we can map
// full logical archive paths (e.g. "bdat/common.bdat") to their 24-byte entry indices and offsets.
struct ArhEntryInfo {
    uint32_t idx = 0;
    uint64_t offset = 0;
    uint32_t comp = 0;
    uint32_t unc = 0;
    uint32_t flags = 0;
};

std::map<std::string, ArhEntryInfo> decode_arh_path_map(const std::vector<uint8_t>& arh) {
    std::map<std::string, ArhEntryInfo> out;
    if (arh.size() < 0x28 || std::memcmp(arh.data(), "arh1", 4) != 0) return out;
    auto get32 = [&](size_t off) -> uint32_t {
        return uint32_t(arh[off]) | (uint32_t(arh[off + 1]) << 8) |
               (uint32_t(arh[off + 2]) << 16) | (uint32_t(arh[off + 3]) << 24);
    };
    auto get64 = [&](size_t off) -> uint64_t {
        return uint64_t(get32(off)) | (uint64_t(get32(off + 4)) << 32);
    };
    const uint32_t str_off = get32(0x0C), str_sz = get32(0x10);
    const uint32_t trie_off = get32(0x14), trie_sz = get32(0x18);
    const uint32_t ent_off = get32(0x1C), ent_cnt = get32(0x20);
    const uint32_t key = get32(0x24);
    if (str_off < 0x28 || str_sz < 4 || trie_off < 0x28 || trie_sz < 8 ||
        static_cast<uint64_t>(str_off) + str_sz > arh.size() ||
        static_cast<uint64_t>(trie_off) + trie_sz > arh.size() ||
        static_cast<uint64_t>(ent_off) + static_cast<uint64_t>(ent_cnt) * 24 > arh.size()) {
        return out;
    }
    auto xor4 = [](const uint8_t* src, size_t len, uint32_t k) {
        std::vector<uint8_t> d(src, src + len);
        const uint8_t pad[4] = {
            static_cast<uint8_t>(k & 0xFFu), static_cast<uint8_t>((k >> 8) & 0xFFu),
            static_cast<uint8_t>((k >> 16) & 0xFFu), static_cast<uint8_t>((k >> 24) & 0xFFu)
        };
        for (size_t i = 0; i < len; ++i) d[i] ^= pad[i & 3u];
        return d;
    };
    constexpr uint32_t kTrieXor = 0x0C0CACACu;
    const std::vector<uint8_t> dec_str = xor4(arh.data() + str_off, str_sz, key ^ kTrieXor ^ 0xFFFFFFFFu);
    const std::vector<uint8_t> dec_trie = xor4(arh.data() + trie_off, trie_sz, key ^ kTrieXor);
    const size_t n_nodes = dec_trie.size() / 8;
    auto trie_i32 = [&](size_t node, int field) -> int32_t {
        const size_t p = node * 8 + static_cast<size_t>(field) * 4;
        const uint32_t u = uint32_t(dec_trie[p]) | (uint32_t(dec_trie[p + 1]) << 8) |
                           (uint32_t(dec_trie[p + 2]) << 16) | (uint32_t(dec_trie[p + 3]) << 24);
        return static_cast<int32_t>(u);
    };
    for (size_t node = 1; node < n_nodes; ++node) {
        const int32_t a = trie_i32(node, 0), b = trie_i32(node, 1);
        if (b >= 0) continue;
        const int64_t par = -static_cast<int64_t>(b) - 1;
        if (par < 0 || static_cast<size_t>(par) >= n_nodes) continue;
        const int32_t pa = trie_i32(static_cast<size_t>(par), 0);
        if (pa >= 0) continue;
        const int64_t edge = (-static_cast<int64_t>(pa) - 1) ^ static_cast<int64_t>(node);
        if (edge < 0 || edge >= 256) continue;
        std::string tail;
        uint32_t idx = 0;
        if (a > 0) {
            size_t pos = static_cast<size_t>(a) + 1;
            size_t nul = pos;
            while (nul < dec_str.size() && dec_str[nul] != 0) ++nul;
            if (nul + 5 > dec_str.size()) continue;
            tail.assign(reinterpret_cast<const char*>(dec_str.data() + pos), nul - pos);
            idx = uint32_t(dec_str[nul + 1]) | (uint32_t(dec_str[nul + 2]) << 8) |
                  (uint32_t(dec_str[nul + 3]) << 16) | (uint32_t(dec_str[nul + 4]) << 24);
        } else if (edge == 0 && a < 0) {
            idx = static_cast<uint32_t>(-static_cast<int64_t>(a) - 1);
        } else {
            continue;
        }
        if (idx >= ent_cnt) continue;
        size_t cur = node;
        std::string prefix;
        bool ok = true;
        for (int steps = 0; cur > 0; ++steps) {
            if (steps > 1024) { ok = false; break; }
            const int32_t cb = trie_i32(cur, 1);
            if (cb >= 0) { ok = false; break; }
            const int64_t p = -static_cast<int64_t>(cb) - 1;
            if (p < 0 || static_cast<size_t>(p) >= n_nodes) { ok = false; break; }
            const int32_t pa2 = trie_i32(static_cast<size_t>(p), 0);
            if (pa2 >= 0) { ok = false; break; }
            const int64_t ch = (-static_cast<int64_t>(pa2) - 1) ^ static_cast<int64_t>(cur);
            if (ch < 0 || ch >= 256) { ok = false; break; }
            if (ch != 0) prefix.push_back(static_cast<char>(ch));
            cur = static_cast<size_t>(p);
        }
        if (!ok) continue;
        std::reverse(prefix.begin(), prefix.end());
        std::string full = lower_ascii(prefix + tail);
        while (!full.empty() && full[0] == '/') full.erase(full.begin());
        const size_t epos = static_cast<size_t>(ent_off) + static_cast<size_t>(idx) * 24;
        out[full] = ArhEntryInfo{idx, get64(epos), get32(epos + 8), get32(epos + 12), get32(epos + 16)};
    }
    return out;
}

bool compute_file_adler32(const std::wstring& path, uint32_t* out_adler, uint8_t first4[4]) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint32_t s1 = 1, s2 = 0;
    uint8_t buf[65536];
    DWORD got = 0;
    bool first = true;
    std::memset(first4, 0, 4);
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) {
        if (first) {
            std::memcpy(first4, buf, std::min<DWORD>(got, 4));
            first = false;
        }
        for (DWORD i = 0; i < got; ++i) {
            s1 += buf[i];
            if (s1 >= 65521u) s1 -= 65521u;
            s2 += s1;
            if (s2 >= 65521u) s2 -= 65521u;
        }
    }
    CloseHandle(h);
    *out_adler = (s2 << 16) | s1;
    return true;
}

std::string normalize_rel_key(std::string rel) {
    for (char& c : rel) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
    return rel;
}

// Composes multiple mod bf2.arh files over the vanilla bf2.arh so entries repointed into each mod's append.bin
// are relocated by that mod's tail_base without colliding at +0. Priority runs lowest -> highest (last writer wins).
bool compose_arh_layers(const std::wstring& vanilla_arh, uint64_t ard_real_size,
                        const std::vector<ArhLayer>& layers, std::vector<uint8_t>& merged) {
    auto get32 = [](const uint8_t* p) -> uint32_t {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    };
    auto get64 = [&](const uint8_t* p) -> uint64_t {
        return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
    };
    std::vector<uint8_t> base;
    if (!read_whole_host_file(vanilla_arh, base) || base.size() < 0x24 || std::memcmp(base.data(), "arh1", 4) != 0)
        return false;
    const uint32_t ent_off = get32(base.data() + 0x1C);
    const uint32_t ent_count = get32(base.data() + 0x20);
    if (static_cast<uint64_t>(ent_off) + static_cast<uint64_t>(ent_count) * 24 > base.size()) return false;

    merged = base;
    for (const ArhLayer& layer : layers) {
        std::vector<uint8_t> mod_arh;
        if (!read_whole_host_file(layer.path, mod_arh) || mod_arh.size() != base.size() ||
            std::memcmp(mod_arh.data(), "arh1", 4) != 0) {
            NXLOG_W("mods", "mods: skipping incompatible bf2.arh from %s", layer.owner.c_str());
            continue;
        }
        uint32_t applied_entries = 0;
        for (uint32_t i = 0; i < ent_count; ++i) {
            const size_t pos = static_cast<size_t>(ent_off) + static_cast<size_t>(i) * 24;
            if (std::memcmp(mod_arh.data() + pos, base.data() + pos, 24) == 0) continue;
            std::memcpy(merged.data() + pos, mod_arh.data() + pos, 24);
            uint64_t off = get64(mod_arh.data() + pos);
            if (layer.has_tail && layer.tail_base > 0 && off >= ard_real_size) {
                off += layer.tail_base;
                for (int b = 0; b < 8; ++b) merged[pos + b] = static_cast<uint8_t>(off >> (8 * b));
            }
            applied_entries++;
        }
        NXLOG_I("mods", "mods: bf2.arh merged %u entry change(s) from %s (tail_base=+0x%llx)",
                applied_entries, layer.owner.c_str(), static_cast<unsigned long long>(layer.tail_base));
    }
    return true;
}

ModsResult apply_mods(Node& root, const std::filesystem::path& host_abs) {
    ModsResult out;
    // Every RomFS image is built here, the add-on packs included. A mod lays files over the *game's* romfs; overlaying
    // a pack's image as well would put the same file in seven storages and report it as 'added' six times.
    {
        std::error_code e;
        const std::filesystem::path want = std::filesystem::absolute(
            std::filesystem::path(config_get_str("fs", "romfs", "game/romfs")), e);
        if (e || !std::filesystem::exists(want, e)) return {};
        if (!std::filesystem::equivalent(want, host_abs, e) || e) return {};
    }
    const std::string dir_s = config_get_str("fs", "mods", "mods");
    if (dir_s.empty()) return {};
    const std::filesystem::path dir(dir_s);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        NXLOG_I("mods", "mods: no folder at %s", dir_s.c_str());
        return {};
    }
    std::vector<std::string> present;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name;
        if (!to_utf8(it->path().filename().wstring(), name)) continue;
        if (name.empty() || name[0] == '.') continue;  // skip launcher side-files (.mod_layers.tsv, etc.)
        if (std::filesystem::is_directory(it->symlink_status(ec))) present.push_back(name);
    }
    if (present.empty()) {
        NXLOG_I("mods", "mods: %s is empty", dir_s.c_str());
        return {};
    }
    std::sort(present.begin(), present.end());
    const std::vector<std::string> order = config_name_list("order");
    const std::vector<std::string> disabled = config_name_list("disabled");
    const std::vector<std::string> pending_rebuild = config_name_list("pending_rebuild");
    const std::map<std::string, std::vector<std::string>> extra_layers = read_mod_layers(dir);

    std::vector<std::string> seq;
    for (const std::string& n : order)
        if (std::find(present.begin(), present.end(), n) != present.end()) seq.push_back(n);
    for (const std::string& n : present)
        if (!list_has(seq, n)) seq.push_back(n);
    std::reverse(seq.begin(), seq.end());   // lowest priority first: the last writer wins

    uint32_t applied = 0, off = 0;
    for (const std::string& name : seq) {
        if (list_has(disabled, name)) {
            NXLOG_I("mods", "mods: %s: disabled", name.c_str());
            off++;
            continue;
        }
        if (list_has(pending_rebuild, name)) {
            NXLOG_W("mods", "mods: %s held off until Set up rebuilds code patches", name.c_str());
            off++;
            continue;
        }
        const std::filesystem::path mod_dir = dir / name;
        const std::filesystem::path r = mod_romfs_root(mod_dir);
        ModStats st;
        st.name = name;
        if (!r.empty()) {
            overlay_dir(root, r, st, &out.loose);
        }
        // M4 optional mod.json option sub-layers (mods/.mod_layers.tsv): applied after base romfs in declared order.
        auto lit = extra_layers.find(name);
        if (lit != extra_layers.end()) {
            for (const std::string& rel : lit->second) {
                std::error_code lec;
                const std::filesystem::path opt_dir = mod_dir / std::filesystem::path(rel);
                const std::filesystem::file_status fst = std::filesystem::symlink_status(opt_dir, lec);
                if (lec || std::filesystem::is_symlink(fst) || !std::filesystem::is_directory(fst)) {
                    NXLOG_W("mods", "mods: %s: option layer %s missing or invalid: skipped", name.c_str(), rel.c_str());
                    continue;
                }
                const std::filesystem::path use_dir = std::filesystem::is_directory(opt_dir / "romfs", lec) ? (opt_dir / "romfs") : opt_dir;
                overlay_dir(root, use_dir, st, &out.loose);
                const std::filesystem::path opt_ard = opt_dir / ".ard";
                if (std::filesystem::is_directory(opt_ard, lec)) {
                    for (std::filesystem::recursive_directory_iterator it(opt_ard, lec), end; !lec && it != end; it.increment(lec)) {
                        if (!it->is_regular_file(lec)) continue;
                        std::string a_rel;
                        if (!to_utf8(it->path().lexically_relative(opt_ard).wstring(), a_rel)) continue;
                        if (a_rel.size() < 5 || a_rel.compare(a_rel.size() - 4, 4, ".rec") != 0) continue;
                        a_rel.resize(a_rel.size() - 4);
                        out.recs[normalize_rel_key(std::move(a_rel))] = long_path(it->path().wstring());
                    }
                }
            }
        }
        if (r.empty() && lit == extra_layers.end()) {
            NXLOG_W("mods", "mods: %s has no romfs folder: skipped", name.c_str());
            continue;
        }
        // Pre-built archive records and optional bf2.arh overrides for this mod
        {
            uint64_t this_tail_base = 0;
            bool this_has_tail = false;
            std::error_code rec_ec;
            const std::filesystem::path recs_dir = (dir / name) / ".ard";
            if (std::filesystem::is_directory(recs_dir, rec_ec)) {
                size_t before = out.recs.size();
                for (std::filesystem::recursive_directory_iterator it(recs_dir, rec_ec), end; !rec_ec && it != end; it.increment(rec_ec)) {
                    if (!it->is_regular_file(rec_ec)) continue;
                    std::string rel;
                    if (!to_utf8(it->path().lexically_relative(recs_dir).wstring(), rel)) continue;
                    if (rel.size() < 5 || rel.compare(rel.size() - 4, 4, ".rec") != 0) continue;
                    rel.resize(rel.size() - 4);
                    out.recs[normalize_rel_key(std::move(rel))] = long_path(it->path().wstring());
                }
                NXLOG_I("mods", "mods: %s: %zu archive record(s)", name.c_str(), out.recs.size() - before);
                std::error_code tail_ec;
                const std::filesystem::path tail = recs_dir / "append.bin";
                if (std::filesystem::is_regular_file(tail, tail_ec)) {
                    const uint64_t sz = std::filesystem::file_size(tail, tail_ec);
                    if (!tail_ec && sz > 0) {
                        this_tail_base = out.tail_size;
                        this_has_tail = true;
                        TailSlice sl;
                        sl.kind = TailSlice::Kind::RawFile;
                        sl.path = long_path(tail.wstring());
                        sl.offset = this_tail_base;
                        sl.size = sz;
                        sl.owner = name;
                        out.tails.push_back(std::move(sl));
                        out.tail_size += sz;
                        NXLOG_I("mods", "mods: %s: %llu byte(s) appended after the archive (+0x%llx)",
                                name.c_str(), static_cast<unsigned long long>(sz), static_cast<unsigned long long>(this_tail_base));
                    }
                }
            }
            std::error_code arh_ec;
            const std::filesystem::path mod_arh = r / "bf2.arh";
            if (std::filesystem::is_regular_file(mod_arh, arh_ec)) {
                out.arh_layers.push_back(ArhLayer{long_path(mod_arh.wstring()), this_tail_base, this_has_tail, name});
            }
        }
        NXLOG_I("mods", "mods: %s: %u file(s), %u replaced, %u added", name.c_str(), st.files, st.replaced, st.added);
        applied++;
    }
    NXLOG_I("mods", "mods: %u enabled, %u disabled, %u named in [mods] order", applied, off, static_cast<uint32_t>(order.size()));
    if (!out.recs.empty()) NXLOG_I("mods", "mods: %zu archive record(s) from .ard folders", out.recs.size());
    return out;
}


// Fills dir.kids from the host folder dir.host, recursively. False with msg set at the first entry that cannot be described.
bool scan(Node& dir, std::string& msg) {
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir.host), ec);
    if (ec) {
        msg = "cannot read folder " + shown(dir);
        return false;
    }
    const std::filesystem::directory_iterator end;
    for (;;) {
        if (ec) {
            msg = "cannot read folder " + shown(dir);
            return false;
        }
        if (it == end) break;
        const std::filesystem::directory_entry& e = *it;
        std::string name;
        if (!to_utf8(e.path().filename().wstring(), name) || name.empty() || name.size() > kMaxName) {
            msg = "name cannot be stored in a RomFS (1 to 255 bytes of UTF-8): " + shown(dir) + "/" + name;
            return false;
        }
        Node k;
        k.name = name;
        k.path = dir.path + "/" + name;
        if (k.path.size() > kMaxPath) {
            msg = "path too long for a RomFS (0x300 bytes): " + k.path;
            return false;
        }
        k.host = long_path(e.path().wstring());
        std::error_code sec;
        const std::filesystem::file_status st = e.symlink_status(sec);
        if (sec) {
            msg = "cannot stat " + k.path;
            return false;
        }
        if (std::filesystem::is_symlink(st)) {
            msg = "symbolic link cannot be described: " + k.path;
            return false;
        }
        if (std::filesystem::is_directory(st)) {
            k.is_dir = true;
            if (!scan(k, msg)) return false;
        } else if (std::filesystem::is_regular_file(st)) {
            k.size = e.file_size(sec);
            if (sec) {
                msg = "cannot read the size of " + k.path;
                return false;
            }
        } else {
            msg = "not a file or folder: " + k.path;
            return false;
        }
        dir.kids.push_back(std::move(k));
        it.increment(ec);
    }
    // std::string's operator< compares unsigned char values, which is the byte order the RomFS hash tables expect.
    std::sort(dir.kids.begin(), dir.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    return true;
}

void sort_tree(Node& d) {
    std::sort(d.kids.begin(), d.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    for (Node& k : d.kids)
        if (k.is_dir) sort_tree(k);
}

// Fills root from the RomFS of a .nup package: every file is a range of the package file.
bool scan_nup(const std::string& nup_path, Node& root, std::string& msg) {
    nup::Content c;
    if (!nup::open(nup_path, c, &msg)) return false;
    if (c.romfs.empty()) {
        msg = nup_path + " has no RomFS";
        return false;
    }
    for (const nup::File& f : c.romfs) {
        Node* d = &root;
        size_t at = 0;
        for (size_t slash; (slash = f.path.find('/', at)) != std::string::npos; at = slash + 1) {
            const std::string name = f.path.substr(at, slash - at);
            auto it = std::find_if(d->kids.begin(), d->kids.end(), [&](const Node& k) { return k.is_dir && k.name == name; });
            if (it == d->kids.end()) {
                Node k;
                k.is_dir = true;
                k.name = name;
                k.path = d->path + "/" + name;
                d->kids.push_back(std::move(k));
                it = d->kids.end() - 1;
            }
            d = &*it;
        }
        Node k;
        k.name = f.path.substr(at);
        k.path = "/" + f.path;
        k.host = root.host;
        k.size = f.size;
        k.src = f.offset;
        if (k.name.size() > kMaxName || k.path.size() > kMaxPath) {
            msg = "name or path too long for a RomFS: " + k.path;
            return false;
        }
        d->kids.push_back(std::move(k));
    }
    sort_tree(root);
    return true;
}

void flatten(Node& n, std::vector<Node*>& dirs, std::vector<Node*>& files) {
    for (Node& k : n.kids) {
        if (k.is_dir) {
            dirs.push_back(&k);
            flatten(k, dirs, files);
        } else {
            files.push_back(&k);
        }
    }
}

struct Segment {
    enum class Kind : uint8_t { kHeader, kTables, kZero, kFile, kBlob };
    uint64_t off = 0;            // image offset
    uint64_t len = 0;
    Kind kind = Kind::kZero;
    uint64_t src = 0;            // kHeader/kTables/kBlob: offset in buffer; kFile: offset in host file
    uint32_t file = 0;           // kFile/kBlob: index into Layout::files
};

struct FileRec {
    std::wstring host;
    uint64_t size = 0;
    uint64_t image_off = 0;
    uint64_t src = 0;            // offset of the data in the host file
    std::string path;            // path inside the image ("/a/b.bin"), for the first-read note
};

// Everything build() produces. Replaced as a whole.
struct Layout {
    bool built = false;
    uint64_t size = 0;
    RomfsImage::Stats stats;
    std::array<uint8_t, kHeaderSize> header{};
    std::vector<uint8_t> tables;        // dir hash, dir table, file hash, file table, in that order from dir_hash_table_ofs
    std::vector<uint8_t> blob;          // optional composed bf2.arh when multiple mods alter bf2.arh / append.bin
    std::vector<Segment> segs;          // contiguous, covering [0, size)
    std::vector<FileRec> files;         // in data order
    std::map<std::string, uint32_t> by_path;
    std::map<std::string, std::wstring> ard_recs;   // M2: lower-case name -> <mod>/.ard/<name>.rec
    std::map<uint64_t, std::pair<std::wstring, uint64_t>> ard_prebound;   // M2: bf2.arh exact offset -> (.rec path, size)
    std::vector<TailSlice> ard_tails;               // M2b: concatenated <mod>/.ard/append.bin + loose romfs->ard slices
    uint64_t ard_tail_size = 0;
};

// Lays out the host folder: entries sorted by full path in each table, file data in that order, 16-byte aligned.
bool make_layout(const std::string& host_dir, Layout& L, std::string* err) {
    std::string msg;
    std::wstring wdir;
    const bool packaged = nup::is_nup(host_dir);
    if (!to_wide(host_dir, wdir)) {
        if (err) *err = "host folder name is empty or not valid UTF-8";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(wdir), ec);
    if (ec || (!packaged && !std::filesystem::is_directory(abs, ec))) {
        if (err) *err = "not a folder: " + host_dir;
        return false;
    }
    Node root;
    root.is_dir = true;
    root.host = long_path(abs.wstring());
    if (!(packaged ? scan_nup(host_dir, root, msg) : scan(root, msg))) {
        if (err) *err = msg;
        return false;
    }

    // The overlay is folded in here, before the tables are laid out: one image, no lookup at run time. A .nup serves
    // its own romfs, so mods only apply to the folder route.
    if (packaged) NXLOG_I("mods", "mods: not applied - the romfs comes from a package");
    else {
        std::wstring vanilla_arh;
        uint64_t vanilla_ard_size = 0;
        for (const Node& k : root.kids) {
            if (!k.is_dir && k.name == "bf2.arh") vanilla_arh = k.host;
            if (!k.is_dir && k.name == "bf2.ard") vanilla_ard_size = k.size;
        }
        ModsResult mr = apply_mods(root, abs);
        L.ard_recs = std::move(mr.recs);

        // Decode vanilla bf2.arh once when we have .rec overrides, multiple arh layers, or loose romfs files.
        std::vector<uint8_t> arh_blob;
        bool arh_blob_dirty = false;
        std::map<std::string, ArhEntryInfo> arh_map;
        if (!vanilla_arh.empty() && vanilla_ard_size > 0 &&
            (!L.ard_recs.empty() || !mr.loose.empty() || mr.arh_layers.size() > 1)) {
            if (!mr.arh_layers.empty()) {
                if (compose_arh_layers(vanilla_arh, vanilla_ard_size, mr.arh_layers, arh_blob)) {
                    arh_blob_dirty = (mr.arh_layers.size() > 1);
                }
            } else {
                read_whole_host_file(vanilla_arh, arh_blob);
            }
            if (!arh_blob.empty()) {
                arh_map = decode_arh_path_map(arh_blob);
            }
        }

        // M2 (.ard/*.rec): pre-bind full logical paths directly to their bf2.arh offsets, and warn on unmatched .rec.
        if (!L.ard_recs.empty() && !arh_map.empty()) {
            std::map<std::string, std::wstring> extra_basenames;
            for (const auto& kv : L.ard_recs) {
                auto hit = arh_map.find(kv.first);
                if (hit != arh_map.end()) {
                    std::error_code sz_ec;
                    const uint64_t sz = std::filesystem::file_size(std::filesystem::path(kv.second), sz_ec);
                    if (!sz_ec && sz > 0) {
                        L.ard_prebound[hit->second.offset] = {kv.second, sz};
                        NXLOG_I("mods", "mods: .ard/%s.rec bound to bf2.ard offset 0x%llx (%llu bytes)",
                                kv.first.c_str(), static_cast<unsigned long long>(hit->second.offset),
                                static_cast<unsigned long long>(sz));
                        continue;
                    }
                }
                // Check if basename matches any entry (legacy flat .ard/<name>.rec)
                const size_t slash = kv.first.rfind('/');
                const std::string base = (slash == std::string::npos) ? kv.first : kv.first.substr(slash + 1);
                bool base_matched = false;
                for (const auto& ae : arh_map) {
                    const size_t s2 = ae.first.rfind('/');
                    const std::string b2 = (s2 == std::string::npos) ? ae.first : ae.first.substr(s2 + 1);
                    if (b2 == base) { base_matched = true; break; }
                }
                if (base_matched && slash != std::string::npos) {
                    extra_basenames[base] = kv.second;
                } else if (!base_matched) {
                    NXLOG_W("mods", "mods: .ard/%s.rec matched no entry in bf2.arh (check path or game version)",
                            kv.first.c_str());
                }
            }
            for (auto& kv : extra_basenames) L.ard_recs.emplace(kv.first, std::move(kv.second));
        }

        // M2c (issue #71): loose romfs/<path> files that match a bf2.arh entry automatically override that bf2.ard
        // record via the virtual tail (no manual ard_pack or conflicting bf2.arh needed).
        if (!mr.loose.empty() && !arh_map.empty() && arh_blob.size() >= 0x24) {
            const uint32_t ent_off = uint32_t(arh_blob[0x1C]) | (uint32_t(arh_blob[0x1D]) << 8) |
                                     (uint32_t(arh_blob[0x1E]) << 16) | (uint32_t(arh_blob[0x1F]) << 24);
            auto put32 = [&](size_t pos, uint32_t v) {
                arh_blob[pos + 0] = static_cast<uint8_t>(v & 0xFFu);
                arh_blob[pos + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
                arh_blob[pos + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
                arh_blob[pos + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
            };
            auto put64 = [&](size_t pos, uint64_t v) {
                put32(pos, static_cast<uint32_t>(v & 0xFFFFFFFFull));
                put32(pos + 4, static_cast<uint32_t>(v >> 32));
            };
            for (const auto& kv : mr.loose) {
                if (L.ard_recs.count(kv.first)) continue;   // explicit .rec takes precedence
                auto hit = arh_map.find(kv.first);
                if (hit == arh_map.end()) continue;
                const ArhEntryInfo& ent = hit->second;
                const LooseModFile& lf = kv.second;
                if (lf.size > 0xFFFFFFFFull) continue;
                uint32_t adler = 1;
                uint8_t first4[4] = {};
                if (!compute_file_adler32(lf.path, &adler, first4)) continue;

                TailSlice sl;
                sl.path = lf.path;
                sl.offset = mr.tail_size;
                sl.owner = lf.owner;
                uint32_t new_comp = 0;
                uint32_t new_unc = static_cast<uint32_t>(lf.size);
                const bool raw_entry = (ent.flags == 0) || (std::memcmp(first4, "xbc1", 4) == 0);
                if (raw_entry) {
                    sl.kind = TailSlice::Kind::RawFile;
                    sl.size = lf.size;
                    new_comp = static_cast<uint32_t>(lf.size);
                } else {
                    // Wrap uncompressed loose file as xbc1 + stored-zlib (level 0): zero zlib dependency, zero extra RAM.
                    const uint64_t blocks = (lf.size == 0) ? 1ull : ((lf.size + 65534ull) / 65535ull);
                    const uint64_t comp64 = 2ull + blocks * 5ull + lf.size + 4ull;
                    if (comp64 > 0xFFFFFFFFull) continue;
                    new_comp = static_cast<uint32_t>(comp64);
                    sl.kind = TailSlice::Kind::StoredXbc1;
                    sl.raw_size = lf.size;
                    sl.adler32 = adler;
                    sl.size = 0x30ull + comp64;
                    sl.xbc1_hdr.fill(0);
                    std::memcpy(sl.xbc1_hdr.data(), "xbc1", 4);
                    auto hdr32 = [&](size_t off, uint32_t v) {
                        sl.xbc1_hdr[off + 0] = static_cast<uint8_t>(v & 0xFFu);
                        sl.xbc1_hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
                        sl.xbc1_hdr[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
                        sl.xbc1_hdr[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
                    };
                    hdr32(0x04, 1u);
                    hdr32(0x08, new_unc);
                    hdr32(0x0C, new_comp);
                    hdr32(0x10, 0u);
                    const size_t sp = kv.first.rfind('/');
                    const std::string base = (sp == std::string::npos) ? kv.first : kv.first.substr(sp + 1);
                    std::memcpy(sl.xbc1_hdr.data() + 0x14, base.data(), std::min<size_t>(base.size(), 27));
                }
                const uint64_t new_off = vanilla_ard_size + sl.offset;
                const size_t epos = static_cast<size_t>(ent_off) + static_cast<size_t>(ent.idx) * 24;
                if (epos + 24 <= arh_blob.size()) {
                    put64(epos + 0, new_off);
                    put32(epos + 8, new_comp);
                    put32(epos + 12, new_unc);
                    mr.tail_size += sl.size;
                    mr.tails.push_back(std::move(sl));
                    arh_blob_dirty = true;
                    NXLOG_I("mods", "mods: %s: loose romfs/%s -> bf2.ard (+0x%llx, %llu bytes, %s)",
                            lf.owner.c_str(), kv.first.c_str(), static_cast<unsigned long long>(new_off),
                            static_cast<unsigned long long>(lf.size), raw_entry ? "raw" : "xbc1-stored");
                }
            }
        }

        if (!mr.tails.empty() && mr.tail_size > 0) {
            // The image is ours to build, so the archive file can simply be longer than the archive: the bytes past the
            // real size come from each mod's append.bin / loose-override slice in order. Nothing copies 15 GB.
            bool grew = false;
            for (Node& k : root.kids) {
                if (k.is_dir || k.name != "bf2.ard") continue;
                k.size += mr.tail_size;
                L.ard_tails = std::move(mr.tails);
                L.ard_tail_size = mr.tail_size;
                grew = true;
                NXLOG_I("mods", "mods: bf2.ard grown by %llu byte(s) across %zu tail slice(s)",
                        static_cast<unsigned long long>(mr.tail_size), L.ard_tails.size());
                break;
            }
            if (!grew) NXLOG_W("mods", "mods: append.bin present (%zu slice(s)) but this romfs has no bf2.ard", mr.tails.size());
        }
        if (arh_blob_dirty && !arh_blob.empty()) {
            L.blob = std::move(arh_blob);
            for (Node& k : root.kids) {
                if (k.is_dir || k.name != "bf2.arh") continue;
                k.is_blob = true;
                k.size = L.blob.size();
                break;
            }
        }
    }

    std::vector<Node*> dirs{&root}, files;
    flatten(root, dirs, files);
    auto by_path = [](const Node* a, const Node* b) { return a->path < b->path; };
    std::sort(dirs.begin(), dirs.end(), by_path);
    std::sort(files.begin(), files.end(), by_path);

    // Entry offsets in the two tables (root first: its path is empty).
    uint64_t dir_bytes = 0, file_bytes = 0;
    for (Node* d : dirs) {
        d->entry = static_cast<uint32_t>(dir_bytes);
        dir_bytes += kDirFixed + pad4(d->name.size());
    }
    for (Node* f : files) {
        f->entry = static_cast<uint32_t>(file_bytes);
        file_bytes += kFileFixed + pad4(f->name.size());
    }
    if (dir_bytes >= 0xFFFFFFF0ull || file_bytes >= 0xFFFFFFF0ull) {
        if (err) *err = "too many entries for a RomFS";
        return false;
    }

    // File data: each file at a multiple of 16 from the partition start.
    uint64_t part = 0;
    for (Node* f : files) {
        part = align_up(part, kDataAlign);
        f->data = part;
        part += f->size;
    }

    // Links: each directory's children in name order, as chains of sibling entries.
    for (Node* d : dirs) {
        std::vector<Node*> sub, fil;
        for (Node& k : d->kids) (k.is_dir ? sub : fil).push_back(&k);
        d->first_child = sub.empty() ? kNone : sub.front()->entry;
        d->first_file = fil.empty() ? kNone : fil.front()->entry;
        for (size_t i = 0; i < sub.size(); i++) {
            sub[i]->parent = d->entry;
            sub[i]->sibling = i + 1 < sub.size() ? sub[i + 1]->entry : kNone;
        }
        for (size_t i = 0; i < fil.size(); i++) {
            fil[i]->parent = d->entry;
            fil[i]->sibling = i + 1 < fil.size() ? fil[i + 1]->entry : kNone;
        }
    }

    // Table placement: the header is 0x50 bytes, the partition starts at 0x200, then the four tables, each 4-byte aligned.
    const uint64_t dbuckets = bucket_count(dirs.size());
    const uint64_t fbuckets = bucket_count(files.size());
    const uint64_t dhash_size = 4 * dbuckets, fhash_size = 4 * fbuckets;
    const uint64_t dhash_ofs = align_up(kPartitionOfs + part, 4);
    const uint64_t dtab_ofs = dhash_ofs + dhash_size;
    const uint64_t fhash_ofs = dtab_ofs + dir_bytes;
    const uint64_t ftab_ofs = fhash_ofs + fhash_size;
    const uint64_t image_size = ftab_ofs + file_bytes;

    L.tables.assign(dhash_size + dir_bytes + fhash_size + file_bytes, 0);
    uint8_t* dh = L.tables.data();
    uint8_t* dt = dh + dhash_size;
    uint8_t* fh = dt + dir_bytes;
    uint8_t* ft = fh + fhash_size;
    std::vector<uint32_t> dhead(dbuckets, kNone), fhead(fbuckets, kNone);

    for (Node* d : dirs) {
        const uint32_t b = static_cast<uint32_t>(name_hash(d->parent, d->name) % dbuckets);
        uint8_t* e = dt + d->entry;
        put32(e + 0, d->parent);
        put32(e + 4, d->sibling);
        put32(e + 8, d->first_child);
        put32(e + 12, d->first_file);
        put32(e + 16, dhead[b]);
        put32(e + 20, static_cast<uint32_t>(d->name.size()));
        std::memcpy(e + kDirFixed, d->name.data(), d->name.size());
        dhead[b] = d->entry;
    }
    for (Node* f : files) {
        const uint32_t b = static_cast<uint32_t>(name_hash(f->parent, f->name) % fbuckets);
        uint8_t* e = ft + f->entry;
        put32(e + 0, f->parent);
        put32(e + 4, f->sibling);
        put64(e + 8, f->data);
        put64(e + 16, f->size);
        put32(e + 24, fhead[b]);
        put32(e + 28, static_cast<uint32_t>(f->name.size()));
        std::memcpy(e + kFileFixed, f->name.data(), f->name.size());
        fhead[b] = f->entry;
    }
    for (size_t i = 0; i < dhead.size(); i++) put32(dh + 4 * i, dhead[i]);
    for (size_t i = 0; i < fhead.size(); i++) put32(fh + 4 * i, fhead[i]);

    std::memset(L.header.data(), 0, L.header.size());
    put64(L.header.data() + 0, kHeaderSize);
    put64(L.header.data() + 8, dhash_ofs);
    put64(L.header.data() + 16, dhash_size);
    put64(L.header.data() + 24, dtab_ofs);
    put64(L.header.data() + 32, dir_bytes);
    put64(L.header.data() + 40, fhash_ofs);
    put64(L.header.data() + 48, fhash_size);
    put64(L.header.data() + 56, ftab_ofs);
    put64(L.header.data() + 64, file_bytes);
    put64(L.header.data() + 72, kPartitionOfs);

    // Segments: header, padding, then each file's data with the zero gaps between, the zero gap up to the tables, the tables.
    L.segs.clear();
    L.files.clear();
    L.by_path.clear();
    L.segs.push_back({0, kHeaderSize, Segment::Kind::kHeader, 0, 0});
    L.segs.push_back({kHeaderSize, kPartitionOfs - kHeaderSize, Segment::Kind::kZero, 0, 0});
    uint64_t cursor = kPartitionOfs;
    uint64_t data_bytes = 0;
    for (Node* f : files) {
        const uint64_t img = kPartitionOfs + f->data;
        if (img > cursor) L.segs.push_back({cursor, img - cursor, Segment::Kind::kZero, 0, 0});
        const uint32_t idx = static_cast<uint32_t>(L.files.size());
        L.files.push_back({f->host, f->size, img, f->src, f->path});
        if (f->size > 0) {
            const Segment::Kind sk = f->is_blob ? Segment::Kind::kBlob : Segment::Kind::kFile;
            L.segs.push_back({img, f->size, sk, f->is_blob ? 0 : f->src, idx});
        }
        L.by_path[f->path] = idx;
        cursor = img + f->size;
        data_bytes += f->size;
    }
    if (dhash_ofs > cursor) L.segs.push_back({cursor, dhash_ofs - cursor, Segment::Kind::kZero, 0, 0});
    L.segs.push_back({dhash_ofs, image_size - dhash_ofs, Segment::Kind::kTables, 0, 0});

    L.size = image_size;
    L.stats.files = static_cast<uint32_t>(files.size());
    L.stats.dirs = static_cast<uint32_t>(dirs.size());
    L.stats.data_bytes = data_bytes;
    L.stats.data_offset = kPartitionOfs;
    L.built = true;
    return true;
}

// One host file kept open. The box closes its handle when the last reader and the cache have let go of it.
struct HandleBox {
    HANDLE h = INVALID_HANDLE_VALUE;
    HandleBox() = default;
    HandleBox(const HandleBox&) = delete;
    HandleBox& operator=(const HandleBox&) = delete;
    ~HandleBox() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

struct OpenSlot {
    uint32_t file = kNone;
    uint64_t used = 0;
    std::shared_ptr<HandleBox> box;
};

}  // namespace

struct RomfsImage::Impl {
    Layout l;
    mutable std::mutex mu;                  // guards cache, tick and read_seen
    mutable std::vector<uint8_t> read_seen; // per file: read at least once (the game's loads, in the log and the crash report)
    void note_read(uint32_t idx) const {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (idx >= read_seen.size() || read_seen[idx]) return;
            read_seen[idx] = 1;
        }
        const std::string& path = l.files[idx].path;
        NXLOG_D("fs", "romfs: first read of %s", path.c_str());
        diag_note_file("rom", path.c_str());
    }
    mutable std::vector<OpenSlot> cache;    // at most kOpenFiles entries
    mutable uint64_t tick = 0;

    // The handle for file idx, opened if it is not cached. The least recently used entry is dropped when the cache is full.
    std::shared_ptr<HandleBox> acquire(uint32_t idx) const {
        std::lock_guard<std::mutex> lk(mu);
        for (OpenSlot& s : cache) {
            if (s.file == idx) {
                s.used = ++tick;
                return s.box;
            }
        }
        const FileRec& f = l.files[idx];
        HANDLE h = CreateFileW(f.host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        auto box = std::make_shared<HandleBox>();
        box->h = h;
        if (cache.size() < kOpenFiles) {
            cache.push_back(OpenSlot{idx, ++tick, box});
        } else {
            auto victim = std::min_element(cache.begin(), cache.end(),
                                           [](const OpenSlot& a, const OpenSlot& b) { return a.used < b.used; });
            *victim = OpenSlot{idx, ++tick, box};
        }
        return box;
    }

    // Positioned reads (OVERLAPPED offset), so concurrent readers of one handle need no seek lock.
    bool read_host(uint32_t idx, uint64_t ofs, uint8_t* out, uint64_t n) const {
        std::shared_ptr<HandleBox> box = acquire(idx);
        if (!box) return false;
        const mem::ww::OsWriteScope ws(out, n);   // ReadFile fails on a page the write tracker made read-only
        while (n > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n, 1ull << 30));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(ofs & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>(ofs >> 32);
            DWORD got = 0;
            // A short read means the file shrank since build(): an error, not zeros.
            if (!ReadFile(box->h, out, chunk, &got, &ov) || got != chunk) return false;
            out += chunk;
            ofs += chunk;
            n -= chunk;
        }
        return true;
    }

    // ---- M2: archive records served from a mod's pre-built .ard files ------------------------------------------------
    // tools/mods/ard_pack.py compresses a mod file into the record the game expects (header + zlib padded to the
    // original comp), because the runtime has no zlib. Here we only slice it: the game asks for 0x30 bytes at a record
    // offset, then for the comp it read from that header, and both land in the same .rec at the same relative offsets.
    struct ArdRec {
        std::wstring path;
        uint64_t size = 0;
    };
    size_t ard_index = static_cast<size_t>(-1);          // the file index of bf2.ard, when the romfs holds one
    std::map<std::string, std::wstring> ard_recs;         // lower-case logical name -> <mod>/.ard/<name>.rec
    mutable std::map<uint64_t, ArdRec> ard_at;            // record offset -> .rec + byte size, learned when its header is read
    std::vector<TailSlice> ard_tails;                     // sorted by .offset, served past the real archive
    uint64_t ard_tail_size = 0;
    uint64_t ard_real_size = 0;                          // the on-disk archive size, before growning for the tail
    mutable std::mutex ard_mu;

    bool read_stored_xbc1(const TailSlice& sl, uint64_t at, uint8_t* dst, uint64_t len) const {
        if (at > sl.size || len > sl.size - at) return false;
        const uint64_t blocks = (sl.raw_size == 0) ? 1ull : ((sl.raw_size + 65534ull) / 65535ull);
        const uint64_t blocks_start = 0x32ull;
        const uint64_t trailer_start = blocks_start + blocks * 5ull + sl.raw_size;
        const uint8_t zhdr[2] = {0x78, 0x01};
        const uint8_t trailer[4] = {
            static_cast<uint8_t>((sl.adler32 >> 24) & 0xFFu),
            static_cast<uint8_t>((sl.adler32 >> 16) & 0xFFu),
            static_cast<uint8_t>((sl.adler32 >> 8) & 0xFFu),
            static_cast<uint8_t>(sl.adler32 & 0xFFu),
        };
        while (len > 0) {
            if (at < 0x30ull) {
                const uint64_t n = std::min(len, 0x30ull - at);
                std::memcpy(dst, sl.xbc1_hdr.data() + at, static_cast<size_t>(n));
                dst += n; at += n; len -= n;
            } else if (at < blocks_start) {
                const uint64_t n = std::min(len, blocks_start - at);
                std::memcpy(dst, zhdr + (at - 0x30ull), static_cast<size_t>(n));
                dst += n; at += n; len -= n;
            } else if (at < trailer_start) {
                const uint64_t rel = at - blocks_start;
                constexpr uint64_t kFullBlock = 5ull + 65535ull;
                const uint64_t bi = std::min(rel / kFullBlock, blocks - 1ull);
                const uint64_t b_start = bi * kFullBlock;
                const uint64_t raw_off = bi * 65535ull;
                const uint16_t blen = static_cast<uint16_t>(std::min<uint64_t>(65535ull, sl.raw_size - raw_off));
                const uint16_t nlen = static_cast<uint16_t>(~blen);
                const uint8_t bhdr[5] = {
                    static_cast<uint8_t>((bi + 1ull == blocks) ? 0x01u : 0x00u),
                    static_cast<uint8_t>(blen & 0xFFu), static_cast<uint8_t>(blen >> 8),
                    static_cast<uint8_t>(nlen & 0xFFu), static_cast<uint8_t>(nlen >> 8),
                };
                const uint64_t in_b = rel - b_start;
                if (in_b < 5ull) {
                    const uint64_t n = std::min(len, 5ull - in_b);
                    std::memcpy(dst, bhdr + in_b, static_cast<size_t>(n));
                    dst += n; at += n; len -= n;
                } else {
                    const uint64_t in_data = in_b - 5ull;
                    const uint64_t n = std::min(len, static_cast<uint64_t>(blen) - in_data);
                    if (!read_rec(sl.path, raw_off + in_data, dst, n)) return false;
                    dst += n; at += n; len -= n;
                }
            } else {
                const uint64_t n = std::min(len, sl.size - at);
                std::memcpy(dst, trailer + (at - trailer_start), static_cast<size_t>(n));
                dst += n; at += n; len -= n;
            }
        }
        return true;
    }

    bool read_tail(uint64_t at, uint8_t* dst, uint64_t len) const {
        if (at > ard_tail_size || len > ard_tail_size - at || ard_tails.empty()) return false;
        while (len > 0) {
            auto it = std::upper_bound(ard_tails.begin(), ard_tails.end(), at,
                                       [](uint64_t v, const TailSlice& s) { return v < s.offset; });
            if (it == ard_tails.begin()) return false;
            --it;
            const uint64_t in_slice = at - it->offset;
            if (in_slice >= it->size) return false;
            const uint64_t chunk = std::min(len, it->size - in_slice);
            const bool ok = (it->kind == TailSlice::Kind::StoredXbc1)
                ? read_stored_xbc1(*it, in_slice, dst, chunk)
                : read_rec(it->path, in_slice, dst, chunk);
            if (!ok) return false;
            dst += chunk;
            at += chunk;
            len -= chunk;
        }
        return true;
    }

    static constexpr uint64_t kArdHeader = 0x30;

    bool read_rec(const std::wstring& path, uint64_t at, uint8_t* dst, uint64_t len, uint64_t* file_size = nullptr) const {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        if (file_size) {
            LARGE_INTEGER sz{};
            if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
                CloseHandle(h);
                return false;
            }
            *file_size = static_cast<uint64_t>(sz.QuadPart);
        }
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(at);
        bool ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
        if (ok) {
            DWORD got = 0;
            ok = ReadFile(h, dst, static_cast<DWORD>(len), &got, nullptr) != 0 && got == len;
        }
        CloseHandle(h);
        return ok;
    }

    bool serve_ard(uint64_t off, uint8_t* dst, uint64_t len) const {
        if (ard_index == static_cast<size_t>(-1) || ard_recs.empty()) return false;
        std::wstring hit_path;
        uint64_t hit_rel = 0;
        {
            std::lock_guard<std::mutex> lk(ard_mu);
            auto it = ard_at.upper_bound(off);
            if (it != ard_at.begin()) {
                --it;
                const uint64_t rel = off - it->first;
                if (rel < it->second.size && rel + len <= it->second.size) {
                    hit_path = it->second.path;
                    hit_rel = rel;
                }
            }
        }
        if (!hit_path.empty()) return read_rec(hit_path, hit_rel, dst, len);
        uint8_t hdr[kArdHeader];
        if (!read_host(static_cast<uint32_t>(ard_index), off, hdr, kArdHeader)) return false;
        if (std::memcmp(hdr, "xbc1", 4) != 0) return false;
        std::string name;
        for (size_t i = 0x14; i < kArdHeader && hdr[i] != 0; ++i) name.push_back(static_cast<char>(hdr[i]));
        std::string low = name;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = ard_recs.find(low);
        if (it == ard_recs.end()) return false;
        ArdRec hit{it->second, 0};
        if (!read_rec(hit.path, 0, dst, len, &hit.size)) return false;
        {
            std::lock_guard<std::mutex> lk(ard_mu);
            ard_at[off] = hit;
        }
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 200)
            NXLOG_I("mods", "mods: ard: %s served from a mod record", name.c_str());
        return true;
    }
};

RomfsImage::RomfsImage() : p_(std::make_unique<Impl>()) {}
RomfsImage::~RomfsImage() = default;

bool RomfsImage::build(const std::string& host_dir, std::string* err) {
    Layout fresh;
    const bool ok = make_layout(host_dir, fresh, err);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cache.clear();
    p_->l = ok ? std::move(fresh) : Layout{};
    p_->read_seen.assign(p_->l.files.size(), 0);
    p_->ard_recs = std::move(p_->l.ard_recs);
    p_->ard_tails = std::move(p_->l.ard_tails);
    p_->ard_tail_size = p_->l.ard_tail_size;
    p_->ard_index = static_cast<size_t>(-1);
    p_->ard_real_size = 0;
    {
        std::lock_guard<std::mutex> ard_lk(p_->ard_mu);
        p_->ard_at.clear();
        for (const auto& kv : p_->l.ard_prebound) {
            p_->ard_at[kv.first] = Impl::ArdRec{kv.second.first, kv.second.second};
        }
    }
    if (ok) {
        auto it = p_->l.by_path.find("/bf2.ard");
        if (it != p_->l.by_path.end()) {
            p_->ard_index = it->second;
            // The real size is the grown size minus the tail; the on-disk file stops there.
            p_->ard_real_size = p_->l.files[p_->ard_index].size - p_->ard_tail_size;
        }
    }
    return ok;
}

bool RomfsImage::built() const { return p_->l.built; }
uint64_t RomfsImage::size() const { return p_->l.size; }
RomfsImage::Stats RomfsImage::stats() const { return p_->l.stats; }

bool RomfsImage::read(uint64_t offset, void* dst, uint64_t len) const {
    const Layout& L = p_->l;
    if (!L.built || offset > L.size || len > L.size - offset) return false;
    if (len == 0) return true;
    // M2 probe (temporary): the first 240 reads that touch the game's archive, by file offset and length. [log] level =
    // debug turns it on; the counter keeps a busy run from filling the log. Used to find out whether the game reads a
    // record as 'header first, then the size the header states' - which decides how an archive override can be served.
    {
        static std::atomic<uint32_t> n_ard_reads{0};
        if (n_ard_reads.load(std::memory_order_relaxed) < 240) {
            uint64_t aid_off = 0, aid_size = 0;
            if (file_extent("/bf2.ard", &aid_off, &aid_size) && offset < aid_off + aid_size && offset + len > aid_off) {
                const uint32_t k = n_ard_reads.fetch_add(1, std::memory_order_relaxed);
                if (k < 240)
                    NXLOG_D("fs", "ard read #%u: off 0x%llx len %llu", k, static_cast<unsigned long long>(offset - aid_off),
                            static_cast<unsigned long long>(len));
            }
        }
    }
    uint8_t* out = static_cast<uint8_t*>(dst);
    auto it = std::upper_bound(L.segs.begin(), L.segs.end(), offset,
                               [](uint64_t v, const Segment& g) { return v < g.off; });
    size_t i = static_cast<size_t>(it - L.segs.begin()) - 1;
    while (len > 0) {
        const Segment& g = L.segs[i];
        const uint64_t within = offset - g.off;
        uint64_t n = std::min(len, g.len - within);
        bool advance = true;
        switch (g.kind) {
            case Segment::Kind::kHeader: std::memcpy(out, L.header.data() + g.src + within, n); break;
            case Segment::Kind::kTables: std::memcpy(out, L.tables.data() + g.src + within, n); break;
            case Segment::Kind::kZero: std::memset(out, 0, n); break;
            case Segment::Kind::kBlob:
                p_->note_read(g.file);
                if (g.src + within + n > L.blob.size()) return false;
                std::memcpy(out, L.blob.data() + g.src + within, n);
                break;
            case Segment::Kind::kFile:
                p_->note_read(g.file);
                if (g.file == p_->ard_index && p_->ard_tail_size > 0) {
                    const uint64_t file_off = g.src + within;
                    if (file_off >= p_->ard_real_size) {
                        const uint64_t at = file_off - p_->ard_real_size;
                        if (!p_->read_tail(at, out, n)) return false;
                        static std::atomic<uint32_t> tail_logged{0};
                        if (tail_logged.fetch_add(1, std::memory_order_relaxed) < 8)
                            NXLOG_I("mods", "mods: ard: tail read at +0x%llx, %llu byte(s)", static_cast<unsigned long long>(at),
                                    static_cast<unsigned long long>(n));
                        break;
                    }
                    if (file_off + n > p_->ard_real_size) {
                        n = p_->ard_real_size - file_off;
                        advance = false;
                    }
                }
                if (g.file == p_->ard_index && !p_->ard_recs.empty() && p_->serve_ard(g.src + within, out, n)) break;
                if (!p_->read_host(g.file, g.src + within, out, n)) return false;
                break;
        }
        out += n;
        offset += n;
        len -= n;
        if (advance) i++;
    }
    return true;
}

bool RomfsImage::file_extent(const std::string& path, uint64_t* offset, uint64_t* size) const {
    const Layout& L = p_->l;
    auto it = L.by_path.find(path);
    if (it == L.by_path.end()) return false;
    const FileRec& f = L.files[it->second];
    if (offset) *offset = f.image_off;
    if (size) *size = f.size;
    return true;
}

}  // namespace nxrt::services::fs
