// accel_sampler: DEVELOPER-ONLY sampling profiler (NOT SHIPPED).
//
// Finds where CK3 spends late-game tick CPU. A background thread samples every other thread's
// instruction pointer (RIP) at ~500 Hz, buckets each RIP to the containing ck3.exe function
// (resolved from the .pdata RUNTIME_FUNCTION table), and periodically dumps the hottest functions
// to the console and logs\sampler.csv. No hooks; reads memory only.
//
// Active-thread filter: the handle-refresh step uses GetThreadTimes to measure each thread's CPU
// delta (kernel+user) over the ~250 ms refresh window and keeps only threads over
// kActiveThreshold100ns. This drops the ~98% idle-thread noise and cuts overhead from ~92% to a
// small fraction. GetThreadTimes runs entirely outside the suspend/resume window.
//
// SAFETY INVARIANT (why this may suspend game threads): between SuspendThread and the matching
// ResumeThread we do NOTHING but read the thread's RIP into a PRE-ALLOCATED buffer. No heap alloc,
// mutex, logging, FunctionTable lookup, or CSV. Each thread is resumed immediately after its RIP
// is read; only after every sampled thread is resumed do we bucket the RIPs. Violating this risks
// deadlocking the game (a suspended thread may hold the CRT heap lock, loader lock, or any lock we
// would take).
//
// CROSS-THREAD FREEZE HAZARD: on the panic hotkey the core kill-switch thread may invoke MinHook's
// MH_ApplyQueued (via hook_engine::disable_all()) to neuter OTHER plugins' hooks, which FREEZES all
// process threads. To avoid being frozen mid-window we (a) check is_kill_switch_active() right
// before opening the suspend loop and skip it once panic is active, and (b) hold AT MOST ONE game
// thread suspended at a time (resume is inside the per-thread loop body). Do not widen either.

#include <ck3accel/core_api.h>
#include <ck3accel/function_table.h>
#include <ck3accel/sample_histogram.h>

#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// x64-only: reads CONTEXT.Rip and the IMAGE_NT_HEADERS64 optional header. ck3.exe is x64.
static_assert(sizeof(void*) == 8, "accel_sampler is x64-only (CONTEXT.Rip)");

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

// ---- tunables ---------------------------------------------------------------
// Sampling rate. 500 Hz with active-only thread filtering keeps overhead low (the active set is
// typically a handful of threads: main sim + active job workers) while giving ample samples over a
// multi-minute run. At 1 kHz with all ~36 threads the sampler used ~92% CPU and buried the signal
// under ~98% idle-thread noise; active-only + 500 Hz fixes both. The overhead banner in each dump
// reports the actual cost.
constexpr unsigned kSampleHz       = 500;
// Minimum CPU-time delta (kernel+user, 100-ns units) a thread must accumulate over the ~250 ms
// refresh window to count as ACTIVE and stay in the sampled set. 10 000 units = 1 ms of CPU.
// Threads below it (parked, sleeping, idle) are closed immediately, contributing zero samples.
// Tunable: raise to 50 000 (5 ms) to cut churn from briefly-waking threads; lower to 1 000
// (0.1 ms) to catch threads that wake only momentarily per window.
constexpr std::uint64_t kActiveThreshold100ns = 10000;
// Thread-handle list refresh cadence: rebuild the open-handle set this often, not every tick.
constexpr DWORD    kRefreshMs      = 250;
// Console + CSV dump cadence.
constexpr double   kDumpEverySec   = 5.0;
// How many hot functions to print/CSV per dump.
constexpr std::size_t kTopN        = 40;
// Hard cap on threads sampled per tick. The RIP buffer is preallocated to this
// size; excess threads in a single tick are skipped (they get sampled next tick
// once the list is small enough, which it virtually always is for CK3).
constexpr std::size_t kMaxThreads  = 512;

// "Return all entries" sentinel for SampleHistogram::top_n: top_n truncates to at most n, so
// SIZE_MAX means no truncation. Named so a future editor can't swap in a finite value and silently
// truncate the sentinel scan that computes non_ck3/ck3_other.
constexpr std::size_t kAllEntries = (std::numeric_limits<std::size_t>::max)();

// Sentinel RVA stored in the histogram for samples that landed OUTSIDE ck3.exe
// (kernel32, ntdll, GPU driver, our own dll, ...). 0 is reserved by the
// histogram/FunctionTable for "in ck3.exe but no containing function" (ck3-other).
constexpr std::uint32_t kRvaNonCk3 = 0xFFFFFFFFu;

