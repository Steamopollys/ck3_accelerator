#pragma once
// O(N) replacement for ck3.exe's O(N^2) family-list dedup.
//
// the engine builds close_family_member / extended_family_member /
// close_or_extended_family_member scope lists via two primitives that dedup by
// linearly scanning the whole output list per relative added (build
// 1.19.0.6-r20260602: add-unique fn_02688C70 and family walker fn_01A5F8C0, which
// inlines a second copy of the scan). this reimplements both on a hash set that
// mirrors the list. membership is a pure function of list content, so the output is
// identical (same entries, same order); test_family_dedup.cpp checks that against a
// literal transcription of the engine's linear algorithm.
//
// header-only and engine-agnostic: the engine is a template parameter (plugin
// supplies real layouts; tests supply a fake graph). nothing allocates on the hot
// path except HandleSet growth.
//
// engine concept `E`:
//   using Char, List;
//   Char* resolve(uint32_t handle);                 // never null (game's null-character sentinel)
//   bool is_valid(Char*); bool is_dead(Char*); uint32_t handle_of(Char*);
//   const uint32_t* children_begin(Char*); const uint32_t* children_end(Char*);
//   const ScopeRef* list_data(List*); int32_t list_count(List*);
//   void push_back(List*, const ScopeRef&);
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ck3accel::family {

// one Jomini scope-list entry (CPdxArray element, 16 bytes). the engine compares
// only type, sub, handle; pad is uninitialised in the original.
struct ScopeRef {
    std::uint16_t type;
    std::uint16_t sub;
    std::uint32_t pad;
    std::uint64_t handle;
};
static_assert(sizeof(ScopeRef) == 16, "ScopeRef must match the engine's 16-byte list entry");
static_assert(offsetof(ScopeRef, handle) == 8, "handle must sit at +8");

constexpr std::uint16_t kCharacterType = 4;
constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFu;

// builder filter flags: [flags+8] include everyone, [flags+9] dead only (else alive only).
struct Filter {
    bool include_all;
    bool dead_only;
};

inline bool filter_passes(const Filter& f, bool dead) {
    return f.include_all || (f.dead_only ? dead : !dead);
}

// open-addressed set of 32-bit character handles (linear probing, load factor <= 1/2).
// kInvalidHandle is the empty marker, never a real handle.
class HandleSet {
public:
    HandleSet() : slots_(kInitialCapacity, kInvalidHandle) {}

    bool contains(std::uint32_t h) const {
        if (h == kInvalidHandle) return false;
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = mix(h) & mask;
        while (slots_[i] != kInvalidHandle) {
            if (slots_[i] == h) return true;
            i = (i + 1) & mask;
        }
        return false;
    }

    // Returns true if inserted, false if already present (or h is the invalid handle).
    bool insert(std::uint32_t h) {
        if (h == kInvalidHandle) return false;
        if ((count_ + 1) * 2 > slots_.size()) grow();
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = mix(h) & mask;
        while (slots_[i] != kInvalidHandle) {
            if (slots_[i] == h) return false;
            i = (i + 1) & mask;
        }
        slots_[i] = h;
        ++count_;
        return true;
    }

    void clear() {
        if (count_ == 0) return;
        std::fill(slots_.begin(), slots_.end(), kInvalidHandle);
        count_ = 0;
    }

    std::size_t size() const { return count_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    static constexpr std::size_t kInitialCapacity = 1024;

    static std::uint32_t mix(std::uint32_t h) {
        h ^= h >> 16; h *= 0x7feb352dU;
        h ^= h >> 15; h *= 0x846ca68bU;
        h ^= h >> 16;
        return h;
    }

    void grow() {
        std::vector<std::uint32_t> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, kInvalidHandle);
        count_ = 0;
        for (std::uint32_t h : old)
            if (h != kInvalidHandle) insert(h);
    }

    std::vector<std::uint32_t> slots_;
    std::size_t count_ = 0;
};

namespace detail {
template <class E>
inline void push_character(E& e, typename E::List* list, std::uint32_t h) {
    ScopeRef r{};
    r.type = kCharacterType;
    r.sub = 0;
    r.pad = 0;
    r.handle = h;
    e.push_back(list, r);
}
}  // namespace detail

// rebuild set from the list's current character entries (what the engine's scan
// could match: type 4, sub 0, handle fits 32 bits).
template <class E>
inline void sync_set_from_list(E& e, HandleSet& set, typename E::List* list) {
    set.clear();
    const ScopeRef* d = e.list_data(list);
    const std::int32_t n = e.list_count(list);
    for (std::int32_t i = 0; i < n; ++i) {
        if (d[i].type == kCharacterType && d[i].sub == 0 && d[i].handle <= 0xFFFFFFFEull)
            set.insert(static_cast<std::uint32_t>(d[i].handle));
    }
}

// fn_02688C70: append the character unless an identical character entry is already present.
template <class E>
inline void add_unique(E& e, HandleSet& set, typename E::List* list, typename E::Char* c) {
    const std::uint32_t h = e.handle_of(c);
    if (set.contains(h)) return;
    detail::push_character(e, list, h);
    set.insert(h);
}

// fn_01A5F8C0: for every child of x (except skip): add the child if it passes the
// filter, then add each grandchild that passes. both are unique-adds. the inner
// loop runs even when the child was filtered out (grandchildren via a dead child
// are still relatives), exactly like the original.
template <class E>
inline void walk(E& e, HandleSet& set, typename E::List* list, const Filter& f,
                 typename E::Char* x, std::uint32_t skip) {
    for (const std::uint32_t* p = e.children_begin(x); p != e.children_end(x); ++p) {
        const std::uint32_t h = *p;
        if (h == skip) continue;
        typename E::Char* y = e.resolve(h);
        if (e.is_valid(y) && filter_passes(f, e.is_dead(y))) add_unique(e, set, list, y);
        for (const std::uint32_t* q = e.children_begin(y); q != e.children_end(y); ++q) {
            typename E::Char* z = e.resolve(*q);
            if (!e.is_valid(z)) continue;
            if (!filter_passes(f, e.is_dead(z))) continue;
            const std::uint32_t hz = e.handle_of(z);
            if (set.contains(hz)) continue;
            detail::push_character(e, list, hz);
            set.insert(hz);
        }
    }
}

}  // namespace ck3accel::family
