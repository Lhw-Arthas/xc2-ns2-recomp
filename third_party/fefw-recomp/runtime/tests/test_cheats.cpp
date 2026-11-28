// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "nxrt/cheats.h"
#include "nxrt/input_source.h"
#include "test_framework.h"

NXTEST_GROUP(cheats)

namespace {

struct MemArena {
    uint8_t* base = nullptr;
    size_t size = 0;
    explicit MemArena(size_t sz) : size(sz) {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    ~MemArena() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    uint64_t addr() const { return reinterpret_cast<uint64_t>(base); }
    template <typename T>
    void write(size_t off, T v) {
        std::memcpy(base + off, &v, sizeof(T));
    }
    template <typename T>
    T read(size_t off) const {
        T v{};
        std::memcpy(&v, base + off, sizeof(T));
        return v;
    }
};

struct TempDir {
    std::filesystem::path root;
    explicit TempDir(const char* name) {
        root = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

}  // namespace

NXTEST(cheats_dmnt_opcodes_pointer_chain_loop_and_keypress) {
    MemArena main_mem(0x4000);
    MemArena heap_mem(0x4000);
    REQUIRE(main_mem.base != nullptr);
    REQUIRE(heap_mem.base != nullptr);

    // Point main+0x100 -> heap_mem + 0x200
    main_mem.write<uint64_t>(0x100, heap_mem.addr() + 0x200);
    // Conditional trigger value at heap+0x40
    heap_mem.write<uint32_t>(0x40, 1);

    nxrt::cheats::RuntimeConfig cfg;
    cfg.main_base = main_mem.addr();
    cfg.heap_base = heap_mem.addr();
    cfg.alias_base = heap_mem.addr();
    cfg.main_text_end = 0x50;

    // 1) Static Heap write + Conditional (== 1) + Else
    nxrt::cheats::CheatProgram static_and_cond{
        "M", "CondWrite",
        {
            0x04100000, 0x00000010, 0x12345678,             // Heap+0x10 = 0x12345678
            0x14150000, 0x00000040, 0x00000001,             // If u32 [Heap+0x40] == 1
            0x04100000, 0x00000014, 0x000000AA,             //   Heap+0x14 = 0xAA
            0x21000000,                                     // Else
            0x04100000, 0x00000014, 0x000000BB,             //   Heap+0x14 = 0xBB
            0x20000000,                                     // EndIf
        }};
    nxrt::cheats::exec_program(static_and_cond, cfg, 0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x10), 0x12345678u);
    CHECK_EQ(heap_mem.read<uint32_t>(0x14), 0xAAu);

    heap_mem.write<uint32_t>(0x40, 2);
    nxrt::cheats::exec_program(static_and_cond, cfg, 0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x14), 0xBBu);

    // 2) Pointer chain: r0 = *(u64*)(main + 0x100); r0 += 0x10; *(u32*)r0 = 99999999 (0x05F5E0FF)
    nxrt::cheats::CheatProgram ptr_chain{
        "M", "PtrChain",
        {
            0x58000000, 0x00000100,                         // r0 = *(u64*)(Main + 0x100)
            0x78000000, 0x00000010,                         // r0 += 0x10 (legacy add: T=8, R=0, C=0)
            0x64000000, 0x00000000, 0x05F5E0FF,             // *(u32*)r0 = 99999999
        }};
    nxrt::cheats::exec_program(ptr_chain, cfg, 0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x210), 0x05F5E0FFu);

