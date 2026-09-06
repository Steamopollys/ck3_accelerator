#include "trigger_service.h"

#include "hook_engine.h"
#include "logger.h"
#include "pattern_scanner.h"

#include <cstdint>
#include <mutex>

namespace ck3accel {
namespace {

// CK3 1.19.0.6-r20260602 trigger evaluator: char eval(void* node, uint8_t* ctx, uint8_t skip).
constexpr const char* kSigTrigger =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 C8 FD FF FF";

using trigger_fn = char (*)(void*, unsigned char*, unsigned char);
trigger_fn g_orig = nullptr;

struct Handler { ck3accel_trigger_handler fn; void* user; int priority; };
constexpr int kMax = 16;
Handler g_handlers[kMax];
int g_count = 0;   // stable after load; dispatch reads it without a lock

std::mutex g_mtx;
enum class State { Untried, Live, Failed };
State g_state = State::Untried;

char chain_invoke(int idx, void* node, void* ctx, unsigned char skip);

// Passed to each handler as `next`; continues the chain from the encoded index.
char chain_next(void* node, void* ctx, unsigned char skip, void* next_ctx) {
    return chain_invoke(static_cast<int>(reinterpret_cast<std::intptr_t>(next_ctx)), node, ctx, skip);
}
char chain_invoke(int idx, void* node, void* ctx, unsigned char skip) {
    if (idx < 0 || idx >= g_count) return g_orig(node, static_cast<unsigned char*>(ctx), skip);
    return g_handlers[idx].fn(node, ctx, skip, &chain_next,
                              reinterpret_cast<void*>(static_cast<std::intptr_t>(idx + 1)),
                              g_handlers[idx].user);
}
// Reentrant by design: a handler's next() eval of a child trigger re-enters here with a fresh stack.
char detour(void* node, unsigned char* ctx, unsigned char skip) {
    return chain_invoke(0, node, ctx, skip);
}

}  // namespace

bool trigger_service_ensure() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_state == State::Live)   return true;
    if (g_state == State::Failed) return false;

    void* t = nullptr;
    if (ScanResult r = scan_text(kSigTrigger); r.status == ScanStatus::Found)
        t = const_cast<void*>(static_cast<const void*>(r.address));
    if (!t) { LOG_WARN("trigger_service: signature NOT FOUND; unavailable"); g_state = State::Failed; return false; }

    const HookSetId set = register_hook_set("core.trigger");
    if (set == kInvalidHookSet) { g_state = State::Failed; return false; }
    if (!(install_hook(set, t, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_orig)) != nullptr && g_orig)) {
        LOG_WARN("trigger_service: hook install failed (a dev probe holding the evaluator?); unavailable");
        g_state = State::Failed; return false;
    }
    g_state = State::Live;
    LOG_INFO("trigger_service: shared trigger-evaluator hook active");
    return true;
}

void trigger_service_register(ck3accel_trigger_handler h, void* user, int priority) {
    if (!h) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_count >= kMax) return;
    int pos = g_count;                                   // insert keeping descending priority (stable)
    while (pos > 0 && g_handlers[pos - 1].priority < priority) { g_handlers[pos] = g_handlers[pos - 1]; --pos; }
    g_handlers[pos] = Handler{h, user, priority};
    ++g_count;
}

}  // namespace ck3accel
