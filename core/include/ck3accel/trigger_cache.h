#pragma once
// per-frame UI trigger-result cache primitives (header-only, unit-tested).
//
//   ScopeRef / SavedScope / ContextView : mirror of ck3.exe's scope context.
//   make_key(ContextView) -> ContextKey : 128-bit hash of node + this/prev/root refs +
//       every saved scope (both stores) + the skip-validation flag. order-sensitive over
//       the saved-scope arrays (the game presents them in stable order within a frame).
//   EpochCache<Cap>  : fixed open-addressed {key,epoch}->result. an entry hits only in the
//       epoch it was written; a new epoch makes every entry stale (no clearing).
//   NodeFlagSet      : growable set of node pointers marked uncacheable (mutating triggers).
//
// the plugin owns the policy (when to consult/skip); this header owns the data structures.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ck3accel::cache {

struct ScopeRef {           // 16 bytes, matches the engine list/scope entry
    std::uint16_t type;
    std::uint16_t sub;
    std::uint32_t pad;
    std::uint64_t handle;
};

struct SavedScope {         // one entry of the context saved-scope store, as we hash it
    std::uint32_t id;
    ScopeRef ref;
};

struct ContextView {        // what make_key reads (filled by the plugin from rcx/rdx)
    std::uint64_t node;             // trigger node pointer
    const ScopeRef* current;        // ctx+0x00 (never null in practice)
    const ScopeRef* prev;           // ctx+0x08 (may be null)
    const ScopeRef* root;           // ctx+0x10
    const SavedScope* saved;        // primary saved-scope store
    std::size_t saved_count;
    const SavedScope* fallback;     // parent/fallback store (ctx+0x18 -> [+0x3d0])
    std::size_t fallback_count;
    std::uint8_t skip_validation;   // r8b
};

struct ContextKey {
    std::uint64_t lo;
    std::uint64_t hi;
    bool operator==(const ContextKey& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const ContextKey& o) const { return !(*this == o); }
};

namespace detail {
inline std::uint64_t mix(std::uint64_t x) {          // splitmix64
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
// two independent rolling accumulators for a 128-bit key (collision-safe at these volumes).
struct Hasher {
    std::uint64_t a = 0x243F6A8885A308D3ull;
    std::uint64_t b = 0x13198A2E03707344ull;
    void add(std::uint64_t v) {
        a = mix(a ^ v);
        b = (b + v) * 0x100000001B3ull ^ (b >> 29);
    }
    void add_ref(const ScopeRef* r) {
        if (!r) { add(0xD00Du); return; }
        add((static_cast<std::uint64_t>(r->type) << 16) | r->sub);
        add(r->handle);
    }
    ContextKey done() const { return ContextKey{a, b ^ mix(a)}; }
};
}  // namespace detail

inline ContextKey make_key(const ContextView& v) {
    detail::Hasher h;
    h.add(v.node);
    h.add_ref(v.current);
    h.add_ref(v.prev);
    h.add_ref(v.root);
    h.add(0x5A5A5A5Au | (static_cast<std::uint64_t>(v.skip_validation) << 32));
    h.add(v.saved_count * 0x1000000ull + v.fallback_count);
    for (std::size_t i = 0; i < v.saved_count; ++i) {
        h.add(0x51u + v.saved[i].id * 2ull);   // tag as primary + id
        h.add_ref(&v.saved[i].ref);
    }
    for (std::size_t i = 0; i < v.fallback_count; ++i) {
        h.add(0x9Du + v.fallback[i].id * 2ull); // tag as fallback + id
        h.add_ref(&v.fallback[i].ref);
    }
    return h.done();
}

template <std::size_t Cap>
class EpochCache {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    struct Stats { std::uint64_t hits = 0, misses = 0, stores = 0, overflow = 0; };

    // true and writes *result on a live hit (same key, same epoch).
    bool lookup(const ContextKey& k, std::uint32_t epoch, std::uint8_t* result) {
        std::size_t i = static_cast<std::size_t>(k.lo) & (Cap - 1);
        for (std::size_t p = 0; p < kMaxProbe; ++p) {
            Slot& s = slots_[i];
            if (s.epoch == epoch && s.occupied && s.key == k) {
                ++stats_.hits; *result = s.result; return true;
            }
            if (!s.occupied || s.epoch != epoch) break;   // empty or stale run end: not present
            i = (i + 1) & (Cap - 1);
        }
        ++stats_.misses;
        return false;
    }

    void store(const ContextKey& k, std::uint32_t epoch, std::uint8_t result) {
        std::size_t i = static_cast<std::size_t>(k.lo) & (Cap - 1);
        for (std::size_t p = 0; p < kMaxProbe; ++p) {
            Slot& s = slots_[i];
            const bool stale = s.occupied && s.epoch != epoch;
            if (!s.occupied || stale || (s.epoch == epoch && s.key == k)) {
                s.key = k; s.epoch = epoch; s.result = result; s.occupied = true;
                ++stats_.stores;
                return;
            }
            i = (i + 1) & (Cap - 1);
        }
        ++stats_.overflow;
    }

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_ = Stats{}; }

private:
    static constexpr std::size_t kMaxProbe = 32;
    struct Slot {
        ContextKey key{0, 0};
        std::uint32_t epoch = 0xFFFFFFFFu;  // sentinel: never a live epoch at start
        std::uint8_t result = 0;
        bool occupied = false;
    };
    std::vector<Slot> slots_{Cap};
    Stats stats_{};
};

// growable open-addressed set of node pointers (uncacheable nodes).
// single-writer/reader on the main thread; never shrinks.
class NodeFlagSet {
public:
    NodeFlagSet() : slots_(kInit, 0) {}
    bool contains(std::uint64_t node) const {
        if (node == 0) return has_zero_;
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(detail::mix(node)) & mask;
        while (slots_[i]) { if (slots_[i] == node) return true; i = (i + 1) & mask; }
        return false;
    }
    void insert(std::uint64_t node) {
        if (node == 0) { has_zero_ = true; return; }
        if ((count_ + 1) * 2 > slots_.size()) grow();
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(detail::mix(node)) & mask;
        while (slots_[i]) { if (slots_[i] == node) return; i = (i + 1) & mask; }
        slots_[i] = node; ++count_;
    }
    std::size_t size() const { return count_ + (has_zero_ ? 1 : 0); }
private:
    static constexpr std::size_t kInit = 4096;
    void grow() {
        std::vector<std::uint64_t> old(slots_.size() * 2, 0);
        old.swap(slots_);
        const std::size_t mask = slots_.size() - 1;
        count_ = 0;
        for (std::uint64_t n : old) if (n) {
            std::size_t i = static_cast<std::size_t>(detail::mix(n)) & mask;
            while (slots_[i]) i = (i + 1) & mask;
            slots_[i] = n; ++count_;
        }
    }
    std::vector<std::uint64_t> slots_;
    std::size_t count_ = 0;
    bool has_zero_ = false;
};

}  // namespace ck3accel::cache
