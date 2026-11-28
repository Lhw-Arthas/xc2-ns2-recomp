// SPDX-License-Identifier: GPL-3.0-or-later
// fsp-srv (S3c): the file system service of the application.
//   * RomFS: OpenDataStorageByCurrentProcess returns an IStorage over a RomFS image synthesised from the loose game folder
//     ([fs] romfs, default game/romfs; fs_romfs.h). The SDK parses the RomFS itself (nn::fs::MountRom), so the storage is byte addressed.
//     OpenDataFileSystemByCurrentProcess serves the same folder as a read-only IFileSystem.
//   * Save data: account, device and cache saves live in folders under [fs] save_dir (default %APPDATA%\<game save_folder>\save, nxrt/game.h; MPRRecomp for Metroid Prime Remastered) with the journal
//     semantics of fs_volume.h: writes are invisible to the committed state until IFileSystem::Commit.
//   * SD card, game card, BIS: absent (the results of a console without them).
// Command ids, argument layouts: Switchbrew "Filesystem services", ScarletNX fs.swipc (GPL-3.0-or-later), Ryujinx IFileSystemProxy (MIT).
// The commit/recovery design is smo-recomp runtime/fs/nnfs.c (GPL-3.0), see fs_volume.h.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "fs_romfs.h"
#include "nxrt/nup.h"
#include "fs_volume.h"

namespace nxrt::media {
bool media_film_active();   // video_codec.cpp: a film's decoder is alive right now
}
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace stdfs = std::filesystem;

namespace nxrt::services {

namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;
using fs::Volume;

constexpr size_t kPathBuf = 0x301;
constexpr uint32_t kUnsupportedWrite = fs::rc::kReadOnlyFileSystem;

// ---- process-wide state ---------------------------------------------------------------------------------------------------------

struct FsState {
    std::mutex mu;
    std::shared_ptr<fs::RomfsImage> rom;
    bool rom_failed = false;
    std::map<std::string, std::weak_ptr<Volume>> saves;   // key -> open volume (one at a time per save)
    std::map<uint64_t, std::string> ids;                  // SaveDataId -> key, for the commands that name a save by id
};
FsState& st() {
    static FsState s;
    return s;
}

// ---- save data keys ---------------------------------------------------------------------------------------------------------------

struct SaveAttr {
    uint64_t application_id = 0;
    uint8_t uid[16] = {};
    uint64_t system_id = 0;
    uint8_t type = 0, rank = 0;
    uint16_t index = 0;
    uint8_t raw[0x40] = {};
};

SaveAttr parse_attr(const uint8_t* p) {
    SaveAttr a;
    std::memcpy(a.raw, p, 0x40);
    std::memcpy(&a.application_id, p, 8);
    std::memcpy(a.uid, p + 8, 16);
    std::memcpy(&a.system_id, p + 24, 8);
    a.type = p[32];
    a.rank = p[33];
    std::memcpy(&a.index, p + 34, 2);
    if (a.application_id == 0) a.application_id = profile().program_id;
    return a;
}

std::string hex_of(const uint8_t* p, size_t n) {
    char b[3];
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// The folder name (relative, '/' separated) of a save: SaveDataType 0 System, 1 Account, 2 Bcat, 3 Device, 4 Temporary, 5 Cache, 6 SystemBcat.
std::string save_key(const SaveAttr& a) {
    std::string k;
    if (a.application_id != profile().program_id) {
        char b[24];
        std::snprintf(b, sizeof b, "app%016llx/", static_cast<unsigned long long>(a.application_id));
        k = b;
    }
    char b[40];
    switch (a.type) {
        case 1: k += "account/" + hex_of(a.uid, 16); break;
        case 3: k += "device"; break;
        case 5: std::snprintf(b, sizeof b, "cache/%u", a.index); k += b; break;
        case 0:
        case 6: std::snprintf(b, sizeof b, "system/%016llx", static_cast<unsigned long long>(a.system_id)); k += b; break;
        case 2: k += "bcat"; break;
        default: std::snprintf(b, sizeof b, "temporary/%u", a.index); k += b; break;
    }
    return k;
}

uint64_t save_id_of(const std::string& key) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    return h;
}

stdfs::path root_of(const std::string& key) {
    stdfs::path p = stdfs::u8path(save_dir());
    p /= stdfs::u8path(key);
    return p;
}
stdfs::path meta_of(const std::string& key) { return stdfs::path(root_of(key).string() + ".meta"); }

struct SaveMeta {
    int64_t size = 0, journal = 0;
    uint64_t owner = 0;
    std::string attr_hex;
    bool ok = false;
};
SaveMeta read_meta(const std::string& key) {
    SaveMeta m;
    std::ifstream in(meta_of(key));
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "size") m.size = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "journal") m.journal = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "owner") m.owner = std::strtoull(v.c_str(), nullptr, 16);
        else if (k == "attr") m.attr_hex = v;
        m.ok = true;
    }
    return m;
}
void write_meta(const std::string& key, const SaveMeta& m) {
    std::error_code ec;
    stdfs::create_directories(meta_of(key).parent_path(), ec);
    const stdfs::path tmp(meta_of(key).string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "size=" << m.size << "\njournal=" << m.journal << "\nowner=" << std::hex << m.owner << std::dec << "\nattr=" << m.attr_hex << "\n";
    }
    stdfs::rename(tmp, meta_of(key), ec);
}

bool save_exists(const std::string& key) {
    std::error_code ec;
    return stdfs::exists(meta_of(key), ec) && stdfs::is_directory(root_of(key), ec);
}

uint32_t create_save(const SaveAttr& a, int64_t size, int64_t journal, uint64_t owner) {
    const std::string key = save_key(a);
    if (save_exists(key)) return fs::rc::kPathAlreadyExists;
    std::error_code ec;
    stdfs::create_directories(root_of(key), ec);
    if (ec) return fs::rc::kGenericFailure;
    SaveMeta m;
    m.size = size;
    m.journal = journal;
    m.owner = owner ? owner : profile().save_data_owner_id;
    m.attr_hex = hex_of(a.raw, 0x40);
    write_meta(key, m);
    std::lock_guard<std::mutex> lk(st().mu);
    st().ids[save_id_of(key)] = key;
    NXLOG_I("fs", "save data created: %s (%lld bytes, journal %lld)", key.c_str(), static_cast<long long>(size), static_cast<long long>(journal));
    return 0;
}

std::shared_ptr<Volume> open_save_volume(const std::string& key, bool read_only) {
    std::lock_guard<std::mutex> lk(st().mu);
    auto it = st().saves.find(key);
    if (!read_only && it != st().saves.end())
        if (auto v = it->second.lock()) return v;
    const SaveMeta m = read_meta(key);
    auto v = std::make_shared<Volume>(root_of(key).string(), true, read_only, static_cast<uint64_t>(m.size));
    if (!v->ok()) return nullptr;
    if (!read_only) st().saves[key] = v;
    st().ids[save_id_of(key)] = key;
    return v;
}

// ---- the interfaces -----------------------------------------------------------------------------------------------------------

bool get_path(const Request& req, size_t n, std::string* out, uint32_t* err) {
    const ipc::Buffer b = req.in_buf(n);
    if (!b.addr) {
        *err = fs::rc::kInvalidArgument;
        return false;
    }
    return fs::normalize_path(reinterpret_cast<const char*>(static_cast<uintptr_t>(b.addr)), static_cast<size_t>(std::min<uint64_t>(b.size, kPathBuf)), out, err);
}

