#include "telemetry.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>

namespace ck3accel {

namespace {

std::mutex    g_mutex;
bool          g_enabled = false;
std::ofstream g_out;
std::string   g_session_id;
std::string   g_game_version;

constexpr const char* kHeader =
    "timestamp_iso8601,session_id,game_version,metric,value";

std::string iso8601_utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string format_value(double value) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17) << value;
    return os.str();
}

} // namespace

void telemetry_init(bool enabled,
                    const std::filesystem::path& csv_path,
                    std::string session_id,
                    std::string game_version) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_out.is_open()) {
        g_out.close();
    }
    g_enabled      = enabled;
    g_session_id   = std::move(session_id);
    g_game_version = std::move(game_version);

    if (!g_enabled) {
        return;  // off by default: no file, no header.
    }

    std::error_code ec;
    const bool existed = std::filesystem::exists(csv_path, ec);

    g_out.open(csv_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!g_out.is_open()) {
        g_enabled = false;  // cannot write; degrade to no-op.
        return;
    }

    if (!existed) {
        g_out << kHeader << '\n';
    }
}

void telemetry_report(const char* name, double value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled || !g_out.is_open()) {
        return;
    }
    g_out << iso8601_utc_now() << ','
          << g_session_id << ','
          << g_game_version << ','
          << (name ? name : "") << ','  // metric names are trusted in-process plugin literals; not CSV-sanitized by design.
          << format_value(value) << '\n';
}

void telemetry_flush() {
    // blocking flush for normal shutdown. crash path must NOT call this on the
    // faulting thread (self-deadlock if it already held g_mutex mid-report); it
    // uses telemetry_flush_try() instead.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_out.is_open()) {
        g_out.flush();
    }
}

bool telemetry_flush_try() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;  // contended (possibly the faulting thread itself); skip.
    }
    if (g_out.is_open()) {
        g_out.flush();
    }
    return true;
}

} // namespace ck3accel
