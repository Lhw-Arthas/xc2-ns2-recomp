// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#include "../src/media/nv12_convert.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/movie_vdec.h"
#include "nxrt/video_codec.h"
#include "test_framework.h"

NXTEST_GROUP(media_vcodec)

namespace nxrt::media {
bool media_film_active();
}

using namespace nxrt;

namespace {

uint64_t call_hle(const char* name, std::initializer_list<uint64_t> args) {
    const uint64_t addr = hle_address(name);
    if (!addr) return 0;
    ::Ctx c{};
    static thread_local unsigned char stack[512];
    c.x[31] = reinterpret_cast<uint64_t>(stack + 256);
    c.x[30] = 0x7100000100ull;
    int i = 0;
    for (uint64_t a : args) c.x[i++] = a;
    dispatch_call(&c, addr);
    return c.x[0];
}

void scalar_i420_to_nv12(std::vector<uint8_t>& out, uint32_t dst_w, uint32_t dst_h,
                         uint32_t src_w, uint32_t src_h,
                         const uint8_t* py, ptrdiff_t sy,
                         const uint8_t* pu, ptrdiff_t su,
                         const uint8_t* pv, ptrdiff_t sv) {
    out.assign(static_cast<size_t>(dst_w) * dst_h + static_cast<size_t>((dst_w + 1) / 2) * 2 * ((dst_h + 1) / 2), 0);
    for (uint32_t r = 0; r < src_h; ++r) {
        std::memcpy(out.data() + static_cast<size_t>(r) * dst_w, py + static_cast<ptrdiff_t>(r) * sy, src_w);
    }
    uint8_t* uv = out.data() + static_cast<size_t>(dst_w) * dst_h;
    const uint32_t cw = (src_w + 1) / 2, ch = (src_h + 1) / 2;
    const uint32_t uv_stride = ((dst_w + 1) / 2) * 2;
    for (uint32_t r = 0; r < ch; ++r) {
        const uint8_t* u = pu + static_cast<ptrdiff_t>(r) * su;
        const uint8_t* v = pv + static_cast<ptrdiff_t>(r) * sv;
        uint8_t* o = uv + static_cast<size_t>(r) * uv_stride;
        for (uint32_t x = 0; x < cw; ++x) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
}

void check_i420_case(uint32_t src_w, uint32_t src_h, ptrdiff_t sy, ptrdiff_t su, ptrdiff_t sv) {
    const uint32_t dst_w = (src_w + 1) & ~1u;
    const uint32_t dst_h = (src_h + 1) & ~1u;
    const uint32_t cw = (src_w + 1) / 2, ch = (src_h + 1) / 2;
    std::vector<uint8_t> y(static_cast<size_t>(sy) * src_h, 0xA5);
    std::vector<uint8_t> u(static_cast<size_t>(su) * ch, 0x5A);
    std::vector<uint8_t> v(static_cast<size_t>(sv) * ch, 0x3C);
    for (uint32_t r = 0; r < src_h; ++r)
        for (uint32_t c = 0; c < src_w; ++c)
            y[static_cast<size_t>(r) * sy + c] = static_cast<uint8_t>((r * 37 + c * 13) & 0xff);
    for (uint32_t r = 0; r < ch; ++r)
        for (uint32_t c = 0; c < cw; ++c) {
            u[static_cast<size_t>(r) * su + c] = static_cast<uint8_t>((r * 19 + c * 7 + 11) & 0xff);
            v[static_cast<size_t>(r) * sv + c] = static_cast<uint8_t>((r * 29 + c * 5 + 101) & 0xff);
        }
    std::vector<uint8_t> ref;
    scalar_i420_to_nv12(ref, dst_w, dst_h, src_w, src_h, y.data(), sy, u.data(), su, v.data(), sv);
    std::vector<uint8_t> got(ref.size(), 0xCC);
    media::i420_to_nv12(got.data(), dst_w, dst_h, src_w, src_h, y.data(), sy, u.data(), su, v.data(), sv);
    CHECK(got == ref);
}

}  // namespace

NXTEST(i420_to_nv12_matches_scalar_even_and_odd) {
    // Unaligned width with padded strides and odd chroma tail
    check_i420_case(35, 23, 64, 40, 48);
    // Exact SIMD multiple + tail
    check_i420_case(66, 18, 66, 33, 33);
    // Tight 4K horizontal slice (fast path for y memcpy + SSE2 UV interleave)
    check_i420_case(3840, 8, 3840, 1920, 1920);
}

NXTEST(finalize_mid_film_joins_worker_and_ends_audio_decoder) {
    const game::Info* saved_info = &game::info();
    game::Info sw2 = *saved_info;
    sw2.platform = game::Platform::Switch2;
    sw2.movie_video_decoder = true;
    game::set_current(&sw2);

    CHECK(media::install_video_codec_hle());
    CHECK(media::install_audio_decoder_hle());

    const auto before = media::video_codec_stats();

    // Set up an AudioDecoder with registered frames and mark it gated/held behind video PTS
    alignas(16) uint8_t adec_obj[64]{};
    alignas(16) uint8_t af0[0x40]{};
    alignas(16) uint8_t abuf[0x40]{};
    std::vector<uint8_t> pcm_out(4096, 0);
    // AudioDecoder layout: AudioFrame +0x00 = Buffer*; Buffer +0x00 = data, +0x08 = cap, +0x10 = filled, +0x18 = time
    uint64_t p_buf = reinterpret_cast<uint64_t>(abuf);
    uint64_t p_data = reinterpret_cast<uint64_t>(pcm_out.data());
    uint64_t cap = pcm_out.size();
    std::memcpy(af0 + 0x00, &p_buf, 8);
    std::memcpy(abuf + 0x00, &p_data, 8);
    std::memcpy(abuf + 0x08, &cap, 8);
    uint64_t frame_list[1] = {reinterpret_cast<uint64_t>(af0)};

    call_hle("_ZN5movie12AudioDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24AudioDecoderOutputFormatENS_14AudioCodecModeE",
             {reinterpret_cast<uint64_t>(adec_obj), 0, 0, 66, 2, 1});
    call_hle("_ZN5movie12AudioDecoder18RegisterAudioFrameEPPNS_10AudioFrameEi",
             {reinterpret_cast<uint64_t>(adec_obj), reinterpret_cast<uint64_t>(frame_list), 1});

    // Initialize an AV1 decoder, submit non-EOS packets while started, then finalize immediately with pending > 0
    alignas(16) uint8_t vdec_obj[64]{};
    call_hle("_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
             {reinterpret_cast<uint64_t>(vdec_obj), 0, 0, 0, 1280, 720, 1, 0, 3, 1});
    call_hle("_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", {reinterpret_cast<uint64_t>(vdec_obj)});

    const std::vector<uint8_t> pkt(256, 0);
    for (int i = 0; i < 4; ++i) {
        call_hle("_ZN2nn5codec27SubmitAv1DecoderInputBufferEPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb",
                 {reinterpret_cast<uint64_t>(vdec_obj), reinterpret_cast<uint64_t>(pkt.data()), pkt.size(),
                  static_cast<uint64_t>(i) * 33'000'000ull, 0});
    }

    // Finalizing while worker is busy / packets remain pending must join cleanly and mark audio EOS
    call_hle("_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", {reinterpret_cast<uint64_t>(vdec_obj)});

    const auto after = media::video_codec_stats();
    CHECK_EQ(after.decoders_created, before.decoders_created + 1);
    CHECK_EQ(after.decoders_destroyed, before.decoders_destroyed + 1);
    CHECK(!media::media_film_active());

    // AudioDecoder must now deliver an EOS-flagged frame (byte +0x10 == 1) without hanging behind the gate
    uint64_t acquired = 0;
    int32_t rc_flag = -1;
    const uint64_t r = call_hle("_ZN5movie12AudioDecoder17AcquireAudioFrameEPPNS_10AudioFrameEPi",
                                {reinterpret_cast<uint64_t>(adec_obj), reinterpret_cast<uint64_t>(&acquired), reinterpret_cast<uint64_t>(&rc_flag)});
    CHECK_EQ(r, 0ull);
    CHECK_EQ(acquired, reinterpret_cast<uint64_t>(af0));
    CHECK_EQ(af0[0x10], 1);

    call_hle("_ZN5movie12AudioDecoder8FinalizeEv", {reinterpret_cast<uint64_t>(adec_obj)});
    game::set_current(saved_info);
}

NXTEST(av1_delayed_frames_drain_on_eos) {
    const char* path = std::getenv("NXRT_TEST_AV1_PKTS");
    if (!path || !*path) {
        nxtest::skip("NXRT_TEST_AV1_PKTS is not set");
        return;
    }
    FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    if (!f) return;
    uint32_t count = 0;
    std::fread(&count, 4, 1, f);
    std::vector<std::vector<uint8_t>> pkts(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t sz = 0;
        std::fread(&sz, 4, 1, f);
        pkts[i].resize(sz);
        std::fread(pkts[i].data(), 1, sz, f);
    }
    std::fclose(f);

    const game::Info* saved_info = &game::info();
    game::Info sw2 = *saved_info;
    sw2.platform = game::Platform::Switch2;
    sw2.movie_video_decoder = true;
    game::set_current(&sw2);
    CHECK(media::install_video_codec_hle());

    const auto before = media::video_codec_stats();

    alignas(16) uint8_t pref[64]{};
    {
        ::Ctx c{};
        static thread_local unsigned char stack[512];
        c.x[31] = reinterpret_cast<uint64_t>(stack + 256);
        c.x[30] = 0x7100000100ull;
        c.x[8] = reinterpret_cast<uint64_t>(pref);
        c.x[0] = 3840;
        c.x[1] = 2160;
        c.x[2] = 3;
        c.x[3] = 0;
        dispatch_call(&c, hle_address("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE"));
    }

    size_t pool_sz = 0, pool_align = 0;
    call_hle("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj",
             {reinterpret_cast<uint64_t>(&pool_sz), reinterpret_cast<uint64_t>(&pool_align), reinterpret_cast<uint64_t>(pref), 4});
    std::vector<uint8_t> pool_mem(pool_sz + 4096);
    alignas(16) uint8_t pool_obj[64]{};
    call_hle("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj",
             {reinterpret_cast<uint64_t>(pool_obj), reinterpret_cast<uint64_t>(pool_mem.data()), pool_sz, reinterpret_cast<uint64_t>(pref), 4});

    alignas(16) uint8_t vdec_obj[64]{};
    call_hle("_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
             {reinterpret_cast<uint64_t>(vdec_obj), 0, 0, 0, 3840, 2160, 1, 0, 3, 1});
    for (uint32_t i = 0; i < 4; ++i) {
        call_hle("_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
                 {reinterpret_cast<uint64_t>(vdec_obj), i, reinterpret_cast<uint64_t>(pool_obj), i});
    }
    call_hle("_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", {reinterpret_cast<uint64_t>(vdec_obj)});

    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        call_hle("_ZN2nn5codec27SubmitAv1DecoderInputBufferEPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb",
                 {reinterpret_cast<uint64_t>(vdec_obj), reinterpret_cast<uint64_t>(pkts[i].data()), pkts[i].size(),
                  static_cast<uint64_t>(i) * 33'333'333ull, 0});
    }
    call_hle("_ZN2nn5codec27SubmitAv1DecoderInputBufferEPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb",
             {reinterpret_cast<uint64_t>(vdec_obj), 0, 0, 0, 1});

