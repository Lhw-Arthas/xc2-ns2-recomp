// SPDX-License-Identifier: GPL-3.0-or-later
// movie::AudioDecoder (the Switch 2 movie library's audio decoder) on the host.
//
// main imports the whole AudioDecoder surface as well as VideoDecoder's. Before this file existed the library ran its own
// implementation, which drives nn::codec and NVDEC - the path this port cannot follow: the player waited for audio frames that
// never came and its picture stopped with them, exactly when the movie's audio stream opened (2026-11-18, 4K CG, 1.3 s in).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <windows.h>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/memory.h"
#include "nxrt/movie_vdec.h"
#include "nxrt/services.h"

#if NXRT_HAVE_VORBIS
#include <vorbis/codec.h>
#endif

namespace nxrt::media {

int64_t media_film_video_pts();   // video_codec.cpp: the time of the last picture handed to the client

namespace {

#define A(i) (c->x[i])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr const char* kTag = "movie";
constexpr uint64_t kMarker = 0x4e58524341444331ull;
// movie::Status_ErrorNoFrame is 12 (0x0c), not 0x7b: verified in main (0x710071b1dc, cmp w8, #12) and
// subsdk0 (0x760001ad88). Returning 0x7b caused the player to fall through with frame == nullptr and crash.
constexpr uint32_t kResultNoFrame = 0x0c;
constexpr uint64_t kBufData = 0x00, kBufSize = 0x08, kBufFilled = 0x10, kBufTime = 0x18, kBufFlags = 0x20;
constexpr uint64_t kAfBuffer = 0x00, kAfChannels = 0x14;   // AudioFrame: buffer, then the channel count

bool g_log = false;
// [media] movie_audio: decode the film's own audio track on the host. ON by default (see install_audio_decoder_hle): every
// packet this library hands the decoder arrives one byte long (measured 2026-11-21: filled 1, payload 0x0a/0x0e, cadence a
// correct 21.3 ms), so it decodes to silence, but the player paces itself on these frames - with the decoder off the opening
// film froze on its first picture for about five seconds on both machines (handover 65, section 249).
bool g_audio = true;
struct Fns {
    uint64_t create_event, attach_event, get_int32, set_int32, set_size;
};

const Fns& fns() {
    static const Fns f = [] {
        Fns o{};
        o.create_event = module_find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        o.attach_event = module_find_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
        o.get_int32 = module_find_symbol("_ZNK2nn5movie13MediaProperty8GetInt32EPiPKc");
        o.set_int32 = module_find_symbol("_ZN2nn5movie13MediaProperty8SetInt32EPKci");
        o.set_size = module_find_symbol("_ZN2nn5movie13MediaProperty7SetSizeEPKcm");
        return o;
    }();
    return f;
}

uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!fn) return 0;
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    c->x[30] = 0;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// One decoded chunk of sound waiting for a frame: PCM16 interleaved, and the packet time it belongs to.
struct Sound {
    std::vector<int16_t> pcm;
    int64_t time = 0;
};

struct AudioDecoder {
    std::mutex mu;
    std::vector<uint64_t> frames, free_;
    std::deque<uint64_t> ready;          // frames holding sound, not acquired yet
    std::deque<Sound> sound;             // decoded chunks waiting for a free frame
    std::vector<uint8_t> config;         // the track's codec configuration (Xiph-laced Vorbis headers)
    uint64_t packets = 0, decoded = 0;
    int mime = 0, fmt = 0, mode = 0, channels = 2, rate = 48000;
    // The film's audio track is a placeholder the player uses as a clock (see deliver_sound). It must still report an end:
    // the player's finish condition covers every track (movie::detail::ExtractorAdapterImpl::AllTracksEndOfStream, and
    // AudioFrame::GetEndOfStreamState). Mark the last frame it hands out; 0x10 is the byte the SDK's own
    // movie::AudioFrame::SetEndOfStreamState writes (subsdk0+0x1b2c0, one store).
    bool eos_pending = false;   // the stream ended; the next frame delivered carries the flag
    bool eos_sent = false;
    bool eos_acquired = false;  // the client actually popped an AudioFrame carrying the end flag
    bool finalized = false;
    bool gated = false;         // held back by the audio-ahead-of-picture gate; resume when video_pts advances
    bool in_paused = false;     // input-ready event held low while sound backlog exceeds kSoundHighWater
    uint64_t last_delivered = 0;   // the frame handed out last: the one that carries the flag when the film ends
    uint64_t last_acquired = 0;    // the frame handed to the client most recently
    bool warned_packet = false;
    services::ServiceEvent in_ready, out_ready;
    alignas(16) uint8_t in_ev[0x100]{}, out_ev[0x100]{};
#if NXRT_HAVE_VORBIS
    vorbis_info vi{};
    vorbis_comment vc{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool vorbis_open = false;
#endif
};

std::mutex g_reg_mu;
std::vector<AudioDecoder*> g_reg;
std::atomic<bool> g_any_gated{false};

// One line a second while the film's audio is in use: the queues that decide whether the player still gets its rhythm.
// Ungated on purpose - [media] video_decoder_log changes the timing this is meant to measure (2026-11-21).
std::atomic<uint64_t> g_adec_acquire{0}, g_adec_miss{0}, g_adec_release{0}, g_adec_packets{0};
void adec_state_tick(const AudioDecoder* d) {
    static std::atomic<int64_t> last{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last.load(std::memory_order_relaxed) < 1000) return;
    last.store(now, std::memory_order_relaxed);
    NXLOG_I(kTag, "adec state: packets %llu decoded %llu sound %zu ready %zu free %zu frames %zu | acquire %llu miss %llu release %llu",
            static_cast<unsigned long long>(d->packets), static_cast<unsigned long long>(d->decoded), d->sound.size(),
            d->ready.size(), d->free_.size(), d->frames.size(),
            static_cast<unsigned long long>(g_adec_acquire.load()), static_cast<unsigned long long>(g_adec_miss.load()),
            static_cast<unsigned long long>(g_adec_release.load()));
}

std::mutex g_map_mu;
std::map<uint64_t, AudioDecoder*> g_map;   // guest object -> host state; the object itself is left untouched

AudioDecoder* adec_of(uint64_t self) {
    if (!self) return nullptr;
    std::lock_guard<std::mutex> lk(g_map_mu);
    auto it = g_map.find(self);
    return it == g_map.end() ? nullptr : it->second;
}
void attach_event(::Ctx* c, uint8_t* guest_ev, services::ServiceEvent& host) {
    if (!fns().attach_event || !fns().create_event) return;
    gcall(c, fns().create_event, reinterpret_cast<uint64_t>(guest_ev), 1);
    const uint32_t h = host.new_handle();
    gcall(c, fns().attach_event, reinterpret_cast<uint64_t>(guest_ev), h, 1, 1);
}

void signal_output_ready(AudioDecoder& d) {
    d.out_ready.signal();
    kernel::signal_system_event(reinterpret_cast<uint64_t>(d.out_ev));
}

void adec_ctor(::Ctx* c) {
    if (A(0)) { wr64(A(0), 0); wr64(A(0) + 8, 0); }
}

void adec_finalize(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    if (d) {
        {
            std::lock_guard<std::mutex> g(g_reg_mu);
            g_reg.erase(std::remove(g_reg.begin(), g_reg.end(), d), g_reg.end());
        }
        bool wake_in = false, wake_out = false;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            d->finalized = true;
            wake_in = d->in_paused;
            wake_out = d->gated || !d->ready.empty() || !d->sound.empty();
            d->frames.clear();
            d->free_.clear();
            d->ready.clear();
            d->sound.clear();
            d->eos_pending = false;
            d->in_paused = false;
            d->gated = false;
        }
        if (wake_in) d->in_ready.signal();
        if (wake_out) signal_output_ready(*d);
    }
    RET(0);
}

void adec_initialize(::Ctx* c) {
    auto* d = new AudioDecoder();
    // Initialize(void* workspace, size_t size, MediaMimeType mime, AudioDecoderOutputFormat fmt, AudioCodecMode mode)
    d->mime = static_cast<int>(A(3));
    d->fmt = static_cast<int>(A(4));
    d->mode = static_cast<int>(A(5));
    attach_event(c, d->in_ev, d->in_ready);
    attach_event(c, d->out_ev, d->out_ready);
    {
        std::lock_guard<std::mutex> lk(g_map_mu);
        g_map[A(0)] = d;
    }
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        g_reg.push_back(d);
    }
    NXLOG_I(kTag, "AudioDecoder: MIME %d, output format %d, mode %d", d->mime, d->fmt, d->mode);
    RET(0);
}