#define PATH_OR_FAIL(var, n)                       \
    std::string var;                               \
    {                                              \
        uint32_t perr = 0;                         \
        if (!get_path(req, n, &var, &perr)) return perr; \
    }

// Gauges (include/nxrt/gauge.h). An open file or directory counts while its service object lives: the command handlers hold OpenCount.
GaugeCounter g_open_files("fs.open_files");
GaugeCounter g_open_dirs("fs.open_dirs");
GaugeCounter g_bytes_read("fs.bytes_read");   // bytes returned by IFile::Read so far
struct OpenCount {
    explicit OpenCount(GaugeCounter& g) : c(g) { c.add(1); }
    ~OpenCount() { c.sub(1); }
    OpenCount(const OpenCount&) = delete;
    OpenCount& operator=(const OpenCount&) = delete;
    GaugeCounter& c;
};

std::shared_ptr<Service> make_file(std::shared_ptr<fs::VFile> f, const std::string& path) {
    auto open = std::make_shared<OpenCount>(g_open_files);
    return make_iface("nn::fssrv::sf::IFile", [f, path, open](Service& s) {
        s.add_cmd(0, "Read", [f, open](Request& req, Response& rsp) -> uint32_t {
            const int64_t off = req.arg<int64_t>(8);
            const uint64_t size = req.arg<uint64_t>(16);
            const ipc::Buffer out = req.out_buf(0);
            if (!out.addr && size) return fs::rc::kInvalidArgument;
            uint64_t got = 0;
            const uint32_t r = f->read(off, reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), std::min(size, out.size), &got);
            if (r) return r;
            g_bytes_read.add(static_cast<int64_t>(got));
            rsp.push<uint64_t>(got);
            return 0;
        });
        s.add_cmd(1, "Write", [f](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(8);
            const uint64_t size = req.arg<uint64_t>(16);
            const ipc::Buffer in = req.in_buf(0);
            if (!in.addr && size) return fs::rc::kInvalidArgument;
            if (in.size < size) return fs::rc::kInvalidArgument;
            const uint32_t r = f->write(off, reinterpret_cast<const void*>(static_cast<uintptr_t>(in.addr)), size);
            if (r) return r;
            if (req.arg<uint32_t>(0) & 1) return f->flush();   // WriteOption flush
            return 0;
        });
        s.add_cmd(2, "Flush", [f](Request&, Response&) -> uint32_t { return f->flush(); });
        s.add_cmd(3, "SetSize", [f](Request& req, Response&) -> uint32_t { return f->set_size(req.arg<int64_t>(0)); });
        s.add_cmd(4, "GetSize", [f, path](Request&, Response& rsp) -> uint32_t {
            int64_t sz = 0;
            const uint32_t r = f->get_size(&sz);
            if (r) return r;
            // M2b probe: the size the game reads for the archive, and the path it asked about. The patched index entry is
            // accepted only while its offset is below this number, which is how we found out that a synthesized image and the
            // folder view can disagree about a file's size.
            static std::atomic<uint32_t> n{0};
            if (n.load(std::memory_order_relaxed) < 40) {
                const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
                if (k < 40) NXLOG_D("fs", "getsize #%u %s = %lld", k, path.c_str(), static_cast<long long>(sz));
            }
            rsp.push<int64_t>(sz);
            return 0;
        });
        s.add_cmd(5, "OperateRange", [](Request&, Response& rsp) -> uint32_t {   // QueryRangeInfo (0x40 bytes), nothing to report
            rsp.out.insert(rsp.out.end(), 0x40, uint8_t{0});
            return 0;
        });
    });
}

std::shared_ptr<Service> make_dir(std::vector<fs::DirItem> items) {
    auto list = std::make_shared<std::vector<fs::DirItem>>(std::move(items));
    auto pos = std::make_shared<size_t>(0);
    auto open = std::make_shared<OpenCount>(g_open_dirs);
    return make_iface("nn::fssrv::sf::IDirectory", [list, pos, open](Service& s) {
        s.add_cmd(0, "Read", [list, pos, open](Request& req, Response& rsp) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            const size_t cap = static_cast<size_t>(out.size / 0x310);
            size_t n = 0;
            while (n < cap && *pos < list->size()) {
                uint8_t* e = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out.addr)) + n * 0x310;
                std::memset(e, 0, 0x310);
                const fs::DirItem& it = (*list)[*pos];
                std::memcpy(e, it.name.data(), std::min<size_t>(it.name.size(), 0x300));
                e[0x304] = it.is_dir ? 0 : 1;
                const int64_t sz = it.is_dir ? 0 : it.size;
                std::memcpy(e + 0x308, &sz, 8);
                n++;
                (*pos)++;
            }
            rsp.push<int64_t>(static_cast<int64_t>(n));
            return 0;
        });
        s.add_cmd(1, "GetEntryCount", [list](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(static_cast<int64_t>(list->size()));
            return 0;
        });
    });
}

std::shared_ptr<Service> make_filesystem(std::shared_ptr<Volume> v, std::string label) {
    return make_iface("nn::fssrv::sf::IFileSystem", [v, label](Service& s) {
        s.add_cmd(0, "CreateFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->create_file(p, req.arg<int64_t>(8));
        });
        s.add_cmd(1, "DeleteFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_file(p);
        });
        s.add_cmd(2, "CreateDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->create_dir(p);
        });
        s.add_cmd(3, "DeleteDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_dir(p);
        });
        s.add_cmd(4, "DeleteDirectoryRecursively", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->delete_dir_recursive(p);
        });
        s.add_cmd(5, "RenameFile", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(a, 0);
            PATH_OR_FAIL(b, 1);
            return v->rename_file(a, b);
        });
        s.add_cmd(6, "RenameDirectory", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(a, 0);
            PATH_OR_FAIL(b, 1);
            return v->rename_dir(a, b);
        });
        s.add_cmd(7, "GetEntryType", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            uint32_t t = 0;
            const uint32_t r = v->entry_type(p, &t);
            if (r) return r;
            rsp.push<uint32_t>(t);
            return 0;
        });
        s.add_cmd(8, "OpenFile", [v, label](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            std::shared_ptr<fs::VFile> f;
            const uint32_t r = v->open_file(p, req.arg<uint32_t>(0), &f);
            diag_note_file("open", (label + p).c_str());
            // Name every movie file the game opens. Which cutscene is playing was the one fact the film logs could not supply, and
            // the file name carries the family (`mv` feature film, `bf`, `of` short insert, `qst`) that decides how the player is
            // driven (2026-11-22).
            if (p.find("stream/movie") != std::string::npos)
                NXLOG_I("fs", "movie open: %s%s -> 0x%x", label.c_str(), p.c_str(), r);
            if (r) {
                NXLOG_D("fs", "OpenFile %s%s (mode %u) -> 0x%x", label.c_str(), p.c_str(), req.arg<uint32_t>(0), r);
                return r;
            }
            rsp.push_object(make_file(f, p));
            return 0;
        });
        s.add_cmd(9, "OpenDirectory", [v, label](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            std::vector<fs::DirItem> items;
            const uint32_t r = v->list_dir(p, &items);
            diag_note_file("dir", (label + p).c_str());
            if (r) return r;
            const uint32_t mode = req.arg<uint32_t>(0);   // 1 directories, 2 files
            std::vector<fs::DirItem> keep;
            for (const fs::DirItem& it : items)
                if ((it.is_dir && (mode & 1)) || (!it.is_dir && (mode & 2))) keep.push_back(it);
            rsp.push_object(make_dir(std::move(keep)));
            return 0;
        });
        s.add_cmd(10, "Commit", [v](Request&, Response&) -> uint32_t { return v->commit(); });
        s.add_cmd(11, "GetFreeSpaceSize", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            const uint64_t cap = v->capacity();
            const uint64_t used = v->used_bytes();
            rsp.push<int64_t>(cap ? static_cast<int64_t>(cap > used ? cap - used : 0) : (int64_t{1} << 40));
            return 0;
        });
        s.add_cmd(12, "GetTotalSpaceSize", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            rsp.push<int64_t>(v->capacity() ? static_cast<int64_t>(v->capacity()) : (int64_t{1} << 40));
            return 0;
        });
        s.add_cmd(13, "CleanDirectoryRecursively", [v](Request& req, Response&) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            return v->clean_dir_recursive(p);
        });
        s.add_cmd(14, "GetFileTimeStampRaw", [v](Request& req, Response& rsp) -> uint32_t {
            PATH_OR_FAIL(p, 0);
            fs::FileTimes t;
            const uint32_t r = v->file_times(p, &t);
            if (r) return r;
            rsp.push<int64_t>(t.created);
            rsp.push<int64_t>(t.modified);
            rsp.push<int64_t>(t.accessed);
            rsp.push<uint64_t>(t.valid ? 1 : 0);   // is_valid + padding
            return 0;
        });
        s.add_cmd(15, "QueryEntry", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(16, "GetFileSystemAttribute", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.insert(rsp.out.end(), 0xC0, uint8_t{0});
            return 0;
        });
    });
}

