#pragma once
#include <ck3accel/core_api.h>
#include "config.h"
#include <ck3accel/version_info.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ck3accel {

// build the process-wide CoreApi handed to plugins. wires every slot:
//   log                  -> ck3accel::log (int level -> LogLevel passthrough)
//   game_version         -> a cached VersionInfo_C from detect_version()
//   is_kill_switch_active-> ck3accel::is_kill_switch_active
//   report_metric        -> telemetry_report + first-call crash_sentinel_mark_stable
//   scan                 -> scan_text (matched address or null)
//   install_hook         -> hook_engine::install_hook
// returns a function-local static; valid for the process lifetime.
const CoreApi* build_core_api();

// unit-tested: of the discovered DLL stems, keep those present and true in the
// allowlist. order preserved.
std::vector<std::string> filter_allowlisted(
    const std::vector<std::string>& dll_stems,
    const std::unordered_map<std::string, bool>& allowlist);

// full load sequence. MUST run on a spawned thread (never in DllMain).
// per allowlisted plugins/*.dll: LoadLibrary -> GetProcAddress CK3Accel_Query
// + CK3Accel_Init -> evaluate_plugin (incl. tombstone safe-mode skip-set) ->
// register_hook_set -> crash_sentinel_set_arming_plugin -> CK3Accel_Init(host, &reg).
// any failure: LOG_WARN, FreeLibrary, continue.
void load_plugins(const Config& cfg, const VersionInfo& vi, std::uint32_t session_mode);

} // namespace ck3accel
