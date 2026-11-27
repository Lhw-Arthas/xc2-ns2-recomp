// SPDX-License-Identifier: GPL-3.0-or-later
#include "xc2_nav_overlay.h"
#include "xc2_nav_mesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace xc2::nav {
namespace {

constexpr const char* kTag = "nav";

// Verified retail 3.0.1 (build id 8BEF68ED1EEBC24B) main-relative read-only offsets.
namespace off301 {
constexpr uint64_t kGfInfoViewMat        = 0x11a9a60;  // float[16]
constexpr uint64_t kGfInfoViewProj       = 0x11a9ae0;  // float[16], row-vector (v * M)
constexpr uint64_t kGfInfoCamPos         = 0x11a9b20;  // float[3]
constexpr uint64_t kGfInfoCamTgt         = 0x11a9b40;  // float[3]
constexpr uint64_t kMapIdSingleton       = 0x1070f88;  // ptr -> +0x0C uint32_t currentMapBdatId
constexpr uint64_t kFormationSingleton   = 0x1070c48;  // ptr -> +0x1A8 uint64_t controlMoverHandle
constexpr uint64_t kObjManagerSingleton  = 0x11b3f70;  // ptr -> +0x420228 bitmap, +0x420230 node[4096] (stride 0x420)
constexpr uint64_t kMenuLocationSingleton = 0x11ac248; // ptr -> cat 4 (+0x18..+0x2C), cat 0..3 (+0x48 + cat*0x430)
constexpr uint64_t kGmkManagerSingleton   = 0x1070978; // ptr -> +0xF358/+0xF360 GmkSet* vector -> +0x1B8 GmkList*[] ('LDMK')
constexpr uint64_t kBattleSingleton       = 0x10708e8; // ptr -> +0x148 u8 bit 1 = GfGameBattle::isBattle()
constexpr uint64_t kEventSingleton        = 0x11a8db8; // ptr -> +0x78  u32 bit 1 = GfGameEvent::isEvent() (cutscenes/events)
constexpr uint64_t kDataBdatSingleton     = 0x0f2d290; // ptr -> +0x10 + idx*8 decrypted BDAT table pointers (526 entries)
constexpr uint32_t kBdatIdxFldMapList     = 93u;       // [93]  (0x05d) FLD_maplist ("resource" -> "ma05a", etc.)
constexpr uint32_t kBdatIdxFldLandmarkMsg = 443u;      // [443] (0x1bb) fld_landmark (active language landmark names)
constexpr uint32_t kBdatIdxFldMapInfoMsg  = 444u;      // [444] (0x1bc) fld_mapinfo  (active language map/Titan names)
constexpr uint32_t kBdatIdxFldNpcNameMsg  = 445u;      // [445] (0x1bd) fld_npcname  (active language NPC names)
constexpr uint32_t kFourCcLdmk            = 0x4b4d444cu; // 'LDMK' (TGmkList<GmkLandmark>)
constexpr uint32_t kFourCcNpc             = 0x0043504eu; // 'NPC\0' (TGmkList<GmkNpc>, stride 0x70)
constexpr uint32_t kFourCcRnpc            = 0x43504e52u; // 'RNPC'  (TGmkList<GmkRoutedNpc>, stride 0xA0)

// 3.0.1 AFieldLocator subclass vtables (relative to main_base).
constexpr uint64_t kVtBatEnemyId         = 0xe28dd8;   // FixedVector<Vec3> arr=+0x830, cnt=+0x930
constexpr uint64_t kVtBatEnemyGroup      = 0xe28e20;   // FixedVector<Vec3> arr=+0x060, cnt=+0x160
constexpr uint64_t kVtBatEnemySpecies    = 0xe28e68;   // FixedVector<Vec3> arr=+0x020, cnt=+0x120
constexpr uint64_t kVtTalkNpcId          = 0xe28eb0;   // active u8=+0x1A, Vec3=+0x20
constexpr uint64_t kVtTalkDummyGroup     = 0xe28ef8;   // FixedVector<Vec3> arr=+0x040, cnt=+0x0D0
constexpr uint64_t kVtReach              = 0xe28f40;   // active u8=+0x18, Vec3=+0x20
constexpr uint64_t kVtTbox               = 0xe28f88;   // active u8=+0x58, Vec3=+0x60
constexpr uint64_t kVtPrecious           = 0xe28fd0;   // FixedVector<Vec3> arr=+0x430, cnt=+0x530
constexpr uint64_t kVtEventPreorder      = 0xe29038;   // active u8=+0x58, Vec3=+0x60
constexpr uint64_t kVtPreciousPreorder   = 0xe29080;   // active u8=+0x58, Vec3=+0x60
constexpr uint64_t kVtGimmick            = 0xe290c8;   // active u8=+0x1A, Vec3=+0x20
constexpr uint64_t kVtDlcArr1            = 0xe29110;   // FixedVector<Vec3> arr=+0x020, cnt=+0x120
constexpr uint64_t kVtDlcArr2            = 0xe29158;   // FixedVector<Vec3> arr=+0x020, cnt=+0x120
}  // namespace off301

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

inline bool is_finite3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 1e6f && std::fabs(v.y) < 1e6f && std::fabs(v.z) < 1e6f;
}

