#include "pe_inspect.h"

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <vector>

namespace ck3accel {

TextSection inspect_main_module() {
    TextSection out;

    HMODULE main_mod = GetModuleHandleW(nullptr);
    if (!main_mod) return out;

    // Resolve module name.
    {
        std::vector<wchar_t> buf(MAX_PATH);
        DWORD len = 0;
        for (;;) {
            len = GetModuleFileNameW(main_mod, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0) return out;
            if (len < buf.size()) break;
            buf.resize(buf.size() * 2);
        }
        std::filesystem::path p(buf.begin(), buf.begin() + len);
        out.module_name = p.filename().string();
    }

    auto* base = reinterpret_cast<const std::uint8_t*>(main_mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    out.pe_timestamp = nt->FileHeader.TimeDateStamp;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            out.base = base + sec->VirtualAddress;
            out.size = sec->Misc.VirtualSize;
            break;
        }
    }
    return out;
}

std::span<const std::uint8_t> as_bytes(const TextSection& t) {
    if (!t.valid()) return {};
    return { t.base, t.size };
}

} // namespace ck3accel
