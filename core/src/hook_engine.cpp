#include "hook_engine.h"

#include "logger.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

// concrete definition of the handle core_api.h forward-declares. HookSetId is a
// global-scope typedef there (extern "C"), so it's unqualified here.
struct HookHandle {
    HookSetId          set      = 0;
    void*              target    = nullptr;  // patched function entry
    void*              detour    = nullptr;  // replacement (plugin code)
    void*              trampoline = nullptr; // call-original (MinHook-owned)
    bool               enabled   = false;
};

namespace ck3accel {

namespace {

// MinHook trampoline buffers are MEMORY_SLOT_SIZE (64) bytes. we don't know a
// detour's true extent (plugin code), so attribute a conservative entry span;
// deeper detour faults are caught by the sentinel's per-module range check.
constexpr std::size_t kRangeSpan = 64;

std::mutex                                                       g_mutex;
bool                                                             g_initialized = false;
std::atomic<HookSetId>                                           g_next_id{1};
std::unordered_map<HookSetId, std::string>                       g_set_names;
std::unordered_map<HookSetId, std::vector<std::unique_ptr<HookHandle>>> g_sets;

// Caller must hold g_mutex.
void log_mh_failure(const char* op, MH_STATUS st) {
    std::ostringstream os;
    os << "hook_engine: " << op << " failed: " << MH_StatusToString(st);
    LOG_ERROR(os.str());
}

bool in_span(std::uintptr_t addr, void* base) {
    if (base == nullptr) {
        return false;
    }
    const auto b = reinterpret_cast<std::uintptr_t>(base);
    return addr >= b && addr < b + kRangeSpan;
}

// caller holds g_mutex. shared by disable_all() (blocking) and try_disable_all()
// (crash path). soft-disables every enabled hook across all sets in one thread-freeze.
void disable_all_locked() {
    bool any = false;
    for (auto& [id, handles] : g_sets) {
        for (auto& h : handles) {
            if (h->enabled) {
                const MH_STATUS st = MH_QueueDisableHook(h->target);
                if (st != MH_OK) {
                    log_mh_failure("MH_QueueDisableHook", st);
                } else {
                    any = true;
                }
            }
        }
    }
    if (any) {
        const MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK) {
            log_mh_failure("MH_ApplyQueued(disable_all)", st);
            return;
        }
        for (auto& [id, handles] : g_sets) {
            for (auto& h : handles) {
                h->enabled = false;
            }
        }
        LOG_INFO("hook_engine: panic - all hook sets disabled");
    }
}

// caller holds g_mutex. shared by address_in_hooked_range() (blocking) and
// try_address_in_hooked_range() (crash path).
bool address_in_hooked_range_locked(std::uintptr_t a) {
    for (const auto& [id, handles] : g_sets) {
        (void)id;
        for (const auto& h : handles) {
            if (in_span(a, h->trampoline) || in_span(a, h->detour)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool hook_engine_init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        return true;
    }
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK) {
        log_mh_failure("MH_Initialize", st);
        return false;
    }
    g_initialized = true;
    LOG_INFO("hook_engine: MinHook initialized");
    return true;
}

void hook_engine_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return;
    }
    const MH_STATUS st = MH_Uninitialize();
    if (st != MH_OK) {
        log_mh_failure("MH_Uninitialize", st);
    }
    g_sets.clear();
    g_set_names.clear();
    g_initialized = false;
    LOG_INFO("hook_engine: MinHook uninitialized");
}

HookSetId register_hook_set(std::string plugin_name) {
    const HookSetId id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_set_names.emplace(id, std::move(plugin_name));
    g_sets.emplace(id, std::vector<std::unique_ptr<HookHandle>>{});
    return id;
}

