#include <ck3accel/eval_accumulators.h>
#include <gtest/gtest.h>

using namespace ck3accel;

TEST(HookAccumulator, FlatCallsCountAndSumInclusive) {
    HookAccumulator a;
    a.on_enter(100); a.on_exit(130);   // 30
    a.on_enter(200); a.on_exit(205);   // 5
    EXPECT_EQ(a.count, 2u);
    EXPECT_EQ(a.incl_tsc, 35u);
}

TEST(HookAccumulator, RecursionCountsEveryCallButInclusiveOnlyOutermost) {
    HookAccumulator a;
    // outer [100,200], nested [110,120].
    a.on_enter(100);
    a.on_enter(110);
    a.on_exit(120);
    a.on_exit(200);
    EXPECT_EQ(a.count, 2u);            // every call counted
    EXPECT_EQ(a.incl_tsc, 100u);       // only the outermost [100,200], no double-count
}

TEST(HookAccumulator, UnbalancedExitIsIgnored) {
    HookAccumulator a;
    a.on_exit(50);                     // exit with depth==0: no-op, no underflow
    EXPECT_EQ(a.count, 0u);
    EXPECT_EQ(a.incl_tsc, 0u);
}

TEST(ShardRegistry, SnapshotSumsAllRegisteredShards) {
    ShardRegistry reg;
    HookAccumulator s1[HOOK_COUNT] = {};
    HookAccumulator s2[HOOK_COUNT] = {};
    s1[HOOK_TRIGGER].count = 3; s1[HOOK_TRIGGER].incl_tsc = 300;
    s2[HOOK_TRIGGER].count = 4; s2[HOOK_TRIGGER].incl_tsc = 400;
    s2[HOOK_DISPATCH].count = 1; s2[HOOK_DISPATCH].incl_tsc = 10;
    reg.register_shard(s1);
    reg.register_shard(s2);
    reg.register_shard(s2);  // idempotent: same pointer not double-counted

    HookStats out[HOOK_COUNT] = {};
    reg.snapshot(out, HOOK_COUNT);
    EXPECT_EQ(out[HOOK_TRIGGER].count, 7u);
    EXPECT_EQ(out[HOOK_TRIGGER].incl_tsc, 700u);
    EXPECT_EQ(out[HOOK_DISPATCH].count, 1u);
    EXPECT_EQ(out[HOOK_DISPATCH].incl_tsc, 10u);
}