void adec_configure(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    if (!d) { RET(0); return; }
    const uint64_t cfg = A(2);
    const size_t n = static_cast<size_t>(A(3));
    {
        std::lock_guard<std::mutex> lk(d->mu);
        d->config.assign(reinterpret_cast<const uint8_t*>(cfg), reinterpret_cast<const uint8_t*>(cfg) + (cfg && n ? n : 0));
    }
    NXLOG_I(kTag, "AudioDecoder: configured with %llu bytes of codec configuration", static_cast<unsigned long long>(n));
    RET(0);
}

// The library's own decoder fills this property and the player sizes its AudioFrames from it; the names it reads are the
// library's, so every plausible one is written - a missing key reads back as 0, and a player that then allocates nothing asserts
// (measured 2026-11-18: the crash followed Configure by microseconds, before the first frame was registered).
void adec_requirements(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    const uint64_t prop = A(1);
    NXLOG_I(kTag, "AudioDecoder: GetAudioFrameRequirements(prop %llx, set_int32 %llx)", static_cast<unsigned long long>(prop),
            static_cast<unsigned long long>(fns().set_int32));
    if (d && prop && fns().set_int32) {
        const auto set = [&](const char* key, uint64_t value) { gcall(c, fns().set_int32, prop, reinterpret_cast<uint64_t>(key), value); };
        set("channels", 2);
        set("channel-count", 2);
        set("sample-rate", 48000);
        set("sampling-rate", 48000);
        set("output-buf-size", 48000 / 60 * 2 * 2);
        set("input-buf-size", 0x40000);
        if (fns().set_size) {
            gcall(c, fns().set_size, prop, reinterpret_cast<uint64_t>("output-buf-size"), 48000 / 60 * 2 * 2);
            gcall(c, fns().set_size, prop, reinterpret_cast<uint64_t>("input-buf-size"), 0x40000);
        }
    }
    RET(0);
}
void adec_register_frames(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    const uint64_t list = A(1);
    const int n = static_cast<int>(A(2));
    if (!d || !list || n <= 0) { RET(0); return; }
    std::lock_guard<std::mutex> lk(d->mu);
    d->frames.clear();
    d->free_.clear();
    d->ready.clear();
    for (int i = 0; i < n; ++i) d->frames.push_back(rd64(list + 8ull * i));
    for (int i = n - 1; i >= 0; --i) d->free_.push_back(d->frames[i]);
    if (d->frames.empty()) { RET(0); return; }
    const uint64_t f = d->frames.front();
    char line[400];
    int used = 0;
    for (int row = 0; row < 4; ++row) {
        used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), "+%02x:", row * 16);
        for (int i = 0; i < 16; ++i)
            used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " %02x", rd8(f + static_cast<uint64_t>(row * 16 + i)));
        used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " |");
    }
    NXLOG_I(kTag, "AudioDecoder: %d frames registered, first at %llx: %s", n, static_cast<unsigned long long>(f), line);
    RET(0);
}

