#include <gtest/gtest.h>
#include "hook_engine.h"

#include <cstdint>

namespace {

// non-trivial body so MinHook has >= 5 prologue bytes to relocate. The volatile
// read keeps it from folding to a constant.
volatile int g_sink = 0;

__declspec(noinline) int target_fn(int x) {
    g_sink = x;
    return x;
}

// call-original pointer, filled in by install_hook.
using target_fn_t = int (*)(int);
target_fn_t g_trampoline = nullptr;

// detour: run original via trampoline, +100.
int detour_fn(int x) {
    return g_trampoline(x) + 100;
}

// call through a volatile pointer so the callee can't inline past the patched
// entry bytes.
int call_target(int x) {
    volatile target_fn_t fn = &target_fn;
    return fn(x);
}

} // namespace

TEST(HookEngineTest, RegisterHookSetIsMonotonic) {
    ck3accel::HookSetId a = ck3accel::register_hook_set("set_a");
    ck3accel::HookSetId b = ck3accel::register_hook_set("set_b");
    EXPECT_GT(a, ck3accel::kInvalidHookSet);
    EXPECT_GT(b, a);
}

TEST(HookEngineTest, InitIsIdempotent) {
    EXPECT_TRUE(ck3accel::hook_engine_init());
    EXPECT_TRUE(ck3accel::hook_engine_init());
    ck3accel::hook_engine_shutdown();
}

TEST(HookEngineTest, InstallDisableEnableRemoveLifecycle) {
    ASSERT_TRUE(ck3accel::hook_engine_init());

    // baseline: no hook yet.
    EXPECT_EQ(call_target(5), 5);

    const ck3accel::HookSetId set = ck3accel::register_hook_set("lifecycle");
    g_trampoline = nullptr;

    ck3accel::HookHandle* h = ck3accel::install_hook(
        set,
        reinterpret_cast<void*>(&target_fn),
        reinterpret_cast<void*>(&detour_fn),
        reinterpret_cast<void**>(&g_trampoline));
    ASSERT_NE(h, nullptr);
    ASSERT_NE(g_trampoline, nullptr);

    // detour live: original (5) + 100.
    EXPECT_EQ(call_target(5), 105);

    // trampoline address is in the hooked range; a stack address isn't.
    int stack_local = 0;
    EXPECT_TRUE(ck3accel::address_in_hooked_range(
        reinterpret_cast<const void*>(g_trampoline)));
    EXPECT_FALSE(ck3accel::address_in_hooked_range(&stack_local));

    // soft-disable: falls through to original.
    ck3accel::disable_set(set);
    EXPECT_EQ(call_target(5), 5);

    // re-enable: detour again.
    ck3accel::enable_set(set);
    EXPECT_EQ(call_target(5), 105);

    // teardown: original bytes restored.
    ck3accel::remove_set(set);
    EXPECT_EQ(call_target(5), 5);

    ck3accel::hook_engine_shutdown();
}
