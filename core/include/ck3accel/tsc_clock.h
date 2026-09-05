#pragma once
#include <cstdint>
namespace ck3accel {

// rate (TSC ticks/sec) = tsc_delta * qpc_freq / qpc_delta, in double to avoid
// 64-bit overflow of the product. 0 if either wall quantity is 0 (can't calibrate yet).
inline std::uint64_t calibrate_tsc_per_sec(std::uint64_t tsc_delta,
                                           std::uint64_t qpc_delta,
                                           std::uint64_t qpc_freq) {
    if (qpc_delta == 0 || qpc_freq == 0) return 0;
    const double rate = static_cast<double>(tsc_delta) *
                        static_cast<double>(qpc_freq) /
                        static_cast<double>(qpc_delta);
    return static_cast<std::uint64_t>(rate);
}

// ns = tsc_delta * 1e9 / tsc_per_sec (double to avoid overflow). 0 if rate unknown.
inline std::uint64_t tsc_to_ns(std::uint64_t tsc_delta, std::uint64_t tsc_per_sec) {
    if (tsc_per_sec == 0) return 0;
    return static_cast<std::uint64_t>(static_cast<double>(tsc_delta) * 1.0e9 /
                                      static_cast<double>(tsc_per_sec));
}

}  // namespace ck3accel
