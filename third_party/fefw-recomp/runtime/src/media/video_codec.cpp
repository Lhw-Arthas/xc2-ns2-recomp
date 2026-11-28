// SPDX-License-Identifier: GPL-3.0-or-later
// nn::codec hardware video decoders (VP9, VP8) + their fences on the host, decoding with libvpx. See include/nxrt/video_codec.h.
//
// Calling conventions, read off Pokopia's subsdk0 (movie::detail::*, which forwards its registers unchanged to nn::codec) and off the
// mangled names (Mario Kart World's variants differ where noted):
//   MakeDecodeImagePreference(u16 w, u16 h, OutputFormat, CodecMode) -> 64-byte VideoDecoderImagePreference returned through x8
//   CalculateDecoderImagePoolBuffer(size_t* size, size_t* align, const Preference*, u32 count)
//   InitializeDecoderImagePool(Pool*, void* mem, size_t, const Preference*, u32 count, CodecMode)    Pool = 32 bytes
//   ReadDecoderOutputImageNv12(const Pool*, u32 image, u16 x, u16 y, u16 w, u16 h, void* y, size_t y_stride, [sp] size_t y_size,
//                              [sp+8] void* uv, [sp+16] size_t uv_stride, [sp+24] size_t uv_size)
//   ReadDecoderOutputImageRgba(const Pool*, u32 image, u16 x, u16 y, u16 w, u16 h, void* dst, size_t stride, [sp] size_t size)
//   CalculateVp9DecoderWorkBuffer(size_t* size, size_t* align, size_t, u16 w, u16 h)
//   InitializeVp9Decoder(Vp9DecoderType*, void* work, size_t, size_t, u16, u16, u32, InputFormat, [sp] OutputFormat)   (transfer memory)
//   InitializeVp9Decoder(Vp9DecoderType*, size_t, u16, u16, u32, InputFormat, OutputFormat)   (remote memory; MKW: an extra size_t)
//   RegisterVp9DecoderBuffer(Vp9DecoderType*, u32 index, Pool*, u32)
//   GetVp9DecoderSubmissionReadyEvent(os::SystemEventType** out, Vp9DecoderType*)
//   SubmitVp9DecoderInputBuffer(u64* out, Vp9DecoderType*, const void* data, size_t size, TimeSpan pts, bool end_of_stream)
//   AcquireVp9DecoderOutputBuffer(u32* index, gs::NativeSyncHandle* fence, Vp9DecoderType*)
//   ReleaseVp9DecoderOutputBuffer(Vp9DecoderType*, u32 index, gs::NativeSyncHandle pre_fence (x2, x3))
//   GetVp9DecoderOutputBufferProperties(u16* w, u16* h, u16*, u16*, TimeSpan* pts, bool* eos, Vp9DecoderType*, u32 index)
//     (MKW: a movie::ColorSpace* after the four u16*, so the decoder is x7 and the index on the stack)
//   nn::syncpt::sdk::{Create,Destroy,Cancel}SyncPointEvent(SyncPointEventType*), RequestSyncPointEvent(evt, const SyncPointHandle&),
//     InitializeMultiWaitHolder(os::MultiWaitHolderType*, evt), TryWaitSyncPointEvent(const evt*); nn::gs::TryWaitNativeSync(const handle&)
// Every nn::codec call the player makes is checked (a non-zero nn::Result aborts), so these answer 0 unless noted.
//
// Guest objects keep {host pointer, marker} in their first 16 bytes. A SyncPointEventType (128 bytes in the player's event manager) holds a
// guest os::EventType at +0 (40 bytes) that the guest's own MultiWait waits on; a fence handle is {kSyncMarker | decoder id, sequence}.
// Fences of a decoder complete in sequence order, so "done" is sequence <= the decoder's last completed sequence; any handle that is not
// one of ours counts as reached (nxrt has no GPU syncpoints: NVN runs on the host).
#include "nxrt/diag.h"
#include <windows.h>
#include <fstream>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/video_codec.h"
#include "nv12_convert.h"
#include "video_nvdec.h"
#include "video_ffmpeg.h"

// The NVN side of the same build (nvn_texture_registry.cpp): a picture this decoder writes into the game's memory has to be
// announced there, or the texture the game samples over that memory never uploads again (see read_nv12).
namespace nxrt::nvn::be {
void textures_cpu_wrote(const void* lo, uint64_t size);
void film_texture_begin_film();
}

namespace nxrt::media {
void player_trace_new_film();   // movie_trace.cpp: reset this film's stall-dump budget and clock
void player_trace_eos();        // movie_trace.cpp: the marker is out; re-arm the dump for what happens after it
void movie_adec_film_end();     // movie_adec.cpp: this placeholder track ends with the picture, not on its own
void movie_adec_video_advanced(); // movie_adec.cpp: wake any decoder waiting on the video-PTS gate
}

#if NXRT_HAVE_LIBVPX
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif

#if NXRT_HAVE_DAV1D
#include <dav1d/dav1d.h>
#endif

namespace nxrt::media {

#if NXRT_HAVE_LIBVPX || NXRT_HAVE_DAV1D
namespace {

#define A(i) (c->x[i])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr uint64_t kDecMarker = 0x4e58524356444543ull;    // "NXRCVDEC"
constexpr uint64_t kPoolMarker = 0x4e585243504f4f4cull;   // "NXRCPOOL"
constexpr uint64_t kPrefMarker = 0x4e58524350524546ull;   // "NXRCPREF"
constexpr uint64_t kEvtMarker = 0x4e58524353594e43ull;    // "NXRCSYNC" at +0x60 of a SyncPointEventType
// nn::codec results (module 111): 3604 is "no buffer to acquire / no picture in the buffer yet", 3603 is "cannot take the
// packet now". The SDK's own library returns them and the players handle them, so we must too.
constexpr uint32_t kResult = 0x6f | (3604u << 9);
constexpr uint32_t kResultNoBuffer = kResult;
constexpr uint32_t kResultBusy = 0x6f | (3603u << 9);
constexpr uint64_t kSyncMarker = 0x4e58435600000000ull;   // "NXCV" in the high half of a fence handle's first word
constexpr uint32_t kManualClear = 0;                      // nn::os::EventClearMode_ManualClear
constexpr size_t kAhead = 4;        // pictures the worker may keep decoded ahead of the player (4 x 12 MB)
constexpr size_t kAheadStartup = 24;   // ... until the client has read its first picture. The film starts while the game loads and
                                       // the player's thread is starved for seconds; when it comes back it must be able to read a
                                       // burst, not one picture per 40 ms decode - it can never catch up that way and skips
                                       // forward instead (2026-11-21). 24 x 12 MB of NV12, only for that first window.
constexpr size_t kPendingMax = 8192;   // packets queued undecoded (~500 MB of AV1 for a film, well past the 2810 of this one). Submits
                                      // only copy; the worker decodes ahead. The queue was 8 for a while and every submit then decoded
                                      // one packet inline (23 ms) on the player's thread, which starved it (frames 60-90 ms late).
constexpr size_t kRefuseAt = 8;  // undecoded packets at which the submission-ready event goes DOWN (a watermark, not a refusal:
                                 // the reference emulator keeps every submitted packet and uses 8 only for this, 2026-11-21)
constexpr size_t kQueueCap = 8192;   // memory bound on queued packets only; a 90 s 4K film is about 9 MB
constexpr size_t kBacklog = 1;   // decoded pictures waiting for a buffer before input stops being "ready". 3 let the movie library's
                                 // adapter submit ~10 packets per frame, and every Submit decodes 4K AV1 inline on the player's own thread:
                                 // the frame loop fell ~180 ms behind and dropped every frame but the first (GetBuffer once, 2026-11-18).

bool g_log = false;
int g_threads = 4;
thread_local bool t_on_worker = false;   // the decode worker: the only thread allowed to take a hardware decoder
bool g_hwdec = true;   // [media] hwdec: NVDEC for VP8/VP9/AV1 when the GPU is an NVIDIA one (video_nvdec.cpp)
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> s_created{0}, s_destroyed{0}, s_packets{0}, s_decoded{0}, s_delivered{0}, s_read{0}, s_errors{0};
std::mutex g_err_mu;
std::string g_first_error;   // the first decode error, kept for the stats line

// Records an error: the first one is logged at Error level and kept, later ones only counted (the stats line shows the total).
void note_error(const char* fmt, ...) {
    s_errors++;
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_err_mu);
    if (!g_first_error.empty()) return;
    g_first_error = buf;
    NXLOG_E("vcodec", "first error: %s", buf);
}

// Decode stats, at most every 5 s and only when something moved: packets in, pictures out of libvpx, pictures placed in the player's
// output buffers (fences signalled), pictures the game read back (ReadDecoderOutputImage*), errors.
void stats_tick() {
    static std::atomic<int64_t> last_ns{0};
    static std::atomic<uint64_t> last_sum{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = last_ns.load(std::memory_order_relaxed);
    if (prev && now - prev < 5'000'000'000ll) return;
    const uint64_t sum = s_packets + s_decoded + s_delivered + s_read + s_errors;
    if (sum == last_sum.load(std::memory_order_relaxed)) return;
    if (!last_ns.compare_exchange_strong(prev, now)) return;
    last_sum = sum;
    std::string first;
    {
        std::lock_guard<std::mutex> lk(g_err_mu);
        first = g_first_error;
    }
    NXLOG_I("vcodec", "stats: decoders %llu/%llu live, packets %llu, decoded %llu, placed in output buffers %llu, read by the game %llu, errors %llu%s%s",
            (unsigned long long)(s_created - s_destroyed), (unsigned long long)s_created.load(), (unsigned long long)s_packets.load(),
            (unsigned long long)s_decoded.load(), (unsigned long long)s_delivered.load(), (unsigned long long)s_read.load(),
            (unsigned long long)s_errors.load(), first.empty() ? "" : " (first: ", first.empty() ? "" : (first + ")").c_str());
}

inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
inline void wr32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
inline void wr16(uint64_t a, uint16_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 2); }

// ---- guest calls ------------------------------------------------------------------------------------------------------------------
// Calls a guest function from inside a handler (on the guest thread that called the import); returns x0.
uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!fn) return 0;
    uint64_t save[9];
    std::memcpy(save, c->x, sizeof save);
    const uint64_t lr = c->x[30], f = c->fn;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    std::memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->fn = f;
    return r;
}
struct OsFns {
    uint64_t init_event, signal_event, clear_event, finalize_event, trywait_event, init_holder_event;
    uint64_t create_sysevent, signal_sysevent, destroy_sysevent;
};
const OsFns& os() {
    static const OsFns f = [] {
        OsFns o{};
        o.init_event = module_find_symbol("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
        o.signal_event = module_find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        o.clear_event = module_find_symbol("_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
        o.finalize_event = module_find_symbol("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE");
        o.trywait_event = module_find_symbol("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE");
        o.init_holder_event = module_find_symbol("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE");
        o.create_sysevent = module_find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        o.signal_sysevent = module_find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
        o.destroy_sysevent = module_find_symbol("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        if (!o.init_event || !o.signal_event || !o.clear_event || !o.init_holder_event || !o.create_sysevent || !o.signal_sysevent)
            NXLOG_E("vcodec", "guest nn::os event functions missing: fences and input events will not work");
        return o;
    }();
    return f;
}

// Bytes one NV12 image of w x h needs (w and h even).
uint64_t nv12_storage(uint32_t w, uint32_t h) {
    return static_cast<uint64_t>(w) * h + static_cast<uint64_t>(w) * h / 2;
}

// NXRT_FILM_TRACE=1: the first 200 nn::codec calls in the order the client makes them, with the arguments that matter. The SDK
// library's own protocol (dm/host/hle_codec_video.c in the NX profile) is the reference for SDK 20/22; this game's movie module
// is SDK 3.0.1 and its client may ask differently, so the sequence is what we have to match (2026-11-21).
bool film_trace_on() {
    static const bool on = std::getenv("NXRT_FILM_TRACE") != nullptr;
    return on;
}
std::atomic<int> g_signals_owed{0};   // signals the decoder workers owe; svc.cpp drains them on any guest call
// True between CreateDecoder and Finalize: lets the NVN side log the textures that appear while a film is running, with their
// storage addresses, so the frame buffer the client reads into can be matched against them (2026-11-21).
std::atomic<bool> g_film_active{false};

// The guest buffers the client has read a picture into. The movie library puts them into a texture with a GPU copy, not a CPU
// upload (no 4K texture is ever uploaded during a film), so the copy is where the frame either reaches the screen or does not.
struct FilmRange {
    uint64_t addr = 0;
    uint64_t size = 0;
};
std::mutex g_film_range_mu;
FilmRange g_film_ranges[16];
int g_film_range_n = 0;

void film_range_note(uint64_t addr, uint64_t size) {
    if (!addr || size < 4096) return;
    std::lock_guard<std::mutex> lk(g_film_range_mu);
    for (int i = 0; i < g_film_range_n; ++i)
        if (g_film_ranges[i].addr == addr) return;
    if (g_film_range_n < 16) g_film_ranges[g_film_range_n++] = FilmRange{addr, size};
}

// True when [addr, addr+size) overlaps a buffer a film frame was read into.

void film_state_tick();   // defined after the decoder registry
void film_trace(const char* what, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0) {
    if (!film_trace_on()) return;
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 1200) NXLOG_I("vcodec", "call %u: %s(%llu, %llu, %llu)", k, what, a, b, c);
}

// ---- image pools --------------------------------------------------------------------------------------------------------------------
struct Pool {
    uint16_t w = 0, h = 0;      // the image size the pool was made for (even)
    uint32_t format = 0;        // nn::codec VideoDecoderOutputFormat (2 RGBA, 3/6 NV12 as the player reads it)
    // CalculateDecoderImagePoolBuffer's count is how many images the pool holds, and the player acquires more than one at a
    // time (16 acquisitions in 0.5 ms, all index 0, free 0, on 2026-11-21). One vector per image index.
    std::vector<std::vector<uint8_t>> imgs;
    std::vector<uint8_t>& img(uint32_t i) {
        if (imgs.size() <= i) imgs.resize(i + 1);
        return imgs[i];
    }
    uint32_t count() const { return static_cast<uint32_t>(imgs.size()); }
};
Pool* pool_of(uint64_t g) { return g && rd64(g + 8) == kPoolMarker ? reinterpret_cast<Pool*>(rd64(g)) : nullptr; }

// ---- decoders -----------------------------------------------------------------------------------------------------------------------
struct Picture {
    std::vector<uint8_t> nv12;
    uint16_t w = 0, h = 0;
    int64_t pts = 0;
    bool eos = false;
};
struct BufInfo {
    uint16_t w = 0, h = 0;
    int64_t pts = 0;
    bool eos = false;
};
// A submitted packet waiting for its turn to be decoded. The movie library's adapter thread submits four packets back to back
// (its own input buffers) and then services the player; with the decode inside Submit* that thread spent all its time decoding
// 4K AV1 (~26 ms a packet), the player's frames arrived ~85 ms late and its pacing dropped every frame but the first
// (GetBuffer once, 2026-11-18). Submits only queue now; one packet is decoded when the player needs a picture.
struct Pending {
    std::vector<uint8_t> data;
    int64_t pts = 0;
    bool eos = false;
};
struct Decoder {
    ~Decoder();
    // NVDEC: the GPU's own decoder when the GPU is NVIDIA (video_nvdec.cpp). Null while the CPU path decodes or when the
    // hardware path is off or unavailable; every failure falls back to libvpx/dav1d for the rest of the stream.
    NvDec* nv = nullptr;
    bool nv_tried = false;
    FfDec* ff = nullptr;      // FFmpeg's libavcodec + D3D11VA: the path the emulators use
    bool ff_tried = false;
    uint32_t id = 0;
    int codec = 9;   // 8 VP8, 9 VP9, 10 AV1 (dav1d; the SDK 3.0.1 movie library decodes AV1)
    std::mutex mu;
#if NXRT_HAVE_LIBVPX
    vpx_codec_ctx_t vpx{};
#endif
    bool vpx_ok = false;
#if NXRT_HAVE_DAV1D
    Dav1dContext* av1 = nullptr;
#endif
    // The output-buffer protocol, as the SDK's own library implements it (dm/host/hle_codec_video.c in the NX profile):
    //   register / release put the index in both queues below; Acquire pops the acquirable queue and answers Result 3604
    //   when it is empty; the decoder writes held pictures into the buffers waiting in "pend", marks the buffer filled and
    //   signals that buffer's generation; properties answer 3604 until the buffer is filled. The client polls the sync handle
    //   and then reads, which is why nothing here may block or complete a fence early (2026-11-21).
    struct Slot {
        bool reg = false;
        uint64_t gen = 1;          // bumped on every release
        bool filled = false;
        uint64_t filled_gen = 0;   // the generation that was filled
        uint16_t w = 0, h = 0;
        int64_t ts = 0;
        bool eos = false;
    };
    std::array<Slot, 32> slot;
    std::map<uint32_t, uint64_t> pools;                   // buffer index -> guest pool
    std::deque<uint32_t> fifo;                            // acquirable (register / release), at most 8
    std::deque<uint32_t> pend;                            // released, waiting for a picture
    std::deque<Picture> ready;                            // decoded pictures with no buffer to go into yet
    std::deque<Pending> pending;                          // submitted packets not decoded yet (input order)
    // What the first frame of a film costs, in the log: the moment the first packet arrived and the moment a picture
    // came out. The gap is what used to show up as a 213 ms frame at the start of a cutscene (0.1.4, RTX 5080).
    std::chrono::steady_clock::time_point t_first_packet{};
    long long first_picture_ms = -1;
    std::mutex dec_mu;                                    // the codec context and last picture: the worker decodes, the guest may fall back
    std::thread worker;                                   // decodes pending packets ahead of the player, off its thread
    std::atomic<uint32_t> completed_early{0};             // acquires that completed their fence with no picture to pair (a film's first acquire)
    std::condition_variable cv;
    bool stop = false;
    bool worker_started = false;
    uint64_t next_seq = 1;
    std::atomic<uint64_t> done_seq{0};
    alignas(16) uint8_t submit_event[0x100]{};            // the guest os::SystemEventType handed out as "submission ready"
    bool submit_event_made = false;
    bool want_submit_signal = false;
    bool started = false;                                 // between Start*Decoder and Stop*Decoder: only then is input "ready"
    bool eos_pending = false;                             // end of stream seen, but the pictures before it have not all gone
    bool eos_queued = false;                              // the end-of-stream marker is already queued: the client resubmits it endlessly
    bool film_end_signaled = false;                       // end-of-stream cleanup already ran (EOS packet or finalize)

    uint64_t pictures = 0;
};

void worker_loop(Decoder& d);   // decodes pending packets ahead of the player (defined with the codec handlers below)

std::mutex g_reg_mu;
std::map<uint32_t, std::shared_ptr<Decoder>> g_decoders;
uint32_t g_next_id = 1;

std::shared_ptr<Decoder> dec_of(uint64_t g) {
    if (!g || rd64(g + 8) != kDecMarker) return nullptr;
    const uint32_t id = static_cast<uint32_t>(rd64(g));
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_decoders.find(id);
    return it == g_decoders.end() ? nullptr : it->second;
}
// A decoder fence handle is {kSyncMarker | decoder id, (generation << 32) | buffer index}, as the SDK's own library makes
// them: it is reached once that buffer holds a picture written at or after that generation (dm/host/hle_codec_video.c).
// Once a second, whenever a decoder call happens: the queues and the counters, so a stall shows which one stopped moving
// (2026-11-21).
void film_state_tick() {
    std::vector<std::shared_ptr<Decoder>> decs;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        for (auto& e : g_decoders) decs.push_back(e.second);
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto& d : decs) {
        static std::atomic<int64_t> last{0};
        if (now - last.load(std::memory_order_relaxed) < 1000) return;
        last.store(now, std::memory_order_relaxed);
        size_t pend = 0, fifo = 0, ready = 0, pkt = 0, filled = 0;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            pend = d->pend.size();
            fifo = d->fifo.size();
            ready = d->ready.size();
            pkt = d->pending.size();
            for (auto& s : d->slot) if (s.filled) ++filled;
        }
        NXLOG_I("vcodec", "state: packets %zu pending %zu held %zu pend %zu fifo %zu filled %zu | placed %llu read %llu owed %d",
                pkt, pkt, ready, pend, fifo, filled, (unsigned long long)s_delivered.load(),
                (unsigned long long)s_read.load(), g_signals_owed.load());
    }
}

