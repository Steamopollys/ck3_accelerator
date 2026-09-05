#pragma once
#include <ck3accel/log_level.h>
#include <string_view>

namespace ck3accel {

// init the global logger; creates log_directory() if missing. idempotent (later
// calls are no-ops). false if the log dir can't be created.
bool init_logger();

// log a message at the given level.
void log(LogLevel level, std::string_view message);

// attach a wincolor stdout console sink: AllocConsole(), redirect CRT
// stdout/stderr, append the sink. single-threaded init only (before the loader
// thread). no-op if the logger isn't initialized.
void enable_console_logging();

// detach the console (FreeConsole). from core_shutdown.
void disable_console_logging();

// convenience macros. use these, not log() directly.
} // namespace ck3accel

#define LOG_TRACE(msg)    ::ck3accel::log(::ck3accel::LogLevel::Trace, (msg))
#define LOG_DEBUG(msg)    ::ck3accel::log(::ck3accel::LogLevel::Debug, (msg))
#define LOG_INFO(msg)     ::ck3accel::log(::ck3accel::LogLevel::Info, (msg))
#define LOG_WARN(msg)     ::ck3accel::log(::ck3accel::LogLevel::Warn, (msg))
#define LOG_ERROR(msg)    ::ck3accel::log(::ck3accel::LogLevel::Error, (msg))
#define LOG_CRITICAL(msg) ::ck3accel::log(::ck3accel::LogLevel::Critical, (msg))
