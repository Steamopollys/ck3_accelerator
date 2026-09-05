#pragma once
#include <filesystem>
#include <string>

namespace ck3accel {

// init the metrics writer. enabled==false: report calls are no-ops, no file.
// writes a header row only if the file is new. session_id is per-launch;
// game_version labels every row.
void telemetry_init(bool enabled,
                    const std::filesystem::path& csv_path,
                    std::string session_id,
                    std::string game_version);

// Append one row. Matches CoreApi.report_metric. Cheap: buffered, no per-row flush.
void telemetry_report(const char* name, double value);

// flush buffered rows (at shutdown). blocks on the telemetry mutex; non-fault
// contexts only.
void telemetry_flush();

// crash-path variant: try_lock the telemetry mutex, flush iff acquired. true iff
// the flush ran. NEVER blocks: safe from a VEH/unhandled handler on the faulting
// thread, which may already hold the mutex mid-report (a blocking flush there
// would self-deadlock). false on contention.
bool telemetry_flush_try();

} // namespace ck3accel