bool fence_done(uint64_t w0, uint64_t seq) {
    {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 16)
            NXLOG_I("vcodec", "sync wait on %llx/%llx (call %u)", (unsigned long long)w0, (unsigned long long)seq, n.load());
    }
    if ((w0 & 0xffffffff00000000ull) != kSyncMarker) return true;   // not a decoder fence: reached
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_decoders.find(static_cast<uint32_t>(w0));
    if (it == g_decoders.end()) return true;
    const uint32_t index = static_cast<uint32_t>(seq & 0xffffffffull);
    const uint64_t gen = seq >> 32;
    if (index >= it->second->slot.size()) return true;
    std::lock_guard<std::mutex> lk2(it->second->mu);
    const Decoder::Slot& s = it->second->slot[index];
    return s.filled && s.filled_gen >= gen;
}

#if NXRT_HAVE_LIBVPX
bool vpx_open(Decoder& d) {
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = static_cast<unsigned>(g_threads);
    const vpx_codec_err_t e = vpx_codec_dec_init(&d.vpx, d.codec == 8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0);
    d.vpx_ok = e == VPX_CODEC_OK;
    if (!d.vpx_ok) note_error("decoder %u: vpx_codec_dec_init failed: %s", d.id, vpx_codec_err_to_string(e));
    return d.vpx_ok;
}
void vpx_close(Decoder& d) {
    if (d.vpx_ok) vpx_codec_destroy(&d.vpx);
    d.vpx_ok = false;
}
#else
bool vpx_open(Decoder& d) {
    note_error("decoder %u: VP%d requested but runtime was built without libvpx", d.id, d.codec);
    d.vpx_ok = false;
    return false;
}
void vpx_close(Decoder& d) { d.vpx_ok = false; }
#endif
#if NXRT_HAVE_DAV1D
void av1_close(Decoder& d) {
    if (d.av1) dav1d_close(&d.av1);
    d.av1 = nullptr;
}
// Opens dav1d for the codec 10 decoders. max_frame_delay 0 lets dav1d choose frame-level parallelism (ceil(sqrt(n_threads)))
// alongside tile threads; delayed frames drain on flush/EOS and acquire_output primes startup when queued packets exist.
bool av1_open(Decoder& d) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = g_threads;
    s.max_frame_delay = 0;
    if (dav1d_open(&d.av1, &s) < 0) {
        d.av1 = nullptr;
        note_error("decoder %u: dav1d failed to start", d.id);
        return false;
    }
    if (d.codec == 10 && d.pictures == 0) NXLOG_I("vcodec", "decoder %u: AV1 on dav1d %s (%d threads)", d.id, dav1d_version(), g_threads);
    return true;
}
#endif

// Signals owed to armed events. nn::codec's client arms an os event on a sync handle and waits on it, so the signal has to be
// executed as guest code, which needs a guest thread: the decoder worker has none. Any guest call drains them instead (svc.cpp
// calls media_drain_signals), and the game makes thousands of them a second (2026-11-21).


// ---- sync point events ----------------------------------------------------------------------------------------------------------
struct Armed {
    uint32_t dec = 0;
    uint64_t seq = 0;
};
std::mutex g_evt_mu;
std::map<uint64_t, Armed> g_armed;   // guest SyncPointEventType -> the fence it waits for

