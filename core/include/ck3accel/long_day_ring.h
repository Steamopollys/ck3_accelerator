#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <ck3accel/eval_accumulators.h>  // HookStats, HOOK_COUNT
namespace ck3accel {

// one simulated-day record: day index (monotonic), wall-ms of the day's
// UpdateTurnTick, and the per-hook counter deltas from it.
struct LongDayRecord {
    std::uint64_t day_index = 0;
    int           year  = 0;   // 0 if the date field was not resolved (v1)
    int           month = 0;
    int           day   = 0;
    double        wall_ms = 0.0;
    HookStats     hooks[HOOK_COUNT] = {};
};

// fixed-capacity ring of the last N day records (overwrites oldest). single
// producer (sim thread, in the UpdateTurnTick detour): no locking.
template <std::size_t N>
class LongDayRing {
    static_assert(N > 0, "LongDayRing capacity must be > 0");
public:
    void push(const LongDayRecord& r) {
        buf_[head_] = r;
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
    }
    std::size_t size() const { return count_; }
    // at(0) = oldest retained, at(size()-1) = newest.
    const LongDayRecord& at(std::size_t i) const {
        const std::size_t start = (count_ < N) ? 0 : head_;
        return buf_[(start + i) % N];
    }
    const LongDayRecord& newest() const { return at(count_ - 1); }
private:
    std::array<LongDayRecord, N> buf_{};
    std::size_t head_  = 0;
    std::size_t count_ = 0;
};

}  // namespace ck3accel
