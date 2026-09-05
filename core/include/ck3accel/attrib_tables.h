#pragma once
// fixed-capacity, allocation-free tables for the attribution probe.
//
//  NodeTable<Cap>          per-key {count, cycles} accumulator (key = trigger node
//                          pointer), open addressing, top-N by count. overflow counted, never silent.
//  StackTable<Cap, Depth>  call-chain (array of function RVAs) -> count, top-N.
//
// record on one hot thread each; top_n/merge at a quiescent boundary. no locks by
// design. storage is inline, so a table is allocated once with new and reused (reset)
// for the process lifetime.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ck3accel {

template <std::size_t Cap>
class NodeTable {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    struct Entry {
        std::uint64_t key;
        std::uint64_t count;
        std::uint64_t cycles;
    };

    NodeTable() { reset(); }

    void record(std::uint64_t key, std::uint64_t cycles) {
        ++total_;
        std::size_t i = mix(key) & (Cap - 1);
        for (std::size_t probe = 0; probe < kMaxProbe; ++probe) {
            Slot& s = slots_[i];
            if (s.used && s.key == key) { ++s.count; s.cycles += cycles; return; }
            if (!s.used) {
                if (used_ * 2 >= Cap) break;  // keep load <= 1/2
                s.used = true; s.key = key; s.count = 1; s.cycles = cycles;
                ++used_;
                return;
            }
            i = (i + 1) & (Cap - 1);
        }
        ++overflow_;
    }

    std::uint64_t total_records() const { return total_; }
    std::size_t distinct() const { return used_; }
    std::uint64_t overflow() const { return overflow_; }

    std::vector<Entry> top_n(std::size_t n) const {
        std::vector<Entry> v;
        v.reserve(used_);
        for (const Slot& s : slots_)
            if (s.used) v.push_back({s.key, s.count, s.cycles});
        std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.cycles != b.cycles) return a.cycles > b.cycles;
            return a.key < b.key;
        });
        if (v.size() > n) v.resize(n);
        return v;
    }

    void merge_from(const NodeTable& o) {
        for (const Slot& s : o.slots_) {
            if (!s.used) continue;
            std::size_t i = mix(s.key) & (Cap - 1);
            bool done = false;
            for (std::size_t probe = 0; probe < kMaxProbe; ++probe) {
                Slot& d = slots_[i];
                if (d.used && d.key == s.key) { d.count += s.count; d.cycles += s.cycles; done = true; break; }
                if (!d.used) {
                    if (used_ * 2 >= Cap) break;
                    d = s; ++used_; done = true; break;
                }
                i = (i + 1) & (Cap - 1);
            }
            if (!done) ++overflow_;
        }
        total_ += o.total_;
        overflow_ += o.overflow_;
    }

    void reset() {
        for (Slot& s : slots_) s.used = false;
        used_ = 0; total_ = 0; overflow_ = 0;
    }

private:
    struct Slot {
        std::uint64_t key;
        std::uint64_t count;
        std::uint64_t cycles;
        bool used;
    };
    static constexpr std::size_t kMaxProbe = 64;

    static std::uint64_t mix(std::uint64_t x) {   // splitmix64 finalizer
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    std::array<Slot, Cap> slots_;
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t overflow_ = 0;
};

template <std::size_t Cap, std::size_t Depth>
class StackTable {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    struct Entry {
        std::uint64_t count;
        std::size_t depth;
        std::array<std::uint32_t, Depth> frames;
    };

    StackTable() { reset(); }

    // record one sample: frames[0] = leaf. chains longer than Depth are truncated.
    void record(const std::uint32_t* frames, std::size_t depth) {
        if (!frames || depth == 0) return;
        if (depth > Depth) depth = Depth;
        ++total_;
        const std::uint64_t h = hash(frames, depth);
        std::size_t i = static_cast<std::size_t>(h) & (Cap - 1);
        for (std::size_t probe = 0; probe < kMaxProbe; ++probe) {
            Slot& s = slots_[i];
            if (s.used && s.hash == h && s.depth == depth &&
                std::memcmp(s.frames.data(), frames, depth * sizeof(std::uint32_t)) == 0) {
                ++s.count;
                return;
            }
            if (!s.used) {
                if (used_ * 2 >= Cap) break;
                s.used = true; s.hash = h; s.depth = depth; s.count = 1;
                std::memcpy(s.frames.data(), frames, depth * sizeof(std::uint32_t));
                ++used_;
                return;
            }
            i = (i + 1) & (Cap - 1);
        }
        ++overflow_;
    }

    std::uint64_t total() const { return total_; }
    std::uint64_t overflow() const { return overflow_; }
    std::size_t distinct() const { return used_; }

    std::vector<Entry> top_n(std::size_t n) const {
        std::vector<Entry> v;
        v.reserve(used_);
        for (const Slot& s : slots_)
            if (s.used) v.push_back({s.count, s.depth, s.frames});
        std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) {
            return a.count != b.count ? a.count > b.count : a.depth < b.depth;
        });
        if (v.size() > n) v.resize(n);
        return v;
    }

    void reset() {
        for (Slot& s : slots_) s.used = false;
        used_ = 0; total_ = 0; overflow_ = 0;
    }

private:
    struct Slot {
        std::uint64_t hash;
        std::uint64_t count;
        std::size_t depth;
        std::array<std::uint32_t, Depth> frames;
        bool used;
    };
    static constexpr std::size_t kMaxProbe = 64;

    static std::uint64_t hash(const std::uint32_t* f, std::size_t n) {   // FNV-1a over frames
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (std::size_t i = 0; i < n; ++i) { h ^= f[i]; h *= 0x100000001b3ull; }
        h ^= n; h *= 0x100000001b3ull;
        return h;
    }

    std::array<Slot, Cap> slots_;
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t overflow_ = 0;
};

}  // namespace ck3accel
