#include "logger.h"
#include "paths.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/wincolor_sink.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace ck3accel {

namespace {
    std::once_flag g_init_flag;
    std::atomic<bool> g_init_ok{false};
    std::shared_ptr<spdlog::logger> g_logger;
    bool g_console_attached = false;

    spdlog::level::level_enum to_spdlog(LogLevel l) {
        switch (l) {
            case LogLevel::Trace:    return spdlog::level::trace;
            case LogLevel::Debug:    return spdlog::level::debug;
            case LogLevel::Info:     return spdlog::level::info;
            case LogLevel::Warn:     return spdlog::level::warn;
            case LogLevel::Error:    return spdlog::level::err;
            case LogLevel::Critical: return spdlog::level::critical;
        }
        return spdlog::level::info;
    }

    std::string session_filename() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::ostringstream os;
        os << "session_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".log";
        return os.str();
    }
}

bool init_logger() {
    std::call_once(g_init_flag, []() {
        std::error_code ec;
        auto log_dir = log_directory();
        std::filesystem::create_directories(log_dir, ec);
        if (ec) {
            return;  // g_init_ok stays false
        }
        auto log_file = log_dir / session_filename();
        try {
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                log_file.string(), /*truncate=*/false);
            auto dbg_sink  = std::make_shared<spdlog::sinks::msvc_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>(
                "ck3accel",
                spdlog::sinks_init_list{file_sink, dbg_sink});
            logger->set_level(spdlog::level::debug);
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
            logger->flush_on(spdlog::level::info);
            g_logger = std::move(logger);   // assigned AFTER fully built
            g_init_ok.store(true, std::memory_order_release);
        } catch (const std::exception&) {
            g_logger.reset();
            // g_init_ok stays false
        }
    });
    return g_init_ok.load(std::memory_order_acquire);
}

void log(LogLevel level, std::string_view message) {
    if (!g_init_ok.load(std::memory_order_acquire)) return;
    g_logger->log(to_spdlog(level), message);
}

void enable_console_logging() {
    if (!g_init_ok.load(std::memory_order_acquire)) return;

    // AllocConsole returns FALSE if a console is already attached; proceed anyway.
    ::AllocConsole();
    ::SetConsoleTitleW(L"CK3 Accelerator live log");

    // redirect CRT stdout/stderr so writes there appear too.
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    // do NOT rely on GetStdHandle(STD_OUTPUT_HANDLE): the Paradox launcher spawns
    // ck3.exe with piped stdio, so AllocConsole keeps the inherited handles and a
    // stdout-based sink writes into the launcher's pipe, leaving the console empty
    // (seen live 2026-09-02). open the console's screen buffer explicitly and hand
    // that handle to the sink.
    HANDLE conout = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    if (conout == INVALID_HANDLE_VALUE) conout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    else ::SetStdHandle(STD_OUTPUT_HANDLE, conout);

    // wincolor sink on that handle, same pattern as the file sink.
    auto console_sink = std::make_shared<spdlog::sinks::wincolor_sink<spdlog::details::console_mutex>>(
        conout, spdlog::color_mode::automatic);
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    // append to the logger's sink list. safe: single-threaded init.
    g_logger->sinks().push_back(console_sink);

    g_console_attached = true;
}

void disable_console_logging() {
    if (!g_console_attached) return;
    g_console_attached = false;
    ::FreeConsole();
}

} // namespace ck3accel