// After a decoder completed fences: the armed events whose fence is now reached (disarmed; the caller signals them).
std::vector<uint64_t> take_reached(uint32_t dec, uint64_t /*done*/) {
    std::vector<uint64_t> out;
    std::lock_guard<std::mutex> lk(g_evt_mu);
    for (auto it = g_armed.begin(); it != g_armed.end();) {
        if (it->second.dec == dec && fence_done(kSyncMarker | dec, it->second.seq)) {
            out.push_back(it->first);
            it = g_armed.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

// ---- picture conversion -----------------------------------------------------------------------------------------------------------
#if NXRT_HAVE_LIBVPX
Picture to_nv12(const vpx_image_t* img, int64_t pts) {
    Picture p;
    p.w = static_cast<uint16_t>((img->d_w + 1) & ~1u);
    p.h = static_cast<uint16_t>((img->d_h + 1) & ~1u);
    p.pts = pts;
    const size_t w = p.w, h = p.h;
    p.nv12.resize(w * h + w * h / 2);
    i420_to_nv12(p.nv12.data(), p.w, p.h, img->d_w, img->d_h,
                 img->planes[0], img->stride[0],
                 img->planes[1], img->stride[1],
                 img->planes[2], img->stride[2]);
    return p;
}
#endif

// Copies `p` into the pool's image. The picture decides the pool's size: the preference the library computed is not needed for
// the copy, and the frame properties reported to it come from the same picture, so a stale or rounded preference cannot crop it.
// The time of the last picture actually handed to the client. The film's audio track is only its rhythm source, so it is not
// allowed to run ahead of this: during the loading screen the picture thread is starved for seconds, and an audio clock that
// kept going made the player skip the picture forward to catch up with it (measured 2026-11-21).
std::atomic<int64_t> g_film_video_pts{0};
void place(Pool& pool, uint32_t index, const Picture& p) {
    pool.w = p.w;
    pool.h = p.h;
    if (p.pts > g_film_video_pts.load(std::memory_order_relaxed)) {
        g_film_video_pts.store(p.pts, std::memory_order_relaxed);
        movie_adec_video_advanced();
    }
    const size_t pw = pool.w, ph = pool.h;
    std::vector<uint8_t>& image = pool.img(index);
    image.assign(pw * ph + pw * ph / 2, 0);
    std::memset(image.data(), 16, pw * ph);
    std::memset(image.data() + pw * ph, 128, pw * ph / 2);
    if (p.nv12.empty()) return;
    const size_t cw = std::min<size_t>(pw, p.w), chh = std::min<size_t>(ph, p.h);
    for (size_t y = 0; y < chh; y++) std::memcpy(&image[y * pw], &p.nv12[y * p.w], cw);
    for (size_t y = 0; y < chh / 2; y++) std::memcpy(&image[pw * ph + y * pw], &p.nv12[size_t(p.w) * p.h + y * p.w], cw);
}

// Pairs decoded pictures with acquired buffers (both first in first out). Caller holds d.mu. Returns true when a fence completed.
// Writes held pictures into the buffers waiting in "pend" (release order), fills their slot and lets the guest poll the sync
// handle. Caller holds d.mu. Returns true when a fence completed.
bool assign(Decoder& d) {
    bool any = false;
    while (!d.pend.empty() && !d.ready.empty()) {
        const uint32_t index = d.pend.front();
        d.pend.pop_front();
        Picture p = std::move(d.ready.front());
        d.ready.pop_front();
        auto it = d.pools.find(index);
        if (Pool* pool = it == d.pools.end() ? nullptr : pool_of(it->second)) place(*pool, index, p);
        else note_error("decoder %u: buffer %u has no image pool", d.id, index);
        if (index < d.slot.size()) {
            Decoder::Slot& s = d.slot[index];
            s.w = p.w;
            s.h = p.h;
            s.ts = p.pts;
            s.eos = p.eos;
            s.filled = true;
            s.filled_gen = s.gen;
            // Says whether the end flag reaches a buffer the client can read, and how far its reads had got by then.
            if (p.eos)
                NXLOG_I("vcodec", "decoder %u: end-of-stream picture into buffer %u (placed %llu, read %llu)", d.id, index,
                        (unsigned long long)s_delivered.load(), (unsigned long long)s_read.load());
        }
        any = true;
        s_delivered++;
        g_signals_owed.fetch_add(1, std::memory_order_relaxed);   // an armed event may now be reachable
        {
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 40) {
                auto it2 = d.pools.find(index);
                if (Pool* pool = it2 == d.pools.end() ? nullptr : pool_of(it2->second)) {
                    const std::vector<uint8_t>& im = pool->img(index);
                    uint64_t h = 1469598103934665603ull;
                    for (size_t i = 0; i + 64 < im.size(); i += 4096) h = (h ^ im[i]) * 1099511628211ull;
                    if (k < 40) NXLOG_I("vcodec", "fill %u: buffer %u gen %llu h %016llx", k, index, static_cast<unsigned long long>(d.slot[index].gen),
                                        static_cast<unsigned long long>(h));
                }
            }
        }
        {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 8)
                NXLOG_I("vcodec", "decoder %u: picture %ux%u into buffer %u, gen %llu (delivered %llu)", d.id, p.w, p.h, index,
                        static_cast<unsigned long long>(d.slot[index].gen), static_cast<unsigned long long>(s_delivered.load()));
        }
    }
    // End of stream, and nothing left to hand out: fill one free buffer with the last picture marked eos, once. The client
    // reads it, learns the film ended, and stops on its own (the reference library's drain does the same).
    // ...and only when no packet is left to decode: a marker that turned up while the stream was still being decoded said
    // "the film has ended" mid-film, and the client stopped drawing right there (2026-11-22).
    if (d.eos_pending && d.pending.empty() && d.ready.empty() && !d.pend.empty()) {
        const uint32_t index = d.pend.front();
        d.pend.pop_front();
        uint16_t lw = 0, lh = 0;
        if (index < d.slot.size()) {
            Decoder::Slot& s = d.slot[index];
            // No picture: w=h=0 is what the SDK's own library answers here (hle_codec_video.c:117). A flag on a real picture
            // is the thing players drop.
            s.w = 0;
            s.h = 0;
            s.ts = 0;
            s.eos = true;
            s.filled = true;
            s.filled_gen = s.gen;
        }
        d.eos_pending = false;
        any = true;
        s_delivered++;
        g_signals_owed.fetch_add(1, std::memory_order_relaxed);
        NXLOG_I("vcodec", "decoder %u: end of stream delivered into buffer %u", d.id, index);
    }
    // A picture just left "ready" for a buffer, so the worker may be able to decode again - and it is asleep on exactly that
    // condition (ready.size() < kAhead). It used to be woken only by Submit, so the sequence "client releases its buffer, the
    // queue still holds 8 packets, the worker sleeps because ready was full" parked the whole film: the worker slept with
    // pending 8 and ready 0 while the client armed a fence nothing could ever reach (measured 2026-11-21).
    if (any) d.cv.notify_all();
    return any;
}

// Signals what a decoder state change made due: reached fences, and "submission ready" when the backlog went down. No locks held.
void after_change(::Ctx* c, Decoder& d, bool fences) {
    stats_tick();
    film_state_tick();   // no decoder lock is held here, so it cannot deadlock against fence_done
    if (fences)
        for (uint64_t evt : take_reached(d.id, d.done_seq.load())) gcall(c, os().signal_event, evt);
    bool signal = false;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        // Submission readiness depends on the INPUT queue only. It used to require ready.size() < kBacklog as well, i.e. no
        // decoded picture waiting for a buffer - and a client whose pool has ONE image then never saw the event again: it
        // stopped submitting after 135 packets, the film ended at 4 s and the picture froze, with the decoder idle and every
        // fence already completed (measured 2026-11-22). Eight queued packets is the watermark the reference keeps.
        if (d.submit_event_made && d.started && d.pending.size() < kRefuseAt) {
            signal = d.want_submit_signal;
            d.want_submit_signal = false;
        }
    }
    if (signal) gcall(c, os().signal_sysevent, reinterpret_cast<uint64_t>(d.submit_event));
}

}  // namespace

// See the note above g_film_ranges: true when [addr, addr+size) overlaps a buffer a film frame was read into. Outside the
// anonymous namespace so the NVN copy path can ask (api_cmd_work.cpp).
bool media_film_active() { return g_film_active.load(std::memory_order_relaxed); }
int64_t media_film_video_pts() { return g_film_video_pts.load(std::memory_order_relaxed); }

bool media_decoder_has_backlog() {
    std::vector<std::shared_ptr<Decoder>> decs;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        for (auto& e : g_decoders) decs.push_back(e.second);
    }
    for (auto& d : decs) {
        std::lock_guard<std::mutex> lk(d->mu);
        if (!d->ready.empty()) return true;
    }
    return false;
}

// Diagnostic for the player trace's stall watchdog (no locks held by the caller): what the decoder holds, and what the client
// is still armed on. "REACHED" means the picture is in the buffer and our signal never got out; "not reached" means no picture
// was placed at all; nothing printed means the client is not waiting on a fence - it is waiting on something else entirely.
void media_dump_state(const char* why) {
    std::vector<std::shared_ptr<Decoder>> decs;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        for (auto& e : g_decoders) decs.push_back(e.second);
    }
    for (auto& d : decs) {
        size_t pending = 0, ready = 0, pend = 0, fifo = 0, filled = 0;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            pending = d->pending.size();
            ready = d->ready.size();
            pend = d->pend.size();
            fifo = d->fifo.size();
            for (auto& s : d->slot) if (s.filled) ++filled;
        }
        NXLOG_W("vcodec", "%s: decoder %u pending %zu ready %zu pend %zu fifo %zu filled %zu | placed %llu read %llu pictures %llu",
                why, d->id, pending, ready, pend, fifo, filled, (unsigned long long)s_delivered.load(),
                (unsigned long long)s_read.load(), (unsigned long long)d->pictures);
    }
    std::lock_guard<std::mutex> lk(g_evt_mu);
    if (g_armed.empty()) NXLOG_W("vcodec", "%s: the client is not armed on any decoder fence", why);
    for (auto& kv : g_armed) {
        const bool reached = fence_done(kSyncMarker | kv.second.dec, kv.second.seq);
        NXLOG_W("vcodec", "%s: client armed evt %llx on decoder %u gen %llu index %llu -> %s", why, (unsigned long long)kv.first,
                kv.second.dec, (unsigned long long)(kv.second.seq >> 32), (unsigned long long)(kv.second.seq & 0xffffffffull),
                reached ? "REACHED (the picture is there; our signal never went out)" : "not reached (no picture was placed)");
    }
}

bool media_film_range_hit(uint64_t addr, uint64_t size) {
    if (!addr || !size) return false;
    std::lock_guard<std::mutex> lk(g_film_range_mu);
    for (int i = 0; i < g_film_range_n; ++i) {
        const FilmRange& r = g_film_ranges[i];
        if (addr < r.addr + r.size && r.addr < addr + size) return true;
    }
    return false;
}

// Delivers signals the decoder workers owe (see g_signals_owed). Called from the SVC dispatcher with the calling guest thread's
// context, which is the only place a guest event may be signalled.
void media_drain_signals(::Ctx* c) {
    if (!c || g_signals_owed.load(std::memory_order_relaxed) <= 0) return;
    std::vector<std::shared_ptr<Decoder>> decs;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        for (auto& e : g_decoders) decs.push_back(e.second);
    }
    bool any = false;
    for (auto& d : decs) {
        for (uint64_t evt : take_reached(d->id, 0)) {
            gcall(c, os().signal_event, evt);
            {
                static std::atomic<uint32_t> n{0};
                if (n.fetch_add(1, std::memory_order_relaxed) < 20) NXLOG_I("vcodec", "signal event %llx (decoder %u)", (unsigned long long)evt, d->id);
            }
            any = true;
        }
        // The submission-ready event too: the client stops feeding packets when the decoder holds too many pictures and
        // waits for this, and a client that stops feeding also stops calling, so it has to be raised from another thread's
        // call as well (the film read one picture and then did nothing, 2026-11-21).
        bool signal = false;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            // Only when a packet would actually be accepted now: a manual-clear event that stays signalled makes the
            // client resubmit the same packet in a tight loop (200 us apart, 2026-11-21).
            if (d->submit_event_made && d->started && d->want_submit_signal && d->pending.size() < kRefuseAt) {
                signal = true;
                d->want_submit_signal = false;
            }
        }
        if (signal) {
            gcall(c, os().signal_sysevent, reinterpret_cast<uint64_t>(d->submit_event));
            any = true;
        }
    }
    if (any) g_signals_owed.store(0, std::memory_order_relaxed);
}