// NXRT_FILM_TRACE=1: the first 120 movie::AudioDecoder calls in order, the same trace the video side has. The film's audio
// track is a one-byte-a-packet placeholder and its sound comes from the game's own stream, but the player still paces itself on
// this decoder's frames, so where its handshake stops is worth seeing (2026-11-21).
static bool atrace_on() {
    static const bool on = std::getenv("NXRT_FILM_TRACE") != nullptr;
    return on;
}
static void atrace(const char* what, unsigned long long a = 0, unsigned long long b = 0) {
    if (!atrace_on()) return;
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 120) NXLOG_I("movie", "acall %u: %s(%llu, %llu)", k, what, a, b);
}

void adec_input_event(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    if (A(1)) wr64(A(1), d ? reinterpret_cast<uint64_t>(d->in_ev) : 0);
    NXLOG_I(kTag, "AudioDecoder: input-ready event asked for");
    RET(0);
}

void adec_output_event(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    if (A(1)) wr64(A(1), d ? reinterpret_cast<uint64_t>(d->out_ev) : 0);
    NXLOG_I(kTag, "AudioDecoder: output-ready event asked for");
    RET(0);
}

// Start makes the decoder ready for input: the player waits on the input-ready event before it submits a packet, so nothing
// moves until it is signalled (the video decoder signals it here for the same reason).
#if NXRT_HAVE_VORBIS
// The configuration of a Vorbis track is its three headers in Xiph lacing (one count byte, then the sizes of all but the last).
bool open_vorbis(AudioDecoder& d) {
    if (d.vorbis_open || d.config.empty()) return d.vorbis_open;
    if (g_log) {
        char line[200];
        int used = 0;
        for (int i = 0; i < 48 && i < static_cast<int>(d.config.size()); ++i)
            used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " %02x", d.config[static_cast<size_t>(i)]);
        NXLOG_I(kTag, "AudioDecoder: %zu config bytes,%s", d.config.size(), line);
    }
    std::vector<std::pair<const uint8_t*, size_t>> parts;
    size_t i = 0;
    if (d.config.size() > 7 && d.config[0] == 1 && std::memcmp(d.config.data() + 1, "vorbis", 6) == 0) {
        // The whole extradata: the three headers one after another, the identification header first (measured 2026-11-18: 3862
        // bytes starting 01 "vorbis"). Split them the way the format defines them rather than guessing a lacing.
        const size_t id_len = 30;   // packet type, "vorbis", version, channels, rate, bitrates, block sizes, framing
        size_t p = id_len;
        if (d.config.size() > p + 7 && d.config[p] == 5) {
            // No comment header: this container keeps only the identification and setup headers (measured 2026-11-18: byte 30 is
            // 05, the setup header's type). Vorbis needs a comment header to reach the setup one, so an empty one is made here.
            static const uint8_t kEmptyComment[16] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
            parts.push_back({d.config.data(), id_len});
            parts.push_back({kEmptyComment, sizeof kEmptyComment});
            parts.push_back({d.config.data() + p, d.config.size() - p});
            if (g_log) NXLOG_I(kTag, "AudioDecoder: no comment header in the configuration; an empty one stands in");
        } else if (d.config.size() < p + 7 + 5 || d.config[p] != 3) {
            NXLOG_W(kTag, "AudioDecoder: no comment header where the 30-byte identification header ends (byte %zu is %02x)", p, d.config[p]);
            return false;
        } else {
        size_t q = p + 7;
        auto le32 = [&](size_t at) {
            return static_cast<uint32_t>(d.config[at]) | (static_cast<uint32_t>(d.config[at + 1]) << 8) |
                   (static_cast<uint32_t>(d.config[at + 2]) << 16) | (static_cast<uint32_t>(d.config[at + 3]) << 24);
        };
        if (q + 4 > d.config.size()) return false;
        const uint32_t vlen = le32(q);
        q += 4 + vlen;
        if (q + 4 > d.config.size()) return false;
        const uint32_t ncomments = le32(q);
        q += 4;
        for (uint32_t i = 0; i < ncomments; ++i) {
            if (q + 4 > d.config.size()) return false;
            const uint32_t clen = le32(q);
            q += 4 + clen;
            if (q > d.config.size()) return false;
        }
        ++q;   // framing bit
        if (q >= d.config.size()) return false;
            parts.push_back({d.config.data(), id_len});
            parts.push_back({d.config.data() + p, q - p});
            parts.push_back({d.config.data() + q, d.config.size() - q});
        }
    } else if (d.config[0] == 2) {   // three headers: count - 1 in the first byte
        i = 1;
        int sizes[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            int v = 0, b;
            do {
                if (i >= d.config.size()) return false;
                b = d.config[i++];
                v += b;
            } while (b == 255);
            sizes[k] = v;
        }
        size_t off = i;
        parts.push_back({d.config.data() + off, static_cast<size_t>(sizes[0])});
        off += static_cast<size_t>(sizes[0]);
        parts.push_back({d.config.data() + off, static_cast<size_t>(sizes[1])});
        off += static_cast<size_t>(sizes[1]);
        parts.push_back({d.config.data() + off, d.config.size() - off});
    } else {
        parts.push_back({d.config.data(), d.config.size()});   // a single header, as an older container writes it
    }
    vorbis_info_init(&d.vi);
    vorbis_comment_init(&d.vc);
    int ok = 1;
    for (size_t k = 0; k < parts.size() && ok; ++k) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char*>(parts[k].first);
        op.bytes = static_cast<long>(parts[k].second);
        op.b_o_s = k == 0 ? 1 : 0;
        op.packetno = static_cast<ogg_int64_t>(k);
        ok = vorbis_synthesis_headerin(&d.vi, &d.vc, &op) == 0;
        if (g_log) NXLOG_I(kTag, "AudioDecoder: header %zu/%zu (%zu bytes) -> %s", k + 1, parts.size(), parts[k].second, ok ? "ok" : "rejected");
    }
    if (!ok) {
        NXLOG_W(kTag, "AudioDecoder: the codec configuration is not Vorbis headers (%zu bytes)", d.config.size());
        vorbis_comment_clear(&d.vc);
        vorbis_info_clear(&d.vi);
        return false;
    }
    const int si = vorbis_synthesis_init(&d.vd, &d.vi), bi = si ? 1 : vorbis_block_init(&d.vd, &d.vb);
    if (si || bi) {
        NXLOG_W(kTag, "AudioDecoder: vorbis synthesis did not start (synthesis_init %d, block_init %d; %d channels at %ld Hz, %zu config bytes, first %02x %02x %02x %02x)",
                si, bi, d.vi.channels, static_cast<long>(d.vi.rate), d.config.size(), d.config[0], d.config[1], d.config[2], d.config[3]);
        return false;
    }
    d.vorbis_open = true;
    d.channels = d.vi.channels;
    d.rate = static_cast<int>(d.vi.rate);
    NXLOG_I(kTag, "AudioDecoder: Vorbis %d channels at %d Hz", d.channels, d.rate);
    return true;
}
#endif

