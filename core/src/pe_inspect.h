#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace ck3accel {

struct TextSection {
    const std::uint8_t* base = nullptr;   // pointer into target module's image
    std::size_t size = 0;
    std::uint32_t pe_timestamp = 0;       // IMAGE_FILE_HEADER.TimeDateStamp
    std::string module_name;              // e.g. "ck3.exe"
    bool valid() const { return base && size > 0; }
};

// main exe module (GetModuleHandle(NULL)): its .text section, PE timestamp, and
// file name. invalid TextSection on failure.
TextSection inspect_main_module();

// non-owning view over the .text bytes.
std::span<const std::uint8_t> as_bytes(const TextSection& t);

} // namespace ck3accel
