#include "panel_registry.h"

#include <atomic>
#include <mutex>

namespace ck3accel {
namespace {
constexpr int kMax = 32;
CK3AccelPanel g_panels[kMax];
std::atomic<int> g_count{0};   // release on publish / acquire on read; slots are write-once
std::mutex g_reg_mtx;          // serializes registration only — reads are lock-free
}

void panel_register(const CK3AccelPanel* p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    const int n = g_count.load(std::memory_order_relaxed);
    if (n >= kMax) return;
    g_panels[n] = *p;                                  // shallow copy; the pointers reference plugin globals (process lifetime)
    g_count.store(n + 1, std::memory_order_release);   // publish the slot only once it's filled
}

int panel_count() { return g_count.load(std::memory_order_acquire); }

const CK3AccelPanel* panel_get(int index) {
    if (index < 0 || index >= g_count.load(std::memory_order_acquire)) return nullptr;
    return &g_panels[index];   // registry is stable after load; the reader dereferences without a lock
}

}  // namespace ck3accel