namespace {
void decode_into(Decoder& d, const uint8_t* data, size_t size, int64_t pts, std::vector<Picture>& out) {
    // [media] hwdec (default on): let the GPU's decoder do VP8/VP9/AV1 when it is an NVIDIA one. It decodes 4K AV1 far
    // faster than dav1d on the CPU, which is what the DLC openings need to play in real time (2026-11-20).
    // Hardware decoding only on the worker thread: a decode in the player's own submit call blocks the player - the
    // 2026-11-18 finding of the dav1d path (52, 155.3) - and the picture then never advances (one frame, then frozen).
    // FFmpeg is used from the worker and from the player's own fallback alike: one decoder per stream. Feeding the same
    // packets to dav1d as well left two decoder states and the picture stopped after a few frames (2026-11-20).
    // Whoever decodes creates the one hardware decoder for this stream, worker or not: a submit only queues, and the one
    // place outside the worker that decodes is Acquire, when the worker is late. Letting that path fall through to dav1d
    // opened a SECOND decoder in the middle of the stream - dav1d cannot start from a non-keyframe, answered -22, and the
    // cutscene came out in blocky garbage (0.1.4 on the 6800 XT, 2026-11-21; the blocks lasted until the worker's decoder
    // took over). Creation runs under dec_mu, which serialises it against the worker.
    if (g_hwdec && data && size && (!d.ff_tried || d.ff)) {
        // No lock of d.mu here: decode_one already runs the decode under dec_mu, and the submit path holds d.mu. Taking both
        // in the other order deadlocked on the first frame (2026-11-21).
        if (!d.ff && !d.ff_tried) {
            d.ff_tried = true;
            std::string why;
            d.ff = ffdec_create(d.codec, &why);
            if (d.ff) NXLOG_I("vcodec", "decoder %u: %s on FFmpeg hardware (%s)", d.id, d.codec == 10 ? "AV1" : d.codec == 9 ? "VP9" : "VP8",
                              ffdec_hw_name());
            else NXLOG_I("vcodec", "decoder %u: no FFmpeg hardware decoder (%s)", d.id, why.c_str());
        }
        if (d.ff) {
            std::vector<FfDecPicture> pics;
            if (ffdec_decode(d.ff, data, size, pts, &pics)) {
                for (FfDecPicture& q : pics) {
                    Picture pic;
                    pic.w = static_cast<uint16_t>(q.w);
                    pic.h = static_cast<uint16_t>(q.h);
                    pic.pts = q.pts;
                    pic.nv12 = std::move(q.nv12);
                    out.push_back(std::move(pic));
                }
                return;
            }
            NXLOG_W("vcodec", "decoder %u: FFmpeg hardware decode failed; falling back", d.id);
            ffdec_destroy(d.ff);
            d.ff = nullptr;
        }
    }
    if (g_hwdec && t_on_worker && data && size) {
        if (!d.nv && !d.nv_tried) {
            d.nv_tried = true;
            std::string why;
            d.nv = nvdec_create(d.codec, 0, 0, &why);
            if (d.nv) NXLOG_I("vcodec", "decoder %u: %s on NVDEC (hardware)", d.id, d.codec == 10 ? "AV1" : d.codec == 9 ? "VP9" : "VP8");
            else NXLOG_I("vcodec", "decoder %u: no NVDEC (%s); the CPU decodes", d.id, why.c_str());
        }
        if (d.nv) {
            std::vector<NvDecPicture> pics;
            if (nvdec_decode(d.nv, data, size, pts, &pics)) {
                for (NvDecPicture& q : pics) {
                    Picture pic;
                    pic.w = static_cast<uint16_t>(q.w);
                    pic.h = static_cast<uint16_t>(q.h);
                    pic.pts = q.pts;
                    pic.nv12 = std::move(q.nv12);
                    out.push_back(std::move(pic));
                }
                return;
            }
            NXLOG_W("vcodec", "decoder %u: NVDEC failed; the CPU takes over for the rest of this stream", d.id);
            nvdec_destroy(d.nv);
            d.nv = nullptr;
        }
    }
#if NXRT_HAVE_DAV1D
    if (d.codec == 10) {   // AV1 (nn::codec's Av1DecoderType): the SDK 3.0.1 movie library's video path
        auto drain_dav1d = [&] {
            for (;;) {
                Dav1dPicture pic{};
                if (dav1d_get_picture(d.av1, &pic) < 0) break;
                if (pic.p.bpc == 8 && pic.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
                    Picture p;
                    p.w = static_cast<uint16_t>((pic.p.w + 1) & ~1u);
                    p.h = static_cast<uint16_t>((pic.p.h + 1) & ~1u);
                    p.pts = pic.m.timestamp;
                    const size_t w = p.w, h = p.h;
                    p.nv12.resize(w * h + w * h / 2);
                    i420_to_nv12(p.nv12.data(), p.w, p.h,
                                 static_cast<uint32_t>(pic.p.w), static_cast<uint32_t>(pic.p.h),
                                 static_cast<const uint8_t*>(pic.data[0]), pic.stride[0],
                                 static_cast<const uint8_t*>(pic.data[1]), pic.stride[1],
                                 static_cast<const uint8_t*>(pic.data[2]), pic.stride[1]);
                    if (d.pictures++ == 0) NXLOG_I("vcodec", "decoder %u: first picture %ux%u (AV1, dav1d %s, %d threads)", d.id, p.w, p.h, dav1d_version(), g_threads);
                    out.push_back(std::move(p));
                    s_decoded++;
                } else {
                    note_error("decoder %u: AV1 picture %d bits, layout %d not handled (8-bit 4:2:0 only)", d.id, pic.p.bpc, static_cast<int>(pic.p.layout));
                }
                dav1d_picture_unref(&pic);
            }
        };
        if (!data || !size) {
            if (d.av1) drain_dav1d();   // drain delayed frames on flush / EOS
            return;
        }
        if (!d.av1 && !av1_open(d)) return;
        Dav1dData in{};
        uint8_t* b = dav1d_data_create(&in, size);
        if (!b) { note_error("decoder %u: out of memory for a %zu-byte packet", d.id, size); return; }
        std::memcpy(b, data, size);
        in.m.timestamp = pts;
        for (int guard = 0; in.sz && guard < 64; ++guard) {
            const int r = dav1d_send_data(d.av1, &in);
            if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
                note_error("decoder %u: dav1d_send_data(%zu bytes, pts %lld) failed: %d", d.id, size, static_cast<long long>(pts), r);
                break;
            }
            drain_dav1d();
        }
        if (in.sz) dav1d_data_unref(&in);
        return;
    }
#endif
#if NXRT_HAVE_LIBVPX
    if (!d.vpx_ok && !vpx_open(d)) return;
    const vpx_codec_err_t e = vpx_codec_decode(&d.vpx, data, static_cast<unsigned>(size), nullptr, 0);
    if (e != VPX_CODEC_OK)
        note_error("decoder %u: vpx_codec_decode(%zu bytes, pts %lld) failed: %s (%s)", d.id, size, (long long)pts, vpx_codec_err_to_string(e),
                   vpx_codec_error_detail(&d.vpx) ? vpx_codec_error_detail(&d.vpx) : "");
    vpx_codec_iter_t it = nullptr;
    while (const vpx_image_t* img = vpx_codec_get_frame(&d.vpx, &it)) {
        if (img->fmt != VPX_IMG_FMT_I420) {
            note_error("decoder %u: picture format 0x%x not handled (8-bit 4:2:0 only)", d.id, img->fmt);
            continue;
        }
        Picture p = to_nv12(img, pts);
        if (d.pictures++ == 0) NXLOG_I("vcodec", "decoder %u: first picture %ux%u (VP%d, %d threads)", d.id, img->d_w, img->d_h, d.codec, g_threads);
        out.push_back(std::move(p));
        s_decoded++;
    }
#else
    note_error("decoder %u: VP%d packet received but runtime was built without libvpx", d.id, d.codec);
#endif
}

// ---- nn::codec handlers -----------------------------------------------------------------------------------------------------------
void make_pref(::Ctx* c) {   // MakeDecodeImagePreference(w, h, format, mode) -> Preference via x8
    const uint64_t out = c->x[8];
    // The mode is the whole question: 2 is the SDK's native-texture mode, where the decoder writes each picture straight
    // into the client's own NVN texture instead of the client reading it out (hle_movie.c: MD_MODE_TEX). Nothing in this
    // runtime reads it, so if the client asks for it we silently degrade to CPU reads (2026-11-21).
    {
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
        if (k < 16)
            NXLOG_I("vcodec", "MakeDecodeImagePreference #%u: %ux%u format %u mode %u", k, static_cast<uint32_t>(A(0)),
                    static_cast<uint32_t>(A(1)), static_cast<uint32_t>(A(2)), static_cast<uint32_t>(A(3)));
    }
    std::memset(reinterpret_cast<void*>(out), 0, 64);
    wr16(out + 0, static_cast<uint16_t>(A(0)));
    wr16(out + 2, static_cast<uint16_t>(A(1)));
    wr32(out + 4, static_cast<uint32_t>(A(2)));
    wr32(out + 8, static_cast<uint32_t>(A(3)));
    wr64(out + 16, kPrefMarker);
    RET(out);
}
void calc_pool(::Ctx* c) {   // CalculateDecoderImagePoolBuffer(size*, align*, pref*, count): the pictures live on the host
    // The size has to be what one 4K NV12 picture really needs per image: 0x1000 a picture told the library a pool this big
    // holds one image, and it then asked for count 1 and armed fences nothing could complete (2026-11-21).
    const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(A(3)));
    uint32_t w = 0, h = 0;
    const uint64_t pref = A(2);
    if (pref && rd64(pref + 16) == kPrefMarker) {
        std::memcpy(&w, reinterpret_cast<const void*>(pref), 2);
        std::memcpy(&h, reinterpret_cast<const void*>(pref + 2), 2);
    }
    const uint64_t per = (w && h) ? nv12_storage(w, h) : 0x1000ull;
    if (A(0)) wr64(A(0), per * n);
    if (A(1)) wr64(A(1), 0x1000);
    {
        static std::atomic<uint32_t> n2{0};
        const uint32_t k = n2.fetch_add(1, std::memory_order_relaxed);
        if (k < 16)
            NXLOG_I("vcodec", "CalculateDecoderImagePoolBuffer #%u: %ux%u count %u -> size %llu align 0x1000 (one NV12 picture %llu)",
                    k, w, h, n, (unsigned long long)(per * n), (unsigned long long)per);
    }
    RET(0);
}
void init_pool(::Ctx* c) {   // InitializeDecoderImagePool(pool, mem, size, pref*, count, mode)
    const uint64_t g = A(0), mem = A(1), size = A(2), pref = A(3);
    Pool* p = new Pool();
    if (pref && rd64(pref + 16) == kPrefMarker) {
        std::memcpy(&p->w, reinterpret_cast<const void*>(pref), 2);
        std::memcpy(&p->h, reinterpret_cast<const void*>(pref + 2), 2);
        std::memcpy(&p->format, reinterpret_cast<const void*>(pref + 4), 4);
    }
    p->w = static_cast<uint16_t>((p->w + 1) & ~1);
    p->h = static_cast<uint16_t>((p->h + 1) & ~1);
    wr64(g, reinterpret_cast<uint64_t>(p));
    wr64(g + 8, kPoolMarker);
    // The client hands us the memory its pool is supposed to live in (mem, size). This runtime keeps the pictures on the host
    // and copies them out on ReadDecoderOutputImage, so nothing here ever writes that memory - but if the game builds its film
    // texture over it, the screen is showing memory nobody updates. Registering the range lets the texture probe say so.
    film_range_note(mem, size);
    // The library can read a pool image before the first picture is placed in it (the first acquire happens before any packet is
    // decoded). An all-zero image decodes as bright green, which flashed for the first seconds of the 4K CG (uv0=0 at read #1,
    // 127 from read #2). Start it as limited-range black instead.
    if (p->w && p->h) {
        // count images, not one: the player holds more than one acquired buffer at a time (see Pool)
        const uint32_t count = std::max<uint32_t>(1, static_cast<uint32_t>(A(4)));
        const size_t n = static_cast<size_t>(p->w) * p->h;
        for (uint32_t i = 0; i < count; ++i) {
            std::vector<uint8_t>& im = p->img(i);
            im.assign(n + n / 2, 128);
            std::memset(im.data(), 16, n);
        }
    }
    {
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
        if (k < 64)
        NXLOG_I("vcodec", "pool #%u InitializeDecoderImagePool %llx: %ux%u format %u count %u mode %u | mem %llx size %llu (image %zu bytes, uv0 %u)", k, (unsigned long long)g, p->w, p->h,
                p->format, p->count(), static_cast<uint32_t>(A(5)), (unsigned long long)mem, (unsigned long long)size,
                p->imgs.empty() ? size_t(0) : p->imgs[0].size(),
                (!p->imgs.empty() && p->imgs[0].size() > static_cast<size_t>(p->w) * p->h) ? p->imgs[0][static_cast<size_t>(p->w) * p->h] : 999u);
    }
    RET(0);
}
void fini_pool(::Ctx* c) {
    if (Pool* p = pool_of(A(0))) delete p;
    if (A(0)) { wr64(A(0), 0); wr64(A(0) + 8, 0); }
    RET(0);
}
std::atomic<int64_t> g_last_acquire_ns{0};
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void read_nv12(::Ctx* c) {
    const int64_t t0 = now_ns();
    Pool* p = pool_of(A(0));
    const uint64_t sp = c->x[31];
    const uint32_t x0 = static_cast<uint16_t>(A(2)), y0 = static_cast<uint16_t>(A(3));
    const uint32_t w = static_cast<uint16_t>(A(4)), h = static_cast<uint16_t>(A(5));
    uint8_t* ydst = reinterpret_cast<uint8_t*>(A(6));
    const size_t ystride = A(7), ysize = rd64(sp);
    uint8_t* uvdst = reinterpret_cast<uint8_t*>(rd64(sp + 8));
    const size_t uvstride = rd64(sp + 16), uvsize = rd64(sp + 24);
    s_read++;
    stats_tick();
    // Every 150 reads a summary of the whole path: on the AMD machine the film sits on its first picture while the decoder
    // has long finished, and this says whether the player keeps reading (2026-11-21).
    if (g_log && (s_read % 150) == 0)
        NXLOG_I("vcodec", "read tick: %llu reads so far, pool image %zux%zu, y0 %u h %u, ystride %zu ysize %zu, uvsize %zu",
                static_cast<unsigned long long>(s_read.load()), p ? p->w : 0, p ? p->h : 0, y0, h, ystride, ysize, uvsize);
    if (g_log) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 6)
            NXLOG_I("vcodec", "ReadDecoderOutputImageNv12(index %u, %ux%u at %u,%u) pool %llx", static_cast<uint32_t>(A(1)), w, h, x0, y0,
                    (unsigned long long)A(0));
    }
    if (g_log) {
        static std::atomic<uint32_t> uv_n{0};
        if (uv_n.fetch_add(1, std::memory_order_relaxed) < 300) {
            const bool have = p && !p->imgs.empty() && !p->imgs[0].empty();
            const size_t pn = have ? static_cast<size_t>(p->w) * p->h : 0;
            NXLOG_I("vcodec", "read #%u: uvdst=0x%llx uvstride=%zu uvsize=%zu uv0=%u | pool 0x%llx %zux%zu image %zu uv0 %u",
                    uv_n.load(), (unsigned long long)uvdst, uvstride, uvsize,
                    (uvdst && uvsize > 2) ? static_cast<unsigned>(uvdst[0]) : 999u,
                    (unsigned long long)A(0), p ? p->w : 0, p ? p->h : 0, have ? p->imgs[0].size() : size_t(0),
                    (have && p->imgs[0].size() > pn) ? p->imgs[0][pn] : 999u);
        }
    }
    const uint32_t image_index = static_cast<uint32_t>(A(1) & 0xffff);
    if (!p || p->imgs.empty() || image_index >= p->count() || p->imgs[image_index].empty() || !ydst) { RET(0); return; }

    const std::vector<uint8_t>& nv12 = p->imgs[image_index];
    film_range_note(reinterpret_cast<uint64_t>(ydst), static_cast<uint64_t>(h) * ystride * 3 / 2);
    // NXRT_FILM_HASH=1: a numeric fingerprint of every read, sampled over the luma plane. Numbers in the log answer "does the
    // picture the player reads change" without comparing images: same hash twice means the same picture came out (2026-11-21).
    static const bool read_hash = std::getenv("NXRT_FILM_HASH") != nullptr;
    const size_t pw = p->w, ph = p->h;
    const size_t cw = x0 < pw ? std::min<size_t>(w, pw - x0) : 0;
    for (size_t y = 0; y < h && y0 + y < ph; y++) {
        if ((y + 1) * ystride > ysize && y * ystride + cw > ysize) break;
        std::memcpy(ydst + y * ystride, &nv12[(y0 + y) * pw + x0], cw);
    }
    if (uvdst)
        for (size_t y = 0; y < h / 2 && y0 / 2 + y < ph / 2; y++) {
            if (y * uvstride + cw > uvsize) break;
            std::memcpy(uvdst + y * uvstride, &nv12[pw * ph + (y0 / 2 + y) * pw + (x0 & ~1u)], cw);
        }
    // The client's picture is sampled from this memory: no buffer->texture copy ever happens in the film path, so a texture
    // built over these buffers is what reaches the screen. A write through this pointer comes from the host, not from the
    // guest's own store, so the texture write-watch never records it and the overlay keeps showing the first frame. Mark the
    // range the way the runtime does after its own CPU writes (2026-11-21).
    nxrt::nvn::be::textures_cpu_wrote(ydst, static_cast<uint64_t>(h) * ystride);
    if (uvdst) nxrt::nvn::be::textures_cpu_wrote(uvdst, static_cast<uint64_t>(h / 2) * uvstride);
    if (g_log) {
        static std::atomic<uint32_t> stat_n{0};
        if (stat_n.fetch_add(1, std::memory_order_relaxed) < 4 && p && !p->imgs.empty() && !nv12.empty()) {
            const size_t pn = static_cast<size_t>(p->w) * p->h;
            uint64_t ysum = 0, uvsum = 0;
            size_t yn = 0, uvn = 0;
            for (size_t k = 0; k < pn; k += 4096) { ysum += nv12[k]; yn++; }
            for (size_t k = pn; k < nv12.size() && uvn < 4096; k += 4096) { uvsum += nv12[k]; uvn++; }
            NXLOG_I("vcodec", "pool image stats: Y avg %.1f (n=%zu), UV avg %.1f (n=%zu)", yn ? double(ysum) / yn : 0.0, yn,
                    uvn ? double(uvsum) / uvn : 0.0, uvn);
        }
        static std::atomic<uint32_t> n{0};
        const int64_t t1 = now_ns(), since = t0 - g_last_acquire_ns.load(std::memory_order_relaxed);
        if ((since > 40'000'000 || t1 - t0 > 10'000'000) && n.fetch_add(1, std::memory_order_relaxed) < 30)
            NXLOG_I("vcodec", "read timing: %.1f ms after acquire, copy took %.1f ms", since / 1e6, (t1 - t0) / 1e6);
    }
    RET(0);
}
inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
void read_rgba(::Ctx* c) {   // BT.709 limited range
    Pool* p = pool_of(A(0));
    const uint32_t x0 = static_cast<uint16_t>(A(2)), y0 = static_cast<uint16_t>(A(3));
    const uint32_t w = static_cast<uint16_t>(A(4)), h = static_cast<uint16_t>(A(5));
    uint8_t* dst = reinterpret_cast<uint8_t*>(A(6));
    const size_t stride = A(7), size = rd64(c->x[31]);
    s_read++;
    const uint32_t rgba_index = static_cast<uint32_t>(A(1) & 0xffff);
    if (!p || p->imgs.empty() || rgba_index >= p->count() || p->imgs[rgba_index].empty() || !dst) { RET(0); return; }
    const std::vector<uint8_t>& nv12 = p->imgs[rgba_index];
    const size_t pw = p->w, ph = p->h;
    for (size_t y = 0; y < h && y0 + y < ph; y++) {
        if ((y + 1) * stride > size) break;
        uint8_t* o = dst + y * stride;
        const uint8_t* yr = &nv12[(y0 + y) * pw];
        const uint8_t* uvr = &nv12[pw * ph + ((y0 + y) / 2) * pw];
        for (size_t x = 0; x < w && x0 + x < pw; x++) {
            const size_t sx = x0 + x;
            const int Y = (yr[sx] - 16) * 298, U = uvr[sx & ~size_t(1)] - 128, V = uvr[(sx & ~size_t(1)) + 1] - 128;
            o[4 * x + 0] = clamp8((Y + 459 * V + 128) >> 8);
            o[4 * x + 1] = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
            o[4 * x + 2] = clamp8((Y + 541 * U + 128) >> 8);
            o[4 * x + 3] = 255;
        }
    }
    // As in read_nv12: a host-side write must be announced or the texture over this buffer is never uploaded again.
    if (dst) nxrt::nvn::be::textures_cpu_wrote(dst, static_cast<uint64_t>(h) * stride);
    RET(0);
}
void read_rgba_hdr(::Ctx* c) {   // same pixels as Rgba; the transfer function is applied by the player's shader
    read_rgba(c);
}
void native_image_handle(::Ctx* c) {
    // The client asks for a native image only if it means to sample the pool's memory as a texture (the reference
    // implementation hands out the game's own buffer for exactly that). We answer 0, so it has to fall back to CPU reads.
    // Logged on every call, ungated: whether it asks at all, and how often, separates "it needs the image and we refuse" from
    // "it never wanted one" (2026-11-21).
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 64) {
        Pool* p = pool_of(A(0));
        NXLOG_W("vcodec", "GetDecoderImagePoolNativeImageHandle #%u: pool %llx index %u (%ux%u, %u image(s)) -> 0 (not implemented, CPU reads only)",
                k, (unsigned long long)A(0), static_cast<uint32_t>(A(1)), p ? p->w : 0, p ? p->h : 0, p ? p->count() : 0);
    }
    RET(0);
}

