// Tests for ck3accel/attrib_tables.h, the two header-only tables behind the
// attribution probe: NodeTable (per-node count + inclusive cycles, top-N) and
// StackTable (call-stack chains -> count, top-N). Both fixed-capacity, alloc-free
// on the record path.
#include <ck3accel/attrib_tables.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using ck3accel::NodeTable;
using ck3accel::StackTable;

// ---------------------------------------------------------------- NodeTable
TEST(NodeTable, AccumulatesCountAndCyclesPerKey) {
    NodeTable<64> t;
    t.record(0x1000, 10);
    t.record(0x1000, 5);
    t.record(0x2000, 7);
    EXPECT_EQ(t.total_records(), 3u);
    EXPECT_EQ(t.distinct(), 2u);
    const auto top = t.top_n(10);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].key, 0x1000u);   // sorted by count desc
    EXPECT_EQ(top[0].count, 2u);
    EXPECT_EQ(top[0].cycles, 15u);
    EXPECT_EQ(top[1].key, 0x2000u);
    EXPECT_EQ(top[1].count, 1u);
}

TEST(NodeTable, KeyZeroIsAnOrdinaryKey) {
    NodeTable<64> t;
    t.record(0, 1);
    t.record(0, 1);
    EXPECT_EQ(t.distinct(), 1u);
    EXPECT_EQ(t.top_n(1)[0].count, 2u);
}

TEST(NodeTable, OverflowIsCountedNotSilent) {
    NodeTable<4> t;   // capacity 4 slots
    for (std::uint64_t k = 1; k <= 8; ++k) t.record(k * 0x10, 1);
    EXPECT_GT(t.overflow(), 0u);
    EXPECT_LE(t.distinct(), 4u);
}

TEST(NodeTable, MergeFromAndReset) {
    NodeTable<64> a, b;
    a.record(1, 3); a.record(2, 4);
    b.record(2, 6); b.record(3, 1);
    a.merge_from(b);
    const auto top = a.top_n(10);
    ASSERT_EQ(top.size(), 3u);
    // key 2: count 2, cycles 10
    bool found = false;
    for (const auto& e : top) if (e.key == 2) { found = true; EXPECT_EQ(e.count, 2u); EXPECT_EQ(e.cycles, 10u); }
    EXPECT_TRUE(found);
    a.reset();
    EXPECT_EQ(a.distinct(), 0u);
    EXPECT_EQ(a.total_records(), 0u);
    EXPECT_TRUE(a.top_n(10).empty());
}

TEST(NodeTable, TopNOrdersByCountThenCycles) {
    NodeTable<64> t;
    t.record(1, 100);              // count 1, cycles 100
    t.record(2, 1); t.record(2, 1);// count 2
    t.record(3, 500);              // count 1, cycles 500
    const auto top = t.top_n(3);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].key, 2u);
    EXPECT_EQ(top[1].key, 3u);     // same count as key 1, more cycles first
    EXPECT_EQ(top[2].key, 1u);
}

// ---------------------------------------------------------------- StackTable
TEST(StackTable, IdenticalChainsAggregateDifferentChainsDoNot) {
    StackTable<64, 8> t;
    const std::uint32_t a[] = {0x10, 0x20, 0x30};
    const std::uint32_t b[] = {0x10, 0x20, 0x31};
    t.record(a, 3);
    t.record(a, 3);
    t.record(b, 3);
    const auto top = t.top_n(10);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].count, 2u);
    ASSERT_EQ(top[0].depth, 3u);
    EXPECT_EQ(top[0].frames[2], 0x30u);
    EXPECT_EQ(top[1].count, 1u);
}

TEST(StackTable, ChainLongerThanMaxDepthIsTruncatedNotDropped) {
    StackTable<64, 4> t;
    const std::uint32_t a[] = {1, 2, 3, 4, 5, 6};
    t.record(a, 6);
    const auto top = t.top_n(1);
    ASSERT_EQ(top.size(), 1u);
    EXPECT_EQ(top[0].depth, 4u);
    EXPECT_EQ(top[0].frames[3], 4u);
}

TEST(StackTable, EmptyChainIsIgnored) {
    StackTable<64, 8> t;
    t.record(nullptr, 0);
    EXPECT_TRUE(t.top_n(5).empty());
    EXPECT_EQ(t.total(), 0u);
}

TEST(StackTable, OverflowCountedAndResetClears) {
    StackTable<2, 4> t;
    for (std::uint32_t k = 1; k <= 6; ++k) { const std::uint32_t c[] = {k, k + 1}; t.record(c, 2); }
    EXPECT_GT(t.overflow(), 0u);
    t.reset();
    EXPECT_EQ(t.total(), 0u);
    EXPECT_EQ(t.overflow(), 0u);
    EXPECT_TRUE(t.top_n(5).empty());
}
