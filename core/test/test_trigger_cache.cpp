// Tests for ck3accel/trigger_cache.h, the per-frame UI trigger-result cache:
//   ContextKey   : 128-bit hash of (node, this/prev/root refs, saved scopes, flags)
//   EpochCache   : fixed open-addressed table keyed by that hash + epoch
//   NodeFlagSet  : set of node pointers marked uncacheable
#include <ck3accel/trigger_cache.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using ck3accel::cache::ContextKey;
using ck3accel::cache::ContextView;
using ck3accel::cache::EpochCache;
using ck3accel::cache::NodeFlagSet;
using ck3accel::cache::SavedScope;
using ck3accel::cache::ScopeRef;

namespace {
ScopeRef ref(std::uint16_t type, std::uint64_t handle) { ScopeRef r{}; r.type = type; r.sub = 0; r.pad = 0; r.handle = handle; return r; }

ContextView basic(const ScopeRef* cur, const ScopeRef* prev, const ScopeRef* root, std::uint64_t node = 0x1000, std::uint8_t skip = 0) {
    ContextView v{};
    v.node = node; v.current = cur; v.prev = prev; v.root = root; v.skip_validation = skip;
    return v;
}
}  // namespace

// ---------------------------------------------------------------- ContextKey
TEST(ContextKey, SameInputsSameKey_AndEachComponentMatters) {
    const ScopeRef a = ref(4, 11), b = ref(4, 12), t = ref(7, 99);
    ContextView v = basic(&a, &b, &a);
    const ContextKey k0 = ck3accel::cache::make_key(v);
    EXPECT_EQ(k0, ck3accel::cache::make_key(v));

    ContextView v1 = v; v1.node = 0x2000;                    EXPECT_NE(k0, ck3accel::cache::make_key(v1));
    ContextView v2 = v; v2.current = &b;                     EXPECT_NE(k0, ck3accel::cache::make_key(v2));
    ContextView v3 = v; v3.prev = &a;                        EXPECT_NE(k0, ck3accel::cache::make_key(v3));
    ContextView v4 = v; v4.root = &t;                        EXPECT_NE(k0, ck3accel::cache::make_key(v4));
    ContextView v5 = v; v5.skip_validation = 1;              EXPECT_NE(k0, ck3accel::cache::make_key(v5));
    ContextView v6 = v; v6.prev = nullptr;                   EXPECT_NE(k0, ck3accel::cache::make_key(v6));
}

TEST(ContextKey, SavedScopesAreInTheKey_OrderIndependentIdsAndRefs) {
    const ScopeRef a = ref(4, 11);
    SavedScope s1{5, ref(4, 100)}, s2{6, ref(4, 200)}, s2b{6, ref(4, 201)}, s1_other_id{7, ref(4, 100)};
    ContextView v = basic(&a, nullptr, &a);
    v.saved = &s1; v.saved_count = 1;
    const ContextKey k1 = ck3accel::cache::make_key(v);
    ContextView v0 = basic(&a, nullptr, &a);                 // no saved scopes
    EXPECT_NE(k1, ck3accel::cache::make_key(v0));
    SavedScope two[2] = {s1, s2};
    ContextView v2 = v; v2.saved = two; v2.saved_count = 2;
    const ContextKey k2 = ck3accel::cache::make_key(v2);
    EXPECT_NE(k1, k2);
    SavedScope two_b[2] = {s1, s2b};                         // same id, different ref
    ContextView v3 = v2; v3.saved = two_b;
    EXPECT_NE(k2, ck3accel::cache::make_key(v3));
    ContextView v4 = v; v4.saved = &s1_other_id;             // same ref, different id
    EXPECT_NE(k1, ck3accel::cache::make_key(v4));
    // fallback store contributes too
    ContextView v5 = v; v5.fallback = &s2; v5.fallback_count = 1;
    EXPECT_NE(k1, ck3accel::cache::make_key(v5));
}

// ---------------------------------------------------------------- EpochCache
TEST(EpochCache, HitWithinEpoch_MissAfterEpochChange) {
    EpochCache<64> c;
    ContextKey k{0x1111, 0x2222};
    std::uint8_t r = 0xEE;
    EXPECT_FALSE(c.lookup(k, 7, &r));
    c.store(k, 7, 1);
    EXPECT_TRUE(c.lookup(k, 7, &r));
    EXPECT_EQ(r, 1);
    EXPECT_FALSE(c.lookup(k, 8, &r));          // new frame -> stale
    c.store(k, 8, 0);
    EXPECT_TRUE(c.lookup(k, 8, &r));
    EXPECT_EQ(r, 0);
}

TEST(EpochCache, DifferentKeysNeverAlias) {
    EpochCache<64> c;
    ContextKey a{0x1, 0x2}, b{0x1, 0x3}, d{0x9, 0x2};
    c.store(a, 1, 1);
    std::uint8_t r = 9;
    EXPECT_FALSE(c.lookup(b, 1, &r));
    EXPECT_FALSE(c.lookup(d, 1, &r));
    c.store(b, 1, 0);
    EXPECT_TRUE(c.lookup(a, 1, &r)); EXPECT_EQ(r, 1);
    EXPECT_TRUE(c.lookup(b, 1, &r)); EXPECT_EQ(r, 0);
}

TEST(EpochCache, FullTableOverflowsQuietlyAndStaleSlotsAreReused) {
    EpochCache<16> c;
    for (std::uint64_t i = 0; i < 64; ++i) c.store(ContextKey{i * 0x9E37ull, i}, 1, 1);
    EXPECT_GT(c.stats().overflow, 0u);
    // next epoch: all stale, so stores succeed again, no overflow growth
    const auto before = c.stats().overflow;
    for (std::uint64_t i = 0; i < 8; ++i) c.store(ContextKey{i * 0x9E37ull, i}, 2, 0);
    EXPECT_EQ(c.stats().overflow, before);
    std::uint8_t r;
    EXPECT_TRUE(c.lookup(ContextKey{0, 0}, 2, &r));
    EXPECT_EQ(r, 0);
}

// ---------------------------------------------------------------- NodeFlagSet
TEST(NodeFlagSet, MarkAndTestAndGrow) {
    NodeFlagSet s;
    EXPECT_FALSE(s.contains(0x10));
    s.insert(0x10);
    EXPECT_TRUE(s.contains(0x10));
    for (std::uint64_t i = 1; i < 20000; ++i) s.insert(i * 0x1000);
    EXPECT_TRUE(s.contains(0x10));
    EXPECT_TRUE(s.contains(19999 * 0x1000));
    EXPECT_FALSE(s.contains(0x18));
}