void calc_work(::Ctx* c) {   // Calculate*DecoderWorkBuffer(size*, align*, ...)
    if (A(0)) wr64(A(0), 0x1000);
    if (A(1)) wr64(A(1), 0x1000);
    RET(0);
}
void create_decoder(::Ctx* c, int codec, uint32_t w, uint32_t h) {
    const uint64_t g = A(0);
    g_film_active.store(true, std::memory_order_relaxed);
    player_trace_new_film();   // each film gets its own budget of stall dumps (movie_trace.cpp)
    nxrt::nvn::be::film_texture_begin_film();   // and its own set of 4K textures to watch (nvn_texture_registry.cpp)
    auto d = std::make_shared<Decoder>();
    d->codec = codec;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        d->id = g_next_id++;
        g_decoders[d->id] = d;
    }
    if (codec != 10) vpx_open(*d);   // AV1 opens dav1d on its first packet (create_decoder has no configuration record)
    d->worker = std::thread([raw = d.get()] { worker_loop(*raw); });   // joined in ~Decoder, before the decoder dies
    d->worker_started = true;
    wr64(g, d->id);
    wr64(g + 8, kDecMarker);
    s_created++;
#if NXRT_HAVE_DAV1D
    if (codec == 10)
        NXLOG_I("vcodec", "AV1 decoder %u created (%ux%u max) on dav1d %s (%d threads)", d->id, w, h, dav1d_version(), g_threads);
    else
#endif
#if NXRT_HAVE_LIBVPX
        NXLOG_I("vcodec", "VP%d decoder %u created (%ux%u max) on libvpx %s", codec, d->id, w, h, vpx_codec_version_str());
#else
        NXLOG_W("vcodec", "VP%d decoder %u created (%ux%u max) without libvpx", codec, d->id, w, h);
