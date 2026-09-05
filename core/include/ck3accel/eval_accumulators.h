#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
namespace ck3accel {

// One slot per instrumented hot hook. (Freeze/UpdateTurnTick is not counted here.)
enum HookId : std::size_t {
    HOOK_DISPATCH = 0,
    HOOK_TRIGGER,
    HOOK_EVENTTARGET,
    HOOK_EVENTSCOPE,
    HOOK_LINK,
    HOOK_COUNT
};

// per-thread, per-hook accumulator. on_enter/on_exit take __rdtsc() readings in
// the hot detour. depth-guarded so recursive nesting (trigger -> sub-trigger ->
// event-target) records inclusive time ONLY at the outermost frame, while every
// call increments count. no locks, no allocation; TLS-resident.
struct HookAccumulator {
    std::uint64_t count    = 0;
    std::uint64_t incl_tsc = 0;
    std::uint32_t depth    = 0;
    std::uint64_t t0       = 0;
    void on_enter(std::uint64_t tsc) {
        if (depth++ == 0) t0 = tsc;
        ++count;
    }
    void on_exit(std::uint64_t tsc) {
        if (depth == 0) return;            // unbalanced exit: ignore (no underflow)
        if (--depth == 0) incl_tsc += (tsc - t0);
    }
};

// Merged totals read out by the dump thread.
struct HookStats {
    std::uint64_t count    = 0;
    std::uint64_t incl_tsc = 0;
};

// registry of every thread's HookAccumulator[HOOK_COUNT] shard. each worker
// registers its TLS shard once; the dump thread sums all shards under one lock,
// off the hot path. (shards are cumulative; snapshot recomputes the global sum.)
class ShardRegistry {
public:
    void register_shard(HookAccumulator* shard) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto* p : shards_) if (p == shard) return;  // idempotent
        shards_.push_back(shard);
    }
    // zero out[0..n) then add every registered shard's first n slots.
    void snapshot(HookStats* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) { out[i].count = 0; out[i].incl_tsc = 0; }
        std::lock_guard<std::mutex> lk(m_);
        for (const HookAccumulator* s : shards_) {
            for (std::size_t i = 0; i < n; ++i) {
                out[i].count    += s[i].count;
                out[i].incl_tsc += s[i].incl_tsc;
            }
        }
    }
private:
    std::mutex m_;
    std::vector<HookAccumulator*> shards_;
};

}  // namespace ck3accel
