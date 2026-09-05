#include <ck3accel/repeat_table.h>
#include <gtest/gtest.h>

using ck3accel::RepeatTable;
using ck3accel::RepeatTally;

TEST(RepeatTable, DistinctRepeatAndTotal) {
    RepeatTable<16> t;
    t.record(100, 1);             // distinct key 100
    t.record(100, 1);             // repeat (consistent)
    t.record(200, 0);             // distinct key 200
    const RepeatTally r = t.tally();
    EXPECT_EQ(r.total, 3u);
    EXPECT_EQ(r.distinct, 2u);
    EXPECT_EQ(r.repeated_keys, 1u);     // only key 100 had count>1
    EXPECT_EQ(r.inconsistent_keys, 0u);
    EXPECT_EQ(r.overflow, 0u);
}

TEST(RepeatTable, InconsistentResultFlagged) {
    RepeatTable<16> t;
    t.record(100, 1);
    t.record(100, 0);             // same key, different result -> inconsistent
    const RepeatTally r = t.tally();
    EXPECT_EQ(r.distinct, 1u);
    EXPECT_EQ(r.repeated_keys, 1u);
    EXPECT_EQ(r.inconsistent_keys, 1u);
}

TEST(RepeatTable, ZeroKeyIsHandled) {
    RepeatTable<16> t;
    t.record(0, 1);
    t.record(0, 1);               // key 0 must not be confused with 'empty'
    const RepeatTally r = t.tally();
    EXPECT_EQ(r.distinct, 1u);
    EXPECT_EQ(r.total, 2u);
}

TEST(RepeatTable, OverflowCountedNotSilent) {
    RepeatTable<2> t;             // capacity 2
    t.record(1, 1);
    t.record(2, 1);
    t.record(3, 1);               // table full -> overflow
    const RepeatTally r = t.tally();
    EXPECT_EQ(r.distinct, 2u);
    EXPECT_EQ(r.overflow, 1u);
}

TEST(RepeatTable, MergeCombinesCountsAndCrossShardInconsistency) {
    RepeatTable<16> a, b;
    a.record(100, 1); a.record(100, 1);   // a: key100 count2 result1
    b.record(100, 0);                      // b: key100 count1 result0
    b.record(300, 1);                      // b: key300 count1
    RepeatTable<16> merged;
    merged.merge_from(a);
    merged.merge_from(b);
    const RepeatTally r = merged.tally();
    EXPECT_EQ(r.distinct, 2u);             // keys 100, 300
    EXPECT_EQ(r.total, 4u);                // 2 + 1 + 1
    EXPECT_EQ(r.inconsistent_keys, 1u);    // key100 returned both 1 and 0 across shards
}

TEST(RepeatTable, ResetClears) {
    RepeatTable<16> t;
    t.record(100, 1);
    t.reset();
    EXPECT_EQ(t.tally().distinct, 0u);
}

// bounded probe length turns "table full" into counted overflow, not a scan of
// every slot per record. Unbounded, a full 1M-slot table costs ~1 ms per new key
// (the probe's save-load slowdown, 2026-09-02).
TEST(RepeatTable, BoundedProbeOverflowsInsteadOfScanningWholeTable) {
    RepeatTable<1024, 8> bounded;           // capacity 1024, at most 8 probes per record
    for (std::uint64_t k = 1; k <= 1024; ++k) bounded.record(k * 0x9E3779B97F4A7C15ull, 1);
    const RepeatTally b = bounded.tally();
    EXPECT_GT(b.overflow, 0u);              // could not place everything within 8 probes
    EXPECT_LT(b.distinct, 1024u);
    EXPECT_EQ(b.total + b.overflow, 1024u); // every record is either counted or overflowed

    RepeatTable<1024> unbounded;            // default: probe length == capacity
    for (std::uint64_t k = 1; k <= 1024; ++k) unbounded.record(k * 0x9E3779B97F4A7C15ull, 1);
    const RepeatTally u = unbounded.tally();
    EXPECT_EQ(u.overflow, 0u);
    EXPECT_EQ(u.distinct, 1024u);
}