void adec_start(::Ctx* c) {
    atrace("start");
    AudioDecoder* d = adec_of(A(0));
    if (d) {
#if NXRT_HAVE_VORBIS
        if (g_audio && !open_vorbis(*d)) NXLOG_W(kTag, "AudioDecoder: no Vorbis decoder; its sound stays silent");
        if (!g_audio) NXLOG_I(kTag, "AudioDecoder: the host decoder is off ([media] movie_audio = 0): no frames are delivered");
#endif
        d->in_ready.signal();
    }
    NXLOG_I(kTag, "AudioDecoder: Start");
    RET(0);
}
void adec_stop(::Ctx* c) { RET(0); }

void adec_flush(::Ctx* c) {
    AudioDecoder* d = adec_of(A(0));
    bool resume_in = false;
    if (d) {
        std::lock_guard<std::mutex> lk(d->mu);
        d->ready.clear();
        d->free_.clear();
        d->sound.clear();
        d->eos_pending = false;
        d->eos_sent = false;
        d->eos_acquired = false;
        d->last_delivered = 0;
        d->last_acquired = 0;
        resume_in = d->in_paused;
        d->in_paused = false;
        d->gated = false;
        for (size_t i = d->frames.size(); i > 0; --i) d->free_.push_back(d->frames[i - 1]);
    }
    if (resume_in) d->in_ready.signal();
    RET(0);
}
// Moves decoded sound into free frames (caller holds the lock): PCM into the nn::movie::Buffer the AudioFrame carries, then the
// output-ready event. A chunk larger than the frame's buffer is cut, which the player hears as a click but never as a stall.
void deliver_sound(AudioDecoder& d) {
    bool any = false;
    uint64_t last_frame = 0;
    // The film's audio track is a placeholder the player uses as a clock, not sound anyone hears (the game plays its own).
    // It must not run ahead of the picture: while the loading screen starves the picture thread for seconds, an audio clock
    // that kept going made the player skip the picture forward to catch up with it - "stalls 3-5 s, then jumps into the
    // middle" (measured 2026-11-21).
    d.gated = false;
    const int64_t video_pts = media_film_video_pts();
    // Allow 1.0 s lead (above the ~0.55 s AudioOut startup pre-roll) so startup buffering never trips the gate
    // on frame 0/1 while still preventing multi-second runaway jumps during loading stalls.
    constexpr int64_t kAudioGateAheadNs = 1'000'000'000ll;
    while (!d.sound.empty() && !d.free_.empty()) {
        if (d.sound.front().time > video_pts + kAudioGateAheadNs) {
            d.gated = true;
            g_any_gated.store(true, std::memory_order_release);
            // Say it out loud when the gate bites: it is the one place where the film's clock is deliberately held back, and a
            // gate that stays closed stops the player from being handed sound at all - which stops it feeding the picture too
            // (2026-11-22).
            static std::atomic<int64_t> last_ms{0};
            const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t prev = last_ms.load(std::memory_order_relaxed);
            if (ms - prev >= 1000 && last_ms.compare_exchange_strong(prev, ms))
                NXLOG_W("movie", "audio gate: next sound at %.3f s, picture at %.3f s (%.3f s ahead), %zu sound(s) held",
                        d.sound.front().time / 1e9, video_pts / 1e9, (d.sound.front().time - video_pts) / 1e9, d.sound.size());
            break;
        }
        const uint64_t f = d.free_.back();
        d.free_.pop_back();
        last_frame = f;
        d.last_delivered = f;
        wr8(f + 0x10, 0);   // a normal frame: the end flag is set on the last one only (see below)
        Sound& s = d.sound.front();
        const uint64_t buf = rd64(f + kAfBuffer);
        uint64_t wrote = 0;
        if (buf) {
            const uint64_t dst = rd64(buf + kBufData), cap = rd64(buf + kBufSize);
            const uint64_t bytes = static_cast<uint64_t>(s.pcm.size()) * 2;
            wrote = bytes <= cap ? bytes : cap;
            if (dst && wrote) std::memcpy(reinterpret_cast<void*>(dst), s.pcm.data(), static_cast<size_t>(wrote));
            wr64(buf + kBufFilled, wrote);
            wr64(buf + kBufTime, static_cast<uint64_t>(s.time));
        }
        d.ready.push_back(f);
        d.sound.pop_front();
        any = true;
    }
    // The track ended - its own zero-byte marker, or the picture's end for this placeholder (movie_adec_film_end). The last
    // frame handed out carries the flag the player's end check reads; without it the player treats the film as still running
    // and the scene gives up on it (~35 s later, measured 2026-11-22).
    if (d.eos_pending && d.sound.empty() && last_frame) {
        wr8(last_frame + 0x10, 1);
        d.eos_pending = false;
        d.eos_sent = true;
        any = true;
        NXLOG_I(kTag, "AudioDecoder: end of stream marked on the last audio frame");
    }
    if (any) signal_output_ready(d);
    constexpr size_t kSoundLowWater = 32;
    if (d.in_paused && d.sound.size() <= kSoundLowWater) {
        d.in_paused = false;
        d.in_ready.signal();
    }
}

