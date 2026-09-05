#pragma once
#include <cstdint>
#include <algorithm>
#include <vector>
namespace ck3accel {
class FunctionTable {
public:
    void add(std::uint32_t begin_rva, std::uint32_t end_rva) {  // half-open [begin,end)
        if (end_rva > begin_rva) ranges_.push_back({begin_rva, end_rva});
    }
    void finalize() {  // sort by begin; idempotent
        std::sort(ranges_.begin(), ranges_.end(),
                  [](const Range& a, const Range& b){ return a.begin < b.begin; });
        finalized_ = true;
    }
    // begin-RVA of the containing function, or 0 if none / not finalized.
    std::uint32_t lookup(std::uint32_t rva) const {
        if (!finalized_ || ranges_.empty()) return 0u;
        // last range whose begin <= rva
        std::size_t lo = 0, hi = ranges_.size();
        while (lo < hi) { std::size_t mid = lo + (hi - lo)/2;
            if (ranges_[mid].begin <= rva) lo = mid + 1; else hi = mid; }
        if (lo == 0) return 0u;
        const Range& r = ranges_[lo - 1];
        return (rva < r.end) ? r.begin : 0u;
    }
    std::size_t size() const { return ranges_.size(); }
private:
    struct Range { std::uint32_t begin; std::uint32_t end; };
    std::vector<Range> ranges_;
    bool finalized_ = false;
};
} // namespace ck3accel
