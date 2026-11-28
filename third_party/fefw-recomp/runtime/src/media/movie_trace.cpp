// SPDX-License-Identifier: GPL-3.0-or-later
// [media] player_trace = 1: pass-through logging of the nn::os calls the movie player's frame loop is built from.
//
// The player is four main threads (created at 36.9 s: MoviePlayerThread, MovieEventHandler, AudioHandler, RenderThread):
// RenderThread is the per-frame state machine (main f_7100722460). It blocks in nn::os::ReceiveMessageQueue(player+0x1E0),
// takes a frame when the decoder's output-ready handler posts message 2, and starts the per-frame periodic timer
// (player+0x888, StartPeriodicTimerEvent) when something posts message 8. MovieEventHandler runs a MultiWait loop
// (nn::os::WaitAny, f_7100724c20) with that timer linked into it; the timer's handler clears it and posts message 9, which is
// how the state machine processes and releases a frame.
//
// With the library's own VideoDecoder the codec side is healthy (997 packets decoded, EOS) yet the player reads one picture and
// stops, and no ClearTimerEvent is ever called. These wrappers answer where that chain stops: each import is redirected here and
// then calls the SDK's own function. Message lines are capped per (queue, value) so the audio handler's burst cannot hide the
// video queue's later messages; timer lines come from the player only (the game's own 5 ms timer is in main's data, not the heap).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"