void adec_decode_async(::Ctx* c) {
    atrace("decode", A(1) ? 1 : 0, 0);
    AudioDecoder* d = adec_of(A(0));
    const uint64_t buf = A(1);
    if (!d || !buf) { RET(0); return; }
    if (g_log) {
        static std::once_flag once;
        std::call_once(once, [&] {
            char line[400];
            int used = 0;
            for (int row = 0; row < 4; ++row) {
                used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), "+%02x:", row * 16);
                for (int i = 0; i < 16; ++i)
                    used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " %02x", rd8(buf + static_cast<uint64_t>(row * 16 + i)));
                used += std::snprintf(line + used, sizeof line - static_cast<size_t>(used), " |");
            }
            NXLOG_I(kTag, "AudioDecoder: first Buffer at %llx: %s", static_cast<unsigned long long>(buf), line);
        });
    }
    const uint64_t bytes = rd64(buf + kBufFilled), time = rd64(buf + kBufTime);
    // A packet with nothing in it is the end of the track, not a packet: feeding it as "the whole capacity" (what the
    // length fallback below does) turns the marker into 256 KB of garbage. The demuxer sends this once, at the end.
    if (bytes == 0) {
        bool need_out_signal = false;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            d->eos_pending = true;
            // All sound chunks may already sit in d->ready when the zero-byte EOS marker arrives.
            if (d->sound.empty() && !d->ready.empty() && !d->eos_sent) {
                wr8(d->ready.back() + 0x10, 1);
                d->eos_pending = false;
                d->eos_sent = true;
                need_out_signal = true;
                NXLOG_I(kTag, "AudioDecoder: end of stream marked on queued audio frame");
            }
        }
        d->in_ready.signal();
        if (need_out_signal) signal_output_ready(*d);
        NXLOG_I(kTag, "AudioDecoder: end-of-stream packet after %llu packets", static_cast<unsigned long long>(d->packets));
        RET(0);
        return;
    }
    const uint64_t data = rd64(buf + kBufData);
    const uint64_t cap = rd64(buf + kBufSize);
    ++d->packets;
    // One-shot diagnostic (2026-11-21): the film's audio is silent and every packet reports one byte. Dump the packet
    // header, the payload's first bytes and where its non-zero run ends, so the length field can be identified from a run
    // instead of guessed.
    if (g_log && d->packets <= 6) {
        char head[3 * 16 + 1] = {};
        int used = 0;
        uint64_t nz = 0;
        if (data && nxrt::mem::is_mapped(data, 4096)) {
            const uint8_t* q = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(data));
            for (uint64_t i = 0; i < 4096; ++i)
                if (q[i]) nz = i + 1;
            for (int i = 0; i < 16; ++i) used += std::snprintf(head + used, sizeof head - static_cast<size_t>(used), " %02x", q[i]);
        } else {
            std::snprintf(head, sizeof head, " (not mapped)");
        }
        NXLOG_I(kTag, "AudioDecoder: packet %llu from caller 0x%llx", static_cast<unsigned long long>(d->packets),
                static_cast<unsigned long long>(c->x[30]));
        NXLOG_I(kTag, "AudioDecoder: packet %llu: data %llx filled %llu cap %llu time %lld flags %llu, last non-zero %llu, head:%s",
                static_cast<unsigned long long>(d->packets), static_cast<unsigned long long>(data),
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(cap), static_cast<long long>(time),
                static_cast<unsigned long long>(rd64(buf + kBufFlags)), static_cast<unsigned long long>(nz), head);
    }
    constexpr size_t kSoundHighWater = 64;   // ~1.36 s of Vorbis chunks: enough to keep audio gate fed without decoding the whole file up front
    bool signal_in = true;
