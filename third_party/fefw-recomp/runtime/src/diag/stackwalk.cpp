// SPDX-License-Identifier: GPL-3.0-or-later
// Pattern from smo-recomp runtime/core/hostrt.c crash_filter (RtlVirtualUnwind loop).
#include "stackwalk.h"

namespace nxrt::detail {

namespace {

bool stack_readable(DWORD64 addr, size_t bytes) {
    if (addr < 0x10000 || (addr & 7) != 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & kReadable)) return false;
    const DWORD64 region_end = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) + mbi.RegionSize;
    return addr + bytes <= region_end;
}

}  // namespace

size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    if (!ctx || !frames || max == 0) return 0;
    CONTEXT c = *ctx;
    size_t n = 0;
    __try {
        while (n < max && c.Rip) {
            frames[n++] = c.Rip;
            DWORD64 image_base = 0;
            RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
            if (!rf) {  // leaf function without unwind info: the return address is at [rsp]
                if (!stack_readable(c.Rsp, sizeof(DWORD64))) break;
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
                if (n > 1) break;   // only the first frame may be a leaf; anything else means we lost the chain
                continue;
            }
            if (!stack_readable(c.Rsp, 64)) break;
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Under Wine/Proton, unwinding a thread suspended inside a unix-call transition can fault in ntdll!RtlVirtualUnwind.
    }
    return n;
}

}  // namespace nxrt::detail
