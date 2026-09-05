#include <gtest/gtest.h>
#include "plugin_gate.h"

#include <ck3accel/core_api.h>

#include <cstdint>
#include <string>

namespace {

using ck3accel::GateDecision;
using ck3accel::SessionContext;

// plugin that passes every gate with the defaults below.
CK3AccelPluginInfo good_info() {
    CK3AccelPluginInfo info{};
    info.struct_size      = sizeof(CK3AccelPluginInfo);
    info.magic            = CK3ACCEL_PLUGIN_MAGIC;
    info.required_abi     = CK3ACCEL_ABI_VERSION;
    info.name             = "accel_save_load";
    info.semver           = "0.1.0";
    info.min_game_version = "1.19.0.6";
    info.max_game_version = "1.19.0.6";
    info.mode_flags       = CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN;
    return info;
}

SessionContext sp_context() {
    SessionContext ctx;
    ctx.game_version = "1.19.0.6";
    ctx.session_mode = CK3ACCEL_MODE_SP;
    return ctx;
}

} // namespace

TEST(CompareVersionsTest, Equal) {
    EXPECT_EQ(ck3accel::compare_versions("1.19.0.6", "1.19.0.6"), 0);
}

TEST(CompareVersionsTest, LessAndGreater) {
    EXPECT_EQ(ck3accel::compare_versions("1.19.0.5", "1.19.0.6"), -1);
    EXPECT_EQ(ck3accel::compare_versions("1.19.0.7", "1.19.0.6"), 1);
    EXPECT_EQ(ck3accel::compare_versions("1.20.0.0", "1.19.9.9"), 1);
}

TEST(CompareVersionsTest, MissingComponentsTreatedAsZero) {
    EXPECT_EQ(ck3accel::compare_versions("1.19", "1.19.0.0"), 0);
    EXPECT_EQ(ck3accel::compare_versions("1.19", "1.19.0.1"), -1);
    EXPECT_EQ(ck3accel::compare_versions("2", "1.99.99.99"), 1);
}

TEST(CompareVersionsTest, NonNumericComponentsCompareAsZero) {
    EXPECT_EQ(ck3accel::compare_versions("1.x.0.6", "1.0.0.6"), 0);
    EXPECT_EQ(ck3accel::compare_versions("any", "0"), 0);
}

TEST(GateDecisionStrTest, EveryValueHasAStableName) {
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::Accept), "Accept");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectNotAllowlisted),
                 "RejectNotAllowlisted");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectAutoDisabled),
                 "RejectAutoDisabled");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectMagic), "RejectMagic");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectAbi), "RejectAbi");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectGameVersion),
                 "RejectGameVersion");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectMode), "RejectMode");
    EXPECT_STREQ(ck3accel::gate_decision_str(GateDecision::RejectBadContext),
                 "RejectBadContext");
}

TEST(PluginGateTest, AcceptsAGoodPlugin) {
    auto info = good_info();
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::Accept);
}

TEST(PluginGateTest, RejectsWhenNotAllowlisted) {
    auto info = good_info();
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/false, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectNotAllowlisted);
}

TEST(PluginGateTest, RejectsWhenAutoDisabled) {
    auto info = good_info();
    // allowlist passes, auto_disable wins (checked second).
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/true);
    EXPECT_EQ(d, GateDecision::RejectAutoDisabled);
}

TEST(PluginGateTest, RejectsBadMagic) {
    auto info = good_info();
    info.magic = 0xDEADBEEFu;
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectMagic);
}

TEST(PluginGateTest, RejectsAbiTooNew) {
    auto info = good_info();
    info.required_abi = CK3ACCEL_ABI_VERSION + 1u;  // wants a newer host than we are
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectAbi);
}

TEST(PluginGateTest, RejectsGameVersionBelowMin) {
    auto info = good_info();
    info.min_game_version = "1.19.0.7";  // session is 1.19.0.6 -> below min
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectGameVersion);
}

TEST(PluginGateTest, RejectsGameVersionAboveMax) {
    auto info = good_info();
    info.max_game_version = "1.19.0.5";  // session is 1.19.0.6 -> above max
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectGameVersion);
}

TEST(PluginGateTest, AcceptsWithAnyWildcardBounds) {
    auto info = good_info();
    info.min_game_version = "any";
    info.max_game_version = "any";
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::Accept);
}

TEST(PluginGateTest, SkipsVersionGateWhenSessionVersionEmpty) {
    auto info = good_info();
    info.min_game_version = "1.19.0.7";  // would reject if version were known
    info.max_game_version = "1.19.0.9";
    SessionContext ctx;
    ctx.game_version = "";                // Unknown build -> version gate skipped
    ctx.session_mode = CK3ACCEL_MODE_SP;
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, ctx,
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::Accept);
}

TEST(PluginGateTest, RejectsModeMismatch) {
    auto info = good_info();
    info.mode_flags = CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN;  // MP not supported
    SessionContext ctx;
    ctx.game_version = "1.19.0.6";
    ctx.session_mode = CK3ACCEL_MODE_MULTIPLAYER;
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, ctx,
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectMode);
}

TEST(PluginGateTest, RejectsPluginWithNoSupportedModes) {
    // well-formed context, but plugin declares mode_flags == 0 (supports nothing).
    auto info = good_info();
    info.mode_flags = 0u;
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, sp_context(),
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectMode);
}

TEST(PluginGateTest, RejectsBadContextWhenSessionModeIsZero) {
    // session_mode == 0 is a caller bug: must give RejectBadContext, not RejectMode.
    auto info = good_info();
    SessionContext ctx;
    ctx.game_version = "1.19.0.6";
    ctx.session_mode = 0u;  // malformed: no mode bit set
    auto d = ck3accel::evaluate_plugin(info, CK3ACCEL_ABI_VERSION, ctx,
                                       /*allowlisted=*/true, /*auto_disabled=*/false);
    EXPECT_EQ(d, GateDecision::RejectBadContext);
}
