// accel_save_load: replaces CK3's gamestate decompression with libdeflate.
//
// Substitutes the engine's stream->stream raw-DEFLATE pump (ck3.exe 1.19.0.6 RVA
// 0x035BEAB0). A bug corrupts a save load; the win is small (~300-640 ms of a ~15 s
// load, zero on uncompressed saves), so output must be byte-identical and CRC-verified.
//
// Hooked wrapper contract (static RE + a real .ck3 save):
//   bool __fastcall decompress(reader /*rcx*/, writer /*rdx*/,
//                              uint32_t compressedSize /*r8d*/, uint32_t expectedCrc32 /*r9d*/);
//   * reader: [reader+0]=vtable; read is vtable[0x20]:
//       uint32_t read(reader /*rcx*/, dst /*rdx*/, uint32_t n /*r8d*/), returns bytes read.
//     Stock requests <=1 MiB and trusts full delivery; we loop to compressedSize and stop
//     if read() returns 0, so a short reader can't hang or over-read.
//   * writer: [writer+0]=vtable; write is vtable[0x50]:
//       void write(writer /*rcx*/, src /*rdx*/, uint32_t n /*r8d*/).
//     Stock chunks ~8 MiB; we chunk <=8 MiB per call to match it.
//   * Stream is RAW DEFLATE (inflate_state.wrap==0, windowBits -15), not zlib/gzip-wrapped:
//     use libdeflate_deflate_decompress, never libdeflate_zlib_decompress.
//   * Return = (crc32(0, out) == expectedCrc32); the engine's CRC is zlib crc32 (init 0),
//     bit-identical to libdeflate_crc32, so this fn IS the save's integrity check. A false
//     return runs the caller's decompress-failed cleanup and aborts the load (verified at
//     the sole call site RVA 0x035BFC3C), so false-without-writing is the engine's own safe
//     failure mode and never corrupts a save.
//
// Fallback: draining the reader is irreversible, so we can't defer to the original.
// libdeflate only rejects malformed DEFLATE (BAD_DATA) or signals INSUFFICIENT_SPACE
// (cured by growing); on valid data with room it yields the unique correct output. So we
// write and return success only when libdeflate succeeds AND the CRC matches; otherwise
// write nothing and return false (handled like a corrupt save). Never wrong or partial output.

#include <ck3accel/core_api.h>

#include <libdeflate.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