HookHandle* install_hook(HookSetId set, void* target, void* detour, void** trampoline_out) {
    std::lock_guard<std::mutex> lock(g_mutex);

    auto set_it = g_sets.find(set);
    if (set_it == g_sets.end()) {
        std::ostringstream os;
        os << "hook_engine: install_hook for unknown set " << set;
        LOG_ERROR(os.str());
        return nullptr;
    }

    void* trampoline = nullptr;
    MH_STATUS st = MH_CreateHook(target, detour, &trampoline);
    if (st != MH_OK) {
        log_mh_failure("MH_CreateHook", st);
        return nullptr;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK) {
        log_mh_failure("MH_EnableHook", st);
        MH_RemoveHook(target);  // best-effort cleanup of the created-but-unenabled hook
        return nullptr;
    }

    auto handle = std::make_unique<HookHandle>();
    handle->set        = set;
    handle->target     = target;
    handle->detour     = detour;
    handle->trampoline = trampoline;
    handle->enabled    = true;

    if (trampoline_out != nullptr) {
        *trampoline_out = trampoline;
    }

    HookHandle* raw = handle.get();
    set_it->second.push_back(std::move(handle));

    std::ostringstream os;
    os << "hook_engine: installed hook for set " << set
       << " (" << g_set_names[set] << ") target=" << target
       << " detour=" << detour;
    LOG_INFO(os.str());
    return raw;
}

void disable_set(HookSetId set) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto set_it = g_sets.find(set);
    if (set_it == g_sets.end()) {
        return;
    }
    bool any = false;
    for (auto& h : set_it->second) {
        if (h->enabled) {
            const MH_STATUS st = MH_QueueDisableHook(h->target);
            if (st != MH_OK) {
                log_mh_failure("MH_QueueDisableHook", st);
            } else {
                any = true;
            }
        }
    }
    if (any) {
        const MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK) {
            log_mh_failure("MH_ApplyQueued(disable_set)", st);
            return;
        }
        for (auto& h : set_it->second) {
            h->enabled = false;
        }
        std::ostringstream os;
        os << "hook_engine: disabled set " << set << " (" << g_set_names[set] << ")";
        LOG_INFO(os.str());
    }
}

void enable_set(HookSetId set) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto set_it = g_sets.find(set);
    if (set_it == g_sets.end()) {
        return;
    }
    bool any = false;
    for (auto& h : set_it->second) {
        if (!h->enabled) {
            const MH_STATUS st = MH_QueueEnableHook(h->target);
            if (st != MH_OK) {
                log_mh_failure("MH_QueueEnableHook", st);
            } else {
                any = true;
            }
        }
    }
    if (any) {
        const MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK) {
            log_mh_failure("MH_ApplyQueued(enable_set)", st);
            return;
        }
        for (auto& h : set_it->second) {
            h->enabled = true;
        }
        std::ostringstream os;
        os << "hook_engine: enabled set " << set << " (" << g_set_names[set] << ")";
        LOG_INFO(os.str());
    }
}

void disable_all() {
    std::lock_guard<std::mutex> lock(g_mutex);
    disable_all_locked();
}

void remove_set(HookSetId set) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto set_it = g_sets.find(set);
    if (set_it == g_sets.end()) {
        return;
    }
    for (auto& h : set_it->second) {
        const MH_STATUS st = MH_RemoveHook(h->target);
        if (st != MH_OK) {
            log_mh_failure("MH_RemoveHook", st);
        }
    }
    g_sets.erase(set_it);
    auto name_it = g_set_names.find(set);
    std::ostringstream os;
    os << "hook_engine: removed set " << set
       << " (" << (name_it != g_set_names.end() ? name_it->second : std::string{"?"}) << ")";
    LOG_INFO(os.str());
    g_set_names.erase(set);
}

bool address_in_hooked_range(const void* addr) {
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    std::lock_guard<std::mutex> lock(g_mutex);
    return address_in_hooked_range_locked(a);
}

bool try_address_in_hooked_range(const void* addr, bool* determined) {
    const auto a = reinterpret_cast<std::uintptr_t>(addr);
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        if (determined != nullptr) {
            *determined = false;
        }
        return false;  // could not verify; caller treats as "undetermined"
    }
    if (determined != nullptr) {
        *determined = true;
    }
    return address_in_hooked_range_locked(a);
}

bool try_disable_all() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;  // contended; crash path does the minimal safe thing
    }
    disable_all_locked();
    return true;
}

} // namespace ck3accel
