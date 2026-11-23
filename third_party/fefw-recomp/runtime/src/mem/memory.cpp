// SPDX-License-Identifier: GPL-3.0-or-later
// Guest address space on the host (layout and contract: include/nxrt/memory.h).
//  - fixed reservations at exact addresses (images, stacks, the kernel region), committed on demand;
//  - the heap: one pagefile-backed section (SEC_RESERVE) with a primary view at kHeapBase and alias views of the same
//    section pages, so one physical page can be seen at two guest addresses;
//  - a Horizon-style vm map kept beside the real mappings for svcQueryMemory.
#include "nxrt/game.h"
#include "nxrt/memory.h"

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nxrt::mem {
namespace {

constexpr const char* kTag = "mem";
constexpr uint64_t kAddrLimit = 0x800000000000ull;  // user-mode address limit of x64 Windows (47 bits)
constexpr uint64_t kVmEnd = 0x8000000000ull;         // end of the address space reported by vm_query
constexpr int kAliasTries = 16;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Whole pages covering [addr, addr+size). False when the range leaves the user address space.
bool page_span(uint64_t addr, uint64_t size, uint64_t* start, uint64_t* end) {
    if (size > kAddrLimit || addr > kAddrLimit - size) return false;
    *start = addr & ~(kPage - 1);
    *end = align_up(addr + size, kPage);
    return true;
}

DWORD protect_of(Prot p) {
    switch (p) {
        case Prot::R: return PAGE_READONLY;
        case Prot::RW: return PAGE_READWRITE;
        case Prot::RX: return PAGE_EXECUTE_READ;
        case Prot::RWX: return PAGE_EXECUTE_READWRITE;
        case Prot::None: break;
    }
    return PAGE_NOACCESS;
}

// ---- fixed reservations (guarded by g_fixed_mu) ----
struct Fixed {
    uint64_t size;
    std::string tag;
};
std::mutex g_fixed_mu;
std::map<uint64_t, Fixed> g_fixed;

// ---- module state ----
std::mutex g_state_mu;                    // init / shutdown
std::atomic<bool> g_inited{false};
std::atomic<uint64_t> g_default_heap{0};  // bytes, from Config or [memory] heap_gb
std::mutex g_alias_base_mu;
std::atomic<uint64_t> g_alias_base{0};

bool host_range_free(uint64_t base, uint64_t size, MEMORY_BASIC_INFORMATION* blocker = nullptr) {
    uint64_t cur = base;
    const uint64_t end = base + size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof mbi) != sizeof mbi) return false;
        if (mbi.State != MEM_FREE) {
            if (blocker) *blocker = mbi;
            return false;
        }
        const uint64_t r_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + static_cast<uint64_t>(mbi.RegionSize);
        if (r_end <= cur) return false;
        cur = r_end;
    }
    return true;
}

uint64_t select_alias_base() {
    MEMORY_BASIC_INFORMATION blocker{};
    if (host_range_free(kAliasBase, kAliasSize, &blocker)) return kAliasBase;
    static constexpr uint64_t kPreferred[] = {
        0x4000000000ull, 0x3000000000ull, 0x1000000000ull, 0x5000000000ull,
    };
    for (uint64_t cand : kPreferred) {
        if (cand + kAliasSize <= kMainBase && host_range_free(cand, kAliasSize)) {
            NXLOG_W(kTag, "alias region %llx (+%llx) blocked on host at %p (+%llx, state 0x%lx, type 0x%lx); using %llx instead",
                    static_cast<unsigned long long>(kAliasBase), static_cast<unsigned long long>(kAliasSize),
                    blocker.BaseAddress, static_cast<unsigned long long>(blocker.RegionSize),
                    static_cast<unsigned long>(blocker.State), static_cast<unsigned long>(blocker.Type),
                    static_cast<unsigned long long>(cand));
            return cand;
        }
    }
    for (uint64_t cand = 0x0800000000ull; cand + kAliasSize <= kMainBase; cand += 0x10000000ull) {
        if (host_range_free(cand, kAliasSize)) {
            NXLOG_W(kTag, "alias region %llx (+%llx) blocked on host at %p (+%llx, state 0x%lx, type 0x%lx); using %llx instead",
                    static_cast<unsigned long long>(kAliasBase), static_cast<unsigned long long>(kAliasSize),
                    blocker.BaseAddress, static_cast<unsigned long long>(blocker.RegionSize),
                    static_cast<unsigned long>(blocker.State), static_cast<unsigned long>(blocker.Type),
                    static_cast<unsigned long long>(cand));
            return cand;
        }
    }
    NXLOG_W(kTag, "alias region %llx (+%llx) blocked on host at %p (+%llx) and no free 128 GiB window found; keeping %llx",
            static_cast<unsigned long long>(kAliasBase), static_cast<unsigned long long>(kAliasSize),
            blocker.BaseAddress, static_cast<unsigned long long>(blocker.RegionSize),
            static_cast<unsigned long long>(kAliasBase));
    return kAliasBase;
}

