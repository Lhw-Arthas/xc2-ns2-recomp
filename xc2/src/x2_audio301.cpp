// SPDX-License-Identifier: GPL-3.0-or-later
// x2_audio301.cpp - SDK 21's nn::audio OutputStream on the runtime's own audio sink (HANDOVER 90).
//
// The retail 3.0.1 build does not use audren:u for its sound. It takes nn::audio's OutputStream API, which on the console goes to
// the auddmg:u device manager; our host has no such service, and the SDK then read a null device list
// (nn::audio::devmgr::ListVirtualDeviceNamesImpl, HANDOVER 90). nxnative solves this by replacing the same nn::audio entry points
// with host ones (dm/host/hle_audio.c, "SDK 21's OutputStream"); here each stream is written straight into the runtime's audio
// sink (nxrt/audio_sink.h, the one audren uses: XAudio2 or the null sink), so the game's 48 kHz float frames are played as 16-bit
// interleaved blocks.
#include <windows.h>
#include <chrono>
#include <thread>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/audio_sink.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

namespace {
constexpr const char* kTag = "audio301";
constexpr int kRate = 48000;
constexpr int kFmtPcm16 = 2, kFmtFloat = 5;
constexpr int kMaxQueuedBlocks = 8;

struct Stream {
    int ch = 2, fmt = kFmtPcm16, cap = 0, state = 0;   // state: 0 not open, 1 open (stopped), 2 started
    uint64_t event = 0;
    uint32_t last_queued = 0;
    std::vector<int16_t> pcm;
};
std::mutex g_mu;
std::unordered_map<uint64_t, Stream*> g_streams;

// Calls a guest function with the first n registers set to args and returns its x0; the caller's registers are put back.
uint64_t call(Ctx* c, uint64_t fn, int n, const uint64_t* args) {
    if (!fn) return 0;
    const Ctx saved = *c;
    for (int i = 0; i < n; ++i) c->x[i] = args[i];
    c->x[30] = 0;
    nxrt::dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

Stream* of(uint64_t g) {
    std::lock_guard lk(g_mu);
    auto it = g_streams.find(g);
    return it == g_streams.end() ? nullptr : it->second;
}

void default_device(Ctx* c) {   // (AudioDeviceName*)
    const uint64_t name = c->x[0];
    if (name && nxrt::mem::is_mapped(name, 0x100)) {
        std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(name)), 0, 0x100);
        std::strncpy(reinterpret_cast<char*>(static_cast<uintptr_t>(name)), "AudioVirtualOutput", 0xff);
    }
    NXLOG_W(kTag, "GetDefaultVirtualOutputDevice -> AudioVirtualOutput");
    c->x[0] = 0;
}

// The stream's system event is what the game waits on for "a buffer was played": on the console nn::audio signals it as the
// device consumes. Our sink does the consuming, so a watcher signals the event when the queued count drops - without it a
// caller that waits (the movie player does) blocks for ever, which froze the 4K CG at its first frame (measured 2026-11-18).
void signal_event(uint64_t ev) {
    if (!ev || !nxrt::mem::is_mapped(ev, 4)) return;
    // The SDK's SystemEvent parks its waiters on a futex inside the type, so the handle alone is not enough.
    nxrt::kernel::signal_system_event(ev);
}

void watch_start();

// Reads a StreamFormat {format, channels, sample_rate} and opens the stream.
bool open_stream(Ctx* c, uint64_t g, uint64_t fmt_p, int cap, uint64_t event) {
    int fmt = kFmtPcm16, ch = 2, rate = kRate;
    if (fmt_p && nxrt::mem::is_mapped(fmt_p, 12)) {
        const int32_t* f = reinterpret_cast<const int32_t*>(static_cast<uintptr_t>(fmt_p));
        fmt = f[0];
        ch = f[1];
        rate = f[2];
    }
    NXLOG_W(kTag, "OpenOutputStream(g 0x%llx, fmt %d, ch %d, rate %d, cap %d, event 0x%llx)", static_cast<unsigned long long>(g), fmt, ch,
            rate, cap, static_cast<unsigned long long>(event));
    if (fmt != kFmtFloat && fmt != kFmtPcm16) return false;
    if (rate && rate != kRate) return false;
    if (ch <= 0 || cap <= 0) return false;
    Stream* s = new Stream();
    s->ch = ch;
    s->fmt = fmt;
    s->cap = cap;
    s->state = 1;
    s->event = event;
    {
        std::lock_guard lk(g_mu);
        g_streams[g] = s;
    }
    if (auto sink = nxrt::audio::sink()) sink->open(static_cast<uint32_t>(rate ? rate : kRate), static_cast<uint32_t>(ch));
    watch_start();
    // The console's OpenOutputStream creates the stream's system event in the storage the caller passed (the game then waits on it
    // for a buffer to be released). Replacing the function means nobody creates it, and the audio thread's first
    // nn::os::TimedWaitSystemEvent on an uninitialised event asserts (HANDOVER 90), so create it here - auto-clear, as the
    // release event of a stream is.
    if (event) {
        const uint64_t create = nxrt::nxmodule_find_export("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        uint64_t a[3] = {event, 1 /* EventClearMode_AutoClear */, 0};
        const uint64_t r = call(c, create, 3, a);
        NXLOG_W(kTag, "stream event 0x%llx created (0x%llx)", static_cast<unsigned long long>(event), static_cast<unsigned long long>(r));
    }
    c->x[0] = 0;
    return true;
}

void open_with_event(Ctx* c) {   // (g, event, buf, size, name, format, cap, ?, flags)
    open_stream(c, c->x[0], c->x[5], static_cast<int>(c->x[6]), c->x[1]);
}
void open_no_event(Ctx* c) {     // (g, buf, size, name, format, cap, flags)
    open_stream(c, c->x[0], c->x[4], static_cast<int>(c->x[5]), 0);
}

void close_stream(Ctx* c) {
    Stream* s = nullptr;
    {
        std::lock_guard lk(g_mu);
        auto it = g_streams.find(c->x[0]);
        if (it != g_streams.end()) { s = it->second; g_streams.erase(it); }
    }
    if (s) {
        if (auto sink = nxrt::audio::sink()) sink->close();
        if (s->event) {
            const uint64_t destroy = nxrt::nxmodule_find_export("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
            uint64_t a[1] = {s->event};
            call(c, destroy, 1, a);
        }
        delete s;
    }
    c->x[0] = 0;
}

void start_stream(Ctx* c) {
    Stream* s = of(c->x[0]);
    if (s) s->state = 2;
    c->x[0] = 0;
}
void stop_stream(Ctx* c) {
    Stream* s = of(c->x[0]);
    if (s) s->state = 1;
    c->x[0] = 0;
}
void purge_stream(Ctx* c) {
    c->x[0] = 0;
}
void stream_state(Ctx* c) {
    Stream* s = of(c->x[0]);
    c->x[0] = s ? static_cast<uint64_t>(s->state) : 0;
}
void stream_free(Ctx* c) {
    Stream* s = of(c->x[0]);
    uint32_t queued = 0;
    if (auto sink = nxrt::audio::sink()) queued = sink->queued_frames();
    c->x[0] = s && s->cap > static_cast<int>(queued) ? static_cast<uint64_t>(s->cap - static_cast<int>(queued)) : 0;
}
void set_gain(Ctx* c) { c->x[0] = 0; }
void stats(Ctx* c) {   // (g, b): zero the statistics struct
    const uint64_t out = c->x[0];
    if (out && nxrt::mem::is_mapped(out, 0x40)) std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(out)), 0, 0x40);
}

