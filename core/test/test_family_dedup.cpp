// Tests for ck3accel/family_dedup.h, the O(N) replacement for ck3.exe's O(N^2)
// family-list dedup. The "reference" functions below are literal transcriptions of
// the engine's linear algorithm (fn_02688C70 add-unique, fn_01A5F8C0 walker on build
// 1.19.0.6-r20260602); production code must produce a byte-identical list (same
// entries, same order) on every input.
#include <ck3accel/family_dedup.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using ck3accel::family::Filter;
using ck3accel::family::HandleSet;
using ck3accel::family::ScopeRef;
using ck3accel::family::kCharacterType;

namespace {

// ---- fake engine: characters indexed by handle, sentinel for bad handles ----
struct FakeChar {
    std::uint32_t handle = 0xFFFFFFFFu;
    bool valid = false;
    bool dead = false;
    std::vector<std::uint32_t> children;
};

struct FakeList {
    std::vector<ScopeRef> v;
};

struct FakeEngine {
    using Char = FakeChar;
    using List = FakeList;
    std::vector<FakeChar> chars;
    FakeChar sentinel;   // invalid, no children

    Char* resolve(std::uint32_t h) {
        if (h < chars.size() && chars[h].valid) return &chars[h];
        // invalid handle (or a reused slot) resolves like the game's db lookup
        if (h < chars.size()) return &chars[h];   // slot exists but may be !valid
        return &sentinel;
    }
    bool is_valid(Char* c) const { return c->valid; }
    bool is_dead(Char* c) const { return c->dead; }
    std::uint32_t handle_of(Char* c) const { return c->handle; }
    const std::uint32_t* children_begin(Char* c) const { return c->children.data(); }
    const std::uint32_t* children_end(Char* c) const { return c->children.data() + c->children.size(); }
    const ScopeRef* list_data(List* l) const { return l->v.data(); }
    std::int32_t list_count(List* l) const { return static_cast<std::int32_t>(l->v.size()); }
    void push_back(List* l, const ScopeRef& e) { l->v.push_back(e); }
};

// ---- reference (linear) semantics, transcribed from the disassembly ----
bool ref_filter(const Filter& f, FakeEngine& e, FakeChar* c) {   // fn_02688C00
    if (!e.is_valid(c)) return false;
    if (f.include_all) return true;
    return f.dead_only ? e.is_dead(c) : !e.is_dead(c);
}

bool ref_list_contains(FakeEngine& e, FakeList* l, std::uint32_t h) {
    const ScopeRef* d = e.list_data(l);
    const std::int32_t n = e.list_count(l);
    for (std::int32_t i = 0; i < n; ++i)
        if (d[i].type == kCharacterType && d[i].sub == 0 && d[i].handle == h) return true;
    return false;
}

void ref_add_unique(FakeEngine& e, FakeList* l, FakeChar* c) {   // fn_02688C70
    const std::uint32_t h = e.handle_of(c);
    if (ref_list_contains(e, l, h)) return;
    ScopeRef r{}; r.type = kCharacterType; r.sub = 0; r.handle = h;
    e.push_back(l, r);
}

void ref_walk(FakeEngine& e, FakeList* l, const Filter& f, FakeChar* x, std::uint32_t skip) {  // fn_01A5F8C0
    for (const std::uint32_t* p = e.children_begin(x); p != e.children_end(x); ++p) {
        const std::uint32_t h = *p;
        if (h == skip) continue;
        FakeChar* y = e.resolve(h);
        if (e.is_valid(y) && ref_filter(f, e, y)) ref_add_unique(e, l, y);
        for (const std::uint32_t* q = e.children_begin(y); q != e.children_end(y); ++q) {
            FakeChar* z = e.resolve(*q);
            if (!e.is_valid(z)) continue;
            if (!(f.include_all || (f.dead_only ? e.is_dead(z) : !e.is_dead(z)))) continue;
            const std::uint32_t hz = e.handle_of(z);
            if (ref_list_contains(e, l, hz)) continue;
            ScopeRef r{}; r.type = kCharacterType; r.sub = 0; r.handle = hz;
            e.push_back(l, r);
        }
    }
}

// ---- random family graphs ----
FakeEngine make_graph(std::mt19937& rng, std::size_t n, double dead_p, double invalid_p, int max_kids) {
    FakeEngine e;
    e.chars.resize(n);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (std::size_t i = 0; i < n; ++i) {
        e.chars[i].handle = static_cast<std::uint32_t>(i);
        e.chars[i].valid = u(rng) >= invalid_p;
        e.chars[i].dead = u(rng) < dead_p;
    }
    // each character (past the first few) gets 1-2 parents among earlier ones
    for (std::size_t i = 4; i < n; ++i) {
        std::uniform_int_distribution<std::size_t> pick(0, i - 1);
        const std::size_t pa = pick(rng);
        e.chars[pa].children.push_back(static_cast<std::uint32_t>(i));
        if (u(rng) < 0.8) {
            const std::size_t pb = pick(rng);
            if (pb != pa) e.chars[pb].children.push_back(static_cast<std::uint32_t>(i));
        }
    }
    // a few prolific parents, some bogus child handles (purged characters)
    std::uniform_int_distribution<std::size_t> anyc(0, n - 1);
    for (int k = 0; k < max_kids; ++k) {
        e.chars[anyc(rng)].children.push_back(static_cast<std::uint32_t>(anyc(rng)));
        e.chars[anyc(rng)].children.push_back(0xFFFFFFFFu);
        e.chars[anyc(rng)].children.push_back(static_cast<std::uint32_t>(n + 1000 + k));
    }
    return e;
}

bool same_list(const FakeList& a, const FakeList& b) {
    if (a.v.size() != b.v.size()) return false;
    for (std::size_t i = 0; i < a.v.size(); ++i)
        if (a.v[i].type != b.v[i].type || a.v[i].sub != b.v[i].sub || a.v[i].handle != b.v[i].handle) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------- HandleSet
TEST(HandleSet, EmptyContainsNothing) {
    HandleSet s;
    EXPECT_FALSE(s.contains(0));
    EXPECT_FALSE(s.contains(12345));
    EXPECT_EQ(s.size(), 0u);
}

TEST(HandleSet, InsertThenContains_DuplicateInsertReturnsFalse) {
    HandleSet s;
    EXPECT_TRUE(s.insert(42));
    EXPECT_TRUE(s.contains(42));
    EXPECT_FALSE(s.insert(42));
    EXPECT_EQ(s.size(), 1u);
}

TEST(HandleSet, HandleZeroAndMaxValidAreOrdinaryKeys) {
    HandleSet s;
    EXPECT_TRUE(s.insert(0));
    EXPECT_TRUE(s.insert(0xFFFFFFFEu));
    EXPECT_TRUE(s.contains(0));
    EXPECT_TRUE(s.contains(0xFFFFFFFEu));
    EXPECT_FALSE(s.contains(1));
}

TEST(HandleSet, GrowsWithoutLosingKeys) {
    HandleSet s;
    for (std::uint32_t i = 0; i < 100000; ++i) EXPECT_TRUE(s.insert(i * 7919u));
    EXPECT_EQ(s.size(), 100000u);
    for (std::uint32_t i = 0; i < 100000; ++i) EXPECT_TRUE(s.contains(i * 7919u));
    EXPECT_FALSE(s.contains(3));
}

TEST(HandleSet, ClearEmptiesAndKeepsWorking) {
    HandleSet s;
    for (std::uint32_t i = 0; i < 1000; ++i) s.insert(i);
    s.clear();
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.contains(5));
    EXPECT_TRUE(s.insert(5));
    EXPECT_TRUE(s.contains(5));
}

// ---------------------------------------------------------------- sync from an existing list
TEST(FamilyDedup, SyncFromListIgnoresNonCharacterEntriesAndDuplicates) {
    FakeEngine e;
    FakeList l;
    ScopeRef a{}; a.type = kCharacterType; a.sub = 0; a.handle = 7;
    ScopeRef b{}; b.type = 3;              b.sub = 0; b.handle = 9;   // not a character entry
    ScopeRef c{}; c.type = kCharacterType; c.sub = 1; c.handle = 11;  // sub != 0: never matched by the engine
    l.v = {a, b, a, c};
    HandleSet s;
    ck3accel::family::sync_set_from_list(e, s, &l);
    EXPECT_TRUE(s.contains(7));
    EXPECT_FALSE(s.contains(9));
    EXPECT_FALSE(s.contains(11));
    EXPECT_EQ(s.size(), 1u);
}

// ---------------------------------------------------------------- add_unique
TEST(FamilyDedup, AddUniqueMatchesReferenceIncludingPreexistingEntries) {
    FakeEngine e;
    e.chars.resize(3);
    for (std::uint32_t i = 0; i < 3; ++i) { e.chars[i].handle = i; e.chars[i].valid = true; }
    FakeList ref, fast;
    ScopeRef pre{}; pre.type = kCharacterType; pre.sub = 0; pre.handle = 1;
    ref.v = {pre}; fast.v = {pre};
    HandleSet s;
    ck3accel::family::sync_set_from_list(e, s, &fast);
    for (std::uint32_t h : {1u, 2u, 2u, 0u, 1u}) {
        ref_add_unique(e, &ref, &e.chars[h]);
        ck3accel::family::add_unique(e, s, &fast, &e.chars[h]);
    }
    EXPECT_TRUE(same_list(ref, fast));
    EXPECT_EQ(fast.v.size(), 3u);
}

// ---------------------------------------------------------------- walker property test
TEST(FamilyDedup, WalkerMatchesReferenceOnRandomGraphs) {
    std::mt19937 rng(20260902u);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int iter = 0; iter < 300; ++iter) {
        FakeEngine e = make_graph(rng, 40 + iter % 60, 0.3, 0.05, 6);
        std::uniform_int_distribution<std::size_t> anyc(0, e.chars.size() - 1);
        const Filter f{u(rng) < 0.4, u(rng) < 0.3};
        FakeList ref, fast;
        // pre-existing entry (the builders' output list may not start empty)
        if (u(rng) < 0.5) {
            ScopeRef pre{}; pre.type = kCharacterType; pre.sub = 0; pre.handle = static_cast<std::uint32_t>(anyc(rng));
            ref.v.push_back(pre); fast.v.push_back(pre);
        }
        HandleSet s;
        ck3accel::family::sync_set_from_list(e, s, &fast);
        // composite-builder-like: several walks + adds on the same list
        for (int step = 0; step < 4; ++step) {
            FakeChar* x = e.resolve(static_cast<std::uint32_t>(anyc(rng)));
            const std::uint32_t skip = u(rng) < 0.5 ? static_cast<std::uint32_t>(anyc(rng)) : 0xFFFFFFFFu;
            ref_walk(e, &ref, f, x, skip);
            ck3accel::family::walk(e, s, &fast, f, x, skip);
            FakeChar* p = e.resolve(static_cast<std::uint32_t>(anyc(rng)));
            if (e.is_valid(p) && ref_filter(f, e, p)) {
                ref_add_unique(e, &ref, p);
                ck3accel::family::add_unique(e, s, &fast, p);
            }
            ASSERT_TRUE(same_list(ref, fast)) << "iter " << iter << " step " << step;
        }
    }
}