#endif
    RET(0);
}
void init_vp9_tm(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_vp9_rm(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }
void init_vp9_rm2(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(3)), static_cast<uint16_t>(A(4))); }
void init_vp8_tm(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_vp8_rm(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }
void init_vp8_rm2(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(3)), static_cast<uint16_t>(A(4))); }
// SDK 3.0.1's nn::codec adds *DecoderByTransferMemory / ByRemoteMemory: (type, mem, size, size, u16 w, u16 h, u32, in fmt, out fmt,
// mode) / (type, size, u16 w, u16 h, u32, in fmt, out fmt, mode). The height is in the same registers as the older Initialize*Decoder.
void init_av1_tm(::Ctx* c) { create_decoder(c, 10, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_av1_rm(::Ctx* c) { create_decoder(c, 10, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }

void finalize_decoder(::Ctx* c) {
    const uint64_t g = A(0);
    auto d = dec_of(g);
    if (d) {
        bool no_live_decoders = false;
        {
            std::lock_guard<std::mutex> lk(g_reg_mu);
            g_decoders.erase(d->id);
            no_live_decoders = g_decoders.empty();
        }
        // fences of a finalized decoder count as reached: wake whoever still waits
        for (uint64_t evt : take_reached(d->id, ~0ull)) gcall(c, os().signal_event, evt);
        if (d->submit_event_made && os().destroy_sysevent) gcall(c, os().destroy_sysevent, reinterpret_cast<uint64_t>(d->submit_event));
        // Stop and join the worker BEFORE closing codec contexts: finalizing mid-film with pending packets
        // would otherwise close d->av1/vpx while decode_one runs dav1d_send_data without d->mu (#52).
        {
            std::lock_guard<std::mutex> lk(d->mu);
            d->stop = true;
            d->pending.clear();
            d->ready.clear();
            d->pend.clear();
            d->eos_pending = false;
        }
        d->cv.notify_all();
        if (d->worker.joinable()) d->worker.join();
        {
            std::lock_guard<std::mutex> dlk(d->dec_mu);
            std::lock_guard<std::mutex> lk(d->mu);
            vpx_close(*d);
#if NXRT_HAVE_DAV1D
            av1_close(*d);
#endif
            if (d->nv) { nvdec_destroy(d->nv); d->nv = nullptr; }
            if (d->ff) { ffdec_destroy(d->ff); d->ff = nullptr; }
        }
        if (!d->film_end_signaled) {
            d->film_end_signaled = true;
            movie_adec_film_end();
            hitch_note_film_end();
        }
        if (no_live_decoders) {
            g_film_video_pts.store(0, std::memory_order_relaxed);
            g_film_active.store(false, std::memory_order_relaxed);
        }
        NXLOG_I("vcodec", "decoder %u finalized after %llu pictures", d->id, (unsigned long long)d->pictures);
        s_destroyed++;
        wr64(g, 0);
        wr64(g + 8, 0);
    }
    RET(0);
}
void register_buffer(::Ctx* c) {   // (dec, index, pool, u32)
    film_trace("register_buffer", A(1), A(2), A(3));
    auto d = dec_of(A(0));
    if (d) {
        std::lock_guard<std::mutex> lk(d->mu);
        const uint32_t index = static_cast<uint32_t>(A(1));
        d->pools[index] = A(2);
        if (index < d->slot.size()) {
            Decoder::Slot& s = d->slot[index];
            s.reg = true;
            s.gen = 1;
            s.filled = false;
            d->pend.push_back(index);                       // waiting for a picture
            if (d->fifo.size() < 8) d->fifo.push_back(index);   // and acquirable, as the SDK's library does on register
        }
        // How many output buffers the client hands us decides whether it can pipeline at all: the SDK's own library makes one
        // image pool per picture and registers each. Not gated on [media] video_decoder_log - this is the first line to read
        // when a film plays for a second and then stops (2026-11-21).
        {
            static std::atomic<uint32_t> rn{0};
            const uint32_t k = rn.fetch_add(1, std::memory_order_relaxed);
            if (k < 64) {
                Pool* p = pool_of(A(2));
                NXLOG_I("vcodec", "register #%u: decoder %u buffer %u pool %llx (%ux%u, %u image(s)) | pools %zu fifo %zu pend %zu",
                        k, d->id, index, (unsigned long long)A(2), p ? p->w : 0, p ? p->h : 0, p ? p->count() : 0,
                        d->pools.size(), d->fifo.size(), d->pend.size());
            }
        }
        if (index >= d->slot.size())
            NXLOG_W("vcodec", "register: buffer index %u is past the %zu slots and was dropped", index, d->slot.size());
    }
    RET(0);
}
// Start*Decoder: from here on input is accepted, so the submission-ready event is signalled. It must not be signalled before: subsdk0's
// decoder adapter thread wakes on it, forwards "input available" to the player only while the adapter is started, and clears the event
// either way, so a signal that comes before Start is lost and the player never sends its first packet.
void start_decoder(::Ctx* c) {
    auto d = dec_of(A(0));
    if (d) {
        {
            std::lock_guard<std::mutex> lk(d->mu);
            d->started = true;
            d->want_submit_signal = true;
        }
        NXLOG_I("vcodec", "decoder %u started", d->id);
        after_change(c, *d, false);
    }
    RET(0);
}
void stop_decoder(::Ctx* c) {
    if (auto d = dec_of(A(0))) {
        std::lock_guard<std::mutex> lk(d->mu);
        d->started = false;
    }
    RET(0);
}
void flush_decoder(::Ctx* c) {
    auto d = dec_of(A(0));
    if (!d) { RET(0); return; }
    {
        std::lock_guard<std::mutex> lk(d->mu);
        // Flush: decoded pictures are dropped; every registered buffer goes back to waiting for a picture (the release path
        // bumps the generation, so an old fence handle does not read a stale image).
        for (uint32_t i = 0; i < d->slot.size(); ++i) {
            Decoder::Slot& s = d->slot[i];
            if (!s.reg) continue;
            s.filled = false;
            s.gen++;
            s.w = s.h = 0;
            s.ts = 0;
            s.eos = false;
            if (d->pend.size() < 32) d->pend.push_back(i);
            if (d->fifo.size() < 8) d->fifo.push_back(i);
        }
        d->ready.clear();
        vpx_close(*d);
#if NXRT_HAVE_DAV1D
        av1_close(*d);   // the next packet after a flush starts at a key frame and carries the sequence header again
#endif
        d->want_submit_signal = true;
    }
    if (g_log) NXLOG_I("vcodec", "decoder %u flushed", d->id);
    after_change(c, *d, true);
    RET(0);
}
void submission_event(::Ctx* c) {   // (SystemEventType** out, dec)
    auto d = dec_of(A(1));
    if (!d) { if (A(0)) wr64(A(0), 0); RET(0); return; }
    bool make = false;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        make = !d->submit_event_made;
        d->submit_event_made = true;
    }
    const uint64_t ev = reinterpret_cast<uint64_t>(d->submit_event);
    if (make) {
        gcall(c, os().create_sysevent, ev, kManualClear, 0);
        bool started;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            started = d->started;
        }
        if (started) gcall(c, os().signal_sysevent, ev);   // asked for after Start: input is accepted now (else Start signals it)
    }
    if (A(0)) wr64(A(0), ev);
    if (g_log) NXLOG_I("vcodec", "decoder %u: submission ready event at 0x%llx (created %d)", d->id, (unsigned long long)ev, make ? 1 : 0);
    RET(0);
}
// SDK 3.0.1's Get*DecoderSubmissionReadyEvent(type) returns the nn::os::SystemEventType* itself (no out parameter).
void submission_event_ret(::Ctx* c) {   // (dec) -> SystemEventType*
    auto d = dec_of(A(0));
    if (!d) { RET(0); return; }
    bool make = false;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        make = !d->submit_event_made;
        d->submit_event_made = true;
    }
    const uint64_t ev = reinterpret_cast<uint64_t>(d->submit_event);
    if (make) {
        gcall(c, os().create_sysevent, ev, kManualClear, 0);
        bool started;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            started = d->started;
        }
        if (started) gcall(c, os().signal_sysevent, ev);
    }
    if (g_log) NXLOG_I("vcodec", "decoder %u: submission ready event -> 0x%llx (created %d)", d->id, (unsigned long long)ev, make ? 1 : 0);
    RET(ev);
}
// Decodes one queued packet into pictures (no d.mu held while the decode runs, so the player's Acquire never waits on it).
// Holding d.dec_mu before popping from d.pending preserves strict packet order whenever both the worker and Acquire call decode_one.
void decode_one(Decoder& d) {
    std::vector<Picture> out;
    bool is_eos = false;
    {
        std::lock_guard<std::mutex> dlk(d.dec_mu);   // serialize pop + decode so packets never overtake each other
        Pending p;
        {
            std::lock_guard<std::mutex> lk(d.mu);
            if (d.pending.empty()) return;
            p = std::move(d.pending.front());
            d.pending.pop_front();
        }
        is_eos = p.eos;
        if (!p.data.empty()) decode_into(d, p.data.data(), p.data.size(), p.pts, out);
        if (p.eos) {
            d.film_end_signaled = true;
            player_trace_eos();   // from here no new picture is expected: the dump that follows is the hand-back, or its absence
            movie_adec_film_end();   // the placeholder audio track ends with the picture; the player's finish covers every track
            if (d.vpx_ok || d.av1) decode_into(d, nullptr, 0, p.pts, out);   // drain
            d.eos_pending = true;
            NXLOG_I("vcodec", "decoder %u: end of stream after %llu pictures (%zu still queued)", d.id,
                    (unsigned long long)d.pictures, d.pending.size());
            hitch_note_film_end();   // the hitch summaries say how long ago this was, so a slow stretch after a cutscene is visible
        }
        if (out.empty()) {
            if (is_eos) {
                std::lock_guard<std::mutex> lk(d.mu);
                d.eos_pending = true;
                NXLOG_I("vcodec", "decoder %u: end of stream waiting for the drain (%zu pictures in hand, %llu read so far)", d.id,
                        d.ready.size(), (unsigned long long)s_read.load());
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(d.mu);
            if (d.first_picture_ms < 0 && d.t_first_packet.time_since_epoch().count() != 0) {
                d.first_picture_ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - d.t_first_packet).count());
                NXLOG_I("vcodec", "decoder %u: first picture %lld ms after its first packet", d.id, d.first_picture_ms);
            }
            for (auto& pic : out) d.ready.push_back(std::move(pic));
        }
    }
    d.cv.notify_all();
}
// Decodes ahead of the player, so the decode (4K AV1, ~20 ms) overlaps the library's fill and the player's texture work instead of
// adding to them. It only fills ready: fences are completed on the guest thread (Assign in Acquire / Submit), which is the only
// place a guest event may be signalled.
void worker_loop(Decoder& d) {
    t_on_worker = true;
    // The film starts while the game is still loading, and the player's own thread is what loses the CPU then (frames every
    // 2-8 s for the first ~20 s). Keeping the decoders off its way is right for the steady state - but not for the FIRST
    // picture: below normal during loading, the frame arrived 1089 ms after the acquire, the player counted it as late and
    // jumped the film forward (measured 2026-11-21). So: normal priority until a picture exists, below normal after.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (g_hwdec) ffdec_prewarm();
    bool lowered = false;
    std::unique_lock<std::mutex> lk(d.mu);
    const auto born = std::chrono::steady_clock::now();
    for (;;) {
        // Heartbeat: a worker that stops popping while packets are queued is what a stalled film looks like from outside
        // (2026-11-21), and a counter says whether it is asleep in its wait or stuck inside a decode.
        d.cv.wait(lk, [&] {
            // Wider window for the first ten seconds of a film: it starts while the game loads and the player's thread is
            // starved for seconds; when it comes back it must be able to read a burst rather than one picture per 40 ms
            // decode, which it can never catch up with (2026-11-21).
            const bool fresh = std::chrono::steady_clock::now() - born < std::chrono::seconds(10);
            const size_t ahead = fresh ? kAheadStartup : kAhead;
            if (d.stop || d.pending.empty()) return d.stop;
            // The end-of-stream marker is metadata, not a picture: decoding it marks the last picture already waiting. Holding it
            // behind the preload deadlocked the film - the client waited for the mark before reading, and the decoder waited for
            // the client to read before reaching the mark (measured 2026-11-22: the marker only appeared at teardown). So a
            // marker at the head of the queue is always decoded.
            if (d.pending.front().eos) return true;
            return d.ready.size() < ahead;
        });
        if (d.stop) return;
        {
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 400 && (k < 8 || (k % 50) == 0))
                NXLOG_I("vcodec", "worker %u: popped, pending %zu held %zu pend %zu fifo %zu", k, d.pending.size(), d.ready.size(),
                        d.pend.size(), d.fifo.size());
        }
        lk.unlock();
        decode_one(d);
        lk.lock();
        if (!lowered && !d.ready.empty()) {
            lowered = true;   // the first picture exists: from here on the worker stays out of the player's way
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
        // Pictures go into the buffers waiting in "pend" at once: the client polls those sync handles, so a picture that
        // only moved on the next guest call would look like a stalled film.
        assign(d);
    }
}
Decoder::~Decoder() {
    {
        std::lock_guard<std::mutex> lk(mu);
        stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    if (nv) nvdec_destroy(nv);
    if (ff) ffdec_destroy(ff);
}
// The body shared by the two Submit*DecoderInputBuffer shapes: SDK 3.0.1's AV1 one has no size_t* out parameter, the VP8 / VP9 one
// has it first.
void submit_packet(::Ctx* c, uint64_t dec, const uint8_t* data, size_t size, int64_t pts, bool eos) {
    film_trace("submit", size, static_cast<unsigned long long>(pts), eos ? 1 : 0);
    auto d = dec_of(dec);
    if (!d) { RET(0); return; }
    if (d->t_first_packet.time_since_epoch().count() == 0) d->t_first_packet = std::chrono::steady_clock::now();
    s_packets++;
    bool fences = false;
    {
        std::unique_lock<std::mutex> lk(d->mu);
        // Never block: this runs on the player's own thread (the library submits from it), and waiting here froze the player
        // (2026-11-18: one frame read, green picture, no VideoFrame after that). The queue holds the film's packets instead.
        Pending p;
        if (data && size) p.data.assign(data, data + size);
        p.pts = pts;
        p.eos = eos;
        // 12 pictures waiting for a released buffer is the SDK library's ceiling; past it Submit answers 3603 and the client
        // waits for the submission-ready event (dm/host/hle_codec_video.c, CV_HOLD).
        // Also refuse while the queue of undecoded packets is full: the client submits the whole film in one burst, and
        // accepting every packet piles up tens of thousands of them, so the end-of-stream packet arrives long after the
        // client sent it and the picture stops a few seconds in (2026-11-21). The SDK library refuses, and the client
        // resubmits the same packet after the submission-ready event.
        // Accept what the client submits. The upstream audit of the reference emulator (#472) settled this the hard way: the
        // eight-packet admission limit was reverted because the client ignores 3603 and moves on instead of retrying (606
        // rejected packets, no retry, in one session), and 8 is only the watermark at which the ready event goes down. Ours
        // refused, the client dropped the rest of the stream, and the film ran out at frame 61 with five packets left
        // (measured 2026-11-21). The ceiling below only bounds memory: a 90 s 4K film is about 9 MB.
        if (!eos && d->pending.size() >= kQueueCap) {
            d->want_submit_signal = true;
            g_signals_owed.fetch_add(1, std::memory_order_relaxed);
            lk.unlock();
            RET(kResultBusy);
            return;
        }
        // One end-of-stream marker, however often the client resubmits it. It resubmits until the call is accepted, and with
        // Accept taking everything the queue gained 1,121,948 copies in one session - 32,000 a second - which buried the real
        // packets, filled memory and left the worker nothing to do but process markers while the picture stood still
        // (measured 2026-11-22). Accept the call, queue nothing more.
        if (eos && d->eos_queued) {
            d->want_submit_signal = true;
            g_signals_owed.fetch_add(1, std::memory_order_relaxed);
            lk.unlock();
            RET(0);
            return;
        }
        if (eos) d->eos_queued = true;
        d->pending.push_back(std::move(p));
        // Never decode here: the adapter thread submits the whole movie ahead, and a decode per submit (23 ms) left it no time to
        // service the player (the acquire-to-read gap was 48-69 ms with the copies at 1.1-1.7 ms, 2026-11-18). The worker decodes
        // ahead instead; this only wakes it.
        d->cv.notify_all();
        assign(*d);
        d->want_submit_signal = true;
        g_signals_owed.fetch_add(1, std::memory_order_relaxed);
    }
    static std::atomic<uint32_t> first_submits{0};
    if (g_log || first_submits.fetch_add(1) < 3)
        NXLOG_I("vcodec", "decoder %u: submit %zu bytes pts %lld%s (%llu pictures so far)", d->id, size, (long long)pts, eos ? " eos" : "",
                (unsigned long long)d->pictures);
    after_change(c, *d, fences);
    RET(0);
}
void submit(::Ctx* c) {   // (u64* out, dec, data, size, pts, eos): VP8 / VP9
    if (A(0)) wr64(A(0), 0);
    submit_packet(c, A(1), reinterpret_cast<const uint8_t*>(A(2)), A(3), static_cast<int64_t>(A(4)), (A(5) & 1) != 0);
}
void submit_av1(::Ctx* c) {   // (dec, data, size, pts, eos): SDK 3.0.1
    submit_packet(c, A(0), reinterpret_cast<const uint8_t*>(A(1)), A(2), static_cast<int64_t>(A(3)), (A(4) & 1) != 0);
}
void acquire_output(::Ctx* c) {   // (u32* index, NativeSyncHandle* fence, dec)
    film_trace("acquire", 0, 0, 0);
    auto d = dec_of(A(2));
    uint32_t index = 0;
    uint64_t seq = 0;
    bool fences = false;
    if (d) {
        std::unique_lock<std::mutex> lk(d->mu);
        if (d->fifo.empty()) {
            // The SDK's library answers Result 3604 when it has no buffer to hand out; the client polls its sync handles and
            // comes back. Returning success here made the player spin (11 acquisitions in 0.3 ms, 2026-11-21).
            const size_t n_pend = d->pend.size(), n_held = d->ready.size(), n_pending = d->pending.size();
            static std::atomic<uint32_t> nq{0};
            const uint32_t qk = nq.fetch_add(1, std::memory_order_relaxed);
            lk.unlock();
            if (qk < 24)
                NXLOG_I("vcodec", "acquire -> 3604 (no buffer): pend %zu fifo 0 held %zu pending %zu | the client has to come back later",
                        n_pend, n_held, n_pending);
            if (A(0)) wr32(A(0), 0);
            if (A(1)) { wr64(A(1), 0); wr64(A(1) + 8, 0); }
            RET(kResultNoBuffer);
            return;
        } else {
            index = d->fifo.front();
            d->fifo.pop_front();
            seq = (d->slot[index].gen << 32) | index;
            // The library arms its fence right after this call and waits for it, so a fence nothing completes stalls the
            // player for as long as the picture takes - measured 381 ms from acquire to the first read on the runs of main that
            // failed, against every read under 40 ms on the runs that played (2026-11-21). Decode one packet here, outside the
            // lock, so the picture exists before the client arms anything. main has this first-frame fallback; the protocol
            // rewrite dropped it, and without it the player stalls on runs where the other acquire path gets through.
            for (int tries = 0; d->ready.empty() && !d->pending.empty() && !d->stop && tries < 8; ++tries) {
                lk.unlock();
                decode_one(*d);
                lk.lock();
            }
            // Hand the buffer out as it is: the client waits on its sync handle and polls the properties, exactly as the SDK's
            // own library expects. It becomes readable once the decoder writes a picture into it (assign fills the slot).
            if (d->slot[index].filled && !d->ready.empty()) assign(*d);
            fences = assign(*d);
        }
    }
    g_last_acquire_ns.store(now_ns(), std::memory_order_relaxed);
    if (A(0)) wr32(A(0), index);
    if (A(1)) {
        wr64(A(1), d && seq ? (kSyncMarker | d->id) : 0);
        wr64(A(1) + 8, seq);
    }
    if (d) {
        // Always, first 16: the film's first second decides whether it plays, and this is what says why (2026-11-21).
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 16)
            NXLOG_I("vcodec", "decoder %u: acquire -> index %u gen %llu (held %zu, pending %zu, pend %zu, fifo %zu, pool %u)",
                    d->id, index, static_cast<unsigned long long>(seq >> 32), d->ready.size(), d->pending.size(), d->pend.size(),
                    d->fifo.size(), [&] { auto it = d->pools.find(index); Pool* p = it == d->pools.end() ? nullptr : pool_of(it->second); return p ? p->count() : 0u; }());
    }
    if (d) after_change(c, *d, fences);
    RET(0);
}
void release_output(::Ctx* c) {   // (dec, index, pre-fence)
    film_trace("release", A(1), 0, 0);
    if (auto d = dec_of(A(0))) {
        size_t n_pend = 0, n_fifo = 0;
        uint64_t gen = 0;
        bool had_eos = false;   // the client is giving back the end-of-stream output without acting on it
        {
            std::lock_guard<std::mutex> lk(d->mu);
            const uint32_t i = static_cast<uint32_t>(A(1));
            if (i < d->slot.size() && d->slot[i].reg) {
                Decoder::Slot& s = d->slot[i];
                had_eos = s.eos;
                s.gen++;                 // the next picture written here signals this generation
                s.filled = false;
                if (d->pend.size() < 32) d->pend.push_back(i);
                if (d->fifo.size() < 8) d->fifo.push_back(i);
                gen = s.gen;
            }
            n_pend = d->pend.size();
            n_fifo = d->fifo.size();
        }
        // Unconditional, first 32: a client that reads but never releases looks exactly like one that stopped reading, and the
        // difference is whether the film's buffer ever comes back to us (2026-11-21).
        {
            static std::atomic<uint32_t> n{0};
            const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 32)
                NXLOG_I("vcodec", "release #%u: decoder %u index %u gen %llu -> pend %zu fifo %zu", k, d->id,
                        static_cast<uint32_t>(A(1)), static_cast<unsigned long long>(gen), n_pend, n_fifo);
        }
        // If this is the end-of-stream output coming back, the client did not treat it as the end: say so with its caller, and
        // write down where the player's threads are. Without the stack this reads as "the film ended and the player ignored
        // it", which says nothing about what the player is waiting for (measured on a 4060 Laptop, 2026-11-22: the second
        // film's end was delivered and released, then the player sent message 1 nine times and stopped).
        if (had_eos) {
            static std::atomic<uint32_t> n_unhandled{0};
            NXLOG_W("vcodec", "decoder %u: the client released the end-of-stream buffer %u without acting on it, lr 0x%llx", d->id,
                    static_cast<uint32_t>(A(1)), (unsigned long long)c->x[30]);
            if (n_unhandled.fetch_add(1, std::memory_order_relaxed) < 4) {
                media_dump_state("eos-unhandled");
                const std::string dump = nxrt::diag_thread_dump();
                std::ofstream f("eos_unhandled.txt", std::ios::binary | std::ios::trunc);
                if (f) {
                    f << "end of stream released unhandled by decoder " << d->id << ", lr 0x" << std::hex
                      << static_cast<unsigned long long>(c->x[30]) << std::dec << "\n" << dump;
                }
                NXLOG_W("vcodec", "eos_unhandled.txt written (%zu bytes)", dump.size());
            }
        }
    }
    RET(0);
}
int64_t g_pts_shift_ns = 0;      // [media] movie_pts_shift_ms: see the install note
bool g_pts_wallclock = false;   // [media] movie_pts_wallclock: report the frame time as our own elapsed time (diagnostic)
void props_write(uint64_t pw, uint64_t ph, uint64_t pa, uint64_t pb, uint64_t ppts, uint64_t peos, const BufInfo& b) {
    if (pw) wr16(pw, b.w);
    if (ph) wr16(ph, b.h);
    if (pa) wr16(pa, b.w);
    if (pb) wr16(pb, b.h);
    if (ppts) {
        if (g_pts_wallclock) {
            // Diagnostic: the movie's own timestamps run on the movie clock while the player's clock runs on real time, and a 4K AV1
            // decode plus the library's fill put every frame far behind it (it dropped all but the first). Handing out our elapsed
            // time instead keeps the player's "late" check quiet, so the picture should move at whatever rate the pipeline reaches.
            static std::atomic<int64_t> start_ns{0};
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t s0 = start_ns.load(std::memory_order_relaxed);
            if (!s0 && b.pts > 0) { start_ns.compare_exchange_strong(s0, now); s0 = start_ns.load(); }
            wr64(ppts, static_cast<uint64_t>(s0 ? now - s0 : 0));
        } else {
            wr64(ppts, static_cast<uint64_t>(b.pts > 0 ? b.pts + g_pts_shift_ns : b.pts));
        }
    }
    if (peos) *reinterpret_cast<uint8_t*>(peos) = b.eos ? 1 : 0;
    if (g_log) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 30)
            NXLOG_I("vcodec", "output properties: %ux%u, pts %lld, eos %d", b.w, b.h, static_cast<long long>(b.pts), b.eos ? 1 : 0);
    }
}
// Result 3604 until the buffer holds a picture, as the SDK's library answers (dm/host/hle_codec_video.c, cv_dec_props).
bool props_of(uint64_t dec, uint32_t index, BufInfo* out) {
    auto d = dec_of(dec);
    if (!d || index >= d->slot.size()) return false;
    std::lock_guard<std::mutex> lk(d->mu);
    const Decoder::Slot& s = d->slot[index];
    if (!s.filled) return false;
    *out = BufInfo{s.w, s.h, s.ts, s.eos};
    return true;
}
void props(::Ctx* c) {   // (u16*, u16*, u16*, u16*, TimeSpan*, bool*, dec, index)
    film_trace("props", A(7), 0, 0);
    BufInfo b;
    if (!props_of(A(6), static_cast<uint32_t>(A(7)), &b)) { RET(kResultNoBuffer); return; }
    // Diagnostics for the hand-back: what the client asks about, what it is told, and from where. Its caller (the guest lr)
    // is the function that decides whether the film has ended, and it can be read in the lift.
    {
        static std::atomic<uint32_t> np{0};
        const uint32_t k = np.fetch_add(1, std::memory_order_relaxed);
        if (b.eos || k < 12)
            NXLOG_I("vcodec", "props #%u index %llu: %ux%u pts %lld eos %d, lr 0x%llx", k, (unsigned long long)A(7), b.w, b.h,
                    static_cast<long long>(b.pts), b.eos ? 1 : 0, (unsigned long long)c->x[30]);
    }
    props_write(A(0), A(1), A(2), A(3), A(4), A(5), b);
    RET(0);
}
void props_cs(::Ctx* c) {   // MKW: (u16*, u16*, u16*, u16*, ColorSpace*, TimeSpan*, bool*, dec, [sp] index)
    if (A(4)) wr32(A(4), 0);
    BufInfo b;
    if (!props_of(A(7), static_cast<uint32_t>(rd64(c->x[31])), &b)) { RET(kResultNoBuffer); return; }
    props_write(A(0), A(1), A(2), A(3), A(5), A(6), b);
    RET(0);
}
void unsupported_codec(::Ctx* c) {
    static bool once = false;
    if (!once) {
        once = true;
        NXLOG_E("vcodec", "%s: H.264 / AV1 hardware decoding is not implemented on the host", c->in_import ? c->in_import : "nn::codec");
    }
    RET(0x1a6f);   // a stand-in non-zero nn::Result (codec module)
}