// ---- host + cached process facts --------------------------------------------
const CoreApi* g_host = nullptr;
std::uintptr_t g_module_base = 0;          // ck3.exe base
std::uint32_t  g_image_size  = 0;          // OptionalHeader.SizeOfImage
double         g_qpc_to_ms   = 0.0;        // milliseconds per QPC tick
LONGLONG       g_qpc_freq    = 0;

// ---- profiling state (the histogram is owned SOLELY by the sampler thread) --
ck3accel::FunctionTable  g_functions;      // built once in Init, then read-only
ck3accel::SampleHistogram g_hist;          // touched ONLY by sampler_thread_main

// ---- CSV (opened by the sampler thread; no cross-thread access) -------------
FILE* g_csv = nullptr;
bool  g_csv_open_attempted = false;

// ---- non-ck3 module + nearest-export attribution ----------------------------
// The load profile put ~80% of active CPU outside ck3.exe, all under one sentinel. To tell the
// allocator (an arena might help) apart from workers spinning in a yield (idle; the real load is
// serial elsewhere), pin each non-ck3 RIP to its module and nearest exported symbol. Built once on
// the sampler thread, read-only after; resolution happens outside the suspend window.
struct ModSym { std::uintptr_t base; std::uint32_t size; std::string name;
                std::vector<std::pair<std::uint32_t, std::string>> exports; };
std::vector<ModSym> g_mods;                         // sorted by base
std::unordered_map<std::string, std::uint64_t> g_nonck3;   // sym -> sample count

std::string narrow(const wchar_t* w) {
    char buf[MAX_PATH]; int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return (n > 0) ? std::string(buf, buf + n - 1) : std::string();
}