// [fs] trace_reads = 1 (NXRT_FS_TRACE_READS, debug): log every storage read's offset and size. A run then maps back onto
// the archive's own file list (work/oracle/ns2_files.tsv) and shows what the engine was reading when it died (HANDOVER 79).
// [fs] trace_stack_file = mnu314,mnu303 (NXRT_FS_TRACE_STACK_FILE, debug): when a read names a file whose name contains one of the
// comma-separated fragments, log the guest's frame chain (x29) as well. That is the "who asked for this file" half of the UI
// question in HANDOVER 87: the 3.0.0 guest loads mnu314_* / mnu303_* at boot and draws them over the title, and the chain names the
// resource loader's callers. The chain is the same one the crash report walks (diag/crash.cpp): lr, then [x29] and [x29+8] per frame.
bool trace_name_matches(const char* name, const std::string& spec) {
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t comma = spec.find(',', pos);
        const size_t end = comma == std::string::npos ? spec.size() : comma;
        const std::string frag = spec.substr(pos, end - pos);
        if (!frag.empty() && std::strstr(name, frag.c_str())) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

// HANDOVER 95.14: the parser of /monolib/shader/lib_nx.ini is main+0x7c9fe0 (found with [fs] trace_stack_file); its
// caller creates the settings object (a 0x3190-byte allocation) and calls it, keeping the object in x19. Walking the
// frame chain to the frame that returns into main+0x7cd96c gives that frame's x29, and the parser saves x19 at [x29+0x50]
// (prologue: stp x20, x19, [sp, #0x60] with x29 = sp + 0x10). [upscale] pin_render_scale then clamps the 0.7 fields.
std::atomic<uint64_t> g_ini_settings{0};
std::atomic<uint64_t> g_ini_settings2{0};

uint64_t ini_settings_object() { return g_ini_settings.load(std::memory_order_relaxed); }
uint64_t ini_settings_object2() { return g_ini_settings2.load(std::memory_order_relaxed); }

void note_ini_settings(uint64_t fp, uint64_t ret) {
    if (ret != 0x71007cd96cULL || !fp || !nxrt::mem::is_mapped(fp + 0x60, 0x20)) return;
    // prologue: stp x20, x19, [sp, #0x60] with x29 = sp + 0x10 -> x20 at [x29+0x50], x19 at [x29+0x58]
    uint64_t x20 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(fp + 0x50));
    uint64_t x19 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(fp + 0x58));
    if (!x19) return;
    g_ini_settings.store(x19, std::memory_order_relaxed);    // the settings object (the parser did "mov x19, x0")
    g_ini_settings2.store(x20, std::memory_order_relaxed);   // its neighbour, logged for reference
    NXLOG_I("fs", "lib_nx.ini settings object: 0x%llx (its parser kept it in x19; x20 was 0x%llx)",
            static_cast<unsigned long long>(x19), static_cast<unsigned long long>(x20));
}

void trace_guest_stack(const char* name) {
    static const std::string spec = config_get_str("fs", "trace_stack_file", "");
    if (spec.empty() || !name || !name[0] || !trace_name_matches(name, spec)) return;
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) >= 40) return;
    ::Ctx* c = nxrt::kernel::current_ctx();
    if (!c) return;
    char line[640];
    int len = std::snprintf(line, sizeof line, "stack for %s: lr=0x%llx", name, static_cast<unsigned long long>(c->x[30]));
    uint64_t f = c->x[29];
    for (int i = 0; i < 16 && f && nxrt::mem::is_mapped(f, 16) && len > 0 && len < static_cast<int>(sizeof line) - 24; ++i) {
        const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f));
        const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f + 8));
        if (ret) len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " <- 0x%llx", static_cast<unsigned long long>(ret));
        note_ini_settings(f, ret);
        if (next <= f) break;   // frames go up the stack; anything else is a broken chain
        f = next;
    }
    NXLOG_I("fs", "%s", line);
    // HANDOVER 95.14: the callee-saved registers are still those of the loader that opened the file (the parser keeps its
    // object in x19..x28), which is how the settings object behind /monolib/shader/lib_nx.ini is found without a debugger.
    NXLOG_I("fs", "regs at %s: x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx x25=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx",
           name, static_cast<unsigned long long>(c->x[19]), static_cast<unsigned long long>(c->x[20]),
           static_cast<unsigned long long>(c->x[21]), static_cast<unsigned long long>(c->x[22]),
           static_cast<unsigned long long>(c->x[23]), static_cast<unsigned long long>(c->x[24]),
           static_cast<unsigned long long>(c->x[25]), static_cast<unsigned long long>(c->x[26]),
           static_cast<unsigned long long>(c->x[27]), static_cast<unsigned long long>(c->x[28]));
}

