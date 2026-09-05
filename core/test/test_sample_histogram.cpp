#include <gtest/gtest.h>
#include <ck3accel/sample_histogram.h>

#include <cstdint>
#include <vector>

using ck3accel::SampleHistogram;

TEST(SampleHistogramTest, CountsAccumulatePerRva) {
    SampleHistogram h;
    h.add(0x1000u);
    h.add(0x1000u);
    h.add(0x1000u);
    h.add(0x2000u);

    std::vector<SampleHistogram::Entry> top = h.top_n(10u);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].rva, 0x1000u);
    EXPECT_EQ(top[0].count, 3u);
    EXPECT_EQ(top[1].rva, 0x2000u);
    EXPECT_EQ(top[1].count, 1u);
}

TEST(SampleHistogramTest, TotalIsSumOfAllAdds) {
    SampleHistogram h;
    EXPECT_EQ(h.total(), 0u);
    h.add(0x1000u);
    h.add(0x1000u);
    h.add(0x2000u);
    h.add(0u);            // "ck3-other" is just another key
    EXPECT_EQ(h.total(), 4u);
}

TEST(SampleHistogramTest, TopNOrdersByCountDescending) {
    SampleHistogram h;
    for (int i = 0; i < 5; ++i) h.add(0x3000u);   // count 5
    for (int i = 0; i < 2; ++i) h.add(0x1000u);   // count 2
    h.add(0x2000u);                                // count 1

    std::vector<SampleHistogram::Entry> top = h.top_n(3u);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].rva, 0x3000u);
    EXPECT_EQ(top[0].count, 5u);
    EXPECT_EQ(top[1].rva, 0x1000u);
    EXPECT_EQ(top[1].count, 2u);
    EXPECT_EQ(top[2].rva, 0x2000u);
    EXPECT_EQ(top[2].count, 1u);
}

TEST(SampleHistogramTest, TopNTieBreaksByRvaAscending) {
    SampleHistogram h;
    // three RVAs, all count 2; order must be rva asc.
    h.add(0x3000u); h.add(0x3000u);
    h.add(0x1000u); h.add(0x1000u);
    h.add(0x2000u); h.add(0x2000u);

    std::vector<SampleHistogram::Entry> top = h.top_n(3u);
    ASSERT_EQ(top.size(), 3u);
    EXPECT_EQ(top[0].rva, 0x1000u);
    EXPECT_EQ(top[1].rva, 0x2000u);
    EXPECT_EQ(top[2].rva, 0x3000u);
    EXPECT_EQ(top[0].count, 2u);
    EXPECT_EQ(top[1].count, 2u);
    EXPECT_EQ(top[2].count, 2u);
}

TEST(SampleHistogramTest, TopNLargerThanSizeReturnsAll) {
    SampleHistogram h;
    h.add(0x1000u);
    h.add(0x2000u);
    // request more than we have: get every entry, no padding.
    std::vector<SampleHistogram::Entry> top = h.top_n(100u);
    EXPECT_EQ(top.size(), 2u);
}

TEST(SampleHistogramTest, EmptyHistogramReturnsEmpty) {
    SampleHistogram h;
    EXPECT_EQ(h.total(), 0u);
    EXPECT_TRUE(h.top_n(10u).empty());
}

TEST(SampleHistogramTest, TopNZeroReturnsEmpty) {
    SampleHistogram h;
    h.add(0x1000u);
    h.add(0x2000u);
    EXPECT_TRUE(h.top_n(0u).empty());
}