template <typename T>
bool safe_read(uint64_t addr, T& out) {
    if (addr < 0x10000ull || addr > UINT64_MAX - sizeof(T)) return false;
    if (!nxrt::mem::is_mapped(addr, sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

bool safe_read_bytes(uint64_t addr, void* dst, size_t n) {
    if (addr < 0x10000ull || addr > UINT64_MAX - n) return false;
    if (!nxrt::mem::is_mapped(addr, n)) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
    return true;
}

std::atomic<uint64_t> g_main_base{0};
std::atomic<int> g_mode{2};   // 2 = Beacons + 3D Ground Ribbon, 1 = Beacons Only, 0 = Off
std::atomic<bool> g_debug{false};
std::atomic<int64_t> g_toast_until_ms{0};
std::atomic<int64_t> g_reveal_start_ms{0};

struct CustomPinState {
    std::mutex mu;
    bool active = false;
    uint32_t map_id = 0;
    Vec3 pos{};
    std::string name;
    uint8_t group = 0;
    // Cached entries from latest picker query (indexed by PickerItem::id)
    struct Entry {
        Vec3 pos;
        std::string name;
        uint8_t group = 0;
        uint32_t map_id = 0;
    };
    std::vector<Entry> entries;
};

CustomPinState& custom_pin() {
    static CustomPinState s;
    return s;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

enum class MarkerKind : uint8_t {
    MainStory = 0,
    Quest     = 1,
    Battle    = 2,
    Collect   = 3,
    Pin       = 4,
};

struct NavMarker {
    Vec3 pos;
    MarkerKind kind = MarkerKind::Quest;
    uint32_t cat = 0;
    uint32_t icon_id = 0;
    float dist = 0.f;
    float dy = 0.f;
};

bool read_player_pos(uint64_t mb, const Vec3& cam_tgt, Vec3& out) {
    // 1. Try GfGamePlayerFormation::s_instance -> +0x1A8 handle -> GfObjManager -> GfObj -> GfComTransform -> pos
    uint64_t form = 0;
    uint64_t handle = ~0ull;
    if (safe_read(mb + off301::kFormationSingleton, form) && form &&
        safe_read(form + 0x1a8, handle) && handle != ~0ull) {
        uint64_t mgr = 0;
        if (safe_read(mb + off301::kObjManagerSingleton, mgr) && mgr) {
            const uint64_t tbl = mgr + 0x420000ull;
            uint64_t bitmap_ptr = 0, nodes_ptr = 0;
            if (safe_read(tbl + 0x228, bitmap_ptr) && safe_read(tbl + 0x230, nodes_ptr) &&
                bitmap_ptr && nodes_ptr) {
                const uint32_t slot = static_cast<uint32_t>(handle & 0xFFFull);
                const uint32_t word_idx = static_cast<uint32_t>((handle >> 5) & 0x7Full);
                uint32_t bits = 0;
                if (safe_read(bitmap_ptr + static_cast<uint64_t>(word_idx) * 4ull, bits) &&
                    ((bits >> (handle & 31u)) & 1u) != 0u) {
                    const uint64_t node = nodes_ptr + static_cast<uint64_t>(slot) * 0x420ull;
                    uint64_t node_handle = 0;
                    uint64_t obj = 0;
                    if (safe_read(node + 0x08, node_handle) && node_handle == handle &&
                        safe_read(node + 0x18, obj) && obj) {
                        uint64_t com_tf = 0, tf_data = 0;
                        Vec3 p{};
                        if (safe_read(obj + 0x38, com_tf) && com_tf &&
                            safe_read(com_tf + 0x10, tf_data) && tf_data &&
                            safe_read(tf_data + 0x40, p) && is_finite3(p)) {
                            out = p;
                            return true;
                        }
                    }
                }
            }
        }
    }
    // 2. Fallback: camera target is always anchored near the player's chest during field exploration.
    if (is_finite3(cam_tgt) && (std::fabs(cam_tgt.x) + std::fabs(cam_tgt.y) + std::fabs(cam_tgt.z) > 1e-3f)) {
        out = cam_tgt;
        return true;
    }
    return false;
}

bool read_current_map_id(uint64_t mb, uint32_t& out_map_id) {
    uint64_t mgr = 0;
    if (!safe_read(mb + off301::kMapIdSingleton, mgr) || !mgr) return false;
    uint32_t id = 0;
    if (!safe_read(mgr + 0x0C, id)) return false;
    out_map_id = id;
    return id != 0;
}

void push_unique_marker(std::vector<NavMarker>& out, const Vec3& pos, MarkerKind kind, uint32_t cat, uint32_t icon_id) {
    if (!is_finite3(pos)) return;
    if (std::fabs(pos.x) < 1e-4f && std::fabs(pos.y) < 1e-4f && std::fabs(pos.z) < 1e-4f) return;
    for (const auto& m : out) {
        const float dx = m.pos.x - pos.x, dy = m.pos.y - pos.y, dz = m.pos.z - pos.z;
        if (dx * dx + dy * dy + dz * dz < 2.25f) return;  // deduplicate within 1.5m
    }
    if (out.size() >= 32) return;
    NavMarker nm;
    nm.pos = pos;
    nm.kind = kind;
    nm.cat = cat;
    nm.icon_id = icon_id;
    out.push_back(nm);
}

struct OffMapTarget {
    bool valid = false;
    uint32_t map_id = 0;
    uint32_t cat = 0;
    Vec3 pos{};
    bool has_pos = false;
    uint64_t vt_rva = 0;
    uint32_t target_id = 0;  // e.g. NpcID at impl+0x18 for kVtTalkNpcId
};

bool read_in_battle(uint64_t mb) {
    uint64_t ptr = 0;
    uint8_t flags = 0;
    if (!safe_read(mb + off301::kBattleSingleton, ptr) || !ptr) return false;
    if (!safe_read(ptr + 0x148ull, flags)) return false;
    return ((flags >> 1) & 1u) != 0u;
}

bool read_in_event(uint64_t mb) {
    uint64_t ptr = 0;
    uint32_t flags = 0;
    if (!safe_read(mb + off301::kEventSingleton, ptr) || !ptr) return false;
    if (!safe_read(ptr + 0x78ull, flags)) return false;
    return (flags & 2u) != 0u;
}

void collect_locators(uint64_t mb, uint32_t current_map_id, std::vector<NavMarker>& out, OffMapTarget& out_offmap) {
    out_offmap = OffMapTarget{};
    uint64_t loc_mgr = 0;
    if (!safe_read(mb + off301::kMenuLocationSingleton, loc_mgr) || !loc_mgr) return;

    // Category 4: single special/DLC locator at loc_mgr + 0x18 / +0x1A / +0x20.
    {
        uint16_t flag18 = 0;
        uint8_t cnt1a = 0;
        Vec3 p{};
        if (safe_read(loc_mgr + 0x18, flag18) && flag18 != 0 &&
            safe_read(loc_mgr + 0x1A, cnt1a) && cnt1a > 0 &&
            safe_read(loc_mgr + 0x20, p)) {
            push_unique_marker(out, p, MarkerKind::Pin, 4, 0);
        }
    }

    // Categories 0..1 ONLY:
    //   cat 0 = Active Main Story Quest (GfQuestManager::getProgressCurrentQuest(QTYPE=1))
    //   cat 1 = Active Tracked Sub/Blade Quest (GfQuestManager::getProgressCurrentQuest(QTYPE=3/2))
    // Deliberately skip cat 2 and cat 3 (cat 3 = collectOrderableLocators = unaccepted '!' quest givers).
    for (uint32_t cat = 0; cat < 2; ++cat) {
        const uint64_t cat_base = loc_mgr + 0x48ull + static_cast<uint64_t>(cat) * 0x430ull;
        uint64_t begin_ptr = 0, end_ptr = 0;
        if (!safe_read(cat_base, begin_ptr) || !safe_read(cat_base + 8, end_ptr)) continue;
        if (!begin_ptr || end_ptr <= begin_ptr) continue;
        const uint64_t span = end_ptr - begin_ptr;
        if (span > 0x20ull * 128ull) continue;  // sanity guard
        const size_t n = static_cast<size_t>(span / 0x20ull);

        for (size_t i = 0; i < n; ++i) {
            uint64_t impl = 0;
            if (!safe_read(begin_ptr + i * 0x20ull + 0x18ull, impl) || !impl) continue;

            uint64_t vt = 0;
            uint32_t icon_id = 0, map_id = 0;
            if (!safe_read(impl, vt) || !safe_read(impl + 0x0C, icon_id) || !safe_read(impl + 0x10, map_id)) continue;
            if (vt < mb) continue;
            const uint64_t vt_rva = vt - mb;

            const bool is_offmap = (current_map_id != 0 && map_id != 0 && map_id != current_map_id);
            if (is_offmap) {
                if (!out_offmap.valid || (cat == 0 && out_offmap.cat != 0)) {
                    out_offmap.valid = true;
                    out_offmap.map_id = map_id;
                    out_offmap.cat = cat;
                    out_offmap.has_pos = false;
                    out_offmap.vt_rva = vt_rva;
                    out_offmap.target_id = 0;
                    if (vt_rva == off301::kVtTalkNpcId) {
                        safe_read(impl + 0x18ull, out_offmap.target_id);
                    }
                    // Try to extract 3D target coordinates even for off-map locators (active flag may be 0 while off-map).
                    Vec3 p{};
                    uint32_t cnt = 0;
                    if ((vt_rva == off301::kVtReach || vt_rva == off301::kVtTalkNpcId || vt_rva == off301::kVtGimmick) &&
                        safe_read(impl + 0x20, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if ((vt_rva == off301::kVtTbox || vt_rva == off301::kVtEventPreorder || vt_rva == off301::kVtPreciousPreorder) &&
                               safe_read(impl + 0x60, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if (vt_rva == off301::kVtBatEnemyId && safe_read(impl + 0x930, cnt) && cnt > 0 && safe_read(impl + 0x830, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if (vt_rva == off301::kVtBatEnemyGroup && safe_read(impl + 0x160, cnt) && cnt > 0 && safe_read(impl + 0x060, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if ((vt_rva == off301::kVtBatEnemySpecies || vt_rva == off301::kVtDlcArr1 || vt_rva == off301::kVtDlcArr2) &&
                               safe_read(impl + 0x120, cnt) && cnt > 0 && safe_read(impl + 0x020, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if (vt_rva == off301::kVtTalkDummyGroup && safe_read(impl + 0x0D0, cnt) && cnt > 0 && safe_read(impl + 0x040, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    } else if (vt_rva == off301::kVtPrecious && safe_read(impl + 0x530, cnt) && cnt > 0 && safe_read(impl + 0x430, p) && is_finite3(p)) {
                        out_offmap.pos = p; out_offmap.has_pos = true;
                    }
                    if (out_offmap.has_pos && std::fabs(out_offmap.pos.x) + std::fabs(out_offmap.pos.y) + std::fabs(out_offmap.pos.z) < 0.1f) {
                        out_offmap.has_pos = false;
                    }
                }
                continue;
            }

            auto default_kind = [&](MarkerKind fallback) -> MarkerKind {
                if (cat == 0) return MarkerKind::MainStory;
                return fallback;
            };

            auto read_single = [&](uint64_t active_off, uint64_t pos_off, MarkerKind kind) {
                uint8_t active = 0;
                Vec3 p{};
                if (safe_read(impl + active_off, active) && active != 0 && safe_read(impl + pos_off, p)) {
                    push_unique_marker(out, p, default_kind(kind), cat, icon_id);
                }
            };

            auto read_array = [&](uint64_t arr_off, uint64_t cnt_off, MarkerKind kind) {
                uint32_t cnt = 0;
                if (!safe_read(impl + cnt_off, cnt) || cnt == 0) return;
                cnt = std::min<uint32_t>(cnt, 16u);
                for (uint32_t k = 0; k < cnt; ++k) {
                    Vec3 p{};
                    if (safe_read(impl + arr_off + static_cast<uint64_t>(k) * 16ull, p)) {
                        push_unique_marker(out, p, default_kind(kind), cat, icon_id);
                    }
                }
            };

            switch (vt_rva) {
                case off301::kVtReach:
                    read_single(0x18, 0x20, MarkerKind::Quest);
                    break;
                case off301::kVtTalkNpcId:
                    read_single(0x1A, 0x20, MarkerKind::Quest);
                    break;
                case off301::kVtGimmick:
                    read_single(0x1A, 0x20, MarkerKind::Collect);
                    break;
                case off301::kVtTbox:
                case off301::kVtEventPreorder:
                case off301::kVtPreciousPreorder:
                    read_single(0x58, 0x60, MarkerKind::Collect);
                    break;
                case off301::kVtBatEnemyId:
                    read_array(0x830, 0x930, MarkerKind::Battle);
                    break;
                case off301::kVtBatEnemyGroup:
                    read_array(0x060, 0x160, MarkerKind::Battle);
                    break;
                case off301::kVtBatEnemySpecies:
                    read_array(0x020, 0x120, MarkerKind::Battle);
                    break;
                case off301::kVtTalkDummyGroup:
                    read_array(0x040, 0x0D0, MarkerKind::Quest);
                    break;
                case off301::kVtPrecious:
                    read_array(0x430, 0x530, MarkerKind::Collect);
                    break;
                case off301::kVtDlcArr1:
                case off301::kVtDlcArr2:
                    read_array(0x020, 0x120, MarkerKind::Quest);
                    break;
                default:
                    break;
            }
        }
    }
}

struct LandmarkInfo {
    Vec3 pos;
    uint32_t bdat_id = 0;
    uint64_t pop_bdat_fp = 0;  // maXXa_FLD_LandmarkPop BDAT table in guest memory
    uint8_t category = 0;
};

// Minimal read-only BDAT v2 column lookup on already-decrypted in-memory guest tables.
bool bdat_find_column(uint64_t tbl, const char* col_name, uint16_t& out_val_off, uint8_t& out_type) {
    uint32_t magic = 0;
    if (!safe_read(tbl, magic) || magic != 0x54414442u) return false;  // 'BDAT'
    uint16_t bucket_off = 0, bucket_n = 0;
    if (!safe_read(tbl + 0x0A, bucket_off) || !safe_read(tbl + 0x0C, bucket_n) || bucket_n == 0 || bucket_n > 256) {
        return false;
    }

    // Walk all buckets (small table, immune to hash edge cases).
    for (uint16_t b = 0; b < bucket_n; ++b) {
        uint16_t e = 0;
        if (!safe_read(tbl + bucket_off + static_cast<uint64_t>(b) * 2ull, e)) continue;
        for (int guard = 0; guard < 64 && e != 0; ++guard) {
            uint16_t desc = 0, nxt = 0, name_off = 0;
            if (!safe_read(tbl + e, desc) || !safe_read(tbl + e + 2, nxt) || !safe_read(tbl + e + 4, name_off)) break;
            char buf[64] = {};
            if (safe_read_bytes(tbl + name_off, buf, sizeof(buf) - 1)) {
                buf[sizeof(buf) - 1] = '\0';
                if (std::strcmp(buf, col_name) == 0) {
                    uint8_t typ = 0;
                    uint16_t voff = 0;
                    if (!safe_read(tbl + desc + 1, typ) || !safe_read(tbl + desc + 2, voff)) return false;
                    out_type = typ;
                    out_val_off = voff;
                    return true;
                }
            }
            e = nxt;
        }
    }
    return false;
}

bool bdat_row_addr(uint64_t tbl, uint32_t row_id, uint64_t& out_row) {
    uint16_t row_sz = 0, row_off = 0, row_cnt = 0, id_top = 0;
    if (!safe_read(tbl + 0x08, row_sz) || !safe_read(tbl + 0x0E, row_off) ||
        !safe_read(tbl + 0x10, row_cnt) || !safe_read(tbl + 0x12, id_top)) return false;
    if (row_id < id_top || row_id >= static_cast<uint32_t>(id_top) + row_cnt) return false;
    out_row = tbl + row_off + static_cast<uint64_t>(row_id - id_top) * row_sz;
    return true;
}

bool bdat_read_uint(uint64_t tbl, uint32_t row_id, const char* col_name, uint32_t& out_val) {
    uint16_t voff = 0;
    uint8_t typ = 0;
    uint64_t row = 0;
    if (!bdat_find_column(tbl, col_name, voff, typ) || !bdat_row_addr(tbl, row_id, row)) return false;
    // BDAT v2 types: 1=u8, 2=u16, 3=u32, 4=i8, 5=i16, 6=i32, 7=str_off(u32), 8=f32.
    if (typ == 1 || typ == 4) {
        uint8_t v = 0;
        if (!safe_read(row + voff, v)) return false;
        out_val = v;
        return true;
    }
    if (typ == 2 || typ == 5) {
        uint16_t v = 0;
        if (!safe_read(row + voff, v)) return false;
        out_val = v;
        return true;
    }
    uint32_t v = 0;
    if (!safe_read(row + voff, v)) return false;
    out_val = v;
    return true;
}

bool read_landmark_localized_name(uint64_t mb, const LandmarkInfo& lm, char* out, size_t out_n) {
    if (!out || out_n < 2 || !lm.pop_bdat_fp || lm.bdat_id == 0) return false;
    out[0] = '\0';

    // 1. Read MSGID from map's FLD_LandmarkPop table.
    uint32_t msg_id = 0;
    if (!bdat_read_uint(lm.pop_bdat_fp, lm.bdat_id, "MSGID", msg_id) || msg_id == 0) return false;

    // 2. Read active language's fld_landmark message BDAT table pointer from GfDataBdat::s_instance[443].
    uint64_t bdat_mgr = 0, msg_tbl = 0;
    if (!safe_read(mb + off301::kDataBdatSingleton, bdat_mgr) || !bdat_mgr) return false;
    if (!safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldLandmarkMsg) * 8ull, msg_tbl) || !msg_tbl) {
        return false;
    }

    // 3. Look up "name" string offset in fld_landmark (already decrypted in guest memory, in the game's active language).
    uint16_t voff = 0;
    uint8_t typ = 0;
    uint64_t row = 0;
    uint32_t str_tbl_off = 0, str_tbl_sz = 0, str_off = 0;
    if (!safe_read(msg_tbl + 0x18, str_tbl_off) || !safe_read(msg_tbl + 0x1C, str_tbl_sz)) return false;
    const uint64_t tbl_end = static_cast<uint64_t>(str_tbl_off) + static_cast<uint64_t>(str_tbl_sz);
    if (tbl_end < 0x20ull || tbl_end > 64ull * 1024ull * 1024ull) return false;
    if (!bdat_find_column(msg_tbl, "name", voff, typ) || !bdat_row_addr(msg_tbl, msg_id, row)) return false;
    if (!safe_read(row + voff, str_off) || str_off == 0 || str_off >= tbl_end) return false;

    const size_t max_read = static_cast<size_t>(std::min<uint64_t>(out_n - 1, tbl_end - str_off));
    if (max_read == 0 || !safe_read_bytes(msg_tbl + str_off, out, max_read)) return false;
    out[max_read] = '\0';
    for (size_t i = 0; out[i]; ++i) {
        if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    }
    return out[0] != '\0';
}

bool read_map_resource_name(uint64_t mb, uint32_t map_id, char* out, size_t out_n) {
    if (!out || out_n < 2 || map_id == 0) return false;
    out[0] = '\0';
    uint64_t bdat_mgr = 0, map_tbl = 0;
    if (!safe_read(mb + off301::kDataBdatSingleton, bdat_mgr) || !bdat_mgr) return false;
    if (!safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldMapList) * 8ull, map_tbl) || !map_tbl) {
        return false;
    }
    uint16_t voff = 0;
    uint8_t typ = 0;
    uint64_t row = 0;
    uint32_t str_tbl_off = 0, str_tbl_sz = 0, str_off = 0;
    if (!safe_read(map_tbl + 0x18, str_tbl_off) || !safe_read(map_tbl + 0x1C, str_tbl_sz)) return false;
    const uint64_t tbl_end = static_cast<uint64_t>(str_tbl_off) + static_cast<uint64_t>(str_tbl_sz);
    if (tbl_end < 0x20ull || tbl_end > 64ull * 1024ull * 1024ull) return false;
    if (!bdat_find_column(map_tbl, "resource", voff, typ) || !bdat_row_addr(map_tbl, map_id, row)) return false;
    if (!safe_read(row + voff, str_off) || str_off == 0 || str_off >= tbl_end) return false;
    const size_t max_read = static_cast<size_t>(std::min<uint64_t>(out_n - 1, tbl_end - str_off));
    if (max_read == 0 || !safe_read_bytes(map_tbl + str_off, out, max_read)) return false;
    out[max_read] = '\0';
    return out[0] != '\0';
}

uint64_t bdat_find_table_by_name(uint64_t mb, const char* target_name) {
    uint64_t bdat_mgr = 0;
    if (!safe_read(mb + off301::kDataBdatSingleton, bdat_mgr) || !bdat_mgr) return 0;
    for (uint32_t i = 0; i < 600u; ++i) {
        uint64_t tbl = 0;
        if (!safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(i) * 8ull, tbl) || !tbl) continue;
        uint32_t magic = 0;
        uint16_t name_off = 0;
        if (!safe_read(tbl, magic) || magic != 0x54414442u || !safe_read(tbl + 0x06ull, name_off) || name_off < 0x18u) continue;
        char nm[64] = {};
        if (safe_read_bytes(tbl + name_off, nm, sizeof(nm) - 1)) {
            nm[sizeof(nm) - 1] = '\0';
            if (std::strcmp(nm, target_name) == 0) return tbl;
        }
    }
    return 0;
}

bool bdat_read_string(uint64_t tbl, uint32_t row_id, const char* col_name, char* out, size_t out_n) {
    if (!tbl || !out || out_n < 2) return false;
    out[0] = '\0';
    uint16_t voff = 0;
    uint8_t typ = 0;
    uint64_t row = 0;
    uint32_t str_tbl_off = 0, str_tbl_sz = 0, str_off = 0;
    if (!safe_read(tbl + 0x18, str_tbl_off) || !safe_read(tbl + 0x1C, str_tbl_sz)) return false;
    const uint64_t tbl_end = static_cast<uint64_t>(str_tbl_off) + static_cast<uint64_t>(str_tbl_sz);
    if (tbl_end < 0x20ull || tbl_end > 64ull * 1024ull * 1024ull) return false;
    if (!bdat_find_column(tbl, col_name, voff, typ) || !bdat_row_addr(tbl, row_id, row)) return false;
    if (!safe_read(row + voff, str_off) || str_off == 0 || str_off >= tbl_end) return false;
    const size_t max_read = static_cast<size_t>(std::min<uint64_t>(out_n - 1, tbl_end - str_off));
    if (max_read == 0 || !safe_read_bytes(tbl + str_off, out, max_read)) return false;
    out[max_read] = '\0';
    for (size_t i = 0; out[i]; ++i) {
        if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    }
    return out[0] != '\0';
}

// Resolve full 3-level localized Skip-Travel breadcrumb for an off-map objective:
//   Level 1 (out_map): Titan / Big Map  (FLD_maplist.nameID -> fld_mapinfo [444])
//   Level 2 (out_sub): Sub-area / Floor (nearest landmark.menuGroup -> MNU_MapGroup.disp_name -> fld_mapinfo [444])
//   Level 3 (out_lm):  Bottom-most nearest Skip-Travel Landmark (landmark.MSGID -> fld_landmark [443])
void resolve_cross_map_hint(uint64_t mb, const OffMapTarget& offmap,
                            char* out_map, size_t map_n,
                            char* out_sub, size_t sub_n,
                            char* out_lm, size_t lm_n) {
    out_map[0] = '\0';
    out_sub[0] = '\0';
    out_lm[0] = '\0';
    if (!offmap.valid || offmap.map_id == 0) return;

    // Lightweight cache so per-frame HUD rendering does zero archive/BDAT work after first resolution.
    struct Cache {
        uint32_t map_id = 0;
        uint64_t vt_rva = 0;
        uint32_t target_id = 0;
        bool has_pos = false;
        int32_t qx = 0, qy = 0, qz = 0;
        std::string map_s, sub_s, lm_s;
    };
    static Cache s_cache;
    const int32_t qx = offmap.has_pos ? static_cast<int32_t>(std::lround(offmap.pos.x)) : 0;
    const int32_t qy = offmap.has_pos ? static_cast<int32_t>(std::lround(offmap.pos.y)) : 0;
    const int32_t qz = offmap.has_pos ? static_cast<int32_t>(std::lround(offmap.pos.z)) : 0;
    if (s_cache.map_id == offmap.map_id && s_cache.vt_rva == offmap.vt_rva &&
        s_cache.target_id == offmap.target_id && s_cache.has_pos == offmap.has_pos &&
        s_cache.qx == qx && s_cache.qy == qy && s_cache.qz == qz && !s_cache.map_s.empty()) {
        std::snprintf(out_map, map_n, "%s", s_cache.map_s.c_str());
        std::snprintf(out_sub, sub_n, "%s", s_cache.sub_s.c_str());
        std::snprintf(out_lm,  lm_n,  "%s", s_cache.lm_s.c_str());
        return;
    }

    uint64_t bdat_mgr = 0, map_tbl = 0, lm_msg_tbl = 0, map_msg_tbl = 0;
    if (!safe_read(mb + off301::kDataBdatSingleton, bdat_mgr) || !bdat_mgr) return;
    safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldMapList) * 8ull, map_tbl);
    safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldLandmarkMsg) * 8ull, lm_msg_tbl);
    safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldMapInfoMsg) * 8ull, map_msg_tbl);
    if (!map_tbl) return;

    // 1. Level-1 localized Titan/Map name: FLD_maplist.nameID -> fld_mapinfo [444]
    if (map_msg_tbl) {
        uint32_t map_name_id = 0;
        if (bdat_read_uint(map_tbl, offmap.map_id, "nameID", map_name_id) && map_name_id > 0) {
            bdat_read_string(map_msg_tbl, map_name_id, "name", out_map, map_n);
        }
    }

    char res[32] = {};
    if (!read_map_resource_name(mb, offmap.map_id, res, sizeof res)) return;
    if (out_map[0] == '\0') std::snprintf(out_map, map_n, "%s", res);

    // 2. Resolve target 3D position on target map (direct pos on locator, or NpcID -> _FLD_NpcPop + _npc.lvb).
    NavVec3 tgt_pos{offmap.pos.x, offmap.pos.y, offmap.pos.z};
    bool has_tgt_pos = offmap.has_pos;
    if (!has_tgt_pos && offmap.vt_rva == off301::kVtTalkNpcId && offmap.target_id > 0) {
        has_tgt_pos = mesh_find_offmap_npc_pos(res, offmap.target_id, tgt_pos);
    }

    // 3. Find nearest Skip-Travel Landmark (category <= 1) on target map -> Level-3 & Level-2 localized names.
    if (has_tgt_pos && lm_msg_tbl) {
        std::vector<OffMapLandmark> lms;
        if (mesh_get_offmap_landmarks(res, lms) && !lms.empty()) {
            const OffMapLandmark* best = nullptr;
            float best_d2 = 1e30f;
            for (const auto& lm : lms) {
                const float dx = lm.pos.x - tgt_pos.x;
                const float dy = lm.pos.y - tgt_pos.y;
                const float dz = lm.pos.z - tgt_pos.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = &lm;
                }
            }
            if (best && best->msg_id > 0) {
                // Level-3: bottom-most Skip-Travel landmark name
                bdat_read_string(lm_msg_tbl, best->msg_id, "name", out_lm, lm_n);

                // Level-2: Skip-Travel sub-area group (MNU_MapGroup.disp_name -> mnu_map_level_name_ms),
                // with fallback to MNU_MapInfo.disp_name if group name is missing or identical to Level-1.
                static uint64_t s_mnu_grp = 0;
                static uint64_t s_mnu_info = 0;
                static uint64_t s_level_msg = 0;
                static bool s_mnu_probed = false;
                if (!s_mnu_probed) {
                    s_mnu_probed = true;
                    s_mnu_grp   = bdat_find_table_by_name(mb, "MNU_MapGroup");
                    s_mnu_info  = bdat_find_table_by_name(mb, "MNU_MapInfo");
                    s_level_msg = bdat_find_table_by_name(mb, "mnu_map_level_name_ms");
                }
                if (s_level_msg) {
                    uint32_t sub_disp = 0;
                    if (s_mnu_grp && best->menu_group > 0 &&
                        bdat_read_uint(s_mnu_grp, best->menu_group, "disp_name", sub_disp) && sub_disp > 0) {
                        bdat_read_string(s_level_msg, sub_disp, "name", out_sub, sub_n);
                    }
                    if ((out_sub[0] == '\0' || std::strcmp(out_sub, out_map) == 0) &&
                        s_mnu_info && best->menu_map_image > 0 &&
                        bdat_read_uint(s_mnu_info, best->menu_map_image, "disp_name", sub_disp) && sub_disp > 0) {
                        bdat_read_string(s_level_msg, sub_disp, "name", out_sub, sub_n);
                    }
                    // Deduplicate if Level-2 is still identical to Level-1 or Level-3
                    if (std::strcmp(out_sub, out_map) == 0 || std::strcmp(out_sub, out_lm) == 0) {
                        out_sub[0] = '\0';
                    }
                }
            }
        }
    }

    s_cache.map_id = offmap.map_id;
    s_cache.vt_rva = offmap.vt_rva;
    s_cache.target_id = offmap.target_id;
    s_cache.has_pos = offmap.has_pos;
    s_cache.qx = qx;
    s_cache.qy = qy;
    s_cache.qz = qz;
    s_cache.map_s = out_map;
    s_cache.sub_s = out_sub;
    s_cache.lm_s  = out_lm;
    NXLOG_I(kTag, "cross-map skip-travel: map_id=%u (%s) -> '%s' > '%s' > '%s'",
            offmap.map_id, res, out_map, out_sub, out_lm);
}

void collect_landmarks(uint64_t mb, std::vector<LandmarkInfo>& out) {
    uint64_t gmk_mgr = 0;
    if (!safe_read(mb + off301::kGmkManagerSingleton, gmk_mgr) || !gmk_mgr) return;
    uint64_t set_begin = 0, set_end = 0;
    if (!safe_read(gmk_mgr + 0xF358ull, set_begin) || !safe_read(gmk_mgr + 0xF360ull, set_end)) return;
    if (!set_begin || set_end <= set_begin) return;
    const size_t set_n = std::min<size_t>(static_cast<size_t>((set_end - set_begin) / 8ull), 64u);

    for (size_t s = 0; s < set_n; ++s) {
        uint64_t gmk_set = 0;
        if (!safe_read(set_begin + s * 8ull, gmk_set) || !gmk_set) continue;
        // Scan GmkSet::m_lists[] at +0x1B8 for the 'LDMK' (GmkLandmark) list.
        for (size_t t = 0; t < 48u; ++t) {
            uint64_t list = 0;
            if (!safe_read(gmk_set + 0x1B8ull + t * 8ull, list) || !list) continue;
            uint64_t hdr_ptr = 0;
            uint32_t fourcc = 0;
            if (!safe_read(list + 0x1A0ull, hdr_ptr) || !hdr_ptr || !safe_read(hdr_ptr, fourcc) ||
                fourcc != off301::kFourCcLdmk) {
                continue;
            }
            uint64_t pop_bdat_fp = 0;
            safe_read(list + 0x1B8ull, pop_bdat_fp);  // TGmkList<GmkLandmark>::getBindedBdatFp()
            uint64_t lm_begin = 0, lm_end = 0;
            if (!safe_read(list + 0x1F0ull, lm_begin) || !safe_read(list + 0x1F8ull, lm_end)) continue;
            if (!lm_begin || lm_end <= lm_begin) continue;
            const size_t lm_n = std::min<size_t>(static_cast<size_t>((lm_end - lm_begin) / 0x100ull), 256u);
            for (size_t i = 0; i < lm_n; ++i) {
                const uint64_t lm = lm_begin + i * 0x100ull;
                uint64_t bdat_info = 0;
                uint64_t tf_ptr = 0;
                uint32_t bdat_id = 0;
                uint8_t cat = 0xFF;
                Vec3 pos{};
                if (safe_read(lm + 0x30ull, bdat_info) && bdat_info &&
                    safe_read(bdat_info + 0x02ull, cat) && cat <= 1u &&
                    safe_read(lm + 0x10ull, tf_ptr) && tf_ptr &&
                    safe_read(tf_ptr + 0x00ull, pos) && is_finite3(pos)) {
                    safe_read(lm + 0x38ull, bdat_id);
                    out.push_back({pos, bdat_id, pop_bdat_fp, cat});
                }
            }
            break;  // at most one LDMK list per GmkSet
        }
    }
}

struct NpcInfo {
    Vec3 pos;
    std::string name;
};

void collect_npcs(uint64_t mb, std::vector<NpcInfo>& out) {
    uint64_t gmk_mgr = 0;
    if (!safe_read(mb + off301::kGmkManagerSingleton, gmk_mgr) || !gmk_mgr) return;

    uint64_t bdat_mgr = 0, npc_msg_tbl = 0;
    if (!safe_read(mb + off301::kDataBdatSingleton, bdat_mgr) || !bdat_mgr) return;
    safe_read(bdat_mgr + 0x10ull + static_cast<uint64_t>(off301::kBdatIdxFldNpcNameMsg) * 8ull, npc_msg_tbl);
    if (!npc_msg_tbl) return;

    static uint64_t s_npc_list_tbl = 0;
    static bool s_npc_list_probed = false;
    if (!s_npc_list_probed) {
        s_npc_list_probed = true;
        s_npc_list_tbl = bdat_find_table_by_name(mb, "FLD_NpcList");
    }

    uint64_t sets_ptr = 0, sets_cnt = 0;
    if (!safe_read(gmk_mgr + 0xF358ull, sets_ptr) || !safe_read(gmk_mgr + 0xF360ull, sets_cnt)) return;
    if (!sets_ptr || sets_cnt == 0 || sets_cnt > 16ull) return;

    auto resolve_npc_name = [&](uint64_t pop_tbl, uint32_t bdat_id, char* buf, size_t n) -> bool {
        buf[0] = '\0';
        if (!pop_tbl || bdat_id == 0) return false;
        // 1. Direct Name/MSGID on pop table, or NpcID -> FLD_NpcList.Name -> fld_npcname[445]
        uint32_t npc_id = 0;
        const char* id_cols[] = {"NpcID", "NPCID", "npc_id", "NpcId", "ID"};
        for (const char* col : id_cols) {
            if (bdat_read_uint(pop_tbl, bdat_id, col, npc_id) && npc_id > 0) break;
        }
        if (npc_id > 0 && s_npc_list_tbl) {
            uint32_t msg_id = 0;
            const char* nm_cols[] = {"Name", "name", "MSGID", "NameID"};
            for (const char* col : nm_cols) {
                if (bdat_read_uint(s_npc_list_tbl, npc_id, col, msg_id) && msg_id > 0) {
                    if (bdat_read_string(npc_msg_tbl, msg_id, "name", buf, n)) return true;
                }
            }
        }
        // Fallback: try direct MSGID/Name on pop table
        uint32_t direct_msg = 0;
        if ((bdat_read_uint(pop_tbl, bdat_id, "MSGID", direct_msg) ||
             bdat_read_uint(pop_tbl, bdat_id, "Name", direct_msg)) && direct_msg > 0) {
            if (bdat_read_string(npc_msg_tbl, direct_msg, "name", buf, n)) return true;
        }
        return false;
    };

    for (uint64_t si = 0; si < sets_cnt; ++si) {
        uint64_t gmk_set = 0;
        if (!safe_read(sets_ptr + si * 8ull, gmk_set) || !gmk_set) continue;
        for (uint32_t li = 0; li < 64u; ++li) {
            uint64_t glist = 0;
            if (!safe_read(gmk_set + 0x1B8ull + static_cast<uint64_t>(li) * 8ull, glist) || !glist) continue;
            uint64_t fcc_ptr = 0;
            uint32_t fcc = 0;
            if (!safe_read(glist + 0x1A0ull, fcc_ptr) || !fcc_ptr || !safe_read(fcc_ptr, fcc)) continue;
            uint64_t stride = 0;
            if (fcc == off301::kFourCcNpc) stride = 0x70ull;
            else if (fcc == off301::kFourCcRnpc) stride = 0xA0ull;
            else continue;

            uint64_t pop_bdat_fp = 0, inst_ptr = 0, inst_cnt = 0;
            safe_read(glist + 0x1B8ull, pop_bdat_fp);
            if (!safe_read(glist + 0x1F0ull, inst_ptr) || !safe_read(glist + 0x1F8ull, inst_cnt)) continue;
            if (!inst_ptr || inst_cnt == 0 || inst_cnt > 256ull) continue;

            for (uint64_t k = 0; k < inst_cnt; ++k) {
                const uint64_t npc = inst_ptr + k * stride;
                uint64_t tf_ptr = 0;
                uint32_t bdat_id = 0;
                Vec3 pos{};
                if (!safe_read(npc + 0x10ull, tf_ptr) || !tf_ptr ||
                    !safe_read(tf_ptr + 0x00ull, pos) || !is_finite3(pos)) continue;
                if (std::fabs(pos.x) + std::fabs(pos.y) + std::fabs(pos.z) < 0.1f) continue;
                safe_read(npc + 0x38ull, bdat_id);
                char nm[96] = {};
                if (!resolve_npc_name(pop_bdat_fp, bdat_id, nm, sizeof nm)) continue;
                // Deduplicate identical NPCs within 3m
                bool dup = false;
                for (const auto& ex : out) {
                    if (ex.name == nm) {
                        const float dx = ex.pos.x - pos.x, dy = ex.pos.y - pos.y, dz = ex.pos.z - pos.z;
                        if (dx * dx + dy * dy + dz * dz < 9.0f) { dup = true; break; }
                    }
                }
                if (!dup) out.push_back({pos, nm});
                if (out.size() >= 192u) return;
            }
        }
    }
}

struct Projected {
    float sx = 0.f;
    float sy = 0.f;
    bool in_front = false;
    bool on_screen = false;
};

Projected project_world(const float m[16], const Vec3& w, float screen_w, float screen_h) {
    // Row-vector multiply matching gf::util::getScreenPos: clip = vec4(w, 1) * m_viewProj.
    const float cx = w.x * m[0] + w.y * m[4] + w.z * m[8]  + m[12];
    const float cy = w.x * m[1] + w.y * m[5] + w.z * m[9]  + m[13];
    const float cz = w.x * m[2] + w.y * m[6] + w.z * m[10] + m[14];
    const float cw = w.x * m[3] + w.y * m[7] + w.z * m[11] + m[15];

    Projected r;
    r.in_front = (cw > 1e-4f) && (cz >= 0.f);
    const float inv_w = (std::fabs(cw) > 1e-4f) ? (1.0f / std::fabs(cw)) : 1.0f;
    float ndc_x = cx * inv_w;
    float ndc_y = cy * inv_w;
    if (!r.in_front) {
        ndc_x = -ndc_x;
        ndc_y = -ndc_y;
    }
    r.sx = (ndc_x * 0.5f + 0.5f) * screen_w;
    r.sy = (0.5f - ndc_y * 0.5f) * screen_h;
    r.on_screen = r.in_front && r.sx >= 24.f && r.sx <= screen_w - 24.f &&
                  r.sy >= 24.f && r.sy <= screen_h - 24.f;
    return r;
}

uint32_t kind_color(MarkerKind k) {
    switch (k) {
        case MarkerKind::MainStory: return 0xFFD54FFFu;  // warm gold
        case MarkerKind::Quest:     return 0x4FC3F7FFu;  // bright cyan
        case MarkerKind::Battle:    return 0xFF6E6EFFu;  // coral red
        case MarkerKind::Collect:   return 0x69F0AEFFu;  // emerald green
        case MarkerKind::Pin:       return 0xCE93D8FFu;  // soft violet
    }
    return 0x4FC3F7FFu;
}

void format_elev(float dy, char* buf, size_t n, uint32_t& out_col) {
    const int idy = static_cast<int>(std::lround(dy));
    if (idy >= 3) {
        std::snprintf(buf, n, "▲+%dm", idy);
        out_col = 0x80D8FFFFu;  // sky cyan (above)
    } else if (idy <= -3) {
        std::snprintf(buf, n, "▼%dm", idy);
        out_col = 0xFFAB40FFu;  // amber orange (below)
    } else {
        std::snprintf(buf, n, "●0m");
        out_col = 0xC8E6C9FFu;  // soft green (same level)
    }
}

void draw_chevron_up(const nxrt::gfx::HudCanvas& c, float cx, float cy, float w, float h, uint32_t col, float th) {
    c.line(cx - w, cy + h, cx, cy, col, th);
    c.line(cx, cy, cx + w, cy + h, col, th);
}

void draw_chevron_down(const nxrt::gfx::HudCanvas& c, float cx, float cy, float w, float h, uint32_t col, float th) {
    c.line(cx - w, cy - h, cx, cy, col, th);
    c.line(cx, cy, cx + w, cy - h, col, th);
}

void draw_beacon(const nxrt::gfx::HudCanvas& c, float x, float y, float scale,
                 uint32_t col, float dist, float dy, bool primary, const char* extra_line) {
    const float r = (primary ? 11.5f : 8.5f) * scale;
    const uint32_t dark = 0x000000D0u;
    const double t_sec = static_cast<double>(now_ms()) * 0.001;

    // 1. Subtle expanding radial Core-Crystal pulse ring for primary objective
    if (primary) {
        const float ph = static_cast<float>(std::fmod(t_sec * 0.55, 1.0));
        const float pr = r * (1.25f + ph * 1.45f);
        const uint32_t pa = static_cast<uint32_t>((1.0f - ph) * (1.0f - ph) * 140.f) & 0xFFu;
        if (pa > 4u) {
            c.quad(x, y - pr, x + pr, y, x, y + pr, x - pr, y, (col & 0xFFFFFF00u) | pa, 1.5f * scale);
        }
    }

    // 2. Double-layer 45° Core-Crystal diamond (outer glowing wireframe + translucent body + bright core)
    const float ro = r * 1.24f;
    c.quad(x, y - ro, x + ro, y, x, y + ro, x - ro, y, 0x00000088u, 3.0f * scale);
    c.quad(x, y - ro, x + ro, y, x, y + ro, x - ro, y, (col & 0xFFFFFF00u) | 0xE8u, 1.5f * scale);
    c.quad_filled(x, y - r, x + r, y, x, y + r, x - r, y, (col & 0xFFFFFF00u) | 0xC8u);
    const float ir = r * 0.44f;
    c.quad_filled(x, y - ir, x + ir, y, x, y + ir, x - ir, y, 0xFFFFFFF0u);

    // 3. Minimalist breathing elevation chevrons (^ / v) above or below diamond when |ΔY| >= 3m
    const int idy = static_cast<int>(std::lround(dy));
    const float bob = static_cast<float>(std::sin(t_sec * 3.6)) * 2.2f * scale;
    const float cw = 5.5f * scale, ch = 3.6f * scale;
    if (idy >= 3) {
        const uint32_t ecol = 0x80D8FFEEu;
        const float base_y = y - ro - 7.f * scale - bob;
        draw_chevron_up(c, x, base_y, cw, ch, 0x000000AAu, 3.2f * scale);
        draw_chevron_up(c, x, base_y, cw, ch, ecol, 1.8f * scale);
        if (idy >= 15) {
            draw_chevron_up(c, x, base_y - 5.5f * scale, cw * 0.85f, ch * 0.85f, 0x00000088u, 2.8f * scale);
            draw_chevron_up(c, x, base_y - 5.5f * scale, cw * 0.85f, ch * 0.85f, ecol, 1.5f * scale);
        }
    } else if (idy <= -3) {
        const uint32_t ecol = 0xFFAB40EEu;
        const float base_y = y + ro + 7.f * scale + bob;
        draw_chevron_down(c, x, base_y, cw, ch, 0x000000AAu, 3.2f * scale);
        draw_chevron_down(c, x, base_y, cw, ch, ecol, 1.8f * scale);
        if (idy <= -15) {
            draw_chevron_down(c, x, base_y + 5.5f * scale, cw * 0.85f, ch * 0.85f, 0x00000088u, 2.8f * scale);
            draw_chevron_down(c, x, base_y + 5.5f * scale, cw * 0.85f, ch * 0.85f, ecol, 1.5f * scale);
        }
    }

    // 4. Frameless distance readout directly below diamond (1px dark drop shadow, no boxy pill)
    char dist_buf[32];
    if (std::abs(idy) >= 3) {
        std::snprintf(dist_buf, sizeof dist_buf, "%dm (%+dm)", static_cast<int>(std::lround(dist)), idy);
    } else {
        std::snprintf(dist_buf, sizeof dist_buf, "%dm", static_cast<int>(std::lround(dist)));
    }
    const float font_sz = (primary ? 15.5f : 13.f) * scale;
    float dw = 0.f, dh = 0.f;
    c.measure_text(font_sz, dist_buf, &dw, &dh);
    const float W = static_cast<float>(c.width);
    const float H = static_cast<float>(c.height);
    const float below_pad = (idy <= -3) ? (ro + 18.f * scale) : (ro + 7.f * scale);
    const float tx = std::clamp(x - dw * 0.5f, 12.f, std::max(12.f, W - dw - 12.f));
    const float ty = std::clamp(y + below_pad, 12.f, std::max(12.f, H - dh - 12.f));
    c.text_outlined(tx, ty, font_sz, 0xFFFFFFFFu, dark, dist_buf);

    // 5. Subtle frameless skip-travel landmark micro-hint below distance
    if (extra_line && *extra_line) {
        const float sub_sz = 13.f * scale;
        float sw = 0.f, sh = 0.f;
        c.measure_text(sub_sz, extra_line, &sw, &sh);
        const float pad_x = 8.f * scale, pad_y = 2.5f * scale;
        const float slx = std::clamp(x - sw * 0.5f, 16.f, std::max(16.f, W - sw - 16.f));
        const float sly = std::clamp(ty + dh + 4.f * scale, 12.f, std::max(12.f, H - sh - 12.f));
        c.rect_filled(slx - pad_x, sly - pad_y, slx + sw + pad_x, sly + sh + pad_y, 0x06131CA0u, 3.f * scale);
        c.line(slx - pad_x, sly - pad_y, slx + sw + pad_x, sly - pad_y, 0x4DD0E199u, 1.f);
        c.text_outlined(slx, sly, sub_sz, 0xB2EBF2F0u, dark, extra_line);
    }
}

void draw_edge_arrow(const nxrt::gfx::HudCanvas& c, float sx, float sy, float scale,
                     uint32_t col, float dist, float dy, const char* extra_line) {
    const float W = static_cast<float>(c.width);
    const float H = static_cast<float>(c.height);
    const float cx = W * 0.5f, cy = H * 0.5f;
    float dx = sx - cx, dy_s = sy - cy;
    const float len = std::hypot(dx, dy_s);
    if (len < 1e-3f) { dx = 0.f; dy_s = -1.f; }
    else { dx /= len; dy_s /= len; }

    // Elliptical 16:9 safe-zone orbit (smoothly glides around screen instead of sticking to corners)
    const float rx = W * 0.41f;
    const float ry = H * 0.37f;
    const float ex = cx + dx * rx;
    const float ey = cy + dy_s * ry;

    const float sz = 11.f * scale;
    const float px = -dy_s, py = dx;
    const uint32_t col70 = (col & 0xFFFFFF00u) | 0xB4u;  // 70% alpha so off-screen hint never steals focus
    const float tip_x = ex + dx * (sz * 1.15f);
    const float tip_y = ey + dy_s * (sz * 1.15f);
    const float b1_x = ex - dx * (sz * 0.45f) + px * (sz * 0.72f);
    const float b1_y = ey - dy_s * (sz * 0.45f) + py * (sz * 0.72f);
    const float b2_x = ex - dx * (sz * 0.45f) - px * (sz * 0.72f);
    const float b2_y = ey - dy_s * (sz * 0.45f) - py * (sz * 0.72f);

    c.triangle_filled(tip_x, tip_y, b1_x, b1_y, b2_x, b2_y, 0x00000090u);
    c.triangle_filled(tip_x - dx * 1.2f, tip_y - dy_s * 1.2f, b1_x, b1_y, b2_x, b2_y, col70);

    char label[48];
    const int idy = static_cast<int>(std::lround(dy));
    if (std::abs(idy) >= 3) {
        std::snprintf(label, sizeof label, "%dm %s%dm", static_cast<int>(std::lround(dist)), idy > 0 ? "▲+" : "▼", idy);
    } else {
        std::snprintf(label, sizeof label, "%dm", static_cast<int>(std::lround(dist)));
    }
    const float font_sz = 14.5f * scale;
    float tw = 0.f, th = 0.f;
    c.measure_text(font_sz, label, &tw, &th);
    const float lx = std::clamp(ex - dx * (sz * 2.4f) - tw * 0.5f, 18.f, W - tw - 18.f);
    const float ly = std::clamp(ey - dy_s * (sz * 2.4f) - th * 0.5f, 18.f, H - th - 18.f);
    c.text_outlined(lx, ly, font_sz, 0xFFFFFFC8u, 0x000000B8u, label);

    if (extra_line && *extra_line) {
        const float sub_sz = 12.f * scale;
        float sw = 0.f, sh = 0.f;
        c.measure_text(sub_sz, extra_line, &sw, &sh);
        const float slx = std::clamp(lx + (tw - sw) * 0.5f, 18.f, W - sw - 18.f);
        const float sly = std::clamp(ly + th + 3.f * scale, 18.f, H - sh - 18.f);
        c.text_outlined(slx, sly, sub_sz, 0x80DEEAB8u, 0x000000A8u, extra_line);
    }
}

void draw_toast(const nxrt::gfx::HudCanvas& c, float scale) {
    const int64_t until = g_toast_until_ms.load(std::memory_order_relaxed);
    if (until <= 0 || now_ms() > until) return;
    const int mode = g_mode.load(std::memory_order_relaxed);
    const char* msg = (mode == 2) ? "3D 任务指引：信标 + 地面引导线 (F3)"
                    : (mode == 1) ? "3D 任务指引：仅 3D 信标 (F3)"
                                  : "3D 任务指引：已关闭 (F3)";
    const float sz = 16.f * scale;
    float tw = 0.f, th = 0.f;
    c.measure_text(sz, msg, &tw, &th);
    const float pad_x = 12.f * scale, pad_y = 6.f * scale;
    const float x0 = (static_cast<float>(c.width) - tw) * 0.5f - pad_x;
    const float y0 = 56.f * scale;
    c.rect_filled(x0, y0, x0 + tw + 2.f * pad_x, y0 + th + 2.f * pad_y, 0x0B1018D8u, 6.f * scale);
    c.rect(x0, y0, x0 + tw + 2.f * pad_x, y0 + th + 2.f * pad_y, mode > 0 ? 0x4FC3F7CCu : 0x90A4AEAAu, 6.f * scale, 1.f);
    c.text_outlined(x0 + pad_x, y0 + pad_y, sz, mode > 0 ? 0xE1F5FEFFu : 0xCFD8DCFFu, 0x000000CCu, msg);
}

// Build a true 3D horizontal ground conveyor-belt ribbon (triangle strip) and submit it to the native Vulkan pass.
void submit_ground_ribbon(const nxrt::gfx::HudCanvas& c, const float vp[16],
                          const std::vector<NavVec3>& pts, uint32_t base_col, float vis_alpha) {
    if (pts.size() < 2 || vis_alpha <= 0.01f) return;
    constexpr float kLift = 0.22f;       // 22 cm above NavMesh floor (hugs terrain cleanly)
    constexpr float kHalfW = 0.78f;      // 1.56 m world width (XC3-style translucent conveyor belt)
    constexpr float kNearZero = 2.0f;    // fully transparent under player's feet
    constexpr float kNearFull = 5.2f;    // full opacity ahead of player
    constexpr float kFarStart = 42.0f;   // begin soft distance dissipation
    constexpr float kFarEnd = 68.0f;     // dissolve gently before horizon

    static thread_local std::vector<nxrt::gfx::Ribbon3DVertex> verts;
    verts.clear();
    verts.reserve(256);

    float accum_d = 0.f;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) {
            const float dx = pts[i].x - pts[i - 1].x;
            const float dy = pts[i].y - pts[i - 1].y;
            const float dz = pts[i].z - pts[i - 1].z;
            accum_d += std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (accum_d > kFarEnd + 3.f) break;

        // Horizontal XZ tangent for a flat terrain-hugging belt
        const NavVec3& p_prev = pts[i > 0 ? i - 1 : 0];
        const NavVec3& p_next = pts[i + 1 < pts.size() ? i + 1 : pts.size() - 1];
        float tx = p_next.x - p_prev.x;
        float tz = p_next.z - p_prev.z;
        const float tlen = std::hypot(tx, tz);
        if (tlen < 1e-4f) { tx = 1.f; tz = 0.f; }
        else { tx /= tlen; tz /= tlen; }
        // Right vector = cross(Up(0,1,0), Tangent(tx,0,tz)) = (tz, 0, -tx)
        const float rx = tz;
        const float rz = -tx;

        float fade = 1.0f;
        if (accum_d < kNearFull) {
            fade = std::clamp((accum_d - kNearZero) / (kNearFull - kNearZero), 0.f, 1.f);
            fade = fade * fade * (3.f - 2.f * fade);
        } else if (accum_d > kFarStart) {
            fade = std::clamp(1.0f - (accum_d - kFarStart) / (kFarEnd - kFarStart), 0.f, 1.f);
            fade = fade * fade * (3.f - 2.f * fade);
        }

        const float y = pts[i].y + kLift;
        verts.push_back({pts[i].x - rx * kHalfW, y, pts[i].z - rz * kHalfW, -1.0f, accum_d, fade});
        verts.push_back({pts[i].x + rx * kHalfW, y, pts[i].z + rz * kHalfW,  1.0f, accum_d, fade});
    }

    if (verts.size() < 4) return;

    nxrt::gfx::Ribbon3DParams prm;
    std::memcpy(prm.view_proj, vp, sizeof prm.view_proj);
    prm.color[0] = static_cast<float>((base_col >> 24) & 0xFFu) / 255.f;
    prm.color[1] = static_cast<float>((base_col >> 16) & 0xFFu) / 255.f;
    prm.color[2] = static_cast<float>((base_col >> 8)  & 0xFFu) / 255.f;
    prm.color[3] = 0.92f * std::clamp(vis_alpha, 0.f, 1.f);
    const int64_t t_ms = now_ms();
    prm.time_sec = static_cast<float>(static_cast<double>(t_ms % 10000000LL) * 0.001);
    const int64_t rev_t0 = g_reveal_start_ms.load(std::memory_order_relaxed);
    const float rev_frac = (rev_t0 > 0) ? std::clamp(static_cast<float>(t_ms - rev_t0) / 450.f, 0.f, 1.f) : 1.f;
    prm.reveal_dist = 4.f + rev_frac * (kFarEnd + 10.f);
    prm.occluded_alpha = 0.14f;
    c.set_ribbon_3d(verts.data(), static_cast<uint32_t>(verts.size()), prm);
}

void draw_cross_map_banner(const nxrt::gfx::HudCanvas& c, float scale, float vis_alpha,
                           const char* prefix, const char* map_name, const char* sub_name, const char* lm_name, uint32_t accent_rgb) {
    if (vis_alpha <= 0.02f || (!map_name[0] && !sub_name[0] && !lm_name[0])) return;
    char msg[320];
    if (map_name[0] && sub_name[0] && lm_name[0]) {
        std::snprintf(msg, sizeof msg, "%s%s > %s > %s", prefix, map_name, sub_name, lm_name);
    } else if (map_name[0] && lm_name[0]) {
        std::snprintf(msg, sizeof msg, "%s%s > %s", prefix, map_name, lm_name);
    } else if (lm_name[0]) {
        std::snprintf(msg, sizeof msg, "%s%s", prefix, lm_name);
    } else {
        std::snprintf(msg, sizeof msg, "%s%s", prefix, map_name);
    }
    const float sz = 14.5f * scale;
    float tw = 0.f, th = 0.f;
    c.measure_text(sz, msg, &tw, &th);
    const float pad_x = 14.f * scale, pad_y = 5.5f * scale;
    const float bw = tw + 2.f * pad_x;
    const float bh = th + 2.f * pad_y;
    const float x0 = (static_cast<float>(c.width) - bw) * 0.5f;
    const float y0 = 92.f * scale;  // neatly below XC2's top compass bar
    const uint32_t bg_a = static_cast<uint32_t>(vis_alpha * 175.f) & 0xFFu;
    const uint32_t ln_a = static_cast<uint32_t>(vis_alpha * 210.f) & 0xFFu;
    const uint32_t tx_a = static_cast<uint32_t>(vis_alpha * 245.f) & 0xFFu;
    const uint32_t sh_a = static_cast<uint32_t>(vis_alpha * 200.f) & 0xFFu;

    // Frameless angled-cut dark strip with thin top accent line (matches XC2 HUD aesthetic)
    const float cut = 7.f * scale;
    c.quad_filled(x0 + cut, y0, x0 + bw, y0, x0 + bw - cut, y0 + bh, x0, y0 + bh, 0x07111B00u | bg_a);
    c.line(x0 + cut, y0, x0 + bw, y0, (accent_rgb & 0xFFFFFF00u) | ln_a, 1.5f * scale);
    c.text_outlined(x0 + pad_x, y0 + pad_y, sz, 0xF5FAFF00u | tx_a, 0x00000000u | sh_a, msg);
}

}  // namespace

void init(uint64_t main_base) {
    g_main_base.store(main_base, std::memory_order_relaxed);
    const bool en = nxrt::config_get_bool("xc2", "nav_overlay", true);
    const bool dbg = nxrt::config_get_bool("xc2", "nav_debug", false);
    g_mode.store(en ? 2 : 0, std::memory_order_relaxed);
    g_debug.store(dbg, std::memory_order_relaxed);
    const std::string romfs = nxrt::config_get_str("fs", "romfs", "game/romfs");
    mesh_init(romfs);
    NXLOG_I(kTag, "3D nav overlay ready (main=0x%llx, mode=%d, debug=%d, romfs=%s, toggle=F3)",
            static_cast<unsigned long long>(main_base), en ? 2 : 0, dbg ? 1 : 0, romfs.c_str());
}

void shutdown() {
    mesh_shutdown();
}

void toggle() {
    const int cur = g_mode.load(std::memory_order_relaxed);
    const int next = (cur == 2) ? 1 : (cur == 1) ? 0 : 2;
    g_mode.store(next, std::memory_order_relaxed);
    if (next == 2) g_reveal_start_ms.store(now_ms(), std::memory_order_relaxed);
    g_toast_until_ms.store(now_ms() + 2400, std::memory_order_relaxed);
    NXLOG_I(kTag, "3D nav overlay mode=%d (F3)", next);
}

bool enabled() {
    return g_mode.load(std::memory_order_relaxed) > 0;
}

void draw(const nxrt::gfx::HudCanvas& c) {
    if (c.width < 320 || c.height < 240) return;
    const float scale = std::clamp(static_cast<float>(c.height) / 1080.f, 0.75f, 2.25f);
    draw_toast(c, scale);
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode <= 0) return;

    const uint64_t mb = g_main_base.load(std::memory_order_relaxed);
    if (!mb) return;

    // 1. Read camera view-projection matrix + camera pos/target.
    float vp[16] = {};
    Vec3 cam_pos{}, cam_tgt{};
    if (!safe_read_bytes(mb + off301::kGfInfoViewProj, vp, sizeof vp)) return;
    safe_read(mb + off301::kGfInfoCamPos, cam_pos);
    safe_read(mb + off301::kGfInfoCamTgt, cam_tgt);

    // Reject zero/uninitialized matrix (title screen / boot / movie).
    float vp_norm = 0.f;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(vp[i])) return;
        vp_norm += std::fabs(vp[i]);
    }
    if (vp_norm < 1e-3f) return;

    // 2. Read current map ID, player 3D position, and combat/cutscene auto-hide state.
    uint32_t map_id = 0;
    if (!read_current_map_id(mb, map_id)) return;

    const bool in_battle = read_in_battle(mb);
    const bool in_event = read_in_event(mb);
    const bool hide_now = in_battle || in_event;
    static float s_vis_alpha = 1.0f;
    static int64_t s_last_vis_ms = 0;
    const int64_t cur_ms = now_ms();
    const float dt = (s_last_vis_ms > 0) ? std::clamp(static_cast<float>(cur_ms - s_last_vis_ms) * 0.001f, 0.001f, 0.1f) : 0.016f;
    s_last_vis_ms = cur_ms;
    if (hide_now) {
        s_vis_alpha = std::max(0.0f, s_vis_alpha - dt / 0.20f);  // 0.2s smooth fade-out into combat/cutscene
    } else {
        s_vis_alpha = std::min(1.0f, s_vis_alpha + dt / 0.50f);  // 0.5s gentle re-entry after combat/cutscene
    }

    Vec3 player{};
    if (!read_player_pos(mb, cam_tgt, player)) return;

    // 3. Check if player has an active Custom Pin (from F4 picker) on this map; otherwise read active quests (cat 0/1).
    std::vector<NavMarker> markers;
    markers.reserve(16);
    OffMapTarget offmap;
    bool using_custom_pin = false;
    std::string custom_pin_name;
    {
        std::lock_guard<std::mutex> lk(custom_pin().mu);
        if (custom_pin().active) {
            if (custom_pin().map_id != map_id) {
                custom_pin().active = false;  // auto-clear on map change
            } else {
                const float dx = custom_pin().pos.x - player.x;
                const float dy = custom_pin().pos.y - player.y;
                const float dz = custom_pin().pos.z - player.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < 4.5f) {
                    custom_pin().active = false;  // auto-clear on arrival
                } else {
                    NavMarker nm;
                    nm.pos = custom_pin().pos;
                    nm.kind = MarkerKind::Pin;
                    nm.cat = 9;
                    nm.dist = d;
                    nm.dy = dy;
                    markers.push_back(nm);
                    using_custom_pin = true;
                    custom_pin_name = custom_pin().name;
                }
            }
        }
    }
    if (!using_custom_pin) {
        collect_locators(mb, map_id, markers, offmap);
    }

    for (auto& m : markers) {
        const float dx = m.pos.x - player.x;
        const float dy = m.pos.y - player.y;
        const float dz = m.pos.z - player.z;
        m.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        m.dy = dy;
    }

    // Sort: MainStory first, then closest distance.
    std::sort(markers.begin(), markers.end(), [](const NavMarker& a, const NavMarker& b) {
        if ((a.kind == MarkerKind::MainStory) != (b.kind == MarkerKind::MainStory)) {
            return a.kind == MarkerKind::MainStory;
        }
        return a.dist < b.dist;
    });

    // 4. Collect skip-travel landmarks on current map for nearest-landmark hint.
    std::vector<LandmarkInfo> landmarks;
    const bool need_lm = g_debug.load(std::memory_order_relaxed) || (!markers.empty() && markers[0].dist > 100.f);
    if (need_lm) {
        landmarks.reserve(32);
        collect_landmarks(mb, landmarks);
    }

    // Find best skip-travel landmark for primary marker (if far away).
    char lm_hint[160] = {};
    const LandmarkInfo* best_lm = nullptr;
    if (!markers.empty() && markers[0].dist > 100.f && !landmarks.empty()) {
        float best_d = markers[0].dist * 0.75f;
        for (const auto& lm : landmarks) {
            const float dx = lm.pos.x - markers[0].pos.x;
            const float dy = lm.pos.y - markers[0].pos.y;
            const float dz = lm.pos.z - markers[0].pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < best_d) {
                best_d = d;
                best_lm = &lm;
            }
        }
        if (best_lm) {
            const int ldy = static_cast<int>(std::lround(markers[0].pos.y - best_lm->pos.y));
            char lm_name[96] = {};
            if (read_landmark_localized_name(mb, *best_lm, lm_name, sizeof lm_name)) {
                std::snprintf(lm_hint, sizeof lm_hint, ">> %s · %dm (ΔY %+dm)",
                              lm_name, static_cast<int>(std::lround(best_d)), ldy);
            } else {
                std::snprintf(lm_hint, sizeof lm_hint, ">> %dm (ΔY %+dm)",
                              static_cast<int>(std::lround(best_d)), ldy);
            }
        }
    }

    // 4b. Tier-3 async NavMesh 3D conveyor-belt ribbon (mode == 2, non-blocking, hidden during combat/cutscenes).
    PathResult nav_path;
    char map_res[32] = {};
    if (mode >= 2 && s_vis_alpha > 0.01f && !markers.empty() && read_map_resource_name(mb, map_id, map_res, sizeof map_res)) {
        mesh_update_and_get(map_res,
                            {player.x, player.y, player.z},
                            {markers[0].pos.x, markers[0].pos.y, markers[0].pos.z},
                            nav_path);
        if (nav_path.reachable && nav_path.map_res == map_res && nav_path.points.size() >= 2) {
            submit_ground_ribbon(c, vp, nav_path.points, kind_color(markers[0].kind), s_vis_alpha);
        }
    }

    // 4c. Cross-map 3-level Skip-Travel recommendation OR active Custom Pin top strip.
    char cross_map_name[96] = {};
    char cross_sub_name[96] = {};
    char cross_lm_name[96] = {};
    if (using_custom_pin && s_vis_alpha > 0.02f) {
        char pin_hdr[192];
        std::snprintf(pin_hdr, sizeof pin_hdr, "%s  (F4 切换 / 取消)", custom_pin_name.c_str());
        draw_cross_map_banner(c, scale, s_vis_alpha, ">> 自定义导航  ", pin_hdr, "", "", 0xCE93D800u);
    } else if (markers.empty() && offmap.valid && s_vis_alpha > 0.02f) {
        resolve_cross_map_hint(mb, offmap,
                               cross_map_name, sizeof cross_map_name,
                               cross_sub_name, sizeof cross_sub_name,
                               cross_lm_name, sizeof cross_lm_name);
        draw_cross_map_banner(c, scale, s_vis_alpha, ">> 跨图传送指引  ",
                              cross_map_name, cross_sub_name, cross_lm_name,
                              offmap.cat == 0 ? 0xFFD54F00u : 0x4FC3F700u);
    }

    // 5. Project and draw markers (draw secondary markers first so primary sits on top).
    const float W = static_cast<float>(c.width);
    const float H = static_cast<float>(c.height);
    if (s_vis_alpha > 0.05f) {
        const size_t max_draw = std::min<size_t>(markers.size(), 8u);
        for (size_t idx = max_draw; idx-- > 0;) {
            const auto& m = markers[idx];
            const bool is_primary = (idx == 0);
            const Vec3 lifted{m.pos.x, m.pos.y + 1.8f, m.pos.z};
            const Projected pr = project_world(vp, lifted, W, H);
            const uint32_t col = kind_color(m.kind);
            if (pr.on_screen) {
                draw_beacon(c, pr.sx, pr.sy, scale, col, m.dist, m.dy, is_primary, is_primary ? lm_hint : nullptr);
            } else if (is_primary) {
                draw_edge_arrow(c, pr.sx, pr.sy, scale, col, m.dist, m.dy, lm_hint);
            }
        }

        // Draw subtle teal pin on the recommended skip-travel landmark if it is on screen.
        if (best_lm) {
            const Vec3 lifted{best_lm->pos.x, best_lm->pos.y + 2.2f, best_lm->pos.z};
            const Projected pr = project_world(vp, lifted, W, H);
            if (pr.on_screen) {
                const float r = 6.5f * scale;
                c.circle_filled(pr.sx, pr.sy, r + 1.5f * scale, 0x000000B0u);
                c.circle_filled(pr.sx, pr.sy, r, 0x26C6DAEEu);
                c.circle(pr.sx, pr.sy, r + 2.f * scale, 0xE0F7FACC, 1.5f * scale);
            }
        }
    }

    // 6. Optional debug telemetry strip ([xc2] nav_debug = 1).
    if (g_debug.load(std::memory_order_relaxed)) {
        char dbg[320];
        if (!markers.empty()) {
            char pinfo[64] = {};
            if (nav_path.reachable && nav_path.map_res == map_res) {
                std::snprintf(pinfo, sizeof pinfo, " | path=%.0fm(%zu)", nav_path.path_len, nav_path.points.size());
            } else if (map_res[0]) {
                std::snprintf(pinfo, sizeof pinfo, " | mesh=%s(%s)", map_res, nav_path.loaded ? "unreach" : "loading");
            }
            std::snprintf(dbg, sizeof dbg,
                          "NAV map=%u(%s) | P(%.1f, %.1f, %.1f) | T0[c%u](%.1f, %.1f, %.1f) d=%.0fm dy=%+.0fm | loc=%zu off=%u bat=%d evt=%d%s",
                          map_id, map_res[0] ? map_res : "?",
                          player.x, player.y, player.z,
                          markers[0].cat, markers[0].pos.x, markers[0].pos.y, markers[0].pos.z,
                          markers[0].dist, markers[0].dy, markers.size(), offmap.map_id, in_battle ? 1 : 0, in_event ? 1 : 0, pinfo);
        } else {
            std::snprintf(dbg, sizeof dbg,
                          "NAV map=%u | P(%.1f, %.1f, %.1f) | loc=0 off=%u(%s/%s) bat=%d evt=%d",
                          map_id, player.x, player.y, player.z, offmap.map_id,
                          cross_map_name[0] ? cross_map_name : "-", cross_lm_name[0] ? cross_lm_name : "-",
                          in_battle ? 1 : 0, in_event ? 1 : 0);
        }
        const float sz = 14.f * scale;
        float tw = 0.f, th = 0.f;
        c.measure_text(sz, dbg, &tw, &th);
        const float x0 = 14.f * scale, y0 = 14.f * scale;
        c.rect_filled(x0 - 6.f, y0 - 3.f, x0 + tw + 6.f, y0 + th + 3.f, 0x000000B8u, 4.f);
        c.text(x0, y0, sz, 0xA5D6A7FFu, dbg);
    }
}