void trace_read(const fs::RomfsImage* img, int64_t off, int64_t size) {
    // While a film's decoder is alive, every read is logged regardless of the [fs] trace's line budget: the loading screen
    // spends that budget long before a cutscene plays, and "the demuxer read 540 KB and stopped" needs the reads of the film's
    // own file (2026-11-22).
    if (nxrt::media::media_film_active()) {
        static std::atomic<uint32_t> fn{0};
        if (fn.fetch_add(1, std::memory_order_relaxed) < 20000)
            NXLOG_I("fs", "film read off=%lld size=%lld", static_cast<long long>(off), static_cast<long long>(size));
    }
    static const bool on = config_get_bool("fs", "trace_reads", false);
    static const bool stack_on = !config_get_str("fs", "trace_stack_file", "").empty();
    static const bool tag_on = config_get_bool("fs", "tag_textures", false);
    if ((!on && !stack_on && !tag_on) || !img) return;
    // A 0x30-byte read at a record start is the engine fetching a file header: the xbc1 record carries the file's own name at
    // +0x14, so the trace names the file without any offset bookkeeping on our side.
    char name[29] = {};
    if (size == 0x30) {
        uint8_t hdr[0x30];
        if (img->read(static_cast<uint64_t>(off), hdr, sizeof hdr) && std::memcmp(hdr, "xbc1", 4) == 0)
            std::memcpy(name, hdr + 0x14, 28);
    }
    if (stack_on) trace_guest_stack(name);
    // [fs] tag_textures (NXRT_FS_TAG_TEXTURES, HANDOVER 87): name the textures this thread creates next after the resource it just read.
    if (tag_on && name[0]) {
        debug_note_file_read(name);
        static std::atomic<int> tag_logs{0};
        if (tag_logs.fetch_add(1) < 15) NXLOG_I("fs", "tag: %s", name);
    }
    if (!on) return;
    static uint64_t n = 0;
    if (n++ >= 6000) return;
    NXLOG_I("fs", "read off=%lld size=%lld %s%s%s", static_cast<long long>(off), static_cast<long long>(size),
            name[0] ? "file=" : "", name, name[0] ? "" : "");
}

std::shared_ptr<Service> make_storage_rom(std::shared_ptr<fs::RomfsImage> img) {
    return make_iface("nn::fssrv::sf::IStorage", [img](Service& s) {
        s.add_cmd(0, "Read", [img](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(0);
            const int64_t size = req.arg<int64_t>(8);
            trace_read(img.get(), off, size);
            if (off < 0 || size < 0) return fs::rc::kOutOfRange;
            if (size == 0) return 0;
            const ipc::Buffer out = req.out_buf(0);
            if (!out.addr || out.size < static_cast<uint64_t>(size)) return fs::rc::kInvalidArgument;
            if (static_cast<uint64_t>(off) + static_cast<uint64_t>(size) > img->size()) return fs::rc::kOutOfRange;
            return img->read(static_cast<uint64_t>(off), reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), static_cast<uint64_t>(size)) ? 0 : fs::rc::kGenericFailure;
        });
        s.add_cmd(1, "Write", [](Request&, Response&) -> uint32_t { return kUnsupportedWrite; });
        s.add_cmd(2, "Flush", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(3, "SetSize", [](Request&, Response&) -> uint32_t { return kUnsupportedWrite; });
        s.add_cmd(4, "GetSize", [img](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(static_cast<int64_t>(img->size()));
            return 0;
        });
        s.add_cmd(5, "OperateRange", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.insert(rsp.out.end(), 0x40, uint8_t{0});
            return 0;
        });
    });
}

std::shared_ptr<fs::RomfsImage> rom_image() {
    std::lock_guard<std::mutex> lk(st().mu);
    if (st().rom) return st().rom;
    if (st().rom_failed) return nullptr;
    auto img = std::make_shared<fs::RomfsImage>();
    std::string err;
    const std::string dir = romfs_dir();
    if (!img->build(dir, &err)) {
        NXLOG_E("fs", "RomFS folder '%s' cannot be served: %s (set [fs] romfs)", dir.c_str(), err.c_str());
        st().rom_failed = true;
        return nullptr;
    }
    const fs::RomfsImage::Stats s = img->stats();
    NXLOG_I("fs", "RomFS image from %s: %u files, %u directories, %llu bytes of data, image size %llu", dir.c_str(), s.files, s.dirs,
            static_cast<unsigned long long>(s.data_bytes), static_cast<unsigned long long>(img->size()));
    st().rom = img;
    return img;
}

// The RomFS of an add-on content: [aoc] dir (default game/dlc) / <16 hex data id> / romfs, the layout the Switch 2
// package extracts to (HANDOVER 63). Built on first use, one image per content.
std::shared_ptr<fs::RomfsImage> aoc_image(uint64_t id) {
    static std::mutex mu;
    static std::map<uint64_t, std::shared_ptr<fs::RomfsImage>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    if (id == 0x0100E95004039062ull) {
        NXLOG_W("aoc", "refusing incompatible Switch 2 v3.0.0 add-on content 0100e95004039062 (index 98): already merged in 3.0.1 base RomFS");
        cache[id] = nullptr;
        return nullptr;
    }
    std::string name(16, '0');
    for (int i = 0; i < 16; ++i) name[15 - i] = "0123456789abcdef"[static_cast<unsigned>(id >> (4 * i)) & 0xF];
    const std::string base = config_get_str("aoc", "dir", "game/dlc") + "/" + name;
    std::string dir = base + "/romfs";
    std::string err;
    auto img = std::make_shared<fs::RomfsImage>();
    if (!img->build(dir, &err)) {
        // Some dumps keep an extra "0" level between the content id and romfs - <aoc>/<id>/0/romfs, as in issue #20.
        // Try that before giving up, and log which one served so a wrong dump is visible in one line.
        const std::string alt = base + "/0/romfs";
        std::string err2;
        auto alt_img = std::make_shared<fs::RomfsImage>();
        if (!alt_img->build(alt, &err2)) {
            NXLOG_W("aoc", "add-on content %s: %s (%s; also tried %s: %s)", name.c_str(), err.c_str(), dir.c_str(), alt.c_str(),
                    err2.c_str());
            cache[id] = nullptr;
            return nullptr;
        }
        img = alt_img;
        dir = alt;
        NXLOG_I("aoc", "add-on content %s: romfs is one level down (%s)", name.c_str(), dir.c_str());
    }
    NXLOG_I("aoc", "add-on content %s served from %s", name.c_str(), dir.c_str());
    cache[id] = img;
    return img;
}