#if NXRT_HAVE_VORBIS
    // What the player calls the packet's length differs between the paths this library takes; take the field that looks like a
    // packet (a Vorbis packet is a few hundred bytes; the capacity is 0x40000).
    size_t n = static_cast<size_t>(bytes);
    if (n == 0 || n > cap) n = static_cast<size_t>(cap);
    if (g_log && !d->warned_packet) {
        d->warned_packet = true;
        NXLOG_I(kTag, "AudioDecoder: packet fields: data %llx, +0x08 %llu, +0x10 %llu, +0x18 %lld, +0x20 %llu", static_cast<unsigned long long>(data),
                static_cast<unsigned long long>(cap), static_cast<unsigned long long>(bytes), static_cast<long long>(time),
                static_cast<unsigned long long>(rd64(buf + kBufFlags)));
    }
    if (d->vorbis_open && data && n) {
        ogg_packet op{};
        g_adec_packets.fetch_add(1, std::memory_order_relaxed);
        op.packet = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(data));
        op.bytes = static_cast<long>(n);
        op.granulepos = -1;
        op.packetno = static_cast<ogg_int64_t>(d->packets);
        std::lock_guard<std::mutex> lk(d->mu);
        if (vorbis_synthesis(&d->vb, &op) == 0) {
            vorbis_synthesis_blockin(&d->vd, &d->vb);
            float** pcm = nullptr;
            int got = 0;
            while ((got = vorbis_synthesis_pcmout(&d->vd, &pcm)) > 0) {
                Sound s;
                s.time = static_cast<int64_t>(time);
                s.pcm.resize(static_cast<size_t>(got) * static_cast<size_t>(d->channels));
                for (int f = 0; f < got; ++f)
                    for (int chn = 0; chn < d->channels; ++chn) {
                        float v = pcm[chn][f];
                        v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                        s.pcm[static_cast<size_t>(f) * static_cast<size_t>(d->channels) + static_cast<size_t>(chn)] = static_cast<int16_t>(v * 32767.0f);
                    }
                vorbis_synthesis_read(&d->vd, got);
                d->sound.push_back(std::move(s));
                ++d->decoded;
            }
        }
        deliver_sound(*d);
        signal_in = d->sound.size() < kSoundHighWater;
        d->in_paused = !signal_in;
    }
