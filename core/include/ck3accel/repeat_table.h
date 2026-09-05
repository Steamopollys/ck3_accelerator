#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
namespace ck3accel {

struct RepeatTally {
    std::uint64_t total = 0;             // sum of per-key counts (evals recorded)
    std::uint64_t distinct = 0;          // occupied slots (distinct keys)
    std::uint64_t repeated_keys = 0;     // keys with count > 1
    std::uint64_t inconsistent_keys = 0; // keys that returned >1 distinct result
    std::uint64_t overflow = 0;          // evals/keys dropped on a full table
};

// fixed-capacity open-addressed hash map for trigger repeat/consistency measurement.
// one table per worker thread (lock-free single-writer), merged at the tick boundary.
// NCAP must be a power of two. key 0 is remapped to a sentinel so a real 0 works.
// MaxProbe bounds the probe length per record/merge: past it a record counts as
// overflow instead of scanning the rest (an unbounded scan of a FULL 1M-slot table
// costs ~1 ms per new key; it slowed CK3 save loading by minutes in the probe, 2026-09-02).
template <std::size_t NCAP, std::size_t MaxProbe = NCAP>
class RepeatTable {
    static_assert(NCAP >= 2 && (NCAP & (NCAP - 1)) == 0, "NCAP must be a power of two >= 2");
    static_assert(MaxProbe >= 1 && MaxProbe <= NCAP, "MaxProbe must be in [1, NCAP]");
public:
    RepeatTable() : slots_(NCAP) {}   // value-init: occupied=0 everywhere

    void record(std::uint64_t key, std::uint8_t result) {
        const std::uint64_t k = key ? key : kZeroKey;
        std::size_t i = static_cast<std::size_t>(hash(k)) & (NCAP - 1);
        for (std::size_t p = 0; p < MaxProbe; ++p) {
            Slot& s = slots_[i];
            if (!s.occupied) {
                s.key = k; s.count = 1;
                s.first_result = result; s.inconsistent = 0;
                s.occupied = 1;   // commit LAST: the once-per-day merge (single reader,
                                  // sim thread) sees occupied only after the other fields
                                  // are written (x64 store ordering). per-thread tables are
                                  // single-writer, so the merge stays race-free.
                return;
            }
            if (s.key == k) {
                if (s.count != 0xFFFFFFFFu) ++s.count;
                if (result != s.first_result) s.inconsistent = 1;
                return;
            }
            i = (i + 1) & (NCAP - 1);
        }
        ++overflow_;
    }

    // fold every occupied slot of other into this (cross-thread merge): same key ->
    // sum counts; inconsistent if either side is inconsistent or the first_results differ.
    void merge_from(const RepeatTable& other) {
        for (const Slot& s : other.slots_) if (s.occupied) merge_slot(s);
        overflow_ += other.overflow_;
    }

    RepeatTally tally() const {
        RepeatTally t; t.overflow = overflow_;
        for (const Slot& s : slots_) {
            if (!s.occupied) continue;
            ++t.distinct; t.total += s.count;
            if (s.count > 1) ++t.repeated_keys;
            if (s.inconsistent) ++t.inconsistent_keys;
        }
        return t;
    }

    void reset() {
        std::memset(slots_.data(), 0, slots_.size() * sizeof(Slot));
        overflow_ = 0;
    }
    std::uint64_t overflow() const { return overflow_; }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t count = 0;
        std::uint8_t  first_result = 0;
        std::uint8_t  inconsistent = 0;
        std::uint8_t  occupied = 0;
        std::uint8_t  _pad = 0;
    };
    static constexpr std::uint64_t kZeroKey = 0xFFFFFFFFFFFFFFFFull;
    static std::uint64_t hash(std::uint64_t x) {  // splitmix64 finalizer
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27; x *= 0x94d049bb133111ebull;
        x ^= x >> 31; return x;
    }
    void merge_slot(const Slot& s) {
        std::size_t i = static_cast<std::size_t>(hash(s.key)) & (NCAP - 1);
        for (std::size_t p = 0; p < MaxProbe; ++p) {
            Slot& d = slots_[i];
            if (!d.occupied) { d = s; return; }
            if (d.key == s.key) {
                const std::uint64_t c = static_cast<std::uint64_t>(d.count) + s.count;
                d.count = c > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(c);
                if (s.inconsistent || s.first_result != d.first_result) d.inconsistent = 1;
                return;
            }
            i = (i + 1) & (NCAP - 1);
        }
        ++overflow_;
    }
    std::vector<Slot> slots_;
    std::uint64_t overflow_ = 0;
};

}  // namespace ck3accel