// ---- nn::syncpt::sdk / nn::gs ---------------------------------------------------------------------------------------------------
void sp_create(::Ctx* c) {
    const uint64_t evt = A(0);
    std::memset(reinterpret_cast<void*>(evt), 0, 0x60);
    gcall(c, os().init_event, evt, 0, kManualClear);
    wr64(evt + 0x60, kEvtMarker);
    RET(0);
}
void sp_destroy(::Ctx* c) {
    const uint64_t evt = A(0);
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);
        g_armed.erase(evt);
    }
    if (rd64(evt + 0x60) == kEvtMarker) {
        gcall(c, os().finalize_event, evt);
        wr64(evt + 0x60, 0);
    }
    RET(0);
}
void sp_cancel(::Ctx* c) {
    const uint64_t evt = A(0);
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);
        g_armed.erase(evt);
    }
    gcall(c, os().clear_event, evt);
    RET(0);
}
void sp_request(::Ctx* c) {   // (evt, const SyncPointHandle&)
    const uint64_t evt = A(0), h = A(1);
    const uint64_t w0 = h ? rd64(h) : 0, seq = h ? rd64(h + 8) : 0;
    gcall(c, os().clear_event, evt);
    bool reached;
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);   // held across the check and the arm (see take_reached's caller)
        reached = fence_done(w0, seq);
        if (!reached) g_armed[evt] = Armed{static_cast<uint32_t>(w0), seq};
        else g_armed.erase(evt);
        {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 20)
                NXLOG_I("vcodec", "sp_request: evt %llx waits on %llx/%llx (%s)", (unsigned long long)evt, (unsigned long long)w0,
                        (unsigned long long)seq, reached ? "already reached" : "armed");
        }
    }
    if (reached) gcall(c, os().signal_event, evt);
    RET(0);
}
void sp_holder(::Ctx* c) {   // (MultiWaitHolderType*, evt)
    gcall(c, os().init_holder_event, A(0), A(1));
    RET(0);
}
void sp_trywait(::Ctx* c) { RET(gcall(c, os().trywait_event, A(0)) & 1); }
void gs_trywait(::Ctx* c) {   // (const NativeSyncHandle&) -> bool
    const uint64_t h = A(0);
    RET(h ? fence_done(rd64(h), rd64(h + 8)) : 1);
}