std::mutex g_kernel_mu;
uint64_t g_kernel_next = kKernelBase;     // guarded by g_kernel_mu

std::mutex g_vm_mu;
std::map<uint64_t, VmInfo> g_vm;          // start -> block, guarded by g_vm_mu

// ---- heap (guarded by g_heap_mu) ----
std::mutex g_heap_mu;
HANDLE g_section = nullptr;
uint64_t g_heap_size = 0;                 // 0 = no heap; otherwise the section size = size of the primary view
std::vector<uint64_t> g_commit_bits;      // one bit per 4 KiB page of the section, set by heap_commit
uint64_t g_committed = 0;                 // bytes of pages whose bit is set
// Per 4 KiB page of the section, what the host reports for it: 0 never committed, 1 committed but no-access (a decommitted page or one
// the guest made inaccessible), 2 committed and accessible but not writable, 3 committed read-write. VirtualQuery over the section view
// walks every page table entry of the run of identical pages (about 75 ms for a 3 GiB committed heap), so is_mapped, protect and the
// write tracker's arm (heap_rw_pages) answer heap ranges from this table.
std::vector<uint8_t> g_heap_cls;
std::atomic<uint64_t> g_heap_epoch{0};    // bumped by every change of the table, inside the change's OsWriteScope
struct Alias {
    uint64_t size;
    uint64_t offset;                      // section offset of the first byte of the view
};
std::map<uint64_t, Alias> g_aliases;      // view address -> record
// Alias region in use: start -> size for every live view and for every candidate address the kernel refused.
std::map<uint64_t, uint64_t> g_alias_used;

// ---- gauges (include/nxrt/gauge.h); each callback takes the lock of the state it reads ----
// Removes its gauge when this file's statics are torn down (declared after them, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_heap_reserved("mem.heap_reserved", [] { return static_cast<int64_t>(heap_reserved()); });   // section size
GaugeHook gauge_heap_committed("mem.heap_committed", [] { return static_cast<int64_t>(heap_committed()); });   // committed through heap_commit
GaugeHook gauge_aliases("mem.aliases", [] {   // heap alias views live (g_heap_mu)
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_aliases.size());
});
GaugeHook gauge_alias_used("mem.alias_used", [] {   // alias-region ranges in use, plus addresses the kernel refused (never freed: only grows)
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_alias_used.size());
});
GaugeHook gauge_fixed("mem.fixed_regions", [] {   // fixed reservations of this module (images, stacks, kernel region)
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    return static_cast<int64_t>(g_fixed.size());
});
GaugeHook gauge_fixed_bytes("mem.fixed_bytes", [] {
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    uint64_t n = 0;
    for (const auto& kv : g_fixed) n += kv.second.size;
    return static_cast<int64_t>(n);
});
GaugeHook gauge_vm_blocks("mem.vm_blocks", [] {   // blocks of the Horizon-style vm map kept for QueryMemory
    std::lock_guard<std::mutex> lk(g_vm_mu);
    return static_cast<int64_t>(g_vm.size());
});

// [memory] fast_query = false: ask the host (VirtualQuery) instead of the page table above; for before/after measurements.
bool fast_query() {
    static const bool v = config_get_bool("memory", "fast_query", true);
    return v;
}

// True when [start, end) lies inside the heap primary view (g_heap_mu held).
bool in_heap_view(uint64_t start, uint64_t end) {
    return fast_query() && g_heap_size && !g_heap_cls.empty() && start >= kHeapBase && end <= kHeapBase + g_heap_size && start < end;
}

bool bit_test(uint64_t page) { return (g_commit_bits[page / 64] >> (page % 64)) & 1; }
void bit_set(uint64_t page) { g_commit_bits[page / 64] |= uint64_t{1} << (page % 64); }
void bit_clear(uint64_t page) { g_commit_bits[page / 64] &= ~(uint64_t{1} << (page % 64)); }

// ---- lazy commit (HANDOVER 80) ----------------------------------------------------------------------------------------
// SetHeapSize grants the guest a heap of several GiB, but on the host only the pages the game really touches are charged to the
// commit limit. The grant is recorded as a prefix (g_guest_hi) and MEM_COMMIT happens on the first touch: in heap_fault (the
// vectored handler below) for a guest access, or in heap_touch_locked for the paths that need real pages before the guest gets
// there (is_mapped, heap_rw_pages, alias and private views). [memory] heap_commit_mb (default 1024) stays eager so start-up and
// the first pools never fault; 0 commits everything, i.e. the old behaviour.
std::atomic<uint64_t> g_guest_hi{0};                     // end of the guest-committed prefix, offset from kHeapBase
std::atomic<uint64_t> g_real_bytes{0};                   // bytes really MEM_COMMITted
std::unique_ptr<std::atomic<uint64_t>[]> g_real_bits;    // one bit per kPage: really committed
size_t g_real_words = 0;
uint64_t g_eager_bytes = 0;                              // eager commit limit; 0 = no lazy pages
PVOID g_heap_veh = nullptr;

