// accel_decompress_probe: DEVELOPER-ONLY profiling probe (NOT SHIPPED).
//
// Characterises CK3 save-decompression before designing accel_save_load: observes the live game's
// zlib `inflate` calls (chunk sizes, callers, per-call time).
//
// Hooks zlib's `inflate(z_stream* strm, int flush)` (statically linked into ck3.exe, MSVC x64:
// rcx=strm, edx=flush, returns int) and, around the original call, snapshots avail_in/avail_out,
// the caller's return address (as an RVA into ck3.exe), and the call's wall time. Each call appends
// one CSV row to logs\probe_decompress.csv. A plain std::mutex guards it (inflate may run on
// several threads); a dev tool, so it favours not losing data over speed.

#include <ck3accel/core_api.h>

#include <windows.h>
#include <intrin.h>   // _ReturnAddress

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

// LogLevel mirror (CoreApi.log takes an int; core maps it back to LogLevel):
//   0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Critical
namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

// ---- original inflate, captured by the hook engine's trampoline -------------
using inflate_fn = int (*)(void* strm, int flush);
inflate_fn g_orig_inflate = nullptr;

// ---- module base of ck3.exe, cached once for RVA arithmetic -----------------
std::uintptr_t g_module_base = 0;

// ---- monotonically increasing call index ------------------------------------
std::atomic<std::uint64_t> g_call_index{0};

// ---- throttled progress counters for live console output --------------------
std::atomic<std::uint64_t> g_total_out_bytes{0};

// ---- QPC frequency, read once ------------------------------------------------
double g_qpc_to_us = 0.0;  // microseconds per QPC tick

// ---- CSV file, opened once on first use, guarded by g_log_mutex --------------
std::mutex g_log_mutex;
FILE*      g_csv = nullptr;
bool       g_csv_open_attempted = false;
const CoreApi* g_host = nullptr;

// Read a uint32 from a z_stream field at the given byte offset. On MSVC/Win64,
// zlib's uLong/uInt are 4 bytes, so avail_in @ +8 and avail_out @ +24 are u32.
std::uint32_t read_u32(const void* strm, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, static_cast<const unsigned char*>(strm) + off, sizeof(v));
    return v;
}

