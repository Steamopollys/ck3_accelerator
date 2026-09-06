#pragma once
// Resolve a script-node's C++ class name from its vtable via MSVC RTTI (vtable[-8] -> CompleteObjectLocator
// -> TypeDescriptor -> mangled name). SEH-guarded: any bad pointer read just fails instead of crashing.
// Shared by every plugin that keys on trigger class (tick cache, override demo, dev probes) so the walk
// and its quirks (skip ".?AV"/".?AU", drop the "?$" template marker, '@'->':' ) live in exactly one place.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <excpt.h>

namespace ck3accel {
namespace rtti {

// The loaded ck3.exe image, so pointer reads can be bounds-checked before dereferencing.
struct ImageRange { std::uintptr_t base; std::uint32_t size; };

inline bool in_image(const ImageRange& img, const void* q, std::size_t n = 8) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(q);
    return img.base && a >= img.base && a + n <= img.base + img.size;
}

// First qword of a node is its vtable pointer; 0 if the node itself is unreadable.
inline std::uint64_t read_vtable(void* node) {
    __try { return *reinterpret_cast<std::uint64_t*>(node); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Demangle the class name for a vtable into out (NUL-terminated). false on any bad read.
inline bool class_name(const ImageRange& img, std::uint64_t vt, char* out, std::size_t cap) {
    __try {
        const std::uint8_t* v = reinterpret_cast<const std::uint8_t*>(vt);
        if (!in_image(img, v - 8)) return false;
        const std::uint8_t* col = *reinterpret_cast<const std::uint8_t* const*>(v - 8);
        if (!in_image(img, col, 24)) return false;
        std::uint32_t td_rva; std::memcpy(&td_rva, col + 0xC, 4);
        const std::uint8_t* td = reinterpret_cast<const std::uint8_t*>(img.base) + td_rva;
        if (!in_image(img, td, 0x20)) return false;
        const char* m = reinterpret_cast<const char*>(td + 0x10);
        if (m[0] != '.' || m[1] != '?') return false;
        const char* c0 = m + 4;                             // skip ".?AV"/".?AU"
        if (c0[0] == '?' && c0[1] == '$') c0 += 2;          // skip the "?$" template-class marker (e.g. has_trait)
        std::size_t o = 0;
        for (const char* c = c0; *c && o + 1 < cap && c < m + 300; ++c) {
            if (c[0] == '@' && c[1] == '@') { ++c; continue; }
            out[o++] = (*c == '@') ? ':' : *c;
        }
        out[o] = 0; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace rtti
}  // namespace ck3accel
