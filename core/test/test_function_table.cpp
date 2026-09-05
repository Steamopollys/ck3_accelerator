#include <gtest/gtest.h>
#include <ck3accel/function_table.h>

#include <cstdint>

using ck3accel::FunctionTable;

namespace {

// three non-adjacent functions with gaps:
//   fn A: [0x1000, 0x1100)
//   fn B: [0x2000, 0x2080)
//   fn C: [0x3000, 0x3000 + 0x40)
// inserted out of order; finalize() must sort.
FunctionTable three_function_table() {
    FunctionTable t;
    t.add(0x3000u, 0x3040u);   // C first
    t.add(0x1000u, 0x1100u);   // A
    t.add(0x2000u, 0x2080u);   // B
    t.finalize();
    return t;
}

} // namespace

TEST(FunctionTableTest, HitInMiddleReturnsBeginRva) {
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.lookup(0x1080u), 0x1000u);   // inside A
    EXPECT_EQ(t.lookup(0x2040u), 0x2000u);   // inside B
    EXPECT_EQ(t.lookup(0x3020u), 0x3000u);   // inside C
}

TEST(FunctionTableTest, HitAtBeginBoundaryIsInclusive) {
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.lookup(0x1000u), 0x1000u);   // begin is inclusive
    EXPECT_EQ(t.lookup(0x2000u), 0x2000u);
    EXPECT_EQ(t.lookup(0x3000u), 0x3000u);
}

TEST(FunctionTableTest, MissJustPastEndIsExclusive) {
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.lookup(0x1100u), 0u);        // end is exclusive -> in the gap after A
    EXPECT_EQ(t.lookup(0x1101u), 0u);        // deeper into the gap before B
    EXPECT_EQ(t.lookup(0x2080u), 0u);        // exactly at B's end -> gap before C
}

TEST(FunctionTableTest, RvaBeforeFirstFunctionReturnsZero) {
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.lookup(0x0000u), 0u);
    EXPECT_EQ(t.lookup(0x0FFFu), 0u);        // one byte before A's begin
}

TEST(FunctionTableTest, RvaAfterLastFunctionReturnsZero) {
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.lookup(0x3040u), 0u);        // exactly at C's end (exclusive)
    EXPECT_EQ(t.lookup(0xFFFFFFFFu), 0u);    // far past the last function
}

TEST(FunctionTableTest, UnsortedInsertionResolvedByFinalize) {
    // same data; assert size and a spread of lookups land right despite scrambled
    // insertion order.
    FunctionTable t = three_function_table();
    EXPECT_EQ(t.size(), 3u);
    EXPECT_EQ(t.lookup(0x10FFu), 0x1000u);   // last byte of A
    EXPECT_EQ(t.lookup(0x207Fu), 0x2000u);   // last byte of B
    EXPECT_EQ(t.lookup(0x303Fu), 0x3000u);   // last byte of C
}

TEST(FunctionTableTest, EmptyTableReturnsZeroEvenAfterFinalize) {
    FunctionTable t;
    t.finalize();
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.lookup(0x1000u), 0u);
}

TEST(FunctionTableTest, LookupBeforeFinalizeReturnsZero) {
    FunctionTable t;
    t.add(0x1000u, 0x1100u);
    // no finalize() yet: unsorted, so lookup refuses and returns 0.
    EXPECT_EQ(t.lookup(0x1080u), 0u);
    // finalize -> same lookup resolves.
    t.finalize();
    EXPECT_EQ(t.lookup(0x1080u), 0x1000u);
}

TEST(FunctionTableTest, DegenerateRangeIsIgnoredByAdd) {
    FunctionTable t;
    t.add(0x1000u, 0x1000u);   // zero-width: end == begin -> dropped
    t.add(0x2000u, 0x1FFFu);   // inverted: end < begin -> dropped
    t.add(0x3000u, 0x3010u);   // valid
    t.finalize();
    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(t.lookup(0x1000u), 0u);
    EXPECT_EQ(t.lookup(0x3008u), 0x3000u);
}