// Resolve the directory containing THIS dll (the install dir), the same way the
// core does: GetModuleFileNameW on our own HMODULE, then strip the filename.
std::wstring self_dll_directory() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&self_dll_directory),
            &self)) {
        return std::wstring();
    }
    std::wstring buf;
    buf.resize(MAX_PATH);
    for (;;) {
        DWORD len = ::GetModuleFileNameW(self, buf.data(),
                                         static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return std::wstring();
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    // The dll lives in install_dir\plugins\accel_decompress_probe.dll. We want
    // the install dir (parent of \plugins), so strip TWO path components.
    const std::wstring::size_type last = buf.find_last_of(L"\\/");
    if (last == std::wstring::npos) {
        return std::wstring();
    }
    const std::wstring plugins_dir = buf.substr(0, last);          // ...\plugins
    const std::wstring::size_type prev = plugins_dir.find_last_of(L"\\/");
    if (prev == std::wstring::npos) {
        return plugins_dir;  // unexpected layout; fall back to the dll's folder
    }
    return plugins_dir.substr(0, prev);                            // install dir
}

// Open logs\probe_decompress.csv (creating logs\ if needed), write a session
// banner + header. Must be called with g_log_mutex held. Idempotent.
void ensure_csv_open_locked() {
    if (g_csv || g_csv_open_attempted) {
        return;
    }
    g_csv_open_attempted = true;

    const std::wstring install = self_dll_directory();
    if (install.empty()) {
        if (g_host && g_host->log) {
            g_host->log(kLogWarn,
                "accel_decompress_probe: could not resolve install dir; "
                "CSV disabled");
        }
        return;
    }

    const std::wstring logs_dir = install + L"\\logs";
    ::CreateDirectoryW(logs_dir.c_str(), nullptr);  // ok if it already exists

    const std::wstring csv_path = logs_dir + L"\\probe_decompress.csv";

    // Existing file: append. Fresh create: write the header.
    const DWORD attrs = ::GetFileAttributesW(csv_path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) &&
                         !(attrs & FILE_ATTRIBUTE_DIRECTORY);

    if (_wfopen_s(&g_csv, csv_path.c_str(), L"a") != 0 || g_csv == nullptr) {
        g_csv = nullptr;
        if (g_host && g_host->log) {
            g_host->log(kLogWarn,
                "accel_decompress_probe: failed to open probe_decompress.csv; "
                "CSV disabled");
        }
        return;
    }

    if (!existed) {
        std::fprintf(g_csv,
            "call_index,avail_in_before,avail_out_before,"
            "avail_in_after,avail_out_after,caller_rva,elapsed_us,ret_code\n");
    }

    // Session banner: timestamp + ck3.exe base so caller_rva values can be
    // tied back to a module load if ASLR ever moves it.
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::fprintf(g_csv,
        "# session_start %04u-%02u-%02u %02u:%02u:%02u "
        "module_base=0x%llX\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        static_cast<unsigned long long>(g_module_base));
    std::fflush(g_csv);
}

// ---- the detour -------------------------------------------------------------
int detour_inflate(void* strm, int flush) {
    // Caller return address -> RVA into ck3.exe.
    const std::uintptr_t ret_addr =
        reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uintptr_t caller_rva =
        (g_module_base && ret_addr >= g_module_base)
            ? (ret_addr - g_module_base)
            : ret_addr;  // fall back to absolute if base is unknown

    // Snapshot z_stream fields BEFORE the call.
    std::uint32_t avail_in_before  = 0;
    std::uint32_t avail_out_before = 0;
    if (strm) {
        avail_in_before  = read_u32(strm, 8);   // avail_in  @ +8
        avail_out_before = read_u32(strm, 24);  // avail_out @ +24
    }

    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    ::QueryPerformanceCounter(&t0);
    const int rc = g_orig_inflate(strm, flush);
    ::QueryPerformanceCounter(&t1);

    // Snapshot z_stream fields AFTER the call.
    std::uint32_t avail_in_after  = 0;
    std::uint32_t avail_out_after = 0;
    if (strm) {
        avail_in_after  = read_u32(strm, 8);
        avail_out_after = read_u32(strm, 24);
    }

    const double elapsed_us =
        static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpc_to_us;
    const std::uint64_t idx =
        g_call_index.fetch_add(1, std::memory_order_relaxed);

    // Accumulate bytes produced (avail_out decreased by the bytes written).
    const std::uint32_t out_produced =
        (avail_out_before >= avail_out_after)
            ? (avail_out_before - avail_out_after) : 0u;
    g_total_out_bytes.fetch_add(out_produced, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        ensure_csv_open_locked();
        if (g_csv) {
            std::fprintf(g_csv,
                "%llu,%u,%u,%u,%u,0x%llX,%.3f,%d\n",
                static_cast<unsigned long long>(idx),
                avail_in_before, avail_out_before,
                avail_in_after,  avail_out_after,
                static_cast<unsigned long long>(caller_rva),
                elapsed_us, rc);
            std::fflush(g_csv);  // dev tool: never lose a row across a crash
        }
    }

    // Throttled live-progress log: emit roughly every 4096 calls so decompression
    // activity is visible in the debug console without spamming or slowing loads.
    if ((idx & 0xFFFu) == 0u && g_host && g_host->log) {
        const std::uint64_t total_bytes =
            g_total_out_bytes.load(std::memory_order_relaxed);
        // Express as MB with one decimal place; avoid heap allocation in hot path.
        char msg[128];
        std::snprintf(msg, sizeof(msg),
            "inflate: %llu calls, %.1f MB decompressed",
            static_cast<unsigned long long>(idx + 1),
            static_cast<double>(total_bytes) / (1024.0 * 1024.0));
        g_host->log(kLogInfo, msg);
    }

    return rc;
}

// Unique signature for zlib inflate() prologue in ck3.exe (RVA 0x035C4340): 35 bytes
// through the `test rcx,rcx; je` with the je's rel32 wildcarded, one match over .text.
const char* const kInflateSig =
    "89 54 24 10 53 55 56 57 41 54 41 55 41 56 41 57 "
    "48 83 EC 58 44 8B DA 4C 8B E1 48 85 C9 0F 84 ?? ?? ?? ??";

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),  // struct_size (FIRST)
    CK3ACCEL_PLUGIN_MAGIC,                              // magic
    CK3ACCEL_ABI_VERSION,                              // required_abi
    "accel_decompress_probe",                          // name (allowlist key)
    "0.1.0",                                           // semver
    "any",                                             // min_game_version
    "any",                                             // max_game_version
    CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN | CK3ACCEL_MODE_MULTIPLAYER,
};

}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->scan || !host->install_hook) {
        return 1;  // host too old / missing services: stay inert
    }

    // Cache ck3.exe base and the QPC->microseconds factor up front.
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
        g_qpc_to_us = 1.0e6 / static_cast<double>(freq.QuadPart);
    }

    void* addr = host->scan(kInflateSig);
    if (!addr) {
        if (host->log) {
            host->log(kLogWarn,
                "accel_decompress_probe: inflate signature not found; "
                "probe inert");
        }
        return 1;  // soft-fail: nothing installed
    }

    host->install_hook(reg->hook_set, addr,
                       reinterpret_cast<void*>(&detour_inflate),
                       reinterpret_cast<void**>(&g_orig_inflate));

    if (host->log) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "accel_decompress_probe: hooked inflate @ 0x%llX (RVA 0x%llX); "
            "logging to logs\\probe_decompress.csv",
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(addr)),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(addr) - g_module_base));
        host->log(kLogInfo, msg);
    }
    return 0;
}