namespace nxrt::media {

bool media_film_active();   // video_codec.cpp: a film's decoder is alive right now
bool media_decoder_has_backlog();   // video_codec.cpp: decoded pictures are waiting in d->ready

namespace {

#define A(i) (c->x[i])

// A small portable thread id for the log lines: the Windows thread id is not included here and the value only has to tell
// threads apart within one run.
inline unsigned long tid16() {
    return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffu);
}

constexpr const char* kTag = "player";

constexpr const char* kSend = "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm";
constexpr const char* kRecv = "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE";
constexpr const char* kStartTimer = "_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_";
constexpr const char* kStopTimer = "_ZN2nn2os14StopTimerEventEPNS0_14TimerEventTypeE";
constexpr const char* kStartOnce = "_ZN2nn2os22StartOneShotTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanE";
constexpr const char* kWaitTimer = "_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE";
constexpr const char* kClearTimer = "_ZN2nn2os15ClearTimerEventEPNS0_14TimerEventTypeE";
constexpr const char* kWaitAny = "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE";
constexpr const char* kHolderTimer = "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_14TimerEventTypeE";
constexpr const char* kHolderEvent = "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE";
constexpr const char* kLink = "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE";
constexpr const char* kGetTs = "_ZN5movie10VideoFrame12GetTimeStampEPN2nn8TimeSpanE";
constexpr const char* kGetBuf = "_ZN5movie10VideoFrame9GetBufferEPPvPm";
constexpr const char* kSleep = "_ZN2nn2os11SleepThreadENS_8TimeSpanE";
constexpr const char* kReleaseFrame = "_ZN5movie12VideoDecoder17ReleaseVideoFrameEPNS_10VideoFrameE";

bool g_on = false;
bool g_poke = false;   // [media] movie_timer_poke: mark a started periodic timer due from the host (diagnostic)
std::mutex g_seen_mu;
std::unordered_map<uint64_t, uint32_t> g_send_seen, g_recv_seen, g_any_seen, g_holder_seen;

// Logs at most `cap` lines per key (a queue and message value, a holder, a timer), so a busy queue cannot use the budget of
// the one that matters.
bool seen(std::unordered_map<uint64_t, uint32_t>& m, uint64_t key, uint32_t cap) {
    std::lock_guard<std::mutex> lk(g_seen_mu);
    uint32_t& n = m[key];
    if (n >= cap) return false;
    ++n;
    return true;
}

// Calls the SDK's own function and puts back the two guest fields the dispatcher keeps (the function entered, the return address).
uint64_t call_original(::Ctx* c, uint64_t fn) {
    const uint64_t lr = c->x[30], f = c->fn;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    c->x[30] = lr;
    c->fn = f;
    return r;
}

bool from_main(uint64_t lr) { return lr >= 0x7100000000ull && lr < 0x7180000000ull; }
// main's own data lives at 0x7100...-0x7102...: its 5 ms timer is there and would otherwise log twice per tick. The player's
// timer events are guest heap objects.
bool heap_address(uint64_t a) { return a >= 0x2c00000000ull && a < 0x4000000000ull; }

uint64_t fn_send() { static const uint64_t f = module_find_symbol(kSend); return f; }
uint64_t fn_recv() { static const uint64_t f = module_find_symbol(kRecv); return f; }
uint64_t fn_start() { static const uint64_t f = module_find_symbol(kStartTimer); return f; }
uint64_t fn_stop() { static const uint64_t f = module_find_symbol(kStopTimer); return f; }
uint64_t fn_wait() { static const uint64_t f = module_find_symbol(kWaitTimer); return f; }
uint64_t fn_clear() { static const uint64_t f = module_find_symbol(kClearTimer); return f; }
uint64_t fn_wait_any() { static const uint64_t f = module_find_symbol(kWaitAny); return f; }
uint64_t fn_holder_timer() { static const uint64_t f = module_find_symbol(kHolderTimer); return f; }
uint64_t fn_link() { static const uint64_t f = module_find_symbol(kLink); return f; }
uint64_t fn_get_ts() { static const uint64_t f = module_find_symbol(kGetTs); return f; }
uint64_t fn_get_buf() { static const uint64_t f = module_find_symbol(kGetBuf); return f; }
uint64_t fn_sleep() { static const uint64_t f = module_find_symbol(kSleep); return f; }
uint64_t fn_release_frame() { static const uint64_t f = module_find_symbol(kReleaseFrame); return f; }
std::atomic<int64_t> g_last_getbuf_ns{0};
std::atomic<int64_t> g_last_movie_frame_ns{0};
std::atomic<int> g_stall_dumps{0};
std::atomic<bool> g_frame_displayed{false};
std::atomic<int> g_consecutive_drops{0};
int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void release_frame(::Ctx* c) {   // (VideoDecoder*, VideoFrame*): how long the player held the frame (pixels -> texture -> back)
    const uint64_t fn = fn_release_frame();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (!g_frame_displayed.exchange(false, std::memory_order_relaxed)) {
        g_consecutive_drops.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_on) {
        const int64_t t = now_ns(), since = t - g_last_getbuf_ns.load(std::memory_order_relaxed);
        if (seen(g_any_seen, 0x4000, 40)) NXLOG_I(kTag, "ReleaseVideoFrame: %.1f ms after GetBuffer", since / 1e6);
    }
    A(0) = r;
}

void send_msg(::Ctx* c) {   // (nn::os::MessageQueueType*, u64 value) -> nn::Result
    const uint64_t q = A(0), v = A(1), lr = c->x[30];
    const uint64_t fn = fn_send();
    if (!fn) { NXLOG_E(kTag, "[media] player_trace: nn::os::SendMessageQueue not found"); A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && seen(g_send_seen, (q << 8) ^ (v & 0xff), 40))
        NXLOG_I(kTag, "msg send: queue 0x%llx <- %llu, lr 0x%llx, -> 0x%llx", q, v, lr, r);
    // During a film every message is logged, unsampled: the sampled lines above kept 40 (queue, value) pairs and hid who
    // stopped talking. The cutscene's player threads wait on each other through these queues, and the last message before the
    // picture stops is the whole question (2026-11-22).
    if (nxrt::media::media_film_active()) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 20000)
            NXLOG_I(kTag, "film msg send: t%lu queue 0x%llx <- %llu, lr 0x%llx", tid16(), q, v, lr);
    }
    A(0) = r;
}
void recv_msg(::Ctx* c) {   // (u64* out, nn::os::MessageQueueType*) -> nn::Result (blocks)
    const uint64_t out = A(0), q = A(1), lr = c->x[30];
    const uint64_t fn = fn_recv();
    if (!fn) { NXLOG_E(kTag, "[media] player_trace: nn::os::ReceiveMessageQueue not found"); A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on) {
        uint64_t v = 0;
        if (out && mem::is_mapped(out, 8)) std::memcpy(&v, reinterpret_cast<const void*>(out), 8);
        if (seen(g_recv_seen, (q << 8) ^ (v & 0xff), 40))
            NXLOG_I(kTag, "msg recv: queue 0x%llx -> %llu, lr 0x%llx, result 0x%llx", q, v, lr, r);
        if (nxrt::media::media_film_active()) {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 20000)
                NXLOG_I(kTag, "film msg recv: t%lu queue 0x%llx -> %llu, lr 0x%llx", tid16(), q, v, lr);
        }
    }
    A(0) = r;
}
// One host ticker for every periodic timer the player starts. The first version started a detached thread per timer that
// gave up the moment the timer's state byte (+0x13) read 0, which a stop/start cycle does transiently: that thread then
// never came back, and the player waited on a timer nobody poked. Measured 2026-11-19: 7.4 s and 8.3 s of frozen picture
// while the game itself kept 54 fps, and the player's own "frame ready" message sat in its queue for the same 7 s. So: one
// thread, a list, and a state byte of 0 only skips this tick - it never unregisters. StopTimerEvent unregisters, and a timer
// that stays stopped for five seconds is dropped so a freed guest object cannot be written through.
struct PokedTimer {
    uint64_t timer;
    int64_t period_ns;
    bool once = false;         // a one-shot timer fires once and leaves the list
    std::chrono::steady_clock::time_point next;
    std::chrono::steady_clock::time_point idle_since;
};