#endif
    if (signal_in) d->in_ready.signal();   // room for the next packet unless backpressured
    if (d) adec_state_tick(d);
    if (g_log) {
        static std::atomic<uint64_t> n{0};
        const uint64_t k = n.fetch_add(1);
        if (k < 20 || k % 200 == 0)
            NXLOG_I(kTag, "AudioDecoder: DecodeAsync %llu bytes at %lld from %llx (call %llu, frames %zu)", static_cast<unsigned long long>(bytes),
                    static_cast<long long>(time), static_cast<unsigned long long>(data), static_cast<unsigned long long>(k), d->frames.size());
    }
    RET(0);
}

void adec_acquire(::Ctx* c) {
    atrace("acquire");
    AudioDecoder* d = adec_of(A(0));
    if (A(1)) wr64(A(1), 0);
    if (A(2)) wr32(A(2), 0xffffffffu);
    if (!d) { RET(kResultNoFrame); return; }
    uint64_t f = 0;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        g_adec_acquire.fetch_add(1, std::memory_order_relaxed);
        adec_state_tick(d);
        if (d->ready.empty()) deliver_sound(*d);   // a frame may have come back since; the backlog goes into it
        if (d->ready.empty()) {
            const uint64_t m = g_adec_miss.fetch_add(1, std::memory_order_relaxed);
            d->out_ready.clear();
            if (m < 5 || (g_log && (m < 20 || m % 200 == 0)))
                NXLOG_W(kTag, "AudioDecoder: AcquireAudioFrame found nothing (miss %llu): packets %llu decoded %llu sound %zu free %zu frames %zu",
                        static_cast<unsigned long long>(m), static_cast<unsigned long long>(d->packets),
                        static_cast<unsigned long long>(d->decoded), d->sound.size(), d->free_.size(), d->frames.size());
            RET(kResultNoFrame);
            return;
        }
        f = d->ready.front();
        d->ready.pop_front();
        d->last_acquired = f;
        if (rd8(f + 0x10) != 0) d->eos_acquired = true;
        // out_ev is AutoClear (binary), while AudioHandler counts one wakeup per AcquireAudioFrame call:
        // burst deliveries coalesce signals, so leaving >0 frames in d->ready without re-signaling strands
        // trailing frames (including the EOS-marked frame if audio ended before video).
        if (!d->ready.empty()) signal_output_ready(*d);
    }
    if (A(1)) wr64(A(1), f);
    RET(0);
}

void adec_release(::Ctx* c) {
    atrace("release");
    AudioDecoder* d = adec_of(A(0));
    const uint64_t f = A(1);
    if (d && f) {
        g_adec_release.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(d->mu);
        if (std::find(d->frames.begin(), d->frames.end(), f) != d->frames.end()) {
            d->free_.push_back(f);
            // The client returns a frame so the next chunk of sound can go into it. Without this the ring only ever refills
            // inside DecodeAsync - and every packet of this track is submitted in the first second, so once they stop the
            // ring drains to zero ready frames and its output-ready event never fires again: the player waits for audio that
            // is sitting in "sound" and the picture stops with it (2026-11-21, exactly the stall of handover 65 section 249).
            deliver_sound(*d);
        }
        adec_state_tick(d);
    }
    RET(0);
}
struct Import { const char* name; HleFn fn; };
const Import kImports[] = {
    // The constructor and destructor stay the library's: they set up the object's own fields, and a replaced no-op left them
    // uninitialised, which the player then tripped over (measured 2026-11-18: a guest break at 12 s, before any movie).
    {"_ZN5movie12AudioDecoder8FinalizeEv", adec_finalize},
    {"_ZN5movie12AudioDecoder10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24AudioDecoderOutputFormatENS_14AudioCodecModeE", adec_initialize},
    {"_ZN5movie12AudioDecoder9ConfigureEPKN2nn5movie13MediaPropertyEPKvm", adec_configure},
    {"_ZN5movie12AudioDecoder25GetAudioFrameRequirementsEPN2nn5movie13MediaPropertyE", adec_requirements},
    {"_ZN5movie12AudioDecoder18RegisterAudioFrameEPPNS_10AudioFrameEi", adec_register_frames},
    {"_ZN5movie12AudioDecoder18GetInputReadyEventEPPN2nn2os15SystemEventTypeE", adec_input_event},
    {"_ZN5movie12AudioDecoder19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE", adec_output_event},
    {"_ZN5movie12AudioDecoder5StartEv", adec_start},
    {"_ZN5movie12AudioDecoder4StopEv", adec_stop},
    {"_ZN5movie12AudioDecoder5FlushEv", adec_flush},
    {"_ZN5movie12AudioDecoder11DecodeAsyncEPKN2nn5movie6BufferE", adec_decode_async},
    {"_ZN5movie12AudioDecoder17AcquireAudioFrameEPPNS_10AudioFrameEPi", adec_acquire},
    {"_ZN5movie12AudioDecoder17ReleaseAudioFrameEPNS_10AudioFrameE", adec_release},
};

}  // namespace