std::shared_ptr<Service> make_event_notifier() {
    auto ev = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::fssrv::sf::IEventNotifier", [ev](Service& s) {
        s.add_cmd(0, "GetEventHandle", [ev](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<Service> make_device_operator() {
    return make_iface("nn::fssrv::sf::IDeviceOperator", [](Service& s) {
        s.add_cmd(0, "IsSdCardInserted", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(200, "IsGameCardInserted", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
    });
}

std::shared_ptr<Service> make_multi_commit() {
    auto list = std::make_shared<std::vector<std::shared_ptr<Service>>>();
    return make_iface("nn::fssrv::sf::IMultiCommitManager", [list](Service& s) {
        s.add_cmd(1, "Add", [list](Request& req, Response&) -> uint32_t {
            if (!req.in_services.empty() && req.in_services[0]) list->push_back(req.in_services[0]);
            return 0;
        });
        s.add_cmd(2, "Commit", [list](Request&, Response&) -> uint32_t {
            // The file systems were added as objects; each one commits through its own Commit command.
            for (auto& fsys : *list) {
                ipc::Request r;
                r.cmd = 10;
                ipc::Response rr;
                const uint32_t res = fsys->handle(r, rr);
                if (res) return res;
            }
            return 0;
        });
    });
}

// SaveDataInfo (0x60 bytes): Switchbrew "Filesystem services".
void fill_info(uint8_t* e, const std::string& key) {
    std::memset(e, 0, 0x60);
    const SaveMeta m = read_meta(key);
    uint8_t attr[0x40] = {};
    for (size_t i = 0; i < 0x40 && i * 2 + 1 < m.attr_hex.size(); ++i) attr[i] = static_cast<uint8_t>(std::strtoul(m.attr_hex.substr(i * 2, 2).c_str(), nullptr, 16));
    const uint64_t id = save_id_of(key);
    std::memcpy(e, &id, 8);
    e[8] = 1;            // SaveDataSpaceId::User
    e[9] = attr[32];     // type
    std::memcpy(e + 0x10, attr + 8, 16);   // user id
    std::memcpy(e + 0x20, attr + 24, 8);   // system save data id
    std::memcpy(e + 0x28, attr, 8);        // application id
    std::memcpy(e + 0x30, &m.size, 8);
    std::memcpy(e + 0x38, attr + 34, 2);   // index
    e[0x3A] = attr[33];                    // rank
}

void collect_saves(std::vector<std::string>* keys) {
    std::error_code ec;
    const stdfs::path base = stdfs::u8path(save_dir());
    for (stdfs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".meta") continue;
        const auto u8rel = it->path().lexically_relative(base).generic_u8string();
        std::string rel(u8rel.begin(), u8rel.end());
        rel.resize(rel.size() - 5);
        keys->push_back(rel);
    }
    std::sort(keys->begin(), keys->end());
}

std::shared_ptr<Service> make_info_reader() {
    auto keys = std::make_shared<std::vector<std::string>>();
    collect_saves(keys.get());
    {
        std::lock_guard<std::mutex> lk(st().mu);
        for (const std::string& k : *keys) st().ids[save_id_of(k)] = k;
    }
    auto pos = std::make_shared<size_t>(0);
    return make_iface("nn::fssrv::sf::ISaveDataInfoReader", [keys, pos](Service& s) {
        s.add_cmd(0, "ReadSaveDataInfo", [keys, pos](Request& req, Response& rsp) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            const size_t cap = static_cast<size_t>(out.size / 0x60);
            size_t n = 0;
            while (n < cap && *pos < keys->size()) fill_info(reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out.addr)) + 0x60 * n++, (*keys)[(*pos)++]);
            rsp.push<int64_t>(static_cast<int64_t>(n));
            return 0;
        });
    });
}

// ---- fsp-srv ----------------------------------------------------------------------------------------------------------------------

// The common tail of the save data opens: space id 1 (User) and 100/0 (system) are served; the SD card spaces are absent.
uint32_t open_save(uint8_t space, const SaveAttr& a, bool read_only, std::shared_ptr<Service>* out) {
    if (space == 2 || space == 4) return fs::rc::kPortSdCardNoDevice;
    const std::string key = save_key(a);
    // The running title's device save data (type 3) exists before it starts on the console: the system creates it at launch from
    // control.nacp's DeviceSaveDataSize (Ryujinx: EnsureApplicationSaveData when the application is loaded). Here it is made on the
    // first open, when the title profile gives a device save size.
    const TitleProfile& p = profile();
    // The save is keyed by the application id the guest passes in the attribute, which is its own NPDM's id, not the profile's. The
    // 3.0.0 NS1 guest runs as 0100C9F009F7A000 while this profile says 0100E95004038000: with a strict id match it never got save data
    // (the title's Continue stayed disabled and the guest could not save at all, HANDOVER 87). A single-title runtime serves whatever
    // id its one guest uses, so the device and account saves are created for that id.
    if (a.type == 3 && p.device_save_size > 0 && !save_exists(key)) {
        SaveAttr d = a;
        d.type = 3;
        d.application_id = a.application_id;
        std::memcpy(d.raw, &d.application_id, 8);
        d.raw[32] = 3;
        create_save(d, static_cast<int64_t>(p.device_save_size), static_cast<int64_t>(p.device_save_journal), p.save_data_owner_id);
    }
    // Likewise the account save data of the user the title starts with (the console's launch path ensures it for the preselected
    // user; Ryujinx EnsureApplicationSaveData with the current profile): some titles mount it without EnsureSaveData first
    // (Mario Kart World: nn::fs::MountSaveData(name, uid) right at boot, and a failure aborts).
    if (a.type == 1 && p.account_save_size > 0 && !save_exists(key)) {
        uint64_t hi = 0, lo = 0;
        std::memcpy(&hi, a.uid, 8);
        std::memcpy(&lo, a.uid + 8, 8);
        if (hi == p.user_uid_hi && lo == p.user_uid_lo) {
            NXLOG_I("fs", "OpenSaveDataFileSystem %s: creating the account save data of the starting user", key.c_str());
            SaveAttr c = a;
            c.type = 1;
            c.application_id = a.application_id;
            std::memcpy(c.raw, &c.application_id, 8);
            std::memcpy(c.raw + 8, c.uid, 16);
            c.raw[32] = 1;
            create_save(c, static_cast<int64_t>(p.account_save_size), static_cast<int64_t>(p.account_save_journal), p.save_data_owner_id);
        }
    }
    if (!save_exists(key)) {
        NXLOG_I("fs", "OpenSaveDataFileSystem %s: no such save data", key.c_str());
        return fs::rc::kTargetNotFound;
    }
    auto v = open_save_volume(key, read_only);
    if (!v) return fs::rc::kGenericFailure;
    diag_note_file("save", key.c_str());
    *out = make_filesystem(v, "save:" + key);
    return 0;
}

std::shared_ptr<Service> make_fsp() {
    auto s = std::make_shared<Service>("fsp-srv");
    s->add_cmd(1, "SetCurrentProcess", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(2, "OpenDataFileSystemByCurrentProcess", [](Request&, Response& rsp) -> uint32_t {
        if (nup::is_nup(romfs_dir())) return fs::rc::kTargetNotFound;    // a .nup is served as the RomFS image (OpenDataStorage) only
        auto v = std::make_shared<Volume>(romfs_dir(), false, true, 0);
        if (!v->ok()) return fs::rc::kTargetNotFound;
        rsp.push_object(make_filesystem(v, "rom:"));
        return 0;
    });
    s->add_cmd(8, "OpenFileSystemWithId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(9, "OpenDataFileSystemByProgramId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(11, "OpenBisFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(18, "OpenSdCardFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kPortSdCardNoDevice; });
    s->add_cmd(19, "FormatSdCardFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kPortSdCardNoDevice; });
    s->add_cmd(22, "CreateSaveDataFileSystem", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x90) return fs::rc::kInvalidArgument;
        const SaveAttr a = parse_attr(req.in);
        const int64_t size = req.arg<int64_t>(0x40), journal = req.arg<int64_t>(0x48);
        const uint64_t owner = req.arg<uint64_t>(0x58);
        return create_save(a, size, journal, owner);
    });
    s->add_cmd(23, "CreateSaveDataFileSystemBySystemSaveDataId", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x80) return fs::rc::kInvalidArgument;
        const SaveAttr a = parse_attr(req.in);
        return create_save(a, req.arg<int64_t>(0x40), req.arg<int64_t>(0x48), req.arg<uint64_t>(0x58));
    });
    s->add_cmd(25, "DeleteSaveDataFileSystemBySaveDataSpaceId", [](Request& req, Response&) -> uint32_t {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(req.arg<uint64_t>(8));
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
            st().saves.erase(key);
        }
        std::error_code ec;
        stdfs::remove_all(root_of(key), ec);
        stdfs::remove(meta_of(key), ec);
        return 0;
    });
    s->add_cmd(27, "IsExFatSupported", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    s->add_cmd(28, "DeleteSaveDataFileSystemBySaveDataAttribute", [](Request& req, Response&) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        const std::string key = save_key(parse_attr(req.in + 8));
        if (!save_exists(key)) return fs::rc::kTargetNotFound;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            st().saves.erase(key);
        }
        std::error_code ec;
        stdfs::remove_all(root_of(key), ec);
        stdfs::remove(meta_of(key), ec);
        return 0;
    });
    s->add_cmd(32, "ExtendSaveDataFileSystem", [](Request& req, Response&) -> uint32_t {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(req.arg<uint64_t>(8));
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
        }
        SaveMeta m = read_meta(key);
        m.size = std::max<int64_t>(m.size, req.arg<int64_t>(16));
        m.journal = std::max<int64_t>(m.journal, req.arg<int64_t>(24));
        write_meta(key, m);
        return 0;
    });
    s->add_cmd(33, "DeleteCacheStorage", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(34, "GetCacheStorageSize", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(51, "OpenSaveDataFileSystem", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), false, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    s->add_cmd(52, "OpenSaveDataFileSystemBySystemSaveDataId", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), false, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    s->add_cmd(53, "OpenReadOnlySaveDataFileSystem", [](Request& req, Response& rsp) -> uint32_t {
        if (req.in_size < 0x48) return fs::rc::kInvalidArgument;
        std::shared_ptr<Service> fsys;
        const uint32_t r = open_save(req.arg<uint8_t>(0), parse_attr(req.in + 8), true, &fsys);
        if (r) return r;
        rsp.push_object(fsys);
        return 0;
    });
    auto extra = [](Request& req, Response&, uint64_t id) -> uint32_t {   // SaveDataExtraData (0x200 bytes) into the output buffer
        std::string key;
        {
            std::lock_guard<std::mutex> lk(st().mu);
            auto it = st().ids.find(id);
            if (it == st().ids.end()) return fs::rc::kTargetNotFound;
            key = it->second;
        }
        const SaveMeta m = read_meta(key);
        uint8_t e[0x200] = {};
        for (size_t i = 0; i < 0x40 && i * 2 + 1 < m.attr_hex.size(); ++i) e[i] = static_cast<uint8_t>(std::strtoul(m.attr_hex.substr(i * 2, 2).c_str(), nullptr, 16));
        std::memcpy(e + 0x40, &m.owner, 8);
        std::memcpy(e + 0x58, &m.size, 8);       // available size
        std::memcpy(e + 0x60, &m.journal, 8);    // journal size
        buf_write(req.out_buf(0), e, sizeof e);
        return 0;
    };
    s->add_cmd(57, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId", [extra](Request& req, Response& rsp) -> uint32_t { return extra(req, rsp, req.arg<uint64_t>(8)); });
    s->add_cmd(58, "ReadSaveDataFileSystemExtraData", [extra](Request& req, Response& rsp) -> uint32_t { return extra(req, rsp, req.arg<uint64_t>(0)); });
    s->add_cmd(59, "WriteSaveDataFileSystemExtraData", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(66, "WriteSaveDataFileSystemExtraData2", [](Request&, Response&) -> uint32_t { return 0; });
    auto reader = [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_info_reader());
        return 0;
    };
    s->add_cmd(60, "OpenSaveDataInfoReader", reader);
    s->add_cmd(61, "OpenSaveDataInfoReaderBySaveDataSpaceId", reader);
    s->add_cmd(62, "OpenSaveDataInfoReaderOnlyCacheStorage", reader);
    s->add_cmd(100, "OpenImageDirectoryFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(110, "OpenContentStorageFileSystem", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    auto rom_storage = [](Request&, Response& rsp) -> uint32_t {
        auto img = rom_image();
        if (!img) return fs::rc::kTargetNotFound;
        rsp.push_object(make_storage_rom(img));
        return 0;
    };
    s->add_cmd(200, "OpenDataStorageByCurrentProcess", rom_storage);
    s->add_cmd(201, "OpenDataStorageByProgramId", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(202, "OpenDataStorageByDataId", [](Request& req, Response& rsp) -> uint32_t {
        // OpenDataStorageByDataId(u8 storage id, u64 data id) is what nn::fs::MountAddOnContent reaches (sdk+0x11f1a8: it computes
        // data id = base + index and calls the mount with (name, data id, ...)): the storage id is arg(0) and the data id is arg(8).
        // HANDOVER 86.1: the 3.0.0 guest sends in 16 bytes and x21 held 0x0100E95004039001, so reading arg(0) found 0 and the mount
        // failed with kTargetNotFound. A guest that sends only the data id has it in arg(0), so that stays the fallback.
        const uint64_t id = req.arg<uint64_t>(8) ? req.arg<uint64_t>(8) : req.arg<uint64_t>(0);
        auto img = aoc_image(id);
        NXLOG_I("aoc", "OpenDataStorageByDataId(0x%016llX) -> %s", static_cast<unsigned long long>(id), img ? "served" : "not installed");
        if (!img) return fs::rc::kTargetNotFound;
        rsp.push_object(make_storage_rom(img));
        return 0;
    });
    s->add_cmd(203, "OpenPatchDataStorageByCurrentProcess", [](Request&, Response&) -> uint32_t { return fs::rc::kTargetNotFound; });
    s->add_cmd(205, "OpenDataStorageWithProgramIndex", rom_storage);
    s->add_cmd(400, "OpenDeviceOperator", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_device_operator());
        return 0;
    });
    for (uint32_t id : {500u, 501u, 510u}) {
        s->add_cmd(id, id == 500 ? "OpenSdCardDetectionEventNotifier" : id == 501 ? "OpenGameCardDetectionEventNotifier" : "OpenSystemDataUpdateEventNotifier",
                   [](Request&, Response& rsp) -> uint32_t {
                       rsp.push_object(make_event_notifier());
                       return 0;
                   });
    }
    s->add_cmd(511, "NotifySystemDataUpdateEvent", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(600, "SetCurrentPosixTime", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(601, "QuerySaveDataTotalSize", [](Request& req, Response& rsp) -> uint32_t {
        rsp.push<int64_t>(req.arg<int64_t>(0) + req.arg<int64_t>(8));
        return 0;
    });
    s->add_cmd(620, "SetSdCardEncryptionSeed", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(630, "SetSdCardAccessibility", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(631, "IsSdCardAccessible", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    s->add_cmd(800, "GetAndClearFileSystemProxyErrorInfo", [](Request&, Response& rsp) -> uint32_t {
        rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
        return 0;
    });
    s->add_cmd(1001, "SetSaveDataSize", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1003, "DisableAutoSaveDataCreation", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1004, "SetGlobalAccessLogMode", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1005, "GetGlobalAccessLogMode", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1006, "OutputAccessLogToSdCard", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1009, "GetAndClearMemoryReportInfo", [](Request&, Response& rsp) -> uint32_t {
        rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
        return 0;
    });
    s->add_cmd(1011, "GetProgramIndexForAccessLog", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1012, "GetFsStackUsage", [](Request&, Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        return 0;
    });
    s->add_cmd(1013, "UnsetSaveDataRootPath", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1016, "FlushAccessLogOnSdCard", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1017, "OutputApplicationInfoAccessLog", [](Request&, Response&) -> uint32_t { return 0; });
    s->add_cmd(1200, "OpenMultiCommitManager", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_multi_commit());
        return 0;
    });
    return s;
}

}  // namespace

void register_fs() {
    add("fsp-srv", [] { return make_fsp(); });
    NXLOG_I("fs", "fsp-srv: RomFS from '%s', save data in '%s'", romfs_dir().c_str(), save_dir().c_str());
}

void fs_reset() {
    std::lock_guard<std::mutex> lk(st().mu);
    st().rom.reset();
    st().rom_failed = false;
    st().saves.clear();
    st().ids.clear();
}

}  // namespace nxrt::services

namespace nxrt::services {
uint64_t ini_settings_object() { return g_ini_settings.load(std::memory_order_relaxed); }
uint64_t ini_settings_object2() { return g_ini_settings2.load(std::memory_order_relaxed); }
}  // namespace nxrt::services

namespace nxrt::services {
namespace fsapi {

uint32_t ensure_account_save(uint64_t uid_hi, uint64_t uid_lo, bool* created) {
    if (created) *created = false;
    SaveAttr a;
    a.application_id = profile().program_id;
    std::memcpy(a.uid, &uid_hi, 8);
    std::memcpy(a.uid + 8, &uid_lo, 8);
    a.type = 1;
    std::memcpy(a.raw, &a.application_id, 8);
    std::memcpy(a.raw + 8, a.uid, 16);
    a.raw[32] = 1;
    const TitleProfile& p = profile();
    const uint32_t r = create_save(a, static_cast<int64_t>(p.account_save_size), static_cast<int64_t>(p.account_save_journal), p.save_data_owner_id);
    if (r == fs::rc::kPathAlreadyExists) return 0;
    if (r == 0 && created) *created = true;
    return r;
}

uint32_t account_save_size(uint64_t uid_hi, uint64_t uid_lo, int64_t* size, int64_t* journal) {
    SaveAttr a;
    std::memcpy(a.uid, &uid_hi, 8);
    std::memcpy(a.uid + 8, &uid_lo, 8);
    a.type = 1;
    a.application_id = profile().program_id;
    const std::string key = save_key(a);
    if (!save_exists(key)) return fs::rc::kTargetNotFound;
    const SaveMeta m = read_meta(key);
    if (size) *size = m.size;
    if (journal) *journal = m.journal;
    return 0;
}

}  // namespace fsapi

}  // namespace nxrt::services

namespace nxrt::services {
uint64_t ini_settings_object();
uint64_t ini_settings_object2();
}  // namespace nxrt::services

namespace nxrt::extras {
// HANDOVER 95.14: clamp the dynamic-resolution floor of /monolib/shader/lib_nx.ini's settings to 1.0, so the game's
// ladder cannot scale the 3D scene below 100% (that ladder is what changes the upscaler's input size, and every step of
// it is followed by an amdvlk crash in FSR 4.1.2 -- docs/handover/41, sections 95.10-95.14).
namespace {
std::atomic<uint32_t> g_in_w{0}, g_in_h{0};
std::atomic<uint32_t> g_pin_w{0}, g_pin_h{0};
uint64_t g_rsize_cand[8] = {};
int g_rsize_n = 0;
std::mutex g_rsize_mu;

// HANDOVER 95.18: instead of hunting the (w, h) pair -- that only finds the descriptors of the targets the game just
// re-created -- look for the *scale* the renderer is currently running at (0.95, 0.875, 0.8, 0.75 of 1280x720). Those are
// unusual floats, and the field that carries them changes value exactly when the size steps, so a second hit at the same
// address with a different value identifies the dynamic-resolution state.
constexpr int kMaxCand = 48;
uint64_t g_cand_addr[kMaxCand] = {};
uint8_t g_cand_mask[kMaxCand] = {};   // bit per ladder value the address has carried (1:0.95 2:0.875 4:0.8 8:0.75)
int g_cand_n = 0;
int g_cand_done = 0;
std::mutex g_scale_mu;

int ladder_bit(float s) {
    static const float vals[4] = {0.95f, 0.875f, 0.8f, 0.75f};
    for (int i = 0; i < 4; ++i) {
        const float d = s > vals[i] ? s - vals[i] : vals[i] - s;
        if (d <= 0.0004f) return 1 << i;
    }
    return 0;
}

void scan_for_scale(float scale, int bit) {
    const uint64_t obj = nxrt::services::ini_settings_object();
    if (!obj || !bit) return;
    const uint64_t span = 96ull << 20;
    const uint64_t lo = obj > span ? obj - span : 0;
    const uint64_t hi = obj + span;
    int found = 0;
    for (uint64_t block = lo; block + 0x10000 <= hi && found < 8; block += 0x10000) {
        if (!nxrt::mem::is_mapped(block, 0x10000)) continue;
        for (uint64_t p = block; p + 4 <= block + 0x10000 && found < 8; p += 4) {
            float f;
            std::memcpy(&f, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 4);
            if (!std::isfinite(f)) continue;                      // NaN compares false everywhere: it matched every scan
            const float d = f > scale ? f - scale : scale - f;
            if (!(d <= 0.0004f)) continue;
            std::lock_guard<std::mutex> lk(g_scale_mu);
            int slot = -1;
            for (int i = 0; i < g_cand_n; ++i)
                if (g_cand_addr[i] == p) slot = i;
            if (slot < 0 && g_cand_n < kMaxCand) {
                slot = g_cand_n++;
                g_cand_addr[slot] = p;
                NXLOG_I("fs", "scale scan: %.4f at 0x%llx (%+#llx from the settings object)", static_cast<double>(f),
                        static_cast<unsigned long long>(p), static_cast<long long>(p - obj));
            }
            if (slot >= 0) {
                const uint8_t before = g_cand_mask[slot];
                g_cand_mask[slot] |= static_cast<uint8_t>(bit);
                if (before && before != g_cand_mask[slot] && !(before & 0x80)) {
                    g_cand_mask[slot] |= 0x80;   // confirmed: it has carried two different ladder values
                    ++g_cand_done;
                    NXLOG_I("fs", "scale scan: 0x%llx CONFIRMED (mask %#x): the dynamic-resolution field",
                            static_cast<unsigned long long>(p), static_cast<unsigned>(g_cand_mask[slot] & 0x0f));
                    // what sits around it: the level index the size is derived from is normally next door
                    for (int w = -12; w <= 12; ++w) {
                        const uint64_t a = p + static_cast<uint64_t>(static_cast<int64_t>(w) * 4);
                        if (!nxrt::mem::is_mapped(a, 4)) continue;
                        uint32_t u;
                        float f2;
                        std::memcpy(&u, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 4);
                        std::memcpy(&f2, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 4);
                        if (std::isfinite(f2) && f2 > -1000.0f && f2 < 1000.0f)
                            NXLOG_I("fs", "  %+d: int %d / float %.5f", w, static_cast<int>(u), static_cast<double>(f2));
                        else
                            NXLOG_I("fs", "  %+d: int %d", w, static_cast<int>(u));
                    }
                }
            }
            ++found;
        }
    }
}

void scan_for_size(uint32_t w, uint32_t h) {
    const uint64_t obj = nxrt::services::ini_settings_object();
    if (!obj) return;
    const uint64_t span = 24ull << 20;   // 24 MB either side of the settings object
    const uint64_t lo = obj > span ? obj - span : 0;
    const uint64_t hi = obj + span;
    int found = 0;
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    for (uint64_t block = lo; block + 0x10000 <= hi && found < 8; block += 0x10000) {
        if (!nxrt::mem::is_mapped(block, 0x10000)) continue;
        for (uint64_t p = block; p + 8 <= block + 0x10000 && found < 8; p += 4) {
            uint32_t v0, v1;
            std::memcpy(&v0, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 4);
            std::memcpy(&v1, reinterpret_cast<const void*>(static_cast<uintptr_t>(p + 4)), 4);
            bool hit = (v0 == w && v1 == h) || (v0 == h && v1 == w);
            if (!hit) {
                float a, b;
                std::memcpy(&a, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 4);
                std::memcpy(&b, reinterpret_cast<const void*>(static_cast<uintptr_t>(p + 4)), 4);
                hit = (a == fw && b == fh) || (a == fh && b == fw);
            }
            if (hit) {
                std::lock_guard<std::mutex> lk(g_rsize_mu);
                g_rsize_cand[g_rsize_n < 8 ? g_rsize_n : 7] = p;
                if (g_rsize_n < 8) ++g_rsize_n;
                NXLOG_I("fs", "render-size scan: %ux%u also at 0x%llx (%+#llx from the settings object)", w, h,
                        static_cast<unsigned long long>(p), static_cast<long long>(p - obj));
                ++found;
            }
        }
    }
}
}  // namespace

void note_upscaler_input_size(uint32_t w, uint32_t h) {
    // the pinned size is the largest input seen so far (the game's dynamic resolution never goes above it)
    uint32_t pw = g_pin_w.load(std::memory_order_relaxed), ph = g_pin_h.load(std::memory_order_relaxed);
    if (w > pw || h > ph) { g_pin_w.store(w); g_pin_h.store(h); }
    const uint32_t cw = g_in_w.exchange(w), ch = g_in_h.exchange(h);
    if (cw == w && ch == h) return;
    if (cw == 0) return;
    if (!config_get_int("upscale", "scan_render_size", 0)) return;
    // one scan per distinct ladder value (0.95 / 0.875 / 0.8 / 0.75), so the same address can be caught twice
    const float scale = h && w ? static_cast<float>(w) / 1280.0f : 0.0f;
    const int bit = ladder_bit(scale);
    static int scanned = 0;
    if (!bit || (scanned & bit)) return;
    scanned |= bit;
    NXLOG_I("fs", "render-size change: %ux%u -> %ux%u, scale %.4f (scanning)", cw, ch, w, h, static_cast<double>(scale));
    scan_for_scale(scale, bit);
}

void pin_render_size_tick() {
    // HANDOVER 95.18: write 100% into the scale field(s) the scan identified. Only addresses that were seen carrying a
    // ladder value are touched, and only when [upscale] pin_render_size is on.
    static int smode = -1;
    if (smode < 0) smode = config_get_int("upscale", "pin_scale_field", 0) ? 1 : 0;
    if (smode > 0) {
        std::lock_guard<std::mutex> lk(g_scale_mu);
        static bool told = false;
        int pinned = 0;
        for (int i = 0; i < g_cand_n; ++i) {
            if (!(g_cand_mask[i] & 0x80)) continue;       // only addresses that carried two different ladder values
            const uint64_t p = g_cand_addr[i];
            if (!p || !nxrt::mem::is_mapped(p, 4)) continue;
            const float one = 1.0f;
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(p)), &one, 4);
            ++pinned;
        }
        if (!told && pinned) {
            told = true;
            NXLOG_I("fs", "pin_scale_field: writing 1.0 into %d confirmed scale field(s) every frame", pinned);
        }
    }
    static int mode = -1;
    if (mode < 0) mode = config_get_int("upscale", "pin_render_size", 0) ? 1 : 0;
    if (mode <= 0) return;
    const uint32_t w = g_pin_w.load(std::memory_order_relaxed), h = g_pin_h.load(std::memory_order_relaxed);
    if (!w) return;
    static bool told = false;
    std::lock_guard<std::mutex> lk(g_rsize_mu);
    for (int i = 0; i < g_rsize_n; ++i) {
        const uint64_t p = g_rsize_cand[i];
        if (!p || !nxrt::mem::is_mapped(p, 8)) continue;
        uint32_t v0, v1;
        std::memcpy(&v0, reinterpret_cast<const void*>(static_cast<uintptr_t>(p)), 4);
        std::memcpy(&v1, reinterpret_cast<const void*>(static_cast<uintptr_t>(p + 4)), 4);
        uint32_t n0 = w, n1 = h;
        if (v0 > v1) { n0 = h; n1 = w; }   // keep the order the game uses
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(p)), &n0, 4);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(p + 4)), &n1, 4);
    }
    if (!told && g_rsize_n) {
        told = true;
        NXLOG_I("fs", "pin_render_size: writing %ux%u into %d candidate(s) every frame", w, h, g_rsize_n);
    }
}

