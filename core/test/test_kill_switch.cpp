#include <gtest/gtest.h>
#include "kill_switch.h"

#include <windows.h>

// --- parse_hotkey: valid combos ---------------------------------------------

TEST(KillSwitchTest, ParseHotkeyFullCombo) {
    auto hk = ck3accel::parse_hotkey("Ctrl+Shift+F12");
    ASSERT_TRUE(hk.valid);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_TRUE(hk.shift);
    EXPECT_FALSE(hk.alt);
    EXPECT_EQ(hk.vk, VK_F12);
}

TEST(KillSwitchTest, ParseHotkeyAltLetterCaseInsensitive) {
    auto hk = ck3accel::parse_hotkey("alt+k");
    ASSERT_TRUE(hk.valid);
    EXPECT_FALSE(hk.ctrl);
    EXPECT_FALSE(hk.shift);
    EXPECT_TRUE(hk.alt);
    EXPECT_EQ(hk.vk, static_cast<int>('K'));
}

TEST(KillSwitchTest, ParseHotkeyBareDigit) {
    auto hk = ck3accel::parse_hotkey("5");
    ASSERT_TRUE(hk.valid);
    EXPECT_FALSE(hk.ctrl);
    EXPECT_FALSE(hk.shift);
    EXPECT_FALSE(hk.alt);
    EXPECT_EQ(hk.vk, static_cast<int>('5'));
}

TEST(KillSwitchTest, ParseHotkeyControlAliasAndSpaces) {
    auto hk = ck3accel::parse_hotkey("  Control + F1 ");
    ASSERT_TRUE(hk.valid);
    EXPECT_TRUE(hk.ctrl);
    EXPECT_FALSE(hk.shift);
    EXPECT_FALSE(hk.alt);
    EXPECT_EQ(hk.vk, VK_F1);
}

TEST(KillSwitchTest, ParseHotkeyHighFunctionKey) {
    auto hk = ck3accel::parse_hotkey("F24");
    ASSERT_TRUE(hk.valid);
    EXPECT_EQ(hk.vk, VK_F24);
}

// --- parse_hotkey: invalid input --------------------------------------------

TEST(KillSwitchTest, ParseHotkeyEmptyIsInvalid) {
    EXPECT_FALSE(ck3accel::parse_hotkey("").valid);
    EXPECT_FALSE(ck3accel::parse_hotkey("   ").valid);
}

TEST(KillSwitchTest, ParseHotkeyModifiersOnlyIsInvalid) {
    EXPECT_FALSE(ck3accel::parse_hotkey("Ctrl+Shift").valid);
}

TEST(KillSwitchTest, ParseHotkeyUnknownTokenIsInvalid) {
    EXPECT_FALSE(ck3accel::parse_hotkey("Ctrl+Banana").valid);
    EXPECT_FALSE(ck3accel::parse_hotkey("F25").valid);
    EXPECT_FALSE(ck3accel::parse_hotkey("F0").valid);
}

TEST(KillSwitchTest, ParseHotkeyTwoMainKeysIsInvalid) {
    EXPECT_FALSE(ck3accel::parse_hotkey("F12+F11").valid);
}

// --- detect_edge ------------------------------------------------------------

TEST(KillSwitchTest, DetectEdgeFiresOnceOnRisingEdge) {
    bool was_down = false;
    EXPECT_TRUE(ck3accel::detect_edge(true, was_down));   // rising edge fires
    EXPECT_TRUE(was_down);
}

TEST(KillSwitchTest, DetectEdgeStaysLowWhileHeld) {
    bool was_down = false;
    EXPECT_TRUE(ck3accel::detect_edge(true, was_down));    // first press
    EXPECT_FALSE(ck3accel::detect_edge(true, was_down));   // still held
    EXPECT_FALSE(ck3accel::detect_edge(true, was_down));   // still held
}

TEST(KillSwitchTest, DetectEdgeRefiresAfterRelease) {
    bool was_down = false;
    EXPECT_TRUE(ck3accel::detect_edge(true, was_down));    // press
    EXPECT_FALSE(ck3accel::detect_edge(false, was_down));  // release (no fire)
    EXPECT_FALSE(was_down);
    EXPECT_TRUE(ck3accel::detect_edge(true, was_down));    // press again -> fires
}

TEST(KillSwitchTest, DetectEdgeNeverFiresWhileUnpressed) {
    bool was_down = false;
    EXPECT_FALSE(ck3accel::detect_edge(false, was_down));
    EXPECT_FALSE(ck3accel::detect_edge(false, was_down));
    EXPECT_FALSE(was_down);
}

// --- is_kill_switch_active default ------------------------------------------

TEST(KillSwitchTest, NotActiveByDefault) {
    EXPECT_EQ(ck3accel::is_kill_switch_active(), 0);
}