bool install_audio_decoder_hle() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        const game::Info& gi = game::info();
        if (gi.platform != game::Platform::Switch2 || !gi.movie_video_decoder) return;
        g_log = config_get_bool("media", "video_decoder_log", false);
        // On by default since 2026-11-21: this track is a placeholder (one byte a packet, the sound comes from the game's own
    // stream), but the player paces itself on the decoder's frames. With it off the opening film froze on its first picture
    // for about five seconds on both machines, then caught up; with it on the film advances and lip sync holds.
    g_audio = config_get_bool("media", "movie_audio", true);
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        ok = true;
        NXLOG_I(kTag, "movie::AudioDecoder on the host: %zu imports routed", sizeof kImports / sizeof kImports[0]);
    });
    return ok;
}

// The picture stream ended: this placeholder track ends with it. Its only job is the player's clock, and the finish
// condition covers every track, so waiting for an audio end of its own would wait forever (the demuxer's zero-byte marker
// may never come; measured 2026-11-22). Called from video_codec.cpp when the video end-of-stream marker arrives.
void movie_adec_film_end() {
    std::vector<AudioDecoder*> to_signal;
    {
        std::lock_guard<std::mutex> g(g_reg_mu);
        for (AudioDecoder* d : g_reg) {
            std::lock_guard<std::mutex> lk(d->mu);
            if (d->finalized || d->eos_acquired || (d->eos_sent && d->sound.empty())) continue;
            // The picture is over, so the placeholder track is over: ensure the guest acquires a frame carrying the EOS flag
            // rather than waiting on buffered chunks or an already-coalesced output-ready signal (measured 2026-11-23: when
            // audio ended 0.8 s early with 2 frames queued in d->ready, AutoClear coalescing left the last frame stranded).
            const size_t dropped = d->sound.size();
            d->sound.clear();
            if (!d->ready.empty()) {
                wr8(d->ready.back() + 0x10, 1);
                d->eos_pending = false;
                d->eos_sent = true;
                to_signal.push_back(d);
                NXLOG_I(kTag, "AudioDecoder: end of stream signaled on queued frame at the film's end (%zu chunk(s) dropped)", dropped);
            } else if (!d->free_.empty()) {
                const uint64_t f = d->free_.back();
                d->free_.pop_back();
                const uint64_t buf = rd64(f + kAfBuffer);
                if (buf) {
                    wr64(buf + kBufFilled, 0);
                    wr64(buf + kBufTime, static_cast<uint64_t>(media_film_video_pts()));
                }
                wr8(f + 0x10, 1);
                d->ready.push_back(f);
                d->last_delivered = f;
                d->eos_pending = false;
                d->eos_sent = true;
                to_signal.push_back(d);
                NXLOG_I(kTag, "AudioDecoder: end of stream pushed as empty audio frame at the film's end (%zu chunk(s) dropped)", dropped);
            } else if (const uint64_t target = d->last_acquired ? d->last_acquired : d->last_delivered) {
                wr8(target + 0x10, 1);
                d->eos_pending = false;
                d->eos_sent = true;
                NXLOG_I(kTag, "AudioDecoder: end of stream marked on the last audio frame at the film's end (%zu chunk(s) dropped)", dropped);
            } else {
                d->eos_pending = true;
            }
        }
    }
    for (AudioDecoder* d : to_signal) signal_output_ready(*d);
}

// Wakes any audio decoder whose delivery stopped at the video-PTS gate as soon as a new picture lands.
void movie_adec_video_advanced() {
    if (!g_any_gated.exchange(false, std::memory_order_acq_rel)) return;
    std::lock_guard<std::mutex> g(g_reg_mu);
    bool still_gated = false;
    for (AudioDecoder* d : g_reg) {
        std::lock_guard<std::mutex> lk(d->mu);
        if (!d->finalized && d->gated) deliver_sound(*d);
        still_gated = still_gated || (!d->finalized && d->gated);
    }
    if (still_gated) g_any_gated.store(true, std::memory_order_release);
}

}  // namespace nxrt::media
