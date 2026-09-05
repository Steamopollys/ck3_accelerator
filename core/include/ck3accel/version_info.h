#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ck3accel {

enum class DetectionStatus {
    KnownTested,        // version in versions.json, all plugins allowed
    KnownUntested,      // version in versions.json, some plugins auto-disabled
    Unknown,            // not in versions.json; observe-only mode
};

struct VersionInfo {
    DetectionStatus status = DetectionStatus::Unknown;
    std::string version;                       // e.g. "1.19.0.6"
    std::uint32_t pe_timestamp = 0;
    std::string text_sha256;                   // hex
    std::vector<std::string> auto_disable;     // plugin names to skip
};

} // namespace ck3accel
