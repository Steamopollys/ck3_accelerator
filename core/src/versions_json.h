#pragma once
#include <ck3accel/version_info.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ck3accel {

struct VersionsEntry {
    std::string version;                    // map key, e.g. "1.19.0.6"
    std::uint32_t pe_timestamp = 0;
    std::string text_sha256;
    bool tested = false;
    std::vector<std::string> auto_disable;
};

// load versions.json. nullopt on parse failure. placeholder entries
// ("PENDING_TASK_14") are skipped.
std::optional<std::vector<VersionsEntry>>
load_versions_json(const std::filesystem::path& path);

// match by PE timestamp + text hash. nullptr if none.
const VersionsEntry* find_match(
    const std::vector<VersionsEntry>& entries,
    std::uint32_t pe_timestamp,
    const std::string& text_sha256);

} // namespace ck3accel