TEST(FamilyDedup, WalkerOnProlificParentIsLinear) {
    // 20 000 children x 3 kids -> ~80 000 entries; the linear reference would need
    // ~3e9 compares. Must finish fast and match a direct construction.
    FakeEngine e;
    const std::uint32_t n_children = 20000;
    e.chars.resize(1 + n_children * 4);
    for (std::uint32_t i = 0; i < e.chars.size(); ++i) { e.chars[i].handle = i; e.chars[i].valid = true; }
    for (std::uint32_t c = 1; c <= n_children; ++c) {
        e.chars[0].children.push_back(c);
        for (std::uint32_t k = 0; k < 3; ++k) e.chars[c].children.push_back(n_children + 1 + (c - 1) * 3 + k);
    }
    FakeList fast;
    HandleSet s;
    ck3accel::family::walk(e, s, &fast, Filter{true, false}, &e.chars[0], 0xFFFFFFFFu);
    ASSERT_EQ(fast.v.size(), n_children * 4u);
    // order: child, its 3 kids, next child, ...
    EXPECT_EQ(fast.v[0].handle, 1u);
    EXPECT_EQ(fast.v[1].handle, n_children + 1u);
    EXPECT_EQ(fast.v[4].handle, 2u);
    // walking again adds nothing (all present)
    ck3accel::family::walk(e, s, &fast, Filter{true, false}, &e.chars[0], 0xFFFFFFFFu);
    EXPECT_EQ(fast.v.size(), n_children * 4u);
}
