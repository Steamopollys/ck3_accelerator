#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace ck3accel {

struct Tombstone {
    std::string game_version;
    std::string timestamp_utc;
    std::string arming_plugin;   // plugin whose hook was arming/executing
};

// file ops (unit-tested): write/read/clear a tombstone (key=value lines).
void                     write_tombstone(const std::filesystem::path& p, const Tombstone& t);
std::optional<Tombstone> read_tombstone(const std::filesystem::path& p);
void                     clear_tombstone(const std::filesystem::path& p);

// arm the crash handlers. VEH acts only on an access violation whose RIP is in
// our hooked range (try_address_in_hooked_range) or a registered plugin module.
// SetUnhandledExceptionFilter adds a last-resort breadcrumb + best-effort
// telemetry flush. writes the tombstone now; starts a fallback timer that marks
// stable after fallback_seconds.
//
// crash-path safety: live handlers are best-effort and NEVER block on a mutex.
// durable safe-mode is the on-disk tombstone, written here before any hook can
// fault and cleared only at stability. full design in the .cpp header.
void crash_sentinel_install(const std::filesystem::path& tombstone_path,
                            Tombstone initial,
                            int fallback_seconds);

// record the currently-arming plugin (set just before its hooks install).
void crash_sentinel_set_arming_plugin(std::string name);

// register a plugin module's [base, base+SizeOfImage) so the VEH can attribute
// a fault anywhere in its code, not just the hook entry.
void crash_sentinel_register_module(std::string name, const void* module_base);

// idempotent: clear the tombstone (we survived). called on first save-load (via
// report_metric) and by the fallback timer.
void crash_sentinel_mark_stable();

// signal + join the fallback timer and unregister handlers. from core_shutdown
// so no std::thread dies joinable.
void crash_sentinel_shutdown();

} // namespace ck3accel