std::mutex g_poke_mu;
std::vector<PokedTimer> g_poked;
std::once_flag g_poke_once;

void poke_ticker() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(g_poke_mu);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_poked.begin(); it != g_poked.end();) {
                if (rd8(it->timer + 19) == 0) {
                    if (it->idle_since == std::chrono::steady_clock::time_point{}) it->idle_since = now;
                    else if (now - it->idle_since > std::chrono::seconds(5)) { it = g_poked.erase(it); continue; }
                } else {
                    it->idle_since = {};
                    if (now >= it->next) {
                        wr8(it->timer + 18, 1);
                        if (it->once) { it = g_poked.erase(it); continue; }   // fired: a one-shot is done
                        it->next = now + std::chrono::nanoseconds(it->period_ns);
                    }
                }
                ++it;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void poke_add(uint64_t timer, int64_t period_ns, bool once = false) {
    std::call_once(g_poke_once, [] { std::thread(poke_ticker).detach(); });
    std::lock_guard<std::mutex> lk(g_poke_mu);
    for (auto& e : g_poked) {
        if (e.timer == timer) { e.period_ns = period_ns; e.once = once; e.idle_since = {}; return; }
    }
    g_poked.push_back(PokedTimer{timer, period_ns, once, std::chrono::steady_clock::now(), {}});
}

void poke_remove(uint64_t timer) {
    std::lock_guard<std::mutex> lk(g_poke_mu);
    for (auto it = g_poked.begin(); it != g_poked.end(); ++it) {
        if (it->timer == timer) { g_poked.erase(it); return; }
    }
}

// nn::os::StartOneShotTimerEvent was neither wrapped nor poked: only the periodic one was. A player that uses a one-shot delay
// for the per-frame step (the short inserts may well be driven that way) then waits on a timer that nothing ever marks due, and
// its state machine sleeps after the first frames - the picture freezes with every decoder counter healthy (2026-11-22).
void start_timer_once(::Ctx* c) {   // (TimerEventType*, TimeSpan delay)
    const uint64_t t = A(0), delay = A(1), lr = c->x[30];
    static const uint64_t f = module_find_symbol(kStartOnce);
    const uint64_t r = f ? call_original(c, f) : 0;
    NXLOG_I(kTag, "timer start one-shot: 0x%llx, after %lld ns, lr 0x%llx, -> 0x%llx", t, (long long)delay, lr, r);
    if (g_poke && heap_address(t) && delay > 0 && delay <= 1000000000) poke_add(t, delay, true);
    A(0) = r;
}
void start_timer(::Ctx* c) {   // (TimerEventType*, TimeSpan first, TimeSpan period)
    const uint64_t t = A(0), i1 = A(1), i2 = A(2), lr = c->x[30];
    const uint64_t fn = fn_start();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && (from_main(lr) || heap_address(t)) && seen(g_holder_seen, t, 20))
        NXLOG_I(kTag, "timer start: 0x%llx, first %lld ns, period %lld ns, lr 0x%llx, -> 0x%llx", t, (long long)i1, (long long)i2, lr, r);
    // The MultiWait's timer holder reads the timer's own signalled byte (+0x12) and its deadline, and in this runtime the deadline
    // comparison never turns it due (no ClearTimerEvent follows the player's StartPeriodicTimerEvent). Mark it due from the host
    // instead, or the player's frame loop simply never wakes: one picture read, then nothing.
    if (g_poke && heap_address(t) && i2 > 0 && i2 <= 1000000000) {
        if (g_on) NXLOG_I(kTag, "timer poke: 0x%llx every %lld ns", t, (long long)i2);
        poke_add(t, i2);
    }
    A(0) = r;
}
void stop_timer(::Ctx* c) {
    const uint64_t t = A(0), lr = c->x[30];
    const uint64_t fn = fn_stop();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    poke_remove(t);
    if (g_on && seen(g_holder_seen, t ^ 1, 20)) NXLOG_I(kTag, "timer stop: 0x%llx, lr 0x%llx", t, lr);
    A(0) = r;
}
void wait_timer(::Ctx* c) {   // (TimerEventType*): blocks until the timer fires
    const uint64_t t = A(0), lr = c->x[30];
    const uint64_t fn = fn_wait();
    if (!fn) return;
    const bool log = g_on && !(t >= 0x7100000000ull && t < 0x7200000000ull) && seen(g_holder_seen, t ^ 2, 20);
    if (log) NXLOG_I(kTag, "timer wait: 0x%llx, lr 0x%llx", t, lr);
    call_original(c, fn);
    if (log) NXLOG_I(kTag, "timer wait: 0x%llx returned", t);
}
void clear_timer(::Ctx* c) {
    const uint64_t t = A(0), lr = c->x[30];
    const uint64_t fn = fn_clear();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && seen(g_holder_seen, t ^ 3, 20)) NXLOG_I(kTag, "timer clear: 0x%llx, lr 0x%llx", t, lr);
    A(0) = r;
}
void holder_timer(::Ctx* c) {   // (MultiWaitHolderType*, TimerEventType*)
    const uint64_t h = A(0), t = A(1), lr = c->x[30];
    const uint64_t fn = fn_holder_timer();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && seen(g_holder_seen, (h << 8) ^ (t & 0xff), 20))
        NXLOG_I(kTag, "MultiWaitHolder(timer): holder 0x%llx <- timer 0x%llx, lr 0x%llx", h, t, lr);
    A(0) = r;
}
void frame_timestamp(::Ctx* c) {   // (VideoFrame*, nn::TimeSpan* out): what clock domain the player paces against
    const uint64_t f = A(0), out = A(1), lr = c->x[30];
    const uint64_t fn = fn_get_ts();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    // When RenderThread (f_7100722ba0, lr 0x7100722bfc) drops a frame because its PTS is >40 ms behind the initial wall-clock
    // anchor, a CPU decode (dav1d on 4K AV1) that has no pre-decoded backlog in d->ready cannot catch up by dropping frames and
    // would otherwise drop 100% of remaining frames until the user pauses and unpausing sets player[+644]=1 (#35, #17).
    // Setting player[+644]=1 here runs f_7100722ba0's own native clock re-anchor (f_7100724ee0) two instructions later.
    const int drops = g_consecutive_drops.load(std::memory_order_relaxed);
    if (drops > 0 && (!media_decoder_has_backlog() || drops >= 6)) {
        const uint64_t player = c->x[22];
        if (lr == 0x7100722bfcull && heap_address(player) && rd8(player + 819) == 1 && heap_address(rd64(player + 664))) {
            wr8(player + 644, 1);
            g_consecutive_drops.store(0, std::memory_order_relaxed);
            if (g_on && seen(g_any_seen, 0x5000, 40))
                NXLOG_I(kTag, "clock re-anchored after %d dropped frame(s) (player 0x%llx)", drops, (unsigned long long)player);
        }
    }
    if (g_on) {
        int64_t ts = 0;
        if (out && mem::is_mapped(out, 8)) std::memcpy(&ts, reinterpret_cast<const void*>(out), 8);
        if (seen(g_any_seen, 0x1000, 60)) NXLOG_I(kTag, "VideoFrame::GetTimeStamp(0x%llx) -> %lld ns, lr 0x%llx", f, (long long)ts, lr);
    }
    A(0) = r;
}
void frame_buffer(::Ctx* c) {   // (VideoFrame*, void** data, size_t* size): does the player ever read pixels?
    const uint64_t f = A(0), pdata = A(1), psize = A(2), lr = c->x[30];
    const uint64_t fn = fn_get_buf();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    g_frame_displayed.store(true, std::memory_order_relaxed);
    g_consecutive_drops.store(0, std::memory_order_relaxed);
    g_last_getbuf_ns.store(now_ns(), std::memory_order_relaxed);
    g_last_movie_frame_ns.store(now_ns(), std::memory_order_relaxed);
    if (g_on) {
        uint64_t data = 0, size = 0;
        if (pdata && mem::is_mapped(pdata, 8)) std::memcpy(&data, reinterpret_cast<const void*>(pdata), 8);
        if (psize && mem::is_mapped(psize, 8)) std::memcpy(&size, reinterpret_cast<const void*>(psize), 8);
        if (seen(g_any_seen, 0x2000, 40))
            NXLOG_I(kTag, "VideoFrame::GetBuffer(0x%llx) -> %llx bytes, data 0x%llx, lr 0x%llx", f, (unsigned long long)size,
                    (unsigned long long)data, lr);
    }
    A(0) = r;
}
void sleep_thread(::Ctx* c) {   // (TimeSpan ns): how long the player's pacing sleeps, and from where
    const int64_t ns = static_cast<int64_t>(A(0)), lr = static_cast<int64_t>(c->x[30]);
    const uint64_t fn = fn_sleep();
    const bool in_player = lr >= 0x7100710000ll && lr < 0x7100730000ll;   // the movie player's own code in main
    if (fn && g_on && in_player && ns >= 1000000 && seen(g_any_seen, 0x3000, 80))
        NXLOG_I(kTag, "SleepThread(%lld ns) at lr 0x%llx", (long long)ns, (unsigned long long)lr);
    if (fn) call_original(c, fn);
}
// Which EVENT the player links into its MultiWait: that is the one whose signal must arrive for the film's state machine to
// wake. Without this the log said "a timer holder was linked" and nothing about the events (2026-11-22).
void holder_event(::Ctx* c) {   // (MultiWaitHolderType*, EventType*)
    const uint64_t h = A(0), ev = A(1), lr = c->x[30];
    static const uint64_t fp = module_find_symbol(kHolderEvent);
    const uint64_t r = fp ? call_original(c, fp) : 0;
    NXLOG_I(kTag, "MultiWaitHolder(event): holder 0x%llx <- event 0x%llx, lr 0x%llx", h, ev, lr);
    A(0) = r;
}
void link_holder(::Ctx* c) {   // (MultiWaitType*, MultiWaitHolderType*): which multi a holder lands in
    const uint64_t m = A(0), h = A(1), lr = c->x[30];
    const uint64_t fn = fn_link();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && seen(g_holder_seen, (m << 8) ^ (h & 0xff), 30))
        NXLOG_I(kTag, "LinkMultiWaitHolder: multi 0x%llx <- holder 0x%llx, lr 0x%llx", m, h, lr);
    A(0) = r;
}
void wait_any(::Ctx* c) {   // (MultiWaitType*) -> MultiWaitHolder*
    const uint64_t m = A(0), lr = c->x[30];
    const uint64_t fn = fn_wait_any();
    if (!fn) { A(0) = 0; return; }
    const uint64_t r = call_original(c, fn);
    if (g_on && seen(g_any_seen, r, 20))
        NXLOG_I(kTag, "WaitAny: multi 0x%llx -> holder 0x%llx, lr 0x%llx", m, r, lr);
    A(0) = r;
}