bool real_test(uint64_t page) {
    const size_t w = static_cast<size_t>(page >> 6);
    return w < g_real_words && ((g_real_bits[w].load(std::memory_order_relaxed) >> (page & 63)) & 1ull);
}
void real_set(uint64_t page) {
    const size_t w = static_cast<size_t>(page >> 6);
    if (w < g_real_words) g_real_bits[w].fetch_or(uint64_t{1} << (page & 63), std::memory_order_relaxed);
}
void real_clear(uint64_t page) {
    const size_t w = static_cast<size_t>(page >> 6);
    if (w < g_real_words) g_real_bits[w].fetch_and(~(uint64_t{1} << (page & 63)), std::memory_order_relaxed);
}
bool real_claim(uint64_t page) {   // true when this call set the bit
    const size_t w = static_cast<size_t>(page >> 6);
    if (w >= g_real_words) return false;
    const uint64_t m = uint64_t{1} << (page & 63);
    return (g_real_bits[w].fetch_or(m, std::memory_order_acq_rel) & m) == 0;
}

// MEM_COMMIT pages [p0, p1) of the primary view. g_heap_mu held, or the page already claimed by heap_fault.
bool commit_pages_locked(uint64_t p0, uint64_t p1) {
    for (uint64_t p = p0; p < p1;) {
        if (real_test(p)) {
            ++p;
            continue;
        }
        uint64_t q = p + 1;
        while (q < p1 && !real_test(q)) ++q;
        const uint64_t len = (q - p) * kPage;
        if (!VirtualAlloc(reinterpret_cast<void*>(kHeapBase + p * kPage), static_cast<SIZE_T>(len), MEM_COMMIT, PAGE_READWRITE)) {
            NXLOG_E(kTag, "lazy commit [%llx, +%llx): VirtualAlloc failed, error %lu", static_cast<unsigned long long>(p * kPage),
                    static_cast<unsigned long long>(len), static_cast<unsigned long>(GetLastError()));
            return false;
        }
        for (uint64_t r = p; r < q; ++r) real_set(r);
        g_real_bytes.fetch_add(len, std::memory_order_relaxed);
        p = q;
    }
    return true;
}

// Commit whatever part of [start, end) is granted but not committed yet. g_heap_mu held.
bool heap_touch_locked(uint64_t start, uint64_t end) {
    if (!g_heap_size || start >= end || start < kHeapBase || start >= kHeapBase + g_heap_size) return false;
    end = std::min<uint64_t>(end, kHeapBase + g_heap_size);
    const uint64_t p0 = (start - kHeapBase) / kPage;
    const uint64_t p1 = align_up(end - kHeapBase, kPage) / kPage;
    return p0 >= p1 ? true : commit_pages_locked(p0, p1);
}

// First touch of a granted heap page: commit the 64 KiB chunk holding it and let the faulting instruction run again. Reads only
// atomics (no lock: the faulting thread may already hold g_heap_mu, and this runs before every other handler).
LONG CALLBACK heap_fault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!rec || rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    const uint64_t hi = g_guest_hi.load(std::memory_order_acquire);
    if (!hi || a < kHeapBase || a >= kHeapBase + hi) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t page = (a - kHeapBase) / kPage;
    if (real_test(page)) return EXCEPTION_CONTINUE_SEARCH;   // committed already: this is a genuine fault
    // Commit the 64 KiB chunk that holds the page, but ONLY the pages of it that are still reserved: MEM_COMMIT with
    // PAGE_READWRITE also resets the protection of pages that are already committed, which would silently widen a page the guest
    // made read-only or no-access (Monolith's allocator uses exactly that). commit_pages_locked skips committed pages.
    const uint64_t chunk = (page * kPage) & ~(kAllocGranularity - 1);
    const uint64_t len = std::min<uint64_t>(kAllocGranularity, hi - chunk);
    if (!commit_pages_locked(chunk / kPage, (chunk + len) / kPage)) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8 || (k % 4096) == 0)
        NXLOG_I(kTag, "heap fault #%u at %llx: committed %llu KiB at +%llx (real %llu MiB of %llu)", k, static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(len >> 10), static_cast<unsigned long long>(chunk),
                static_cast<unsigned long long>(g_real_bytes.load(std::memory_order_relaxed) >> 20),
                static_cast<unsigned long long>(hi >> 20));
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Bookkeeping for a fresh heap: page tables, the eager-commit limit and the first-touch handler.
void init_heap_tables(uint64_t size) {
    g_heap_size = size;
    g_commit_bits.assign(static_cast<size_t>((size / kPage + 63) / 64), 0);
    g_heap_cls.assign(static_cast<size_t>(size / kPage), 0);
    g_real_words = static_cast<size_t>((size / kPage + 63) / 64);
    g_real_bits = std::make_unique<std::atomic<uint64_t>[]>(g_real_words);
    for (size_t i = 0; i < g_real_words; ++i) g_real_bits[i].store(0, std::memory_order_relaxed);
    g_committed = 0;
    g_guest_hi.store(0, std::memory_order_release);
    g_real_bytes.store(0, std::memory_order_relaxed);
    // Default 0 = commit the whole grant at SetHeapSize (the behaviour before lazy commit existed). A positive value keeps only that
    // many MiB eager and commits the rest on first touch; it is implemented and measured but NOT the default, see HANDOVER 80.4.
    int64_t mb = nxrt::config_get_int("memory", "heap_commit_mb", 0);
    if (mb < 0) mb = 0;
    g_eager_bytes = mb ? static_cast<uint64_t>(mb) << 20 : 0;
    if (!g_heap_veh) g_heap_veh = AddVectoredExceptionHandler(1, heap_fault);
    if (!g_heap_veh) NXLOG_W(kTag, "AddVectoredExceptionHandler failed: the heap cannot commit lazily");
}

