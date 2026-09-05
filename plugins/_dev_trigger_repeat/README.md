# accel_trigger_repeat (dev-only)

Observe-only probe for the trigger-cache GO/NO-GO. Per in-game day it measures the trigger evaluator's (scope,trigger-id) REPEAT RATE (the cache hit ceiling) and RESULT CONSISTENCY (whether a tick-flush cache stays byte-identical). It hooks only the trigger evaluator and UpdateTurnTick, builds no cache, and returns every trigger result unchanged. SP-only. Never shipped.

## Enable

Create `trigger_repeat.conf` next to `ck3accel_core.dll` (the install dir):

    trigger_repeat = true   # master switch

Add `accel_trigger_repeat` to the core `[plugins]` allowlist and disable the other probes for a clean run.

## Output

`logs/trigger_repeat.csv`, one row per in-game day: `day_index,total_evals,distinct_keys,repeat_pct,inconsistent_keys,inconsistency_pct,distinct_keys_ptr,repeat_pct_ptr,overflow`.

- `repeat_pct` (content key = entity handle): the within-tick cache hit-rate ceiling.
- `inconsistency_pct`: of keys evaluated more than once a day, the % that returned more than one result (a tick-flush cache gets these WRONG). Must be ~0 for the cache to be sound.
- `repeat_pct_ptr`: lower bound using the raw scope pointer (undercounts if wrappers realloc).
- `overflow` > 0: a day exceeded table capacity; bump `kCap` and re-run.

## Decision rule

GO if `repeat_pct` is high (>50%) AND `inconsistency_pct` ~0 (<<0.1%). Low repeat: not worth caching. Non-trivial inconsistency: tick-flush is unsound and needs a finer window (the data shows how much).

## Caveats

- RVAs/signatures tied to this ck3.exe build; soft-fails inert on a signature miss.
- Memory: two 32 MiB tables per worker thread plus a 32 MiB merge pair (a dev measurement cost).
