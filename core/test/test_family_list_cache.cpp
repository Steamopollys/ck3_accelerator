// Tests for ck3accel/family_list_cache.h, the per-frame family-list memoization.
#include <ck3accel/family_list_cache.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using ck3accel::famcache::Entry;
using ck3accel::famcache::ListCache;
using ck3accel::famcache::make_key;

namespace {
Entry ent(std::uint64_t h) { Entry e{}; e.type = 4; e.sub = 0; e.pad = 0; e.handle = h; return e; }
std::vector<Entry> run(std::size_t n) { std::vector<Entry> v; for (std::size_t i = 0; i < n; ++i) v.push_back(ent(100 + i)); return v; }
}  // namespace

TEST(FamCacheKey, DependsOnCharacterSelectorAndFilter) {
    const std::uint64_t base = make_key(500, 0, 0);
    EXPECT_EQ(base, make_key(500, 0, 0));
    EXPECT_NE(base, make_key(501, 0, 0));   // different character
    EXPECT_NE(base, make_key(500, 1, 0));   // different builder
    EXPECT_NE(base, make_key(500, 0, 1));   // different filter (alive vs dead)
    EXPECT_NE(0u, make_key(0, 0, 0));       // never the empty marker
}

TEST(FamCache, StoreThenHitWithinEpoch_ReturnsEntries) {
    ListCache<64> c;
    const auto built = run(5);
    const std::uint64_t k = make_key(7, 2, 1);
    std::size_t n = 0; const Entry* got = nullptr;
    EXPECT_FALSE(c.lookup(k, 10, &got, &n));
    c.store(k, 10, built.data(), built.size());
    ASSERT_TRUE(c.lookup(k, 10, &got, &n));
    ASSERT_EQ(n, 5u);
    for (std::size_t i = 0; i < n; ++i) { EXPECT_EQ(got[i].type, 4); EXPECT_EQ(got[i].handle, 100 + i); }
}

TEST(FamCache, NewEpochInvalidates) {
    ListCache<64> c;
    const auto built = run(3);
    const std::uint64_t k = make_key(1, 0, 0);
    c.store(k, 5, built.data(), built.size());
    std::size_t n = 0; const Entry* got = nullptr;
    EXPECT_TRUE(c.lookup(k, 5, &got, &n));
    EXPECT_FALSE(c.lookup(k, 6, &got, &n));   // stale
}

TEST(FamCache, HugeListIsCached_NoLengthCeiling) {
    ListCache<64> c;
    const auto huge = run(20000);   // immortal's close-or-extended family
    const std::uint64_t k = make_key(2, 2, 1);
    c.store(k, 1, huge.data(), huge.size());
    std::size_t n = 0; const Entry* got = nullptr;
    ASSERT_TRUE(c.lookup(k, 1, &got, &n));
    EXPECT_EQ(n, 20000u);
    EXPECT_EQ(got[19999].handle, 100u + 19999u);
}

TEST(FamCache, EmptyListRoundTrips) {
    ListCache<64> c;
    const std::uint64_t k = make_key(9, 0, 1);
    c.store(k, 2, nullptr, 0);
    std::size_t n = 99; const Entry* got = nullptr;
    ASSERT_TRUE(c.lookup(k, 2, &got, &n));   // "no relatives" is a cacheable answer, distinct from a miss
    EXPECT_EQ(n, 0u);
}

TEST(FamCache, CollidingKeysEvictNotAlias) {
    ListCache<2> c;   // 2 slots -> forced collisions
    std::size_t n = 0; const Entry* got = nullptr;
    for (std::uint32_t ch = 0; ch < 8; ++ch) { const auto b = run(1 + ch % 4); c.store(make_key(ch, 0, 0), 1, b.data(), b.size()); }
    for (std::uint32_t ch = 0; ch < 8; ++ch) {
        if (c.lookup(make_key(ch, 0, 0), 1, &got, &n)) { EXPECT_EQ(n, 1u + ch % 4); EXPECT_EQ(got[0].handle, 100u); }
    }
    EXPECT_GT(c.stats().evict, 0u);
}

TEST(FamCache, ReStoreSameKeyUpdatesInPlace) {
    ListCache<64> c;
    const std::uint64_t k = make_key(3, 1, 0);
    c.store(k, 1, run(2).data(), 2);
    c.store(k, 2, run(7).data(), 7);         // next epoch, longer list, same key
    std::size_t n = 0; const Entry* got = nullptr;
    ASSERT_TRUE(c.lookup(k, 2, &got, &n));
    EXPECT_EQ(n, 7u);
    EXPECT_EQ(c.stats().evict, 0u);          // same key is not an eviction
}