// First address >= from at which `size` bytes are free in the alias region; 0 when there is none.
uint64_t find_free_alias(uint64_t from, uint64_t size) {
    const uint64_t base = alias_base();
    const uint64_t limit = base + kAliasSize;
    uint64_t cand = std::max(from, base);
    for (;;) {
        if (cand >= limit || size > limit - cand) return 0;
        bool moved = false;
        auto it = g_alias_used.upper_bound(cand);
        if (it != g_alias_used.begin()) {
            auto p = std::prev(it);
            if (p->first + p->second > cand) {
                cand = align_up(p->first + p->second, kAllocGranularity);
                moved = true;
            }
        }
        if (!moved && it != g_alias_used.end() && it->first < cand + size) {
            cand = align_up(it->first + it->second, kAllocGranularity);
            moved = true;
        }
        if (!moved) return cand;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// init / shutdown

bool init(const Config& cfg) {
    std::lock_guard lk(g_state_mu);
    if (g_inited.load()) return true;

    uint64_t heap = cfg.heap_reserve;
    if (heap == 0) {
        int64_t gb = nxrt::config_get_int("memory", "heap_gb", nxrt::game::info().platform == nxrt::game::Platform::Switch2 ? 12 : 6);
        if (gb < 1 || gb > 1024) {
            NXLOG_W(kTag, "[memory] heap_gb=%lld is out of range: using 6", static_cast<long long>(gb));
            gb = 6;
        }
        heap = static_cast<uint64_t>(gb) << 30;
    }
    g_default_heap.store(align_up(heap, kAllocGranularity));

    if (cfg.reserve_regions) {
        if (!reserve_fixed(kStackBase, kStackSize, "stack")) {
            NXLOG_E(kTag, "init failed: the stack region could not be reserved");
            return false;
        }
        if (!reserve_fixed(kKernelBase, kKernelSize, "kernel")) {
            NXLOG_E(kTag, "init failed: the kernel region could not be reserved");
            release_fixed(kStackBase);
            return false;
        }
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_inited.store(true);
    NXLOG_I(kTag, "init: stack %llx (+%llx), kernel %llx (+%llx), default heap %llu MiB, regions %s",
            static_cast<unsigned long long>(kStackBase), static_cast<unsigned long long>(kStackSize),
            static_cast<unsigned long long>(kKernelBase), static_cast<unsigned long long>(kKernelSize),
            static_cast<unsigned long long>(g_default_heap.load() >> 20), cfg.reserve_regions ? "reserved" : "skipped");
    return true;
}

void shutdown() {
    std::lock_guard lk(g_state_mu);
    {
        std::lock_guard hl(g_heap_mu);
        for (auto& [view, rec] : g_aliases) {
            (void)rec;
            if (!UnmapViewOfFile(reinterpret_cast<void*>(view))) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: UnmapViewOfFile(alias %llx) failed, error %lu",
                        static_cast<unsigned long long>(view), static_cast<unsigned long>(e));
            }
        }
        g_aliases.clear();
        g_alias_used.clear();
        if (g_heap_size && !g_section) {
            VirtualFree(reinterpret_cast<void*>(kHeapBase), 0, MEM_RELEASE);   // the private heap (heap_create)
        } else if (g_heap_size) {
            if (!UnmapViewOfFile(reinterpret_cast<void*>(kHeapBase))) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: UnmapViewOfFile(heap) failed, error %lu", static_cast<unsigned long>(e));
            }
        }
        if (g_section) CloseHandle(g_section);
        g_section = nullptr;
        g_heap_size = 0;
        g_commit_bits.clear();
        g_heap_cls.clear();
        g_committed = 0;
    }
    {
        std::lock_guard fl(g_fixed_mu);
        for (auto& [addr, f] : g_fixed) {
            (void)f;
            if (!VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE)) {
                DWORD e = GetLastError();
                NXLOG_E(kTag, "shutdown: VirtualFree(%llx) failed, error %lu", static_cast<unsigned long long>(addr),
                        static_cast<unsigned long>(e));
            }
        }
        g_fixed.clear();
    }
    {
        std::lock_guard vl(g_vm_mu);
        g_vm.clear();
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_default_heap.store(0);
    g_alias_base.store(0, std::memory_order_release);
    g_inited.store(false);
    NXLOG_I(kTag, "shutdown: all mappings of this module released");
}

