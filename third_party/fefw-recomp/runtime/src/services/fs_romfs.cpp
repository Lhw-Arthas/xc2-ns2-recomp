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
// A layout that names another title id is ignored, so one mod folder can hold content for several games.
std::filesystem::path mod_romfs_root(const std::filesystem::path& mod) {
    std::error_code ec;
    auto dir = [&ec](const std::filesystem::path& p) {
        return std::filesystem::is_directory(p, ec) ? p : std::filesystem::path();
    };
    if (std::filesystem::path plain = dir(mod / "romfs"); !plain.empty()) return plain;
    const uint64_t app = mod_app_id();
    if (app == 0) return {};
    const std::string id = hex16(app);
    for (const char* base : {"atmosphere/contents", "contents"}) {
        if (std::filesystem::path q = dir(mod / base / id / "romfs"); !q.empty()) return q;
    }
    return dir(mod / id / "romfs");
}

Node* find_kid(Node& dir, const std::string& name) {
    for (Node& k : dir.kids)
        if (k.name == name) return &k;
    return nullptr;
}

// Lays one mod's tree over dir. A file the game already has replaces it; a file it does not have is added - mods may
// ship new assets, and tools/mods/mod_check.py is where a wrong game version is diagnosed.
void overlay_dir(Node& dir, const std::filesystem::path& host, ModStats& st) {
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
            overlay_dir(*kid, it->path(), st);
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
        if (kid) {
            kid->host = long_path(it->path().wstring());
            kid->size = size;
            kid->src = 0;   // the mod file starts at 0: src is the extent inside a .nup, and mods never apply to one
            st.replaced++;
        } else {
            Node k;
            k.name = name;
            k.path = dir.path + "/" + name;
            k.host = long_path(it->path().wstring());
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
struct ModsResult {
    std::map<std::string, std::wstring> recs;   // lower-case logical name -> <mod>/.ard/<name>.rec
    std::wstring tail;                          // <mod>/.ard/append.bin of the highest-priority mod that has one
    uint64_t tail_size = 0;
    std::string tail_owner;
};

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
        if (std::filesystem::is_directory(it->symlink_status(ec))) present.push_back(name);
    }
    if (present.empty()) {
        NXLOG_I("mods", "mods: %s is empty", dir_s.c_str());
        return {};
    }
    std::sort(present.begin(), present.end());
    const std::vector<std::string> order = config_name_list("order");
    const std::vector<std::string> disabled = config_name_list("disabled");

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
        const std::filesystem::path r = mod_romfs_root(dir / name);
        if (r.empty()) {
            NXLOG_W("mods", "mods: %s has no romfs folder: skipped", name.c_str());
            continue;
        }
        ModStats st;
        st.name = name;
        overlay_dir(root, r, st);
        // Pre-built archive records for this mod (tools/mods/ard_pack.py): <mod>/.ard/<name>.rec
        {
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
                    for (char& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    out.recs[rel] = long_path(it->path().wstring());
                }
                NXLOG_I("mods", "mods: %s: %zu archive record(s)", name.c_str(), out.recs.size() - before);
                std::error_code tail_ec;
                const std::filesystem::path tail = recs_dir / "append.bin";
                if (std::filesystem::is_regular_file(tail, tail_ec)) {
                    if (!out.tail.empty()) NXLOG_W("mods", "mods: %s and %s both append to the archive; %s wins", out.tail_owner.c_str(), name.c_str(), name.c_str());
                    out.tail = long_path(tail.wstring());
                    out.tail_size = std::filesystem::file_size(tail, tail_ec);
                    out.tail_owner = name;
                    NXLOG_I("mods", "mods: %s: %llu byte(s) appended after the archive", name.c_str(), static_cast<unsigned long long>(out.tail_size));
                }
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
    enum class Kind : uint8_t { kHeader, kTables, kZero, kFile };
    uint64_t off = 0;            // image offset
    uint64_t len = 0;
    Kind kind = Kind::kZero;
    uint64_t src = 0;            // kHeader/kTables: offset in that buffer; kFile: offset in the host file
    uint32_t file = 0;           // kFile: index into Layout::files
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
    std::vector<Segment> segs;          // contiguous, covering [0, size)
    std::vector<FileRec> files;         // in data order
    std::map<std::string, uint32_t> by_path;
    std::map<std::string, std::wstring> ard_recs;   // M2: lower-case name -> <mod>/.ard/<name>.rec
    std::wstring ard_tail;                          // M2b: <mod>/.ard/append.bin, served past the real archive
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
        ModsResult mr = apply_mods(root, abs);
        L.ard_recs = std::move(mr.recs);
        if (!mr.tail.empty() && mr.tail_size > 0) {
            // The image is ours to build, so the archive file can simply be longer than the archive: the bytes past the
            // real size come from the mod's append.bin, and the entry we repointed reads them. Nothing copies 15 GB.
            for (Node& k : root.kids) {
                if (k.is_dir || k.name != "bf2.ard") continue;
                k.size += mr.tail_size;
                L.ard_tail = mr.tail;
                L.ard_tail_size = mr.tail_size;
                NXLOG_I("mods", "mods: bf2.ard grown by %llu byte(s) for %s", static_cast<unsigned long long>(mr.tail_size),
                        mr.tail_owner.c_str());
                break;
            }
            if (L.ard_tail.empty()) NXLOG_W("mods", "mods: %s appends to the archive but this romfs has no bf2.ard", mr.tail_owner.c_str());
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
        if (f->size > 0) L.segs.push_back({img, f->size, Segment::Kind::kFile, f->src, idx});
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
    size_t ard_index = static_cast<size_t>(-1);          // the file index of bf2.ard, when the romfs holds one
    std::map<std::string, std::wstring> ard_recs;         // lower-case logical name -> <mod>/.ard/<name>.rec
    mutable std::map<uint64_t, std::wstring> ard_at;      // record offset -> .rec, learned when its header is read
    std::wstring ard_tail;                               // bytes past the real archive, served from a mod's append.bin
    uint64_t ard_tail_size = 0;
    uint64_t ard_real_size = 0;                          // the on-disk archive size, before growning for the tail
    mutable std::mutex ard_mu;

    static constexpr uint64_t kArdHeader = 0x30;

    bool read_rec(const std::wstring& path, uint64_t at, uint8_t* dst, uint64_t len) const {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
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
        // The payload read starts 0x30 bytes past a record whose header was already served.
        if (off >= kArdHeader) {
            std::lock_guard<std::mutex> lk(ard_mu);
            auto it = ard_at.find(off - kArdHeader);
            if (it != ard_at.end()) return read_rec(it->second, kArdHeader, dst, len);
        }
        std::wstring rec;
        {
            std::lock_guard<std::mutex> lk(ard_mu);
            auto it = ard_at.find(off);
            if (it != ard_at.end()) rec = it->second;
        }
        if (rec.empty()) {
            uint8_t hdr[kArdHeader];
            if (!read_host(static_cast<uint32_t>(ard_index), off, hdr, kArdHeader)) return false;
            if (std::memcmp(hdr, "xbc1", 4) != 0) return false;
            std::string name;
            for (size_t i = 0x14; i < kArdHeader && hdr[i] != 0; ++i) name.push_back(static_cast<char>(hdr[i]));
            std::string low = name;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = ard_recs.find(low);
            if (it == ard_recs.end()) return false;
            rec = it->second;
            {
                std::lock_guard<std::mutex> lk(ard_mu);
                ard_at[off] = rec;
            }
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 200)
                NXLOG_I("mods", "mods: ard: %s served from a mod record", name.c_str());
        }
        return read_rec(rec, 0, dst, len);
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
    p_->ard_tail = std::move(p_->l.ard_tail);
    p_->ard_tail_size = p_->l.ard_tail_size;
    p_->ard_real_size = 0;
    if (ok && p_->ard_index != static_cast<size_t>(-1)) {
        // The real size is the grown size minus the tail; the on-disk file stops there.
        p_->ard_real_size = p_->l.files[p_->ard_index].size - p_->ard_tail_size;
    }
    p_->ard_index = static_cast<size_t>(-1);
    if (ok) {
        auto it = p_->l.by_path.find("/bf2.ard");
        if (it != p_->l.by_path.end()) p_->ard_index = it->second;
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
        const uint64_t n = std::min(len, g.len - within);
        switch (g.kind) {
            case Segment::Kind::kHeader: std::memcpy(out, L.header.data() + g.src + within, n); break;
            case Segment::Kind::kTables: std::memcpy(out, L.tables.data() + g.src + within, n); break;
            case Segment::Kind::kZero: std::memset(out, 0, n); break;
            case Segment::Kind::kFile:
                p_->note_read(g.file);
                if (g.file == p_->ard_index && !p_->ard_recs.empty() && p_->serve_ard(g.src + within, out, n)) break;
                if (g.file == p_->ard_index && p_->ard_tail_size > 0 && g.src + within >= p_->ard_real_size) {
                    const uint64_t at = g.src + within - p_->ard_real_size;
                    if (at + n > p_->ard_tail_size || !p_->read_rec(p_->ard_tail, at, out, n)) return false;
                    static std::atomic<uint32_t> tail_logged{0};
                    if (tail_logged.fetch_add(1, std::memory_order_relaxed) < 8)
                        NXLOG_I("mods", "mods: ard: tail read at +0x%llx, %llu byte(s)", static_cast<unsigned long long>(at),
                                static_cast<unsigned long long>(n));
                    break;
                }
                if (!p_->read_host(g.file, g.src + within, out, n)) return false;
                break;
        }
        out += n;
        offset += n;
        len -= n;
        i++;
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