struct Entry {
    const char* name;
    HleFn fn;
};
const Entry kEntries[] = {
    {kSend, send_msg},        {kRecv, recv_msg},           {kStartTimer, start_timer},   {kStopTimer, stop_timer},
    {kWaitTimer, wait_timer}, {kClearTimer, clear_timer},  {kWaitAny, wait_any},         {kHolderTimer, holder_timer},
    {kLink, link_holder},     {kGetTs, frame_timestamp},   {kGetBuf, frame_buffer},   {kSleep, sleep_thread},
    {kHolderEvent, holder_event}, {kStartOnce, start_timer_once},
    {kReleaseFrame, release_frame},
};

}  // namespace

// video_codec.cpp: the decoder's queues and the fences the client is armed on (same namespace as this file).
void media_dump_state(const char* why);

// Called from install_video_codec_hle before the modules are linked: every import of these names then lands here.
// The film starts while the game is still loading; the player then takes one frame every 2-8 s for up to half a minute before it
// jumps ahead. Dump the guest threads once a stall is clear, so the stack says what the player is waiting on.
// A new film starts: give it its own budget of stall dumps. The first film's start-up stall used to spend all of them, so the
// film that actually broke left nothing in the log (2026-11-21).
void player_trace_new_film() {
    g_stall_dumps.store(0, std::memory_order_relaxed);
    g_frame_displayed.store(false, std::memory_order_relaxed);
    g_consecutive_drops.store(0, std::memory_order_relaxed);
    // The clock too: it used to carry over from the previous film, so a film starting 146 s after the last frame of the
    // one before was already "146 s stalled" at its first packet and spent the whole dump budget in two seconds
    // (measured 2026-11-22: film 2 at 259 s, twelve dumps by 261 s, none left for the pause this trace exists for).
    g_last_movie_frame_ns.store(now_ns(), std::memory_order_relaxed);
}

