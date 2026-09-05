#include "pattern_scanner.h"
#include "pe_inspect.h"

#include <cstddef>

namespace ck3accel {

namespace {

// hex digit to 0..15, or -1 if not hex.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// True if `s` is a wildcard token ("?" or "??").
bool is_wildcard(std::string_view s) {
    return s == "?" || s == "??";
}

} // namespace

Signature parse_signature(std::string_view ida_pattern) {
    Signature sig;

    std::size_t i = 0;
    const std::size_t n = ida_pattern.size();
    while (i < n) {
        while (i < n && is_space(ida_pattern[i])) ++i;
        if (i >= n) break;

        // one whitespace-delimited token.
        const std::size_t start = i;
        while (i < n && !is_space(ida_pattern[i])) ++i;
        const std::string_view tok = ida_pattern.substr(start, i - start);

        if (is_wildcard(tok)) {
            sig.bytes.push_back(0u);
            sig.mask.push_back(false);
            continue;
        }

        // else exactly two hex digits.
        if (tok.size() != 2) {
            return Signature{};
        }
        const int hi = hex_value(tok[0]);
        const int lo = hex_value(tok[1]);
        if (hi < 0 || lo < 0) {
            return Signature{};
        }
        sig.bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        sig.mask.push_back(true);
    }

    if (sig.bytes.empty()) {
        return Signature{};
    }
    return sig;
}

ScanResult scan_span(std::span<const std::uint8_t> haystack, const Signature& sig) {
    ScanResult result;

    if (!sig.valid()) {
        return result;
    }

    const std::size_t hay_len = haystack.size();
    const std::size_t sig_len = sig.bytes.size();
    if (sig_len > hay_len) {
        return result;  // pattern cannot fit: NotFound, count 0
    }

    const std::uint8_t* first_hit = nullptr;
    std::size_t count = 0;

    // last start offset where the pattern still fits.
    const std::size_t last_start = hay_len - sig_len;
    for (std::size_t off = 0; off <= last_start; ++off) {
        bool matched = true;
        for (std::size_t k = 0; k < sig_len; ++k) {
            if (sig.mask[k] && haystack[off + k] != sig.bytes[k]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            ++count;
            if (count == 1) {
                first_hit = haystack.data() + off;
            }
        }
    }

    result.match_count = count;
    if (count == 0) {
        result.status = ScanStatus::NotFound;
    } else if (count == 1) {
        result.status  = ScanStatus::Found;
        result.address = first_hit;
    } else {
        result.status = ScanStatus::Ambiguous;  // address stays null
    }
    return result;
}

ScanResult scan_text(std::string_view ida_pattern) {
    Signature sig = parse_signature(ida_pattern);
    if (!sig.valid()) {
        return ScanResult{};
    }

    TextSection text = inspect_main_module();
    if (!text.valid()) {
        return ScanResult{};
    }

    return scan_span(as_bytes(text), sig);
}

} // namespace ck3accel
