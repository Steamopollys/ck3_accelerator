#pragma once
#include <ck3accel/version_info.h>

namespace ck3accel {

// inspect main module, hash .text, look up in versions.json. on any I/O or
// parse failure: status=Unknown, empty version.
VersionInfo detect_version();

} // namespace ck3accel
