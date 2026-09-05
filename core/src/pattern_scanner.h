#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ck3accel {

// Parsed signature: bytes + mask (true = must match, false = wildcard "?"/"??").
struct Signature {
    std::vector<std::uint8_t> bytes;
    std::vector<bool>         mask;
    bool valid() const { return !bytes.empty() && bytes.size() == mask.size(); }
};

// parse an IDA pattern: space-separated hex byte pairs; "?"/"??" = wildcard.
// invalid (empty) Signature on any parse error.
Signature parse_signature(std::string_view ida_pattern);

enum class ScanStatus { NotFound, Ambiguous, Found };

struct ScanResult {
    ScanStatus           status      = ScanStatus::NotFound;
    std::size_t          match_count = 0;
    const std::uint8_t*  address     = nullptr;  // valid only when status == Found
};

// scan the whole span, count every match: 0=NotFound, 1=Found, >1=Ambiguous.
ScanResult scan_span(std::span<const std::uint8_t> haystack, const Signature& sig);

// parse the pattern and scan the main module's (ck3.exe) .text. NotFound if
// parse fails or .text is unavailable.
ScanResult scan_text(std::string_view ida_pattern);

} // namespace ck3accel