// ---- nn::movie extractor trace ([media] movie_trace) ------------------------------------------------------------------------------
// Pokopia's movie::Extractor (subsdk0) runs the SDK's nn::movie::MediaExtractor in media source mode: its ExtractorSourceModeThread
// reads the file through the game's ClientStreamReader (8 KiB at a time) and appends it with AppendMediaSourceData; the extractor
// (nandroid MatroskaExtractor for WebM) parses from that. movie::Extractor::SetDataSource then checks the track count and the
// "container-mime" of track 0 against the ContainerType (3 = WebM) and answers 0x4000 when they disagree, which the game's player
// treats as "cannot open". These wrappers log what the SDK answered on that path (rate-limited) and then return its answer unchanged.
uint64_t call_original(::Ctx* c, uint64_t fn) {
    const uint64_t lr = c->x[30], f = c->fn;
    dispatch_call(c, fn);
    c->x[30] = lr;
    c->fn = f;
    return c->x[0];
}
struct Traced {
    const char* name;
    std::atomic<uint64_t> orig{0};
    std::atomic<uint32_t> calls{0};
};
Traced t_init{"_ZN2nn5movie14MediaExtractor10InitializeEPvmm"};
Traced t_msm{"_ZN2nn5movie14MediaExtractor20SetUpMediaSourceModeEv"};
Traced t_size{"_ZN2nn5movie6detail18ExtractorUnwrapper19StreamSourceSetSizeEl"};
Traced t_append{"_ZN2nn5movie14MediaExtractor21AppendMediaSourceDataEPKvm"};
Traced t_seekpos{"_ZN2nn5movie14MediaExtractor30GetMediaSourceModeSeekPositionEPl"};
Traced t_count{"_ZN2nn5movie14MediaExtractor13GetTrackCountEPi"};
Traced t_props{"_ZN2nn5movie14MediaExtractor18GetTrackPropertiesEPNS0_13MediaPropertyEi"};
Traced t_getstr{"_ZNK2nn5movie13MediaProperty9GetStringEPciPKc"};
std::atomic<uint64_t> t_appended{0};

// Calls the SDK's own function; the count of calls so far comes back in *n.
uint64_t traced(::Ctx* c, Traced& t, uint32_t* n) {
    uint64_t fn = t.orig.load(std::memory_order_relaxed);
    if (!fn) {
        fn = module_find_symbol(t.name);   // the defining module's address (the override only redirects the imports)
        t.orig = fn;
    }
    *n = t.calls.fetch_add(1) + 1;
    if (!fn) { RET(0); return 0; }
    return call_original(c, fn);
}
void tr_init(::Ctx* c) {
    const uint64_t cache = A(3);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_init, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::Initialize(cache %llu) -> 0x%x", (unsigned long long)cache, r);
}
void tr_msm(::Ctx* c) {
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_msm, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::SetUpMediaSourceMode -> 0x%x", r);
}
void tr_size(::Ctx* c) {
    const int64_t size = static_cast<int64_t>(A(1));
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_size, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "StreamSourceSetSize(%lld) -> 0x%x", (long long)size, r);
}
void tr_append(::Ctx* c) {
    const uint64_t data = A(1), size = A(2);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_append, &n));
    const uint64_t total = t_appended.fetch_add(size) + size;
    static std::atomic<uint32_t> logged{0};
    if ((n <= 3 || size == 0 || r) && logged.fetch_add(1) < 24)
        NXLOG_I("movie.trace", "AppendMediaSourceData #%u: %llu bytes%s (%llu in all) -> 0x%x", n, (unsigned long long)size,
                data ? "" : " (no data: end of stream)", (unsigned long long)total, r);
}
void tr_seekpos(::Ctx* c) {
    const uint64_t out = A(1);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_seekpos, &n));
    if (n <= 16 || r) NXLOG_I("movie.trace", "GetMediaSourceModeSeekPosition -> %lld, 0x%x", out ? (long long)rd64(out) : -1ll, r);
}
void tr_count(::Ctx* c) {
    const uint64_t out = A(1);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_count, &n));
    int32_t k = -1;
    if (out) std::memcpy(&k, reinterpret_cast<const void*>(out), 4);
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::GetTrackCount -> %d tracks, 0x%x", k, r);
}
void tr_props(::Ctx* c) {
    const int32_t track = static_cast<int32_t>(A(2));
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_props, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::GetTrackProperties(track %d) -> 0x%x", track, r);
}
void tr_getstr(::Ctx* c) {   // (const MediaProperty*, char* out, int size, const char* key) -> bool
    const uint64_t out = A(1), key = A(3);
    uint32_t n;
    const uint64_t r = traced(c, t_getstr, &n) & 1;
    const char* k = key ? reinterpret_cast<const char*>(key) : "";
    static std::atomic<uint32_t> logged{0};
    if (std::strstr(k, "mime") && logged.fetch_add(1) < 16)
        NXLOG_I("movie.trace", "MediaProperty::GetString(\"%s\") -> %s \"%.40s\"", k, r ? "found" : "missing",
                r && out ? reinterpret_cast<const char*>(out) : "");
}
struct TraceImport {
    Traced* t;
    HleFn fn;
};
const TraceImport kTraceImports[] = {
    {&t_init, tr_init}, {&t_msm, tr_msm}, {&t_size, tr_size}, {&t_append, tr_append},
    {&t_seekpos, tr_seekpos}, {&t_count, tr_count}, {&t_props, tr_props}, {&t_getstr, tr_getstr},
};

struct Import {
    const char* name;
    HleFn fn;
};
const Import kImports[] = {
    {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", make_pref},
    // SDK 3.0.1 (Xenoblade 2's movie library): the fourth argument is the image tiling mode, and InitializeDecoderImagePool has no
    // trailing codec mode. Same handlers: only the picture size and the output format are read back.
    {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", make_pref},
    {"_ZN2nn5codec3sdk34MakeDecodeImagePreferenceForCompatEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", make_pref},
    {"_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", calc_pool},
    {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", init_pool},
    {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", init_pool},
    {"_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", fini_pool},
    {"_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", read_nv12},
    {"_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", read_rgba},
    {"_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", read_rgba_hdr},
    {"_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", native_image_handle},
    // VP9
    {"_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", calc_work},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_tm},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_rm},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_rm2},
    {"_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE", submission_event},
    {"_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", props},
    {"_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", props_cs},
    // VP8 (same decoder on libvpx's VP8 interface)
    {"_ZN2nn5codec29CalculateVp8DecoderWorkBufferEPmS1_mtt", calc_work},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_tm},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_rm},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_rm2},
    {"_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp8DecoderTypeE", submission_event},
    {"_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj", props},
    {"_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj", props_cs},
    // SDK 3.0.1 (Xenoblade 2's subsdk0): Initialize*DecoderByTransferMemory / ByRemoteMemory with a VideoDecoderOutputMode, and a
    // Get*DecoderSubmissionReadyEvent that returns the event instead of writing it through an out pointer. VP8 / VP9 keep the
    // size_t* out parameter of Submit*DecoderInputBuffer; AV1's was dropped.
    {"_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp9_tm},
    {"_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp9_rm},
    {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE", submission_event_ret},
    {"_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp8_tm},
    {"_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp8_rm},
    {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE", submission_event_ret},
    // AV1 (dav1d): the codec Xenoblade 2's Switch 2 movies use
    {"_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_av1_tm},
    {"_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_av1_rm},
    {"_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE", submission_event_ret},
    {"_ZN2nn5codec27SubmitAv1DecoderInputBufferEPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb", submit_av1},
    // What this SDK actually exports (subsdk0 import 275): the size_t* out parameter is back, so it is the same shape as VP8 / VP9.
    // Without this line the import bound to the SDK's own Submit (its [type+0] is an IPC object there, not our decoder id) and the
    // first submitted packet read address 0x1 (xc2_boot_crash 2026-11-18 20:42).
    {"_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj", props},
    // H.264: not decoded here (a clear error instead of the console driver path)
    {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported_codec},
    {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported_codec},
    {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported_codec},
    {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported_codec},
    // fences
    {"_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", sp_create},
    {"_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", sp_destroy},
    {"_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", sp_cancel},
    {"_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", sp_request},
    {"_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", sp_holder},
    {"_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", sp_trywait},
    {"_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", gs_trywait},
};

}  // namespace

void install_player_trace();   // movie_trace.cpp ([media] player_trace)

bool install_video_codec_hle() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (game::info().platform != game::Platform::Switch2) return;
        const bool mkw = std::strcmp(game::info().id, "mkw") == 0;
        const std::string mode = config_get_str("media", "video_decoder", mkw ? "off" : "library");
        if (mode == "off") {
            NXLOG_I("vcodec", "[media] video_decoder = off: nn::codec video decoders are not on the host (movies are left out)");
            return;
        }
        g_log = config_get_bool("media", "video_decoder_log", false);
        // 4 was the default and is enough for the 1080p films; the retail build's 4K AV1 openings want more, and above 8 a
    // single 3840x2160 frame stops splitting usefully. Half the cores, at least 4, at most 8.
    const int64_t def_threads = std::min<int64_t>(8, std::max<int64_t>(4, static_cast<int64_t>(std::thread::hardware_concurrency()) / 2));
    g_threads = static_cast<int>(std::clamp<int64_t>(config_get_int("media", "video_decoder_threads", def_threads), 1, 16));
    g_hwdec = config_get_bool("media", "hwdec", true);
    // ffdec_prewarm() is deferred to worker_loop when the first movie decoder is created: running av_hwdevice_ctx_create(VULKAN)
    // on a detached thread at t=0.001s raced the main thread's gfx::Device::create() inside amdvlk64.dll on RX 9070 XT (#42).
        // Diagnostic: the player drops a frame whose timestamp is more than a frame or so behind its own movie clock, and a software 4K
        // AV1 decode plus the pool->VideoFrame copies put every frame ~85 ms behind, so it dropped all but the first. Reporting the
        // timestamps this much later makes them look on time (at the cost of that much A/V offset).
        // Default 100 ms: the XAudio2 output queue plus the device buffer, the amount the video has to wait to stay in lip sync.
        // Measured 110-120 ms on the author's machine (1440p window); 100 is the safe default, the launcher can override it.
        // The XAudio2 sink queues 33-40 ms plus the device buffer, so a film that follows its own timestamps runs ahead of its
    // sound; this delays the picture instead of the audio. 110 ms was the value that lined the 4K CG up on the maintainer's
    // device at a 1440p window (2026-11-18/19: 50 too early, 120-180 the sound arrives first). Per-device, so it stays a key.
    // 0 by default since 2026-11-21: the film's audio track is a placeholder (one byte a packet, the sound comes from the game's
    // own stream), so compensating an XAudio2 queue that never carries it only tells the player every picture is 110 ms late -
    // and at the start of the Torna opening film that lost the race often enough to freeze the picture or leave the overlay empty.
    g_pts_shift_ns = config_get_int("media", "movie_pts_shift_ms", 0) * 1000000;
        g_pts_wallclock = config_get_bool("media", "movie_pts_wallclock", false);
        install_player_trace();   // [media] player_trace: the player's message queue and timer, logged as they are called
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        g_enabled = true;
#if NXRT_HAVE_LIBVPX && NXRT_HAVE_DAV1D
        NXLOG_I("vcodec", "nn::codec VP9/VP8 (libvpx %s) and AV1 (dav1d %s) decoders on the host (%d threads): %zu imports routed",
                vpx_codec_version_str(), dav1d_version(), g_threads, sizeof kImports / sizeof kImports[0]);
#elif NXRT_HAVE_DAV1D
        NXLOG_I("vcodec", "nn::codec AV1 decoder on the host (dav1d %s, %d threads; built without libvpx): %zu imports routed",
                dav1d_version(), g_threads, sizeof kImports / sizeof kImports[0]);
#else
        NXLOG_I("vcodec", "nn::codec VP9/VP8 decoders on the host (libvpx %s, %d threads): %zu imports routed", vpx_codec_version_str(), g_threads,
                sizeof kImports / sizeof kImports[0]);
#endif
        if (config_get_bool("media", "movie_trace", true)) {
            for (const TraceImport& ti : kTraceImports) {
                hle_register(ti.t->name, ti.fn);
                module_override_symbol(ti.t->name);
            }
            NXLOG_I("vcodec", "[media] movie_trace: %zu nn::movie extractor calls logged (rate-limited)", sizeof kTraceImports / sizeof kTraceImports[0]);
        }
    });
    return g_enabled;
}

VideoCodecStats video_codec_stats() {
    VideoCodecStats s;
    s.decoders_created = s_created;
    s.decoders_destroyed = s_destroyed;
    s.packets_submitted = s_packets;
    s.frames_decoded = s_decoded;
    s.frames_delivered = s_delivered;
    s.frames_read = s_read;
    s.decode_errors = s_errors;
    return s;
}

#else   // built without libvpx

bool install_video_codec_hle() {
    static bool logged = false;
    if (!logged && game::info().platform == game::Platform::Switch2) {
        logged = true;
        NXLOG_W("vcodec", "built without libvpx (NXRT_HAVE_LIBVPX): movies are left out");
    }
    return false;
}
VideoCodecStats video_codec_stats() { return {}; }
namespace {
std::atomic<bool> g_enabled{false};
}

#endif

bool video_codec_enabled() { return g_enabled; }

}  // namespace nxrt::media
