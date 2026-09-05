#include <ck3accel/long_day_ring.h>
#include <gtest/gtest.h>

using namespace ck3accel;

static LongDayRecord rec(int day, double wall_ms) {
    LongDayRecord r; r.day_index = day; r.wall_ms = wall_ms; return r;
}

TEST(LongDayRing, KeepsLastNAndOutlierStandsOut) {
    LongDayRing<4> ring;
    ring.push(rec(1, 8.0));
    ring.push(rec(2, 9.0));
    ring.push(rec(3, 350.0));   // the freeze (child birth)
    ring.push(rec(4, 7.0));
    ring.push(rec(5, 8.0));     // overwrites day 1 (capacity 4)
    EXPECT_EQ(ring.size(), 4u);

    double max_ms = 0.0; int max_day = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const LongDayRecord& r = ring.at(i);
        if (r.wall_ms > max_ms) { max_ms = r.wall_ms; max_day = static_cast<int>(r.day_index); }
    }
    EXPECT_EQ(max_day, 3);          // freeze retained, stands out
    EXPECT_DOUBLE_EQ(max_ms, 350.0);
}

TEST(LongDayRing, NewestIsMostRecentlyPushed) {
    LongDayRing<2> ring;
    ring.push(rec(1, 1.0));
    ring.push(rec(2, 2.0));
    ring.push(rec(3, 3.0));
    EXPECT_EQ(ring.size(), 2u);
    EXPECT_EQ(ring.newest().day_index, 3);
}