    uint32_t pictures = 0;
    bool got_eos = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!got_eos && std::chrono::steady_clock::now() < deadline) {
        uint32_t idx = 0;
        uint64_t fence[2]{};
        const uint64_t rc = call_hle("_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE",
                                     {reinterpret_cast<uint64_t>(&idx), reinterpret_cast<uint64_t>(fence), reinterpret_cast<uint64_t>(vdec_obj)});
        if (rc != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        while (!call_hle("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", {reinterpret_cast<uint64_t>(fence)})) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        uint16_t pw = 0, ph = 0, dummy0 = 0, dummy1 = 0;
        int64_t pts = 0;
        uint8_t eos = 0;
        call_hle("_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj",
                 {reinterpret_cast<uint64_t>(&pw), reinterpret_cast<uint64_t>(&ph), reinterpret_cast<uint64_t>(&dummy0),
                  reinterpret_cast<uint64_t>(&dummy1), reinterpret_cast<uint64_t>(&pts), reinterpret_cast<uint64_t>(&eos),
                  reinterpret_cast<uint64_t>(vdec_obj), idx});
        if (eos) {
            got_eos = true;
            break;
        }
        if (pw == 3840 && ph == 2160) ++pictures;
        call_hle("_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE",
                 {reinterpret_cast<uint64_t>(vdec_obj), idx, 0, 0});
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  real 4K AV1 via video_codec: %u/%u pictures in %.1f ms (%.1f fps), eos=%d\n",
                pictures, count, ms, pictures * 1000.0 / ms, got_eos ? 1 : 0);

    call_hle("_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", {reinterpret_cast<uint64_t>(vdec_obj)});
    call_hle("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", {reinterpret_cast<uint64_t>(pool_obj)});

    const auto after = media::video_codec_stats();
    CHECK(got_eos);
    CHECK_EQ(pictures, count);
    CHECK_EQ(after.decode_errors, before.decode_errors);
    game::set_current(saved_info);
}
