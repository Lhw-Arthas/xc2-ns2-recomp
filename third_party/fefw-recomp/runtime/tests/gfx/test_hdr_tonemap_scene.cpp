// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::extras::spirv_hdr_tonemap_scene (hdr_tonemap_patch.h) against Xenoblade Chronicles 2's shader cache. The shaders are game-derived data:
// they are read at run time from the folder in NXTEST_XC2_SPIRV (a shader_cache/spirv folder) and never copied into the repository. The cases
// skip when it is not set.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "test_framework.h"

#include "extras/hdr_tonemap_patch.h"

NXTEST_GROUP(hdr_tonemap_scene)

namespace {

namespace fs = std::filesystem;
namespace hdr = nxrt::extras;

const char* kSpirvVal = "C:\\VulkanSDK\\1.4.363.0\\Bin\\spirv-val.exe";
const char* kToneMap = "9b73d3e9a467d01a.spv";   // the tone-map pass (LUT grade, clamp at 1)
const char* kToneMapNoDepth = "bf9de56bc2edcf37.spv";   // the same without the depth write
const char* kFieldToneMap = "8ebbfb88f749b1c8.spv";     // the field's: two volumes blended by depth
const char* kFieldToneMap2 = "fc98f9a31ceeadc3.spv";    // its other variant
const char* const kAll[] = {kToneMap, kToneMapNoDepth, kFieldToneMap, kFieldToneMap2};

fs::path spv_dir() {
    const char* d = std::getenv("NXTEST_XC2_SPIRV");
    return d && *d ? fs::path(d) : fs::path();
}

bool load_words(const fs::path& p, std::vector<uint32_t>& w) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0 || n % 4 != 0) return false;
    w.resize(static_cast<size_t>(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(w.data()), n);
    return static_cast<bool>(f);
}

bool write_words(const fs::path& p, const std::vector<uint32_t>& w) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * 4));
    return static_cast<bool>(f);
}

// count of an opcode (ext: GLSL instruction number) in the body
int count_op(const std::vector<uint32_t>& w, uint32_t op, int ext = -1) {
    int n = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16;
        if (len == 0) return -1;
        if ((w[i] & 0xffffu) == op && (ext < 0 || (len > 4 && w[i + 4] == static_cast<uint32_t>(ext)))) ++n;
        i += len;
    }
    return n;
}

bool tone_map(std::vector<uint32_t>& in, const char* name = kToneMap) {
    const fs::path d = spv_dir();
    std::error_code ec;
    if (d.empty() || !fs::is_regular_file(d / name, ec)) return false;
    return load_words(d / name, in);
}

}  // namespace

NXTEST(patches_both_tone_maps_into_valid_modules) {
    for (const char* name : kAll) {
        std::vector<uint32_t> in, out;
        if (!tone_map(in, name)) {
            nxtest::skip("NXTEST_XC2_SPIRV not set or the tone-map shaders are not in it");
            return;
        }
        CHECK(hdr::spirv_hdr_tonemap_scene(in, out));
        CHECK(out.size() > in.size() && out.size() < in.size() + 400);
        for (size_t i = 0; i < 5 && out.size() >= 5; ++i)
            if (i != 3) CHECK_EQ(out[i], in[i]);
        CHECK(out[3] > in[3]);
        // the inserted code: three stores of the clamp inputs and three of the outputs, three more volume samples (the game's curve)
        printf("hdr_tonemap_scene: %s patched (%zu -> %zu words)\n", name, in.size(), out.size());
        CHECK_EQ(count_op(out, 62), count_op(in, 62) + 6);
        CHECK_EQ(count_op(out, 87), count_op(in, 87) + 3);
        // the other tone-map patches leave it alone
        std::vector<uint32_t> other;
        CHECK(!hdr::spirv_hdr_tonemap(in, other));
        CHECK(!hdr::spirv_hdr_tonemap_lut(in, 2.0f, other));

        std::error_code ec;
        if (!fs::is_regular_file(kSpirvVal, ec)) {
            nxtest::skip("spirv-val not present");
            return;
        }
        const char* temp = std::getenv("TEMP");
        const fs::path tmp = fs::path(temp ? temp : ".") / "nxrt_hdr_scene.spv";
        CHECK(write_words(tmp, out));
        const std::string cmd = std::string("\"\"") + kSpirvVal + "\" --target-env vulkan1.3 \"" + tmp.string() + "\"\"";
        CHECK_EQ(std::system(cmd.c_str()), 0);
        fs::remove(tmp, ec);
    }
}

NXTEST(only_the_tone_map_matches_in_the_cache) {
    const fs::path d = spv_dir();
    std::error_code ec;
    if (d.empty() || !fs::is_directory(d, ec)) {
        nxtest::skip("NXTEST_XC2_SPIRV not set");
        return;
    }
    int tried = 0;
    std::vector<std::string> hits;
    for (const auto& ent : fs::directory_iterator(d, ec)) {
        if (!ent.is_regular_file(ec) || ent.path().extension() != ".spv") continue;
        std::vector<uint32_t> w, out;
        if (!load_words(ent.path(), w)) continue;
        ++tried;
        if (hdr::spirv_hdr_tonemap_scene(w, out)) hits.push_back(ent.path().filename().string());
    }
    printf("hdr_tonemap_scene: %zu of %d cache shaders matched\n", hits.size(), tried);
    for (const std::string& h : hits) printf("  %s\n", h.c_str());
    CHECK(tried > 1000);
    CHECK_EQ(hits.size(), static_cast<size_t>(4));
    for (const std::string& h : hits) CHECK(h == kToneMap || h == kToneMapNoDepth || h == kFieldToneMap || h == kFieldToneMap2);
}

NXTEST(rejects_broken_input) {
    std::vector<uint32_t> in, out;
    CHECK(!hdr::spirv_hdr_tonemap_scene(in, out));
    CHECK(out.empty());
    if (!tone_map(in)) return;
    std::vector<uint32_t> bad = in;
    bad[0] ^= 1u;
    CHECK(!hdr::spirv_hdr_tonemap_scene(bad, out));
    bad = in;
    bad.resize(40);
    CHECK(!hdr::spirv_hdr_tonemap_scene(bad, out));
    CHECK(out.empty());
}
