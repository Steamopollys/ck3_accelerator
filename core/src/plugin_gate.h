#pragma once
#include <ck3accel/core_api.h>
#include <cstdint>
#include <string>

namespace ck3accel {

enum class GateDecision {
    Accept,
    RejectNotAllowlisted,
    RejectAutoDisabled,
    RejectMagic,
    RejectAbi,
    RejectGameVersion,
    RejectMode,
    RejectBadContext,
};

struct SessionContext {
    std::string   game_version;   // detected; may be empty if Unknown
    std::uint32_t session_mode;   // exactly one CK3ACCEL_MODE_* bit
};

// compare dotted versions ("1.19.0.6"): -1, 0, +1. missing/non-numeric components = 0.
int compare_versions(const std::string& a, const std::string& b);

// gate decision. allowlisted = name true in config [plugins]; auto_disabled =
// name in this build's versions.json auto_disable list.
GateDecision evaluate_plugin(const CK3AccelPluginInfo& info,
                             std::uint32_t host_abi,
                             const SessionContext& ctx,
                             bool allowlisted,
                             bool auto_disabled);

const char* gate_decision_str(GateDecision d);

} // namespace ck3accel