uint64_t alias_base() {
    if (const uint64_t cached = g_alias_base.load(std::memory_order_acquire)) return cached;
    std::lock_guard lk(g_alias_base_mu);
    if (const uint64_t cached = g_alias_base.load(std::memory_order_relaxed)) return cached;
    const uint64_t chosen = select_alias_base();
    g_alias_base.store(chosen, std::memory_order_release);
    return chosen;
}

bool initialised() { return g_inited.load(); }

// ---------------------------------------------------------------------------------------------------------------------
// fixed mappings

bool reserve_fixed(uint64_t addr, uint64_t size, const char* tag) {
    if (size == 0 || addr % kAllocGranularity != 0 || addr >= kAddrLimit || size > kAddrLimit - addr) {
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: address must be 64 KiB aligned and inside the user address space",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "");
        return false;
    }
    size = align_up(size, kPage);
    std::lock_guard lk(g_fixed_mu);
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: VirtualAlloc failed, error %lu",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "",
                static_cast<unsigned long>(e));
        return false;
    }
    if (reinterpret_cast<uint64_t>(p) != addr) {  // cannot happen with MEM_RESERVE at a fixed address; never accept it
        VirtualFree(p, 0, MEM_RELEASE);
        NXLOG_E(kTag, "reserve %llx %s: got %p instead", static_cast<unsigned long long>(addr), tag ? tag : "", p);
        return false;
    }
    g_fixed[addr] = Fixed{size, tag ? tag : ""};
    NXLOG_D(kTag, "reserved %llx (+%llx) %s", static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(size), tag ? tag : "");
    return true;
}

bool release_fixed(uint64_t addr) {
    std::lock_guard lk(g_fixed_mu);
    auto it = g_fixed.find(addr);
    if (it == g_fixed.end()) {
        NXLOG_W(kTag, "release %llx: no reservation of this module starts there", static_cast<unsigned long long>(addr));
        return false;
    }
    if (!VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE)) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "release %llx: VirtualFree failed, error %lu", static_cast<unsigned long long>(addr),
                static_cast<unsigned long>(e));
        return false;
    }
    g_fixed.erase(it);
    return true;
}

bool commit(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);   // the write tracker keeps out of the range while it changes
    if (!VirtualAlloc(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), MEM_COMMIT, protect_of(p))) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "commit %llx (+%llx): VirtualAlloc failed, error %lu", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start), static_cast<unsigned long>(e));
        return false;
    }
    return true;
}

bool decommit(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);
    if (!VirtualFree(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), MEM_DECOMMIT)) {
        DWORD e = GetLastError();
        NXLOG_E(kTag, "decommit %llx (+%llx): VirtualFree failed, error %lu", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start), static_cast<unsigned long>(e));
        return false;
    }
    return true;
}

bool protect(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const DWORD np = protect_of(p);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);   // pages the write tracker armed get this protection, not its own
    {   // heap: the page table says whether every page is committed, so one VirtualProtect does it (no VirtualQuery over the view)
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            bool all = true;
            for (uint64_t q = p0; q < p1 && all; ++q) all = g_heap_cls[q] != 0;
            DWORD old = 0;
            if (all && VirtualProtect(reinterpret_cast<void*>(start), static_cast<SIZE_T>(end - start), np, &old)) {
                std::fill(g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p0), g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p1),
                          np == PAGE_NOACCESS ? uint8_t{1} : np == PAGE_READWRITE ? uint8_t{3} : uint8_t{2});
                g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
                return true;
            }
        }
    }
    // Pass 1: every page must be committed, so a failure never leaves the range half changed.
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) {
            NXLOG_E(kTag, "protect %llx: VirtualQuery failed, error %lu", static_cast<unsigned long long>(cur),
                    static_cast<unsigned long>(GetLastError()));
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            NXLOG_E(kTag, "protect %llx: page is not committed", static_cast<unsigned long long>(cur));
            return false;
        }
        cur = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    // Pass 2: region by region (a range can span several allocations, each has its own region).
    g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi);
        const uint64_t chunk_end = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(cur), static_cast<SIZE_T>(chunk_end - cur), np, &old)) {
            NXLOG_E(kTag, "protect %llx (+%llx): VirtualProtect failed, error %lu", static_cast<unsigned long long>(cur),
                    static_cast<unsigned long long>(chunk_end - cur), static_cast<unsigned long>(GetLastError()));
            return false;
        }
        cur = chunk_end;
    }
    return true;
}

uint64_t alloc_kernel(uint64_t size, uint64_t align) {
    if (!initialised() && !init()) return 0;
    if (align == 0) align = kPage;
    if (align & (align - 1)) {
        NXLOG_E(kTag, "alloc_kernel: alignment %llx is not a power of two", static_cast<unsigned long long>(align));
        return 0;
    }
    align = std::max(align, kPage);
    if (size == 0) size = 1;
    if (size > kKernelSize) return 0;
    size = align_up(size, kPage);

    std::lock_guard lk(g_kernel_mu);
    const uint64_t a = align_up(g_kernel_next, align);
    if (a < g_kernel_next || a > kKernelBase + kKernelSize || size > kKernelBase + kKernelSize - a) {
        NXLOG_E(kTag, "alloc_kernel: kernel region exhausted (%llx used of %llx)",
                static_cast<unsigned long long>(g_kernel_next - kKernelBase),
                static_cast<unsigned long long>(kKernelSize));
        return 0;
    }
    if (!commit(a, size, Prot::RW)) return 0;
    g_kernel_next = a + size;
    return a;
}