// The player has been handed the end-of-stream marker: no new picture is expected after it, so "no new movie frame" is
// expected too. Re-arm the dump here, with a fresh clock, so the first sample lands one second later - while the player
// either hands the scene back (nothing to see) or sits on the last picture (the dump then says where it waits).
void player_trace_eos() {
    g_stall_dumps.store(0, std::memory_order_relaxed);
    g_last_movie_frame_ns.store(now_ns(), std::memory_order_relaxed);
}

void stall_watch() {
    int64_t last_dump_ns = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!media_film_active()) continue;
        const int64_t last = g_last_movie_frame_ns.load(std::memory_order_relaxed);
        // 12, not 3: a session can play several films and the first three dumps were used up by the start-up stall of the
        // first one, leaving nothing for the film that actually broke (2026-11-21).
        const int dump_no = g_stall_dumps.load(std::memory_order_relaxed);
        if (!last || dump_no >= 12) continue;
        const int64_t now = now_ns();
        // One dump a second, not four: a 1.5 s start-up stall used to write six dumps and the film that broke later found the
        // budget spent. A second apart still brackets every stall, and 12 of them cover a lot of stalling (2026-11-21).
        if (now - last < 1'000'000'000ll || now - last_dump_ns < 1'000'000'000ll) continue;
        last_dump_ns = now;
        // Only this thread writes the counter (a reset is a plain store), so load-then-store is enough; fetch_add used to
        // tick past the budget on the calls that bailed out.
        g_stall_dumps.store(dump_no + 1, std::memory_order_relaxed);
        media_dump_state("stall");
        const std::string dump = nxrt::diag_thread_dump();
        const std::string path = "stall_" + std::to_string(dump_no + 1) + ".txt";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f) f << "no new movie frame for " << (now - last) / 1e9 << " s\n" << dump;
        }
        NXLOG_W(kTag, "no new movie frame for %.1f s; guest thread dump -> %s (%zu bytes)", (now - last) / 1e9, path.c_str(), dump.size());
    }
}
void install_player_trace() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_on = config_get_bool("media", "player_trace", false);
        g_poke = config_get_bool("media", "movie_timer_poke", true);   // on by default: without it the player's per-frame timer never fires
        // The poke is not a diagnostic. It is what makes the player's frame loop wake at all on this pair of runtimes (see
        // start_timer below: without it, "one picture read, then nothing"). It used to be installed only when the trace was
        // on - and every development run had the trace on, so a plain install degenerated to about one picture a second and
        // then stopped, while the same build with NXRT_MEDIA_PLAYER_TRACE=1 played normally (measured 2026-11-22, first
        // clean install of 0.2.0).
        if (!g_on && !g_poke) return;
        if (g_on) std::thread(stall_watch).detach();
        for (const Entry& e : kEntries) {
            hle_register(e.name, e.fn);
            module_override_symbol(e.name);
        }
        NXLOG_I(kTag, "[media] player_trace: %zu nn::os message-queue / timer / MultiWait entry points %s",
                sizeof kEntries / sizeof kEntries[0], g_on ? "logged" : "routed for the per-frame timer poke");
    });
}

}  // namespace nxrt::media
