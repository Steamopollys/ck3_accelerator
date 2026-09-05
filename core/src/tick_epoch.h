#pragma once
#include <cstdint>

// shared tick-epoch service (core-owned).
//
// MinHook can't hook one function twice, but every cache plugin wants the same two
// signals off the effect executor and UpdateTurnTick: an epoch that bumps on each
// effect and tick boundary (so a tagged cache entry can't outlive a change), and an
// in-tick depth. the core hooks both once and hands the signals out.
//
// lazy: nothing is hooked until a consumer calls tick_epoch_ensure(), so a stock or
// probe-only session installs nothing here (lets dev probes keep hooking the same
// two functions). the hooks live in their own set, so the panic kill-switch drops them too.

namespace ck3accel {

// install the effect + tick hooks if not already (idempotent, thread-safe). true
// once both are live. a caller that gets false must stay inert: no signals, no sound cache.
bool tick_epoch_ensure();

// the epoch. starts at 1, only increases. meaningful only after tick_epoch_ensure() succeeds.
std::uint32_t tick_epoch_get();

// > 0 while inside UpdateTurnTick.
int tick_epoch_in_tick();

// force a bump (e.g. a plugin's periodic safety flush). no-op if the service isn't live.
void tick_epoch_bump();

}  // namespace ck3accel