void write_stream(Ctx* c) {   // (g, void* data, int frames)
    Stream* s = of(c->x[0]);
    const uint64_t data = c->x[1];
    const int frames = static_cast<int>(c->x[2]);
    if (!s || frames <= 0 || !data) { c->x[0] = 0; return; }
    const size_t n = static_cast<size_t>(frames) * static_cast<size_t>(s->ch);
    if (!nxrt::mem::is_mapped(data, s->fmt == kFmtFloat ? n * 4 : n * 2)) { c->x[0] = 0; return; }
    s->pcm.resize(n);
    if (s->fmt == kFmtFloat) {
        const float* in = reinterpret_cast<const float*>(static_cast<uintptr_t>(data));
        for (size_t i = 0; i < n; ++i) {
            const float v = in[i] * 32767.0f;
            s->pcm[i] = static_cast<int16_t>(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
    } else {
        std::memcpy(s->pcm.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(data)), n * 2);
    }
    uint32_t wrote = 0;
    uint32_t queued = 0;
    if (auto sink = nxrt::audio::sink()) {
        wrote = sink->write(s->pcm.data(), static_cast<uint32_t>(frames));
        queued = sink->queued_frames();
    }
    if (wrote > 0) {
        std::lock_guard lk(g_mu);
        if (Stream* cur = g_streams.count(c->x[0]) ? g_streams[c->x[0]] : nullptr) {
            if (queued > cur->last_queued) cur->last_queued = queued;
        }
    }
    c->x[0] = wrote;
}

// Signals every stream's event whenever the sink has played some of what it holds. 5 ms is well under the 40 ms a 1920-frame
// buffer lasts at 48 kHz, so a waiting game wakes before its audio runs dry.
void watch_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        uint32_t queued = 0;
        if (auto sink = nxrt::audio::sink()) queued = sink->queued_frames();
        std::vector<uint64_t> wake;
        {
            std::lock_guard lk(g_mu);
            if (g_streams.empty()) continue;
            for (auto& [g, s] : g_streams) {
                if (!s->event || s->state != 2) continue;
                if (queued > s->last_queued) {
                    s->last_queued = queued;
                } else if (queued < s->last_queued) {
                    s->last_queued = queued;
                    wake.push_back(s->event);
                } else if (queued == 0) {
                    wake.push_back(s->event);   // nothing left: there is room for everything the game writes
                }
            }
        }
        for (uint64_t ev : wake) signal_event(ev);
    }
}

void watch_start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(watch_loop).detach(); });
}
uint64_t sym(const char* name) { return nxrt::nxmodule_find_export(name); }

void force(const char* name, nxrt::HleFn fn) {
    nxrt::hle_register(name, fn);
    nxrt::module_override_symbol(name);
    const uint64_t def = sym(name);
    if (!def) {
        NXLOG_W(kTag, "sdk export %s not found", name);
        return;
    }
    nxrt::fn_register(def, fn);
}
}  // namespace

extern "C" void xc2_audio301_install() {
    force("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", default_device);
    force("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", open_with_event);
    force("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", open_no_event);
    force("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", close_stream);
    force("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", start_stream);
    force("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", stop_stream);
    force("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", purge_stream);
    force("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", stream_state);
    force("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", stream_free);
    force("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", write_stream);
    force("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", set_gain);
    force("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", stats);
    NXLOG_W(kTag, "nn::audio OutputStream -> the runtime's audio sink (XC2_301_AUDIO)");
}