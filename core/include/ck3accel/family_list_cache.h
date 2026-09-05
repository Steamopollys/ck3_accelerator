#pragma once
// family-list cache: memoize the entries a composite family-list builder
// (close_family_member / extended_family_member / close_or_extended_family_member)
// produces for a given (character, builder, filter), valid for one frame-epoch. the
// character window rebuilds the same close-family list ~300x per portrait for one
// character; this collapses that to one real build plus a cheap replay. header-only
// + unit-tested; the plugin owns the engine glue.
//
// each slot owns a heap vector, so there is NO length ceiling: the case that matters
// is the immortal whose close-or-extended family runs to thousands of members (a
// fixed inline slot couldn't hold it, so it'd never cache). keyed by a 64-bit hash of
// (character handle, builder selector, filter flags); a slot hits only within the
// epoch it was written, so a new epoch (tick / effect / frame) invalidates everything.
// single-writer per instance (one cache per thread), so lookup returns a pointer into
// the slot with no copy.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ck3accel::famcache {

struct Entry {          // 16 bytes, matches the engine scope-list element
    std::uint16_t type;
    std::uint16_t sub;
    std::uint32_t pad;
    std::uint64_t handle;
};

inline std::uint64_t make_key(std::uint32_t character, std::uint8_t selector, std::uint8_t filter) {
    std::uint64_t k = (static_cast<std::uint64_t>(character) << 16) |
                      (static_cast<std::uint64_t>(selector) << 8) | filter;
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
    k ^= k >> 33;
    return k ? k : 0x9E3779B97F4A7C15ull;   // never the empty marker
}

template <std::size_t Cap>
class ListCache {
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
public:
    struct Stats { std::uint64_t hits = 0, stores = 0, evict = 0; };

    // on a live hit: sets *out to the stored entries (valid until the next store to
    // this slot on this thread), sets *count, returns true; else false. returns true
    // even for a cached empty list (no relatives), which a null pointer couldn't signal.
    bool lookup(std::uint64_t key, std::uint32_t epoch, const Entry** out, std::size_t* count) {
        Slot& s = slots_[static_cast<std::size_t>(key) & (Cap - 1)];
        if (s.key == key && s.epoch == epoch && s.valid) {
            *out = s.entries.data();
            *count = s.entries.size();
            ++stats_.hits;
            return true;
        }
        return false;
    }

    void store(std::uint64_t key, std::uint32_t epoch, const Entry* entries, std::size_t len) {
        Slot& s = slots_[static_cast<std::size_t>(key) & (Cap - 1)];
        if (s.valid && s.key != key) ++stats_.evict;
        s.key = key; s.epoch = epoch; s.valid = true;
        s.entries.assign(entries, entries + len);
        ++stats_.stores;
    }

    const Stats& stats() const { return stats_; }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t epoch = 0xFFFFFFFFu;
        bool valid = false;
        std::vector<Entry> entries;
    };
    std::vector<Slot> slots_{Cap};
    Stats stats_{};
};

}  // namespace ck3accel::famcache
