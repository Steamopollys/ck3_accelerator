#pragma once
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <vector>
namespace ck3accel {
class SampleHistogram {
public:
    void add(std::uint32_t fn_rva) { ++counts_[fn_rva]; ++total_; }
    std::uint64_t total() const { return total_; }
    struct Entry { std::uint32_t rva; std::uint64_t count; };
    std::vector<Entry> top_n(std::size_t n) const {  // sorted desc by count, tie-break by rva asc
        std::vector<Entry> v; v.reserve(counts_.size());
        for (const auto& kv : counts_) v.push_back({kv.first, kv.second});
        std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b){
            return a.count != b.count ? a.count > b.count : a.rva < b.rva; });
        if (v.size() > n) v.resize(n);
        return v;
    }
private:
    std::unordered_map<std::uint32_t, std::uint64_t> counts_;
    std::uint64_t total_ = 0;
};
} // namespace ck3accel