bool is_mapped(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    {
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            // The caller is about to use these bytes: a granted page that is not committed yet is committed here rather than left to
            // fault, so the answer is the truth and the write tracker / HLE paths get real pages (lazy commit, HANDOVER 80).
            if (!heap_touch_locked(start, end)) return false;
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            for (uint64_t q = p0; q < p1; ++q)
                if (g_heap_cls[q] < 2) return false;
            return true;
        }
    }
    for (uint64_t cur = start; cur < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        cur = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// heap section

bool heap_create(uint64_t reserve_bytes) {
    if (!initialised() && !init()) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size) return true;  // a heap already exists: nothing to do

    const uint64_t want = reserve_bytes ? reserve_bytes : g_default_heap.load();
    if (want == 0) {
        NXLOG_E(kTag, "heap_create: no size given and no configured default");
        return false;
    }
    const uint64_t size = align_up(want, kAllocGranularity);
    if (size > kHeapMax) {
        NXLOG_E(kTag, "heap_create: %llu bytes exceed the heap region limit of %llu bytes",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(kHeapMax));
        return false;
    }
    // [memory] heap_section: auto (default) = a section unless running under Wine (memory.h), 1 = always, 0 = never
    const std::string mode = nxrt::config_get_str("memory", "heap_section", "auto");
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
    // A big heap used to take the private path unconditionally, because a 10.4 GiB MapViewOfFileEx at kHeapBase once failed with
    // ERROR_INVALID_ADDRESS (HANDOVER 80). The section view works again as of 2026-11-14 (measured: "section of 12288 MiB, primary
    // view at 7800000000"), and the private path costs the alias views every NVN pool needs. So the section is now tried for every
    // size and the private path is a REACTIVE fallback: only when the section or its view actually fails. [memory] heap_section = 1
    // still forbids the fallback (a failure is then an error), 0 still forbids the section.
    auto make_private = [&](const char* why) -> bool {
        void* p = VirtualAlloc(reinterpret_cast<void*>(kHeapBase), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
        if (reinterpret_cast<uint64_t>(p) != kHeapBase) {
            NXLOG_E(kTag, "heap_create: VirtualAlloc(MEM_RESERVE %llu MiB) at %llx failed, error %lu", static_cast<unsigned long long>(size >> 20),
                    static_cast<unsigned long long>(kHeapBase), static_cast<unsigned long>(GetLastError()));
            if (p) VirtualFree(p, 0, MEM_RELEASE);
            return false;
        }
        init_heap_tables(size);
        NXLOG_I(kTag, "heap: private memory of %llu MiB at %llx, no section (%s): no alias views, pools import at their own address",
                static_cast<unsigned long long>(size >> 20), static_cast<unsigned long long>(kHeapBase), why);
        return true;
    };
    if (mode == "0") return make_private("[memory] heap_section = 0");
    if (mode != "1" && wine) return make_private("Wine");
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                    static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr);
    if (!sec) {
        DWORD e = GetLastError();
        if (mode == "1") {
            NXLOG_E(kTag, "heap_create: CreateFileMapping(%llu MiB) failed, error %lu",
                    static_cast<unsigned long long>(size >> 20), static_cast<unsigned long>(e));
            return false;
        }
        NXLOG_W(kTag, "heap_create: CreateFileMapping(%llu MiB) failed, error %lu: private memory instead (no alias views)",
                static_cast<unsigned long long>(size >> 20), static_cast<unsigned long>(e));
        return make_private("CreateFileMapping failed");
    }
    void* v = MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size),
                              reinterpret_cast<void*>(kHeapBase));
    if (!v) {
        DWORD e = GetLastError();
        CloseHandle(sec);
        if (mode == "1") {
            NXLOG_E(kTag, "heap_create: MapViewOfFileEx at %llx failed, error %lu",
                    static_cast<unsigned long long>(kHeapBase), static_cast<unsigned long>(e));
            return false;
        }
        NXLOG_W(kTag, "heap_create: MapViewOfFileEx(%llu MiB) at %llx failed, error %lu: private memory instead (no alias views)",
                static_cast<unsigned long long>(size >> 20), static_cast<unsigned long long>(kHeapBase), static_cast<unsigned long>(e));
        return make_private("MapViewOfFileEx failed");
    }
    if (reinterpret_cast<uint64_t>(v) != kHeapBase) {
        NXLOG_E(kTag, "heap_create: primary view landed at %p, not %llx", v, static_cast<unsigned long long>(kHeapBase));
        UnmapViewOfFile(v);
        CloseHandle(sec);
        return false;
    }
    g_section = sec;
    init_heap_tables(size);
    NXLOG_I(kTag, "heap: section of %llu MiB, primary view at %llx", static_cast<unsigned long long>(size >> 20),
            static_cast<unsigned long long>(kHeapBase));
    return true;
}

