#include "tick_epoch.h"

#include "hook_engine.h"
#include "logger.h"
#include "pattern_scanner.h"

#include <atomic>
#include <mutex>

namespace ck3accel {
namespace {

// CK3 1.19.0.6-r20260602. the core owns these hooks, so this is the one place
// that knows the signatures.
constexpr const char* kSigEffect =
    "48 8B C4 48 89 58 08 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 50 03 00 00";
constexpr const char* kSigTick =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FD FF FF 48 81 EC 88 03 00 00 0F 29 B4 24 70 03 00 00";

using effect_fn = void (*)(void*, void*, void*, void*);
using tick_fn   = int  (*)(void*);

std::atomic<std::uint32_t> g_epoch{1};
std::atomic<int>           g_in_tick{0};
std::atomic<bool>          g_live{false};

effect_fn g_orig_effect = nullptr;
tick_fn   g_orig_tick   = nullptr;

std::mutex g_init_mutex;
enum class State { Untried, Live, Failed };
State g_state = State::Untried;

inline void bump() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

// an effect mutates script state. bump on both sides so an entry cached mid-effect
// by another thread can't later be served as predating the change.
void detour_effect(void* effect, void* ctx, void* a, void* b) {
    bump();
    g_orig_effect(effect, ctx, a, b);
    bump();
}

int detour_tick(void* gamestate) {
    g_in_tick.fetch_add(1, std::memory_order_relaxed);
    bump();
    const int rc = g_orig_tick(gamestate);
    bump();
    g_in_tick.fetch_sub(1, std::memory_order_relaxed);
    return rc;
}

}  // namespace

bool tick_epoch_ensure() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_state == State::Live)   return true;
    if (g_state == State::Failed) return false;

    void* eff  = nullptr;
    void* tick = nullptr;
    if (ScanResult r = scan_text(kSigEffect); r.status == ScanStatus::Found)
        eff = const_cast<void*>(static_cast<const void*>(r.address));
    if (ScanResult r = scan_text(kSigTick); r.status == ScanStatus::Found)
        tick = const_cast<void*>(static_cast<const void*>(r.address));
    if (!eff || !tick) {
        LOG_WARN("tick_epoch: effect/tick signature NOT FOUND; shared epoch service unavailable "
                 "(dependent caches will stay inert)");
        g_state = State::Failed;
        return false;
    }

    const HookSetId set = register_hook_set("core.tick_epoch");
    if (set == kInvalidHookSet) {
        LOG_WARN("tick_epoch: could not allocate a hook set; service unavailable");
        g_state = State::Failed;
        return false;
    }
    const bool ok =
        install_hook(set, tick, reinterpret_cast<void*>(&detour_tick),
                     reinterpret_cast<void**>(&g_orig_tick)) != nullptr && g_orig_tick &&
        install_hook(set, eff, reinterpret_cast<void*>(&detour_effect),
                     reinterpret_cast<void**>(&g_orig_effect)) != nullptr && g_orig_effect;
    if (!ok) {
        LOG_WARN("tick_epoch: failed to install effect/tick hooks (already hooked by a probe? "
                 "disable dev probes); service unavailable");
        g_state = State::Failed;
        return false;
    }
    g_live.store(true, std::memory_order_release);
    g_state = State::Live;
    LOG_INFO("tick_epoch: shared effect+tick epoch service active");
    return true;
}

std::uint32_t tick_epoch_get()  { return g_epoch.load(std::memory_order_relaxed); }
int           tick_epoch_in_tick() { return g_in_tick.load(std::memory_order_relaxed); }
void          tick_epoch_bump() { if (g_live.load(std::memory_order_relaxed)) bump(); }

}  // namespace ck3accel