void pin_render_scale_tick() {
    static int mode = -1;
    if (mode < 0) mode = nxrt::config_get_int("upscale", "pin_render_scale", 0) ? 1 : 0;
    static bool done = false;
    if (mode <= 0 || done) return;
    const uint64_t obj = nxrt::services::ini_settings_object();
    if (!obj || !nxrt::mem::is_mapped(obj, 0x400)) return;   // the ini has not been parsed yet
    done = true;
    // HANDOVER 95.15: report what the settings object actually holds instead of guessing where the dynamic-resolution
    // floor lives (the first attempt looked for 0.7 and found nothing). The floor is the value the game clamps its scale
    // between, so it is a float (or a double) below 1.0 and above ~0.5.
    // The dynamic-resolution ladder we measured is 1.0 / 0.95 / 0.875 / 0.8 / 0.75 of 1280x720, so look for those
    // values instead of a wide window: they are distinctive and a clamp cannot hit an unrelated shader parameter.
    static const float kLadder[] = {0.70f, 0.75f, 0.80f, 0.875f, 0.90f, 0.95f};
    for (int which = 0; which < 2; ++which) {
        const uint64_t o = which ? nxrt::services::ini_settings_object2() : obj;
        if (!o || !nxrt::mem::is_mapped(o, 0x400)) continue;
        char hits[768] = {};
        size_t used = 0;
        int found = 0, clamped = 0, ones = 0;
        for (uint32_t off = 0; off + 4 <= 0x3190; off += 4) {
            const float f = *reinterpret_cast<const float*>(static_cast<uintptr_t>(o + off));
            if (f > 0.98f && f < 1.02f) ++ones;
            for (float want : kLadder) {
                const float d = f > want ? f - want : want - f;
                if (d <= 0.006f) {
                    if (found < 24 && used + 14 < sizeof hits)
                        used += static_cast<size_t>(std::snprintf(hits + used, sizeof hits - used, " +0x%x=%.3f", off, static_cast<double>(f)));
                    ++found;
                    *reinterpret_cast<float*>(static_cast<uintptr_t>(o + off)) = 1.0f;
                    ++clamped;
                    break;
                }
            }
        }
        NXLOG_I("fs", "pin_render_scale: object%d 0x%llx: %d ladder float(s)%s (%d clamped to 1.0); %d float(s) already 1.0",
                which, static_cast<unsigned long long>(o), found, found ? hits : " (none)", clamped, ones);
    }
}
}  // namespace nxrt::extras