bool heap_exists() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size != 0;
}

bool heap_has_section() {
    std::lock_guard lk(g_heap_mu);
    return g_section != nullptr;
}

uint64_t heap_reserved() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size;
}

bool heap_commit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) {
        NXLOG_W(kTag, "heap_commit: no heap");
        return false;
    }
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_commit: [%llx, +%llx) is outside the %llu MiB section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(g_heap_size >> 20));
        return false;
    }
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    // The guest's grant is recorded first: from here on a first touch commits the page (heap_fault), and the paths that need real
    // pages commit them themselves (heap_touch_locked). The guest view (g_heap_cls = 3, bit set) is the granted range either way.
    for (uint64_t p = start / kPage; p < end / kPage; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            g_committed += kPage;
        }
        g_heap_cls[p] = 3;
    }
    if (end > g_guest_hi.load(std::memory_order_relaxed)) g_guest_hi.store(end, std::memory_order_release);
    // Eager part: [memory] heap_commit_mb, so start-up and the first pools never take a fault. 0 = commit everything.
    uint64_t eager_end = end;
    if (g_eager_bytes) eager_end = std::min(end, std::max<uint64_t>(g_eager_bytes, kAllocGranularity));
    if (eager_end > start) {
        const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), eager_end - start);   // MEM_COMMIT makes committed pages read-write again
        if (!commit_pages_locked(start / kPage, eager_end / kPage)) return false;
    }
    g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

bool heap_decommit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) return false;
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_decommit: [%llx, +%llx) is outside the section", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size));
        return false;
    }
    // VirtualFree(MEM_DECOMMIT) is refused on pages of a section view (ERROR_INVALID_PARAMETER), so the pages are made
    // PAGE_NOACCESS through the primary view instead: guest accesses fault, the host pages stay charged to the section
    // (as in smo-recomp's SetMemoryHeapSize), and the alias views still show the section's contents.
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    if (end >= g_guest_hi.load(std::memory_order_relaxed)) g_guest_hi.store(start, std::memory_order_release);   // the grant shrinks with the heap
    const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), end - start);
    for (uint64_t p = start / kPage; p < end / kPage;) {
        if (!bit_test(p) || !real_test(p)) {   // never granted, or granted but never committed: already inaccessible
            p++;
            continue;
        }
        uint64_t run = p;
        while (run < end / kPage && bit_test(run)) run++;
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kHeapBase + p * kPage), static_cast<SIZE_T>((run - p) * kPage),
                            PAGE_NOACCESS, &old)) {
            DWORD e = GetLastError();
            NXLOG_E(kTag, "heap_decommit [%llx, +%llx): VirtualProtect failed, error %lu",
                    static_cast<unsigned long long>(p * kPage), static_cast<unsigned long long>((run - p) * kPage),
                    static_cast<unsigned long>(e));
            return false;
        }
        for (uint64_t q = p; q < run; q++) {
            bit_clear(q);
            g_committed -= kPage;
            g_heap_cls[q] = 1;
        }
        g_heap_epoch.fetch_add(1, std::memory_order_seq_cst);
        p = run;
    }
    return true;
}

int heap_rw_pages(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return 0;
    std::lock_guard lk(g_heap_mu);
    if (!in_heap_view(start, end)) return -1;
    // VirtualProtect (the guard pages the write tracker arms) needs committed pages: commit the granted ones here.
    if (!heap_touch_locked(start, end)) return 0;
    const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
    for (uint64_t q = p0; q < p1; ++q)
        if (g_heap_cls[q] != 3) return 0;
    return 1;
}

uint64_t heap_epoch() { return g_heap_epoch.load(std::memory_order_seq_cst); }

uint64_t heap_committed() {   // what the host is charged, not what the guest was granted
    return g_real_bytes.load(std::memory_order_relaxed);
}

uint64_t heap_granted() {     // the guest's SetHeapSize value
    return g_guest_hi.load(std::memory_order_acquire);
}

