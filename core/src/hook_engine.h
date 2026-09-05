#pragma once
#include <ck3accel/core_api.h>   // HookSetId, struct HookHandle (opaque)
#include <string>

namespace ck3accel {

// HookSetId / HookHandle are global-scope in core_api.h (extern "C"). alias them
// into this namespace so callers can keep the ck3accel:: qualifier.
using ::HookHandle;
using ::HookSetId;

constexpr HookSetId kInvalidHookSet = 0;

// Call MH_Initialize exactly once. Idempotent; returns false on MH error.
bool hook_engine_init();
// Call MH_Uninitialize exactly once at shutdown.
void hook_engine_shutdown();

// Allocate a new hook set for a plugin; returns a monotonic id (> 0).
HookSetId register_hook_set(std::string plugin_name);

// create + enable an inline hook in `set`. writes the call-original trampoline to
// *trampoline_out. null on any MinHook failure (logged). target must be executable.
HookHandle* install_hook(HookSetId set, void* target, void* detour, void** trampoline_out);

// batched soft-disable/re-enable of a set (one thread-freeze via MH_Queue*Hook +
// MH_ApplyQueued). disabled calls fall through to the original; trampoline is NOT freed.
void disable_set(HookSetId set);
void enable_set(HookSetId set);

// Soft-disable EVERY hook across ALL sets (panic). Single thread-freeze.
void disable_all();

// Hard teardown: MH_RemoveHook every hook in the set (restores original bytes).
void remove_set(HookSetId set);

// true if addr lies in any installed trampoline/detour we own. used by the crash
// sentinel's VEH to attribute in-range faults.
bool address_in_hooked_range(const void* addr);

// --- crash-path (non-blocking) variants --------------------------------------
// a VEH/unhandled handler runs on the FAULTING thread in arbitrary context. if
// that thread already held g_mutex (faulted inside install_hook / disable_all /
// address_in_hooked_range under the lock), a plain lock_guard would self-deadlock
// into a hang. these use try_lock and NEVER block. crash sentinel only; normal
// callers use the blocking functions above.

// try_lock variant of address_in_hooked_range. on success sets *determined=true
// and returns the in-range result. on contention sets *determined=false and
// returns false (caller treats "undetermined" as not-in-range; the on-disk
// tombstone, written before any fault, still drives next-launch safe-mode).
// determined may be null.
bool try_address_in_hooked_range(const void* addr, bool* determined);

// try_lock variant of disable_all. true iff the lock was acquired and the
// panic-disable ran; false on contention (handler does the minimal safe thing;
// the tombstone already guarantees next-launch disable). disable_all() behavior
// for normal callers is unchanged.
bool try_disable_all();

} // namespace ck3accel