namespace {

// LogLevel mirror (CoreApi.log takes an int; core maps it back to LogLevel):
//   0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Critical
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;
constexpr int kLogError = 4;

const CoreApi* g_host = nullptr;

// ---- ABI mirrors of the engine reader/writer interfaces ---------------------
// We read the first vtable pointer and call one slot through it, so we model just
// enough layout. MSVC x64 fastcall.
using read_fn  = std::uint32_t(__fastcall*)(void* self, void* dst, std::uint32_t n);
using write_fn = void(__fastcall*)(void* self, const void* src, std::uint32_t n);

struct ReaderAbi {
    // vtable is a flat array of function pointers; read is at byte offset 0x20.
    void** vtable;
};
struct WriterAbi {
    // write is at byte offset 0x50 in the writer vtable.
    void** vtable;
};

read_fn reader_read_method(void* reader) {
    void** vtbl = *reinterpret_cast<void***>(reader);   // [reader+0x00]
    void* slot  = vtbl[0x20 / sizeof(void*)];           // vtable[+0x20]
    return reinterpret_cast<read_fn>(slot);
}
write_fn writer_write_method(void* writer) {
    void** vtbl = *reinterpret_cast<void***>(writer);   // [writer+0x00]
    void* slot  = vtbl[0x50 / sizeof(void*)];           // vtable[+0x50]
    return reinterpret_cast<write_fn>(slot);
}

// ---- original wrapper, captured by the hook engine's trampoline -------------
using wrapper_fn = std::uint8_t(__fastcall*)(void* reader, void* writer,
                                             std::uint32_t compressedSize,
                                             std::uint32_t expectedCrc32);
wrapper_fn g_orig_wrapper = nullptr;

// 1 MiB drain chunk: matches the engine's request size, keeping the reader on its
// expected code path.
constexpr std::uint32_t kReadChunk = 0x100000u;

// 8 MiB, matches the engine's observed write chunk size.
constexpr std::uint32_t kWriteChunk = 0x800000u;

void log(int level, const char* msg) {
    if (g_host && g_host->log) {
        g_host->log(level, msg);
    }
}

// Drain exactly compressedSize bytes from the reader into out. Returns true iff we got
// the full amount; false if read() ever yields 0, so a bad reader can't hang or over-read.
bool drain_reader(void* reader, std::uint32_t compressedSize,
                  std::vector<std::uint8_t>& out) {
    out.resize(compressedSize);
    const read_fn read = reader_read_method(reader);
    std::uint32_t got = 0;
    while (got < compressedSize) {
        const std::uint32_t want =
            (compressedSize - got < kReadChunk) ? (compressedSize - got) : kReadChunk;
        const std::uint32_t n = read(reader, out.data() + got, want);
        if (n == 0) {
            return false;  // short read; cannot reconstruct the full input
        }
        // Guard against a reader that over-reports.
        got += (n > want) ? want : n;
    }
    return true;
}

// The detour. Mirrors the engine's success/failure semantics exactly.
std::uint8_t __fastcall detour_decompress(void* reader, void* writer,
                                          std::uint32_t compressedSize,
                                          std::uint32_t expectedCrc32) {
    // Fail closed if anything essential is missing (engine aborts the load). Never delegate
    // to g_orig_wrapper: by the time we detect a problem we may have already drained the reader.
    if (!reader || !writer) {
        return 0u;
    }
    if (compressedSize == 0u) {
        // Nothing to read; an empty stream decompresses to nothing. crc32(0, empty)
        // == 0, so success only if the engine expected 0.
        return (expectedCrc32 == 0u) ? 1u : 0u;
    }

    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    ::QueryPerformanceCounter(&t0);

    // 1) Buffer the entire compressed input.
    std::vector<std::uint8_t> in;
    bool drained = false;
    try {
        drained = drain_reader(reader, compressedSize, in);
    } catch (...) {
        drained = false;  // allocation failure etc.: fail closed.
    }
    if (!drained) {
        log(kLogError, "accel_save_load: short read draining compressed input; "
                       "aborting load (no output written)");
        return 0u;
    }

    // 2) Decompress (RAW deflate), growing the output buffer on INSUFFICIENT_SPACE.
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) {
        log(kLogError, "accel_save_load: libdeflate_alloc_decompressor failed; "
                       "aborting load");
        return 0u;
    }

    std::vector<std::uint8_t> out;
    std::size_t produced = 0;
    libdeflate_result rc = LIBDEFLATE_INSUFFICIENT_SPACE;
    bool ok = false;
    try {
        // Start at 4x compressed size (doubling grow-loop handles undersize), with a floor.
        std::size_t cap = static_cast<std::size_t>(compressedSize) * 4u;
        if (cap < (1u << 20)) {
            cap = (1u << 20);
        }
        for (;;) {
            out.resize(cap);
            produced = 0;
            rc = libdeflate_deflate_decompress(dec, in.data(), in.size(),
                                               out.data(), out.size(), &produced);
            if (rc == LIBDEFLATE_INSUFFICIENT_SPACE) {
                if (cap > (std::size_t(1) << 40)) {  // 1 TiB sanity ceiling
                    log(kLogError,
                        "accel_save_load: decompressed size exceeded 1 TiB "
                        "ceiling; bailing");
                    break;
                }
                cap *= 2u;
                continue;
            }
            break;  // SUCCESS, BAD_DATA, or SHORT_OUTPUT
        }
        ok = (rc == LIBDEFLATE_SUCCESS);
    } catch (...) {
        ok = false;
    }