uint64_t heap_alias(uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size || !g_section) {
        NXLOG_W(kTag, "heap_alias: %s", g_heap_size ? "the heap is private memory (no section): no alias views" : "no heap");
        return 0;
    }
    if (size == 0 || offset % kAllocGranularity || size % kAllocGranularity || offset > g_heap_size ||
        size > g_heap_size - offset) {
        NXLOG_W(kTag, "heap_alias: [%llx, +%llx) must be 64 KiB multiples inside the section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
        return 0;
    }
    if (at % kAllocGranularity) {
        NXLOG_W(kTag, "heap_alias: target %llx is not 64 KiB aligned", static_cast<unsigned long long>(at));
        return 0;
    }
    // A view of a granted page that is not committed yet would read zeros the guest never wrote: commit first (lazy commit).
    if (!heap_touch_locked(kHeapBase + offset, kHeapBase + offset + size)) return 0;
    const DWORD access = (p == Prot::R) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    const DWORD off_hi = static_cast<DWORD>(offset >> 32);
    const DWORD off_lo = static_cast<DWORD>(offset & 0xFFFFFFFFu);

    ww::ban_section(offset, size);   // before the view exists: writes through it would not fault in the primary view
    if (at != 0) {
        void* v = MapViewOfFileEx(g_section, access, off_hi, off_lo, static_cast<SIZE_T>(size), reinterpret_cast<void*>(at));
        if (!v) {
            DWORD e = GetLastError();
            NXLOG_E(kTag, "heap_alias at %llx (section %llx, +%llx): MapViewOfFileEx failed, error %lu",
                    static_cast<unsigned long long>(at), static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size), static_cast<unsigned long>(e));
            return 0;
        }
        if (reinterpret_cast<uint64_t>(v) != at) {
            UnmapViewOfFile(v);
            NXLOG_E(kTag, "heap_alias: view landed at %p, not %llx", v, static_cast<unsigned long long>(at));
            return 0;
        }
        g_aliases.emplace(at, Alias{size, offset});
        g_alias_used[at] = size;
        NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(at));
        return at;
    }

    uint64_t cand = alias_base();
    for (int tries = 0; tries < kAliasTries; tries++) {
        cand = find_free_alias(cand, size);
        if (!cand) break;
        void* v = MapViewOfFileEx(g_section, access, off_hi, off_lo, static_cast<SIZE_T>(size), reinterpret_cast<void*>(cand));
        if (v && reinterpret_cast<uint64_t>(v) == cand) {
            g_aliases.emplace(cand, Alias{size, offset});
            g_alias_used[cand] = size;
            NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(cand));
            return cand;
        }
        if (v) UnmapViewOfFile(v);
        NXLOG_W(kTag, "heap_alias: %llx is taken (error %lu): marking it unusable",
                static_cast<unsigned long long>(cand), static_cast<unsigned long>(GetLastError()));
        g_alias_used[cand] = size;  // refused by the kernel: never offer it again
    }
    NXLOG_E(kTag, "heap_alias: no free range for %llx bytes in the alias region", static_cast<unsigned long long>(size));
    return 0;
}

void* heap_private_view(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_section || size == 0 || offset % kAllocGranularity || offset > g_heap_size || size > g_heap_size - offset) return nullptr;
    // The GPU imports through this view, so the pages must exist: commit the granted ones first (lazy commit, HANDOVER 80).
    if (!heap_touch_locked(kHeapBase + offset, kHeapBase + offset + size)) return nullptr;
    return MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset & 0xFFFFFFFFu), static_cast<SIZE_T>(size));
}

bool heap_private_view_unmap(void* view) { return view && UnmapViewOfFile(view) != 0; }

bool heap_alias_unmap(uint64_t view_addr) {
    std::lock_guard lk(g_heap_mu);
    auto it = g_aliases.find(view_addr);
    if (it == g_aliases.end()) {
        NXLOG_W(kTag, "heap_alias_unmap: %llx is not an alias view", static_cast<unsigned long long>(view_addr));
        return false;
    }
    if (!UnmapViewOfFile(reinterpret_cast<void*>(view_addr))) {
        NXLOG_E(kTag, "heap_alias_unmap %llx: UnmapViewOfFile failed, error %lu",
                static_cast<unsigned long long>(view_addr), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    g_alias_used.erase(view_addr);
    g_aliases.erase(it);
    return true;
}

void heap_alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard lk(g_heap_mu);
    for (const auto& a : g_aliases) out.emplace_back(a.second.offset, a.second.size);
}

size_t heap_alias_count() {
    std::lock_guard lk(g_heap_mu);
    return g_aliases.size();
}

bool heap_offset_of(uint64_t addr, uint64_t* offset) {
    if (!offset) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size && addr >= kHeapBase && addr - kHeapBase < g_heap_size) {
        *offset = addr - kHeapBase;
        return true;
    }
    auto it = g_aliases.upper_bound(addr);
    if (it == g_aliases.begin()) return false;
    --it;
    if (addr - it->first < it->second.size) {
        *offset = it->second.offset + (addr - it->first);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// vm map (svcQueryMemory)

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    if (size == 0 || addr + size < addr) return;
    const uint64_t end = addr + size;
    std::lock_guard lk(g_vm_mu);
    // Cut every block that overlaps [addr, end); the parts outside the new range stay.
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm.emplace(b.addr, VmInfo{b.addr, addr - b.addr, b.state, b.perm});
        if (b.addr + b.size > end) g_vm.emplace(end, VmInfo{end, b.addr + b.size - end, b.state, b.perm});
    }
    if (state != kMemFree) g_vm.emplace(addr, VmInfo{addr, size, state, perm});
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_mu);
    auto it = g_vm.upper_bound(addr);  // first block that starts after addr
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        const uint64_t gap_start = p->second.addr + p->second.size;
        const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
        return VmInfo{gap_start, gap_end - gap_start, kMemFree, 0};
    }
    const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
    return VmInfo{0, gap_end, kMemFree, 0};
}

}  // namespace nxrt::mem
