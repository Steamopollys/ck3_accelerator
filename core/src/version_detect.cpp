#include "version_detect.h"
#include "pe_inspect.h"
#include "versions_json.h"
#include "paths.h"

#include <picosha2.h>

#include <algorithm>
#include <cstddef>

namespace ck3accel {

namespace {
    constexpr std::size_t HASH_PREFIX_BYTES = 4 * 1024 * 1024;  // first 4MB
}

VersionInfo detect_version() {
    VersionInfo info;

    auto text = inspect_main_module();
    if (!text.valid()) return info;

    info.pe_timestamp = text.pe_timestamp;

    // hash a fixed prefix: bounds cost, ignores .text padding tails that differ between builds.
    std::size_t to_hash = std::min(text.size, HASH_PREFIX_BYTES);
    picosha2::hash256_one_by_one h;
    h.process(text.base, text.base + to_hash);
    h.finish();
    info.text_sha256 = picosha2::get_hash_hex_string(h);

    auto entries_opt = load_versions_json(versions_json_path());
    if (!entries_opt) return info;

    auto* m = find_match(*entries_opt, info.pe_timestamp, info.text_sha256);
    if (!m) return info;  // status stays Unknown

    info.version = m->version;
    info.auto_disable = m->auto_disable;
    info.status = m->tested ? DetectionStatus::KnownTested
                            : DetectionStatus::KnownUntested;
    return info;
}

} // namespace ck3accel