    // 3) Loop + Type 4 load static address + Type 6 post-increment array fill (3 x u32 = 42)
    nxrt::cheats::CheatProgram loop_fill{
        "M", "LoopFill",
        {
            0x40020000, static_cast<uint32_t>((heap_mem.addr() + 0x300) >> 32), static_cast<uint32_t>(heap_mem.addr() + 0x300),
            0x30010000, 0x00000003,                         // Loop 3 times (loop id 1)
            0x64021000, 0x00000000, 0x0000002A,             // *(u32*)r2 = 42, r2 += 4
            0x31010000,                                     // EndLoop 1
        }};
    nxrt::cheats::exec_program(loop_fill, cfg, 0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x300), 42u);
    CHECK_EQ(heap_mem.read<uint32_t>(0x304), 42u);
    CHECK_EQ(heap_mem.read<uint32_t>(0x308), 42u);
    CHECK_EQ(heap_mem.read<uint32_t>(0x30C), 0u);

    // 4) Keypress conditional (Type 8) + Type 9 arithmetic (r1 = r2 | 0x20 = 0x25) + Type 6 reg offset (r3 + r1)
    const uint32_t kBtnL = static_cast<uint32_t>(nxrt::hid::btn(nxrt::hid::kBtnL)); // 0x40
    const uint64_t dst_base = heap_mem.addr() + 0x380 - 0x25;
    nxrt::cheats::CheatProgram keypress{
        "M", "KeyJump",
        {
            0x80000000u | kBtnL,                            // While holding L
            0x40020000, 0x00000000, 0x00000005,             // r2 = 5
            0x94612100, 0x00000020,                         // r1 = u32(r2) | 0x20 -> 0x25
            0x40030000, static_cast<uint32_t>(dst_base >> 32), static_cast<uint32_t>(dst_base),
            0x64030110, 0x00000000, 0xCAFEBABE,             // *(u32*)(r3 + r1) = 0xCAFEBABE -> heap+0x380
            0x20000000,
        }};
    nxrt::cheats::exec_program(keypress, cfg, 0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x380), 0u);
    nxrt::cheats::exec_program(keypress, cfg, kBtnL);
    CHECK_EQ(heap_mem.read<uint32_t>(0x380), 0xCAFEBABEu);

    // 5) Broken/null pointer chain must be safely skipped without faulting
    main_mem.write<uint64_t>(0x180, 0x100ull); // unmapped low address
    nxrt::cheats::CheatProgram null_ptr{
        "M", "NullSafe",
        {
            0x58000000, 0x00000180,
            0x58001000, 0x00000008,
            0x64000000, 0x00000000, 0x11111111,
        }};
    nxrt::cheats::exec_program(null_ptr, cfg, 0);
}

NXTEST(cheats_load_filters_code_patches_and_pending_rebuild) {
    TempDir t("xc2-cheats-load");
    const auto mods = t.root / "mods";
    std::filesystem::create_directories(mods / "GoodMod" / "cheats");
    std::filesystem::create_directories(mods / "HoldMod" / "cheats");

    const std::string txt =
        "[Runtime Mem]\n"
        "04100000 00000020 00000777\n\n"
        "[Static Code]\n"
        "04000000 00000010 14000001\n";

    { std::ofstream(mods / "GoodMod" / "cheats" / "8BEF68ED1EEBC24B.txt", std::ios::binary) << txt; }
    { std::ofstream(mods / "HoldMod" / "cheats" / "8BEF68ED1EEBC24B.txt", std::ios::binary) << txt; }
    {
        std::ofstream tsv(mods / ".cheats_enabled.tsv", std::ios::binary);
        tsv << "GoodMod\tRuntime Mem\n";
        tsv << "GoodMod\tStatic Code\n";
        tsv << "HoldMod\tRuntime Mem\n";
    }

    MemArena main_mem(0x2000), heap_mem(0x2000);
    nxrt::cheats::RuntimeConfig cfg{main_mem.addr(), heap_mem.addr(), heap_mem.addr(), 0x100};
    const std::string bid64 = "8bef68ed1eebc24b" + std::string(48, '0');

    nxrt::cheats::load_from_mods_dir(mods, bid64, cfg, {}, {"HoldMod"});
    CHECK_EQ(nxrt::cheats::loaded_count(), 1u); // Static Code excluded + HoldMod held off

    nxrt::cheats::tick_with_buttons(0);
    CHECK_EQ(heap_mem.read<uint32_t>(0x20), 0x777u);
    nxrt::cheats::clear();
}
