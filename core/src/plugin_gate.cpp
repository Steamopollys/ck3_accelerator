#include "plugin_gate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ck3accel {

namespace {

constexpr const char* kAnyVersion = "any";

// parse one dotted version component to an int. leading digits consumed;
// non-digit/empty = 0. clamps at 1e15 to avoid overflow UB on a huge component.
uint64_t parse_component(const std::string& s) {
    constexpr uint64_t kMax = 1'000'000'000'000'000ULL; // 1e15 sentinel cap
    uint64_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            break;  // non-numeric component compares as 0 from here on
        }
        value = value * 10u + static_cast<uint64_t>(c - '0');
        if (value > kMax) {
            value = kMax;
            break;
        }
    }
    return value;
}

std::vector<std::string> split_dotted(const std::string& v) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : v) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    const std::vector<std::string> pa = split_dotted(a);
    const std::vector<std::string> pb = split_dotted(b);
    const std::size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t va = i < pa.size() ? parse_component(pa[i]) : 0u;
        const uint64_t vb = i < pb.size() ? parse_component(pb[i]) : 0u;
        if (va < vb) return -1;
        if (va > vb) return 1;
    }
    return 0;
}

GateDecision evaluate_plugin(const CK3AccelPluginInfo& info,
                             std::uint32_t host_abi,
                             const SessionContext& ctx,
                             bool allowlisted,
                             bool auto_disabled) {
    // 0. zero session_mode is a caller bug; guard it so (mode_flags & 0)==0
    //    doesn't silently RejectMode every plugin with no diagnostic.
    if (ctx.session_mode == 0u) {
        return GateDecision::RejectBadContext;
    }

    // 1. allowlist: name present and true in config [plugins].
    if (!allowlisted) {
        return GateDecision::RejectNotAllowlisted;
    }

    // 2. auto_disable: named in this build's versions.json list.
    if (auto_disabled) {
        return GateDecision::RejectAutoDisabled;
    }

    // 3. Magic sentinel.
    if (info.magic != CK3ACCEL_PLUGIN_MAGIC) {
        return GateDecision::RejectMagic;
    }

    // 4. ABI: refuse a plugin needing a newer host than us.
    if (info.required_abi > host_abi) {
        return GateDecision::RejectAbi;
    }

    // 5. game-version range (skipped when build is Unknown).
    if (!ctx.game_version.empty()) {
        const std::string min_v = info.min_game_version ? info.min_game_version : kAnyVersion;
        const std::string max_v = info.max_game_version ? info.max_game_version : kAnyVersion;
        if (min_v != kAnyVersion && !min_v.empty() && compare_versions(ctx.game_version, min_v) < 0) {
            return GateDecision::RejectGameVersion;
        }
        if (max_v != kAnyVersion && !max_v.empty() && compare_versions(ctx.game_version, max_v) > 0) {
            return GateDecision::RejectGameVersion;
        }
    }

    // 6. session mode: plugin must support the current mode.
    if ((info.mode_flags & ctx.session_mode) == 0u) {
        return GateDecision::RejectMode;
    }

    return GateDecision::Accept;
}

const char* gate_decision_str(GateDecision d) {
    switch (d) {
        case GateDecision::Accept:               return "Accept";
        case GateDecision::RejectNotAllowlisted: return "RejectNotAllowlisted";
        case GateDecision::RejectAutoDisabled:   return "RejectAutoDisabled";
        case GateDecision::RejectMagic:          return "RejectMagic";
        case GateDecision::RejectAbi:            return "RejectAbi";
        case GateDecision::RejectGameVersion:    return "RejectGameVersion";
        case GateDecision::RejectMode:           return "RejectMode";
        case GateDecision::RejectBadContext:     return "RejectBadContext";
    }
    return "Unknown";
}

} // namespace ck3accel