nxrt::gfx::PickerCallbacks make_picker_callbacks() {
    nxrt::gfx::PickerCallbacks cb;
    cb.query = [](std::vector<nxrt::gfx::PickerItem>& out_items, bool& out_has_pin, std::string& out_pin_name) {
        out_items.clear();
        const uint64_t mb = g_main_base.load(std::memory_order_relaxed);
        if (!mb) return;
        uint32_t map_id = 0;
        if (!read_current_map_id(mb, map_id)) return;
        Vec3 cam_tgt{}, player{};
        safe_read(mb + off301::kGfInfoCamTgt, cam_tgt);
        read_player_pos(mb, cam_tgt, player);

        std::vector<LandmarkInfo> lms;
        lms.reserve(64);
        collect_landmarks(mb, lms);

        std::vector<NpcInfo> npcs;
        npcs.reserve(128);
        collect_npcs(mb, npcs);

        std::vector<CustomPinState::Entry> new_entries;
        new_entries.reserve(lms.size() + npcs.size());

        auto push_item = [&](const Vec3& pos, const std::string& name, uint8_t group) {
            const float dx = pos.x - player.x, dy = pos.y - player.y, dz = pos.z - player.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const int idy = static_cast<int>(std::lround(dy));
            char det[48];
            if (std::abs(idy) >= 3) {
                std::snprintf(det, sizeof det, "%dm  %s%dm", static_cast<int>(std::lround(dist)), idy > 0 ? "▲+" : "▼", idy);
            } else {
                std::snprintf(det, sizeof det, "%dm", static_cast<int>(std::lround(dist)));
            }
            const uint64_t idx = new_entries.size();
            new_entries.push_back({pos, name, group, map_id});
            nxrt::gfx::PickerItem it;
            it.id = idx;
            it.group = group;
            it.dist = dist;
            it.name = name;
            it.detail = det;
            out_items.push_back(std::move(it));
        };

        for (const auto& lm : lms) {
            char nm[96] = {};
            if (read_landmark_localized_name(mb, lm, nm, sizeof nm)) {
                push_item(lm.pos, nm, 0);
            }
        }
        for (const auto& npc : npcs) {
            push_item(npc.pos, npc.name, 1);
        }

        std::sort(out_items.begin(), out_items.end(), [](const nxrt::gfx::PickerItem& a, const nxrt::gfx::PickerItem& b) {
            if (a.group != b.group) return a.group < b.group;
            return a.dist < b.dist;
        });

        std::lock_guard<std::mutex> lk(custom_pin().mu);
        custom_pin().entries = std::move(new_entries);
        out_has_pin = custom_pin().active && custom_pin().map_id == map_id;
        out_pin_name = out_has_pin ? custom_pin().name : std::string();
    };

    cb.select = [](uint64_t id) {
        std::lock_guard<std::mutex> lk(custom_pin().mu);
        if (id >= custom_pin().entries.size()) return;
        const auto& e = custom_pin().entries[id];
        custom_pin().active = true;
        custom_pin().map_id = e.map_id;
        custom_pin().pos = e.pos;
        custom_pin().name = e.name;
        custom_pin().group = e.group;
        if (g_mode.load(std::memory_order_relaxed) == 0) g_mode.store(2, std::memory_order_relaxed);
        g_reveal_start_ms.store(now_ms(), std::memory_order_relaxed);
        NXLOG_I(kTag, "custom pin set: '%s' (%.1f, %.1f, %.1f)", e.name.c_str(), e.pos.x, e.pos.y, e.pos.z);
    };

    cb.clear = []() {
        std::lock_guard<std::mutex> lk(custom_pin().mu);
        custom_pin().active = false;
        custom_pin().name.clear();
        NXLOG_I(kTag, "custom pin cleared (auto quest tracking restored)");
    };
    return cb;
}

}  // namespace xc2::nav