    if (!ok) {
        libdeflate_free_decompressor(dec);
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "accel_save_load: libdeflate raw decompress failed (rc=%d); "
            "aborting load (no output written)", static_cast<int>(rc));
        log(kLogError, msg);
        return 0u;  // engine treats this exactly like a failed/corrupt decompress
    }

    // 3) Verify integrity against the engine's expected CRC BEFORE writing a byte.
    //    libdeflate_crc32 is the same standard CRC-32 (init 0) the engine uses.
    const std::uint32_t crc =
        libdeflate_crc32(0u, out.data(), produced);
    libdeflate_free_decompressor(dec);

    if (crc != expectedCrc32) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "accel_save_load: CRC mismatch (computed=0x%08X expected=0x%08X); "
            "no output written — engine will load stock (save may be corrupt)",
            crc, expectedCrc32);
        log(kLogError, msg);
        return 0u;  // never write output we cannot vouch for
    }

    // 4) CRC verified: hand the buffer to the engine's writer, chunked <=8 MiB per call to
    //    match the engine and avoid an unverified large-write assumption.
    if (produced > 0) {
        const write_fn write = writer_write_method(writer);
        std::size_t off = 0;
        while (off < produced) {
            const std::size_t remaining = produced - off;
            const std::uint32_t n =
                (remaining > kWriteChunk) ? kWriteChunk
                                          : static_cast<std::uint32_t>(remaining);
            write(writer, out.data() + off, n);
            off += n;
        }
    }

    ::QueryPerformanceCounter(&t1);
    {
        LARGE_INTEGER freq;
        if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
            const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) *
                              1000.0 / static_cast<double>(freq.QuadPart);
            if (g_host && g_host->report_metric) {
                g_host->report_metric("accel_save_load.gamestate_decompress_ms", ms);
            }
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "accel_save_load: gamestate decompressed %u -> %zu bytes in %.1f ms"
                " (libdeflate, CRC OK)",
                compressedSize, produced, ms);
            log(kLogInfo, msg);
        }
    }

    return 1u;  // matches the engine's success return (crc verified above)
}

// Unique signature for the gamestate decompression wrapper (ck3.exe 1.19.0.6, RVA
// 0x035BEAB0): prologue through the distinctive 'mov ecx, 0x1BF0' (inflate_state alloc
// size) + alloc call, rel32/RIP-disp32 operands wildcarded. One match in .text
// (see sigs/1.19.0.6.yaml).
const char* const kSaveDecompressSig =
    "48 89 5C 24 10 44 89 4C 24 20 44 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 "
    "41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC C0 00 00 00 45 8B E1 4C 8B EA 4C "
    "8B F1 33 F6 48 89 75 A7 48 8D 05 ?? ?? ?? ?? 48 89 45 B7 48 89 75 C7 4C 8D "
    "3D ?? ?? ?? ?? 4C 89 7D BF B9 F0 1B 00 00 E8 ?? ?? ?? ?? 48 8B D8";

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),  // struct_size (FIRST)
    CK3ACCEL_PLUGIN_MAGIC,                              // magic
    CK3ACCEL_ABI_VERSION,                              // required_abi
    "accel_save_load",                                 // name (allowlist key)
    "0.1.0",                                           // semver
    "1.19.0.6",                                        // min_game_version
    "1.19.0.6",                                        // max_game_version
    CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN,          // mode_flags
};

}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->scan || !host->install_hook || !reg) {
        return 1;  // host too old / missing services: refuse, game stays stock
    }

    void* addr = host->scan(kSaveDecompressSig);
    if (!addr) {
        log(kLogWarn,
            "accel_save_load: gamestate decompress signature not found on this "
            "build; refusing to hook (game stays stock)");
        return 1;  // soft-fail: nothing installed, engine decompresses as normal
    }

    struct HookHandle* h = host->install_hook(
        reg->hook_set, addr,
        reinterpret_cast<void*>(&detour_decompress),
        reinterpret_cast<void**>(&g_orig_wrapper));
    if (!h) {
        log(kLogError,
            "accel_save_load: install_hook failed; refusing (game stays stock)");
        return 1;
    }

    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "accel_save_load: hooked gamestate decompress @ 0x%llX "
        "(raw-deflate via libdeflate, CRC-verified)",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)));
    log(kLogInfo, msg);
    return 0;  // success
}