// POD-only SEH helpers: __try cannot share a function with unwinding C++ objects
// (C2712), so all raw reads of possibly-bad module memory live in these. A template
// body with only POD locals is fine under __try (nothing to unwind).
template <class T>
bool seh_read(const void* p, T* out) {
    __try { *out = *reinterpret_cast<const T*>(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_copy_cstr(const char* p, char* out, std::size_t cap) {
    __try {
        std::size_t i = 0;
        for (; i + 1 < cap && p[i]; ++i) out[i] = p[i];
        out[i] = 0; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Parse one loaded module's export table from its mapped image. No __try here
// (uses the POD helpers above), so std::string/std::vector are free to unwind.
void index_module_exports(ModSym& m) {
    auto* base = reinterpret_cast<const std::uint8_t*>(m.base);
    std::uint32_t e_lfanew;
    if (!seh_read(base + 0x3C, &e_lfanew)) return;
    const std::uint8_t* nt = base + e_lfanew;
    std::uint16_t sig; if (!seh_read(nt, &sig) || sig != 0x4550) return;   // 'PE'
    std::uint32_t exp_rva;                                                 // DataDirectory[0]
    if (!seh_read(nt + 0x18 + 0x70, &exp_rva) || !exp_rva) return;
    const std::uint8_t* ed = base + exp_rva;
    std::uint32_t n_names, rva_funcs, rva_names, rva_ords;
    if (!seh_read(ed + 0x18, &n_names) || !seh_read(ed + 0x1C, &rva_funcs) ||
        !seh_read(ed + 0x20, &rva_names) || !seh_read(ed + 0x24, &rva_ords)) return;
    if (n_names > 100000) return;   // sanity
    m.exports.reserve(n_names);
    for (std::uint32_t i = 0; i < n_names; ++i) {
        std::uint32_t nrva, frva; std::uint16_t ord; char nm[256];
        if (!seh_read(base + rva_names + 4u * i, &nrva)) break;
        if (!seh_read(base + rva_ords + 2u * i, &ord)) break;
        if (!seh_read(base + rva_funcs + 4u * ord, &frva)) break;
        if (!seh_copy_cstr(reinterpret_cast<const char*>(base + nrva), nm, sizeof(nm))) break;
        m.exports.emplace_back(frva, std::string(nm));
    }
    std::sort(m.exports.begin(), m.exports.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

void build_module_symbols() {
    HMODULE mods[1024]; DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const std::size_t count = needed / sizeof(HMODULE);
    for (std::size_t i = 0; i < count && i < 1024; ++i) {
        MODULEINFO mi{};
        if (!::GetModuleInformation(::GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        wchar_t nameW[MAX_PATH]; if (!::GetModuleBaseNameW(::GetCurrentProcess(), mods[i], nameW, MAX_PATH)) continue;
        ModSym m; m.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        m.size = mi.SizeOfImage; m.name = narrow(nameW);
        index_module_exports(m);
        g_mods.push_back(std::move(m));
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const ModSym& a, const ModSym& b) { return a.base < b.base; });
}

// Resolve a non-ck3 RIP to "module.dll!NearestExport" (nearest exported symbol at
// or below the RIP), or "module.dll+0xRVA" when the module exports nothing useful.
std::string resolve_nonck3(std::uintptr_t rip) {
    // upper_bound on base, step back one -> the module whose base <= rip.
    std::size_t lo = 0, hi = g_mods.size();
    while (lo < hi) { std::size_t mid = (lo + hi) / 2; if (g_mods[mid].base <= rip) lo = mid + 1; else hi = mid; }
    if (lo == 0) return "unknown";
    const ModSym& m = g_mods[lo - 1];
    if (rip >= m.base + m.size) return "unknown";
    const std::uint32_t rva = static_cast<std::uint32_t>(rip - m.base);
    if (!m.exports.empty() && rva >= m.exports.front().first) {
        std::size_t a = 0, b = m.exports.size();
        while (a < b) { std::size_t mid = (a + b) / 2; if (m.exports[mid].first <= rva) a = mid + 1; else b = mid; }
        return m.name + "!" + m.exports[a - 1].second;
    }
    char buf[32]; std::snprintf(buf, sizeof(buf), "+0x%X", rva);
    return m.name + buf;
}

// Resolve the install dir (parent of \plugins): GetModuleFileNameW on our own HMODULE, then
// strip the trailing \plugins\<dll>.
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
    // Strip \plugins\<dll> (two components) to reach the install dir.
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

// Parse ck3.exe's .pdata (the x64 exception RUNTIME_FUNCTION table) into g_functions and capture
// g_image_size. Reads the PE headers off the loaded image, like core/src/pe_inspect.cpp. Returns
// the number of function ranges added, or 0 on any header-validation failure.
std::size_t build_function_table_from_pdata() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_module_base);
    if (base == nullptr) {
        return 0;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    // Reject anything that is not a 64-bit image before reading the 64-bit
    // OptionalHeader fields (SizeOfImage / DataDirectory). ck3.exe is always x64.
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }

    g_image_size = nt->OptionalHeader.SizeOfImage;

    const IMAGE_DATA_DIRECTORY& exception_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exception_dir.VirtualAddress == 0 || exception_dir.Size == 0) {
        return 0;  // no .pdata (unexpected for ck3.exe, but bail safely)
    }

    // Defense-in-depth: reject a malformed exception directory whose extent
    // exceeds the image. Use 64-bit arithmetic to avoid 32-bit overflow of the
    // sum (VirtualAddress and Size are both DWORD, so their sum can wrap).
    {
        const std::uint64_t pdata_end =
            static_cast<std::uint64_t>(exception_dir.VirtualAddress) +
            static_cast<std::uint64_t>(exception_dir.Size);
        if (pdata_end > static_cast<std::uint64_t>(g_image_size)) {
            if (g_host && g_host->log) {
                g_host->log(kLogWarn,
                    "accel_sampler: .pdata extent exceeds SizeOfImage; "
                    "malformed PE header — profiler inert");
            }
            return 0;
        }
    }

    // _IMAGE_RUNTIME_FUNCTION_ENTRY { DWORD BeginAddress, EndAddress, UnwindData }.
    const auto* fns = reinterpret_cast<const _IMAGE_RUNTIME_FUNCTION_ENTRY*>(
        base + exception_dir.VirtualAddress);
    const std::size_t count = exception_dir.Size / sizeof(_IMAGE_RUNTIME_FUNCTION_ENTRY);

    for (std::size_t i = 0; i < count; ++i) {
        const _IMAGE_RUNTIME_FUNCTION_ENTRY& e = fns[i];
        // add() drops degenerate/empty ranges (end <= begin) for us.
        g_functions.add(e.BeginAddress, e.EndAddress);
    }
    g_functions.finalize();
    return g_functions.size();
}

// Open logs\sampler.csv (creating logs\ if needed), write a session banner +
// header on a fresh create. Called only by the sampler thread, so no lock.
void ensure_csv_open() {
    if (g_csv || g_csv_open_attempted) {
        return;
    }
    g_csv_open_attempted = true;

    const std::wstring install = self_dll_directory();
    if (install.empty()) {
        if (g_host && g_host->log) {
            g_host->log(kLogWarn,
                "accel_sampler: could not resolve install dir; CSV disabled");
        }
        return;
    }

    const std::wstring logs_dir = install + L"\\logs";
    ::CreateDirectoryW(logs_dir.c_str(), nullptr);  // ok if it already exists

    const std::wstring csv_path = logs_dir + L"\\sampler.csv";

    const DWORD attrs = ::GetFileAttributesW(csv_path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) &&
                         !(attrs & FILE_ATTRIBUTE_DIRECTORY);

    if (_wfopen_s(&g_csv, csv_path.c_str(), L"a") != 0 || g_csv == nullptr) {
        g_csv = nullptr;
        if (g_host && g_host->log) {
            g_host->log(kLogWarn,
                "accel_sampler: failed to open sampler.csv; CSV disabled");
        }
        return;
    }

    if (!existed) {
        std::fprintf(g_csv, "dump_seq,rva,samples,pct_of_total,pct_of_ck3\n");
    }

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::fprintf(g_csv,
        "# session_start %04u-%02u-%02u %02u:%02u:%02u "
        "module_base=0x%llX image_size=0x%X functions=%zu sample_hz=%u\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        static_cast<unsigned long long>(g_module_base),
        g_image_size, g_functions.size(), kSampleHz);
    std::fflush(g_csv);
}

// Refresh the set of open thread handles for THIS process, skipping the calling (sampler) thread
// and any OpenThread failure. Closes the previous handles first. Done OUTSIDE the suspend window:
// it allocates and calls the kernel.
//
// Active-thread filter: GetThreadTimes measures each thread's CPU (kernel+user) since the last
// refresh; only threads over kActiveThreshold100ns are retained, idle/parked ones closed
// immediately so they contribute nothing. lastBusy is updated for ALL enumerated threads (active
// and idle) so the next delta is correct. Owned solely by the sampler thread (no lock).
//
// out_active_count receives the number of handles kept (active threads).
void refresh_thread_handles(std::vector<HANDLE>& handles,
                            std::unordered_map<DWORD, std::uint64_t>& lastBusy,
                            std::size_t& out_active_count) {
    for (HANDLE h : handles) {
        ::CloseHandle(h);
    }
    handles.clear();
    out_active_count = 0;

    const DWORD self_pid = ::GetCurrentProcessId();
    const DWORD self_tid = ::GetCurrentThreadId();

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (::Thread32First(snap, &te)) {
        do {
            // Cap BEFORE adding so the preallocated rips[] buffer can never be
            // overrun: stop enumerating once we already hold kMaxThreads handles.
            if (handles.size() >= kMaxThreads) {
                break;
            }
            // THREADENTRY32 is a variable-size struct; only fields up to
            // th32OwnerProcessID are guaranteed present.
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                 sizeof(te.th32OwnerProcessID)) {
                if (te.th32OwnerProcessID == self_pid &&
                    te.th32ThreadID != self_tid) {
                    const DWORD tid = te.th32ThreadID;

                    // Request THREAD_QUERY_INFORMATION so GetThreadTimes works.
                    // Fall back to THREAD_QUERY_LIMITED_INFORMATION on systems
                    // where we lack the broader right (protected-process hosts).
                    HANDLE th = ::OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_QUERY_INFORMATION,
                        FALSE, tid);
                    if (th == nullptr) {
                        th = ::OpenThread(
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_LIMITED_INFORMATION,
                            FALSE, tid);
                    }
                    if (th == nullptr) {
                        // OpenThread failure (thread just exited, or both access levels
                        // denied): skip silently; resample next refresh.
                        continue;
                    }

                    // Measure accumulated CPU time for active-thread filtering.
                    // All arithmetic is done OUTSIDE the suspend window; this
                    // GetThreadTimes call is safe here (thread not suspended).
                    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
                    std::uint64_t busy = 0;
                    if (::GetThreadTimes(th, &ft_create, &ft_exit,
                                         &ft_kernel, &ft_user)) {
                        const std::uint64_t k =
                            (static_cast<std::uint64_t>(ft_kernel.dwHighDateTime) << 32) |
                             static_cast<std::uint64_t>(ft_kernel.dwLowDateTime);
                        const std::uint64_t u =
                            (static_cast<std::uint64_t>(ft_user.dwHighDateTime) << 32) |
                             static_cast<std::uint64_t>(ft_user.dwLowDateTime);
                        busy = k + u;
                    }

                    // Compute delta vs last refresh. 0 if TID was never seen.
                    const auto it = lastBusy.find(tid);
                    const std::uint64_t prev = (it != lastBusy.end()) ? it->second : 0;
                    const std::uint64_t delta = (busy >= prev) ? (busy - prev) : 0;

                    // ALWAYS update lastBusy so the next delta is correct,
                    // regardless of whether we keep or discard this thread.
                    lastBusy[tid] = busy;

                    if (delta >= kActiveThreshold100ns) {
                        // Thread was CPU-active this window: keep it for sampling.
                        handles.push_back(th);
                        ++out_active_count;
                    } else {
                        // Thread is idle/parked: discard the handle now so it
                        // contributes zero samples and no suspend overhead.
                        ::CloseHandle(th);
                    }
                }
            }
            te.dwSize = sizeof(te);
        } while (::Thread32Next(snap, &te));
    }

    ::CloseHandle(snap);
}

// Write the top-N histogram entries (plus the two synthetic sentinel rows) to the console and
// sampler.csv, with the overhead banner. Called only by the sampler thread. dump_seq is the
// monotonic dump counter. active_threads is the number of CPU-active handles tracked (after the
// GetThreadTimes filter). non-ck3 samples represent genuinely active non-ck3 work (render/driver
// thread etc.), not idle waits.
void dump(std::uint64_t dump_seq, double busy_ms, double wall_ms,
          std::size_t active_threads) {
    const std::uint64_t total = g_hist.total();
    if (total == 0) {
        return;  // nothing sampled yet
    }

    // ck3-total = everything attributed to ck3.exe = total minus the non-ck3
    // sentinel bucket. Used as the denominator for pct_of_ck3.
    const std::vector<ck3accel::SampleHistogram::Entry> all = g_hist.top_n(kAllEntries);
    std::uint64_t non_ck3 = 0;
    for (const auto& e : all) {
        if (e.rva == kRvaNonCk3) {
            non_ck3 = e.count;
            break;
        }
    }
    const std::uint64_t ck3_total = (total >= non_ck3) ? (total - non_ck3) : 0;

    const double overhead_pct =
        (wall_ms > 0.0) ? (busy_ms / wall_ms * 100.0) : 0.0;

    ensure_csv_open();

    // ---- console: overhead banner + top-N ------------------------------------
    if (g_host && g_host->log) {
        char banner[192];
        std::snprintf(banner, sizeof(banner),
            "# sampler_overhead busy_ms=%.1f wall_ms=%.1f pct=%.3f%% "
            "(dump %llu, %llu samples, active_threads=%zu)",
            busy_ms, wall_ms, overhead_pct,
            static_cast<unsigned long long>(dump_seq),
            static_cast<unsigned long long>(total),
            active_threads);
        g_host->log(kLogInfo, banner);
    }

    const std::vector<ck3accel::SampleHistogram::Entry> top = g_hist.top_n(kTopN);
    std::size_t rank = 0;
    for (const auto& e : top) {
        ++rank;
        const double pct_total =
            static_cast<double>(e.count) / static_cast<double>(total) * 100.0;
        const double pct_ck3 =
            (e.rva != kRvaNonCk3 && ck3_total > 0)
                ? static_cast<double>(e.count) / static_cast<double>(ck3_total) * 100.0
                : 0.0;

        if (g_host && g_host->log) {
            char line[160];
            std::snprintf(line, sizeof(line),
                "%2zu rva=0x%08X samples=%llu pct=%.2f%%",
                rank, e.rva,
                static_cast<unsigned long long>(e.count), pct_total);
            g_host->log(kLogInfo, line);
        }

        if (g_csv) {
            std::fprintf(g_csv, "%llu,0x%08X,%llu,%.4f,%.4f\n",
                static_cast<unsigned long long>(dump_seq), e.rva,
                static_cast<unsigned long long>(e.count), pct_total, pct_ck3);
        }
    }

    // ---- CSV: synthetic sentinel rows + overhead/total notes -----------------
    if (g_csv) {
        std::uint64_t ck3_other = 0;
        for (const auto& e : all) {
            if (e.rva == 0u) { ck3_other = e.count; break; }
        }
        std::fprintf(g_csv,
            "# dump_seq=%llu total_samples=%llu ck3_total=%llu "
            "ck3_other(rva=0)=%llu non_ck3(rva=0xFFFFFFFF)=%llu\n",
            static_cast<unsigned long long>(dump_seq),
            static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(ck3_total),
            static_cast<unsigned long long>(ck3_other),
            static_cast<unsigned long long>(non_ck3));
        std::fprintf(g_csv,
            "# dump_seq=%llu sampler_overhead busy_ms=%.1f wall_ms=%.1f pct=%.4f\n",
            static_cast<unsigned long long>(dump_seq),
            busy_ms, wall_ms, overhead_pct);
    }

    // ---- non-ck3 module!export breakdown (the spin-vs-allocator question) -----
    if (non_ck3 > 0 && !g_nonck3.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> syms(g_nonck3.begin(), g_nonck3.end());
        const std::size_t show = (syms.size() < 20) ? syms.size() : 20;
        std::partial_sort(syms.begin(), syms.begin() + show, syms.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
        const bool can_log = g_host && g_host->log;
        if (can_log) g_host->log(kLogInfo, "# non-ck3 breakdown (module!nearest-export, % of non-ck3):");
        for (std::size_t i = 0; i < show; ++i) {
            const double pct = static_cast<double>(syms[i].second) / static_cast<double>(non_ck3) * 100.0;
            if (can_log) {
                char line[192];
                std::snprintf(line, sizeof(line), "   %5.1f%%  %llu  %s",
                    pct, static_cast<unsigned long long>(syms[i].second), syms[i].first.c_str());
                g_host->log(kLogInfo, line);
            }
            if (g_csv)
                std::fprintf(g_csv, "# nonck3 dump_seq=%llu rank=%zu count=%llu pct_of_nonck3=%.3f sym=%s\n",
                    static_cast<unsigned long long>(dump_seq), i + 1,
                    static_cast<unsigned long long>(syms[i].second), pct, syms[i].first.c_str());
        }
    }
    if (g_csv) std::fflush(g_csv);  // dev tool: never lose a dump across a crash

    if (g_host && g_host->report_metric) {
        g_host->report_metric("accel_sampler.overhead_pct", overhead_pct);
    }
}

DWORD WINAPI sampler_thread_main(LPVOID /*param*/) {
    // Raise the Windows timer resolution to 1 ms so Sleep(1) sleeps ~1 ms, not the default
    // ~15.6 ms; otherwise the loop runs at ~60 Hz instead of ~1 kHz (15x fewer samples). Process-
    // wide; timeEndPeriod(1) runs on the kill-switch path (below). As a daemon thread the period
    // would be released at process exit anyway, but we restore it explicitly so the panic key
    // undoes the elevated resolution.
    ::timeBeginPeriod(1);

    // Index loaded modules' export tables once, for non-ck3 RIP attribution.
    build_module_symbols();
    if (g_host && g_host->log) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "accel_sampler: indexed %zu modules for non-ck3 attribution", g_mods.size());
        g_host->log(kLogInfo, msg);
    }

    // PRE-ALLOCATED RIP buffer (the invariant requires no alloc in the window).
    std::uintptr_t rips[kMaxThreads];

    std::vector<HANDLE> handles;
    handles.reserve(kMaxThreads);

    // Per-TID cumulative CPU-time state for the active-thread filter.
    // Owned solely by this (sampler) thread; no lock required.
    std::unordered_map<DWORD, std::uint64_t> lastBusy;
    std::size_t active_count = 0;

    LARGE_INTEGER last_refresh; ::QueryPerformanceCounter(&last_refresh);
    LARGE_INTEGER last_dump = last_refresh;
    LARGE_INTEGER session_start = last_refresh;

    // Accumulated sampler busy time (QPC ticks) for the honest overhead banner.
    // busy_ticks includes the suspend loop, the bucketing, AND the dump I/O so
    // the reported overhead is not understated; only the trailing Sleep is
    // excluded (it is idle time, not sampler CPU).
    LONGLONG busy_ticks = 0;
    std::uint64_t dump_seq = 0;
    bool killed = false;

    // Per-tick pacing: target one tick every (freq / kSampleHz) QPC ticks.
    const LONGLONG ticks_per_sample =
        (g_qpc_freq > 0) ? (g_qpc_freq / static_cast<LONGLONG>(kSampleHz)) : 0;

    refresh_thread_handles(handles, lastBusy, active_count);

    for (;;) {
        LARGE_INTEGER tick_start; ::QueryPerformanceCounter(&tick_start);

        // Kill switch: stop sampling, do a final dump once, then idle. This also
        // halts the profiler when the user hits the panic hotkey (Ctrl+Shift+F12).
        if (!killed && g_host && g_host->is_kill_switch_active &&
            g_host->is_kill_switch_active() != 0) {
            killed = true;
            // Account this (kill) iteration's work into busy_ticks BEFORE reading
            // it for the banner, so the final overhead figure is honest.
            LARGE_INTEGER now; ::QueryPerformanceCounter(&now);
            busy_ticks += (now.QuadPart - tick_start.QuadPart);
            const double wall_ms =
                static_cast<double>(now.QuadPart - session_start.QuadPart) * g_qpc_to_ms;
            const double busy_ms = static_cast<double>(busy_ticks) * g_qpc_to_ms;
            dump(++dump_seq, busy_ms, wall_ms, active_count);
            if (g_host && g_host->log) {
                g_host->log(kLogWarn,
                    "accel_sampler: kill switch active; sampling stopped "
                    "(final dump written)");
            }
            // Restore the default timer resolution now that high-rate sampling
            // has stopped. (Daemon caveat: the OS would release it at process
            // exit anyway, but restoring it explicitly keeps the panic key tidy.)
            ::timeEndPeriod(1);
        }

        if (killed) {
            // Suspend no more threads; just idle so the daemon thread exits with
            // the process. (There is no Shutdown export in the ABI.)
            ::Sleep(250);
            continue;
        }

        // Refresh the open-handle list periodically (OUTSIDE any suspend window).
        // GetThreadTimes is called here (not in the suspend window) to classify
        // threads as active or idle before the next sample tick.
        if (static_cast<double>(tick_start.QuadPart - last_refresh.QuadPart) *
                g_qpc_to_ms >= static_cast<double>(kRefreshMs)) {
            refresh_thread_handles(handles, lastBusy, active_count);
            last_refresh = tick_start;
        }

        // FREEZE-HAZARD GUARD: re-check the kill switch RIGHT BEFORE opening the suspend loop. If
        // panic was signalled since the top-of-loop check (the core may be about to freeze all
        // threads via MinHook), do NOT open a new critical window this tick; loop back and let the
        // kill path run.
        if (g_host && g_host->is_kill_switch_active &&
            g_host->is_kill_switch_active() != 0) {
            continue;
        }

        // ============================ CRITICAL WINDOW ============================
        // SAFETY INVARIANT: between SuspendThread and ResumeThread do NOTHING but
        // read the RIP into the preallocated `rips` buffer. NO heap alloc, NO
        // mutex, NO logging, NO FunctionTable lookup, NO CSV. Each thread is
        // resumed IMMEDIATELY after its context is read, so AT MOST ONE game
        // thread is ever held suspended at a time (bounds the MinHook-freeze
        // interaction). Widening this window can deadlock the game.
        //
        // A stack-local CONTEXT is 16-byte aligned by its type; only
        // ContextFlags is consumed by GetThreadContext on input.
        std::size_t n = 0;
        const std::size_t count =
            (handles.size() < kMaxThreads) ? handles.size() : kMaxThreads;
        for (std::size_t i = 0; i < count; ++i) {
            if (n >= kMaxThreads) {
                break;  // hard buffer guard (local proof, independent of `count`)
            }
            const HANDLE th = handles[i];
            if (::SuspendThread(th) == static_cast<DWORD>(-1)) {
                continue;  // thread gone/inaccessible; do NOT resume what we
                           // never suspended.
            }
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;  // we only need Rip
            const BOOL ok = ::GetThreadContext(th, &ctx);
            const std::uintptr_t rip =
                ok ? static_cast<std::uintptr_t>(ctx.Rip) : 0;
            ::ResumeThread(th);                  // resume IMMEDIATELY, always
            if (ok && rip != 0) {
                rips[n++] = rip;                 // preallocated buffer only
            }
        }
        // ========================== END CRITICAL WINDOW ==========================

        // Now that every sampled thread is resumed, it is safe to do lookups and
        // touch the histogram (owned solely by this thread -> no lock needed).
        for (std::size_t i = 0; i < n; ++i) {
            const std::uintptr_t rip = rips[i];
            if (g_image_size != 0 &&
                rip >= g_module_base &&
                rip < g_module_base + g_image_size) {
                const std::uint32_t rva =
                    static_cast<std::uint32_t>(rip - g_module_base);
                g_hist.add(g_functions.lookup(rva));  // 0 = ck3-other if no fn
            } else {
                g_hist.add(kRvaNonCk3);                // non-ck3
                g_nonck3[resolve_nonck3(rip)] += 1;    // module!export breakdown
            }
        }

        // Periodic dump.
        LARGE_INTEGER after_bucket; ::QueryPerformanceCounter(&after_bucket);
        if (static_cast<double>(after_bucket.QuadPart - last_dump.QuadPart) *
                g_qpc_to_ms >= kDumpEverySec * 1000.0) {
            // Provisionally include this tick's work so far so the dump's own
            // busy time is reflected in the banner it prints.
            const double wall_ms =
                static_cast<double>(after_bucket.QuadPart - session_start.QuadPart) *
                g_qpc_to_ms;
            const double busy_ms =
                static_cast<double>(busy_ticks +
                    (after_bucket.QuadPart - tick_start.QuadPart)) * g_qpc_to_ms;
            dump(++dump_seq, busy_ms, wall_ms, active_count);
            last_dump = after_bucket;
        }

        // Account this tick's busy time (suspend loop + bucketing + any dump I/O),
        // measured AFTER the dump so the dump cost is counted, then sleep the
        // remainder of the period. Only the Sleep is excluded from busy_ticks.
        LARGE_INTEGER tick_end; ::QueryPerformanceCounter(&tick_end);
        busy_ticks += (tick_end.QuadPart - tick_start.QuadPart);
        if (ticks_per_sample > 0) {
            const LONGLONG spent = tick_end.QuadPart - tick_start.QuadPart;
            const LONGLONG remain = ticks_per_sample - spent;
            if (remain > 0) {
                const double remain_ms =
                    static_cast<double>(remain) * g_qpc_to_ms;
                // Sleep granularity is coarse (~1-15 ms); rounding to >=1 ms is
                // fine for a statistical profiler and keeps CPU use low.
                const DWORD ms = static_cast<DWORD>(remain_ms);
                ::Sleep(ms > 0 ? ms : 1);
            }
        } else {
            ::Sleep(1);
        }
    }
    // unreachable (daemon thread)
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),  // struct_size (FIRST)
    CK3ACCEL_PLUGIN_MAGIC,                              // magic
    CK3ACCEL_ABI_VERSION,                              // required_abi
    "accel_sampler",                                   // name (allowlist key)
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
    (void)reg;  // read-only profiler: no hook set used
    g_host = host;
    if (!host || !host->log || !host->is_kill_switch_active) {
        return 1;  // host too old / missing the services we rely on: stay inert
    }

    // 1) Cache ck3.exe base and the QPC->milliseconds factor.
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
        g_qpc_freq  = freq.QuadPart;
        g_qpc_to_ms = 1.0e3 / static_cast<double>(freq.QuadPart);
    }

    // 2) Parse ck3.exe .pdata into the function table (also sets g_image_size).
    const std::size_t fn_count = build_function_table_from_pdata();
    if (fn_count == 0 || g_image_size == 0) {
        host->log(kLogWarn,
            "accel_sampler: could not parse ck3.exe .pdata; profiler inert");
        return 1;
    }

    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "accel_sampler: parsed %zu functions from ck3.exe .pdata "
            "(base=0x%llX image_size=0x%X); starting sampler @ %u Hz",
            fn_count,
            static_cast<unsigned long long>(g_module_base),
            g_image_size, kSampleHz);
        host->log(kLogInfo, msg);
    }

    // 3) Start the sampler thread. It is a daemon: there is NO CK3Accel_Shutdown
    // export in the ABI, so it runs until process exit (acceptable for a dev
    // tool). We do not retain the handle.
    HANDLE th = ::CreateThread(nullptr, 0, &sampler_thread_main, nullptr, 0, nullptr);
    if (th == nullptr) {
        host->log(kLogWarn, "accel_sampler: CreateThread failed; profiler inert");
        return 1;
    }
    ::CloseHandle(th);  // detach; the thread keeps running

    return 0;
}
