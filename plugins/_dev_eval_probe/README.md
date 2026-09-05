# accel_eval_probe (dev-only)

Observe-only probe for the GO/NO-GO call: is Jomini trigger/event eval a big share of the late-game tick, and where's the child-birth freeze? SP-only. Never shipped. Mutates nothing.

## Enable

Create `eval_probe.conf` in the install dir (the parent of `\plugins`), next to `accel_eval_probe.dll`. All keys default OFF:

    eval_probe = true          # master switch (required)
    probe_freeze = true        # UpdateTurnTick day timing + freeze RIP burst
    probe_dispatch = false     # parallel-for chunk-runner timing
    probe_trigger = false      # jomini_trigger.cpp
    probe_eventtarget = false  # jomini_eventtarget.cpp
    probe_eventscope = false   # jomini_eventscope.cpp
    probe_link = false         # CJominiEventTargetLink vmethod
    freeze_threshold_ms = 150  # a "long day" is >= this
    dump_interval_s = 5

Add `accel_eval_probe` to the core `[plugins]` allowlist. Keep `accel_sampler` and `accel_save_load` off for a clean profile.

## Bring-up order

One flag at a time, so you can bisect any instability:

1. `probe_freeze` only: lowest risk, targets the child-birth freeze directly.
2. add `probe_dispatch`: tick time inside parallel-for bodies.
3. add the four trigger hooks last: highest-frequency and recursive.

Ctrl+Shift+F12 kills all hooks; then turn the offending flag off.

## Output

- `logs/eval_probe.csv`, every `dump_interval_s`: `dump_seq,hook,calls,calls_per_sec,inclusive_ns,pct_of_measured_tick_wall`. `inclusive_ns` is depth-guarded top-level inclusive (recursion-safe). Dispatch-inclusive already contains nested lambda/trigger time, so don't sum dispatch and trigger inclusive.
- `logs/eval_freeze.csv`, one row per long day: `day_index,wall_ms,d_dispatch,d_trigger,d_eventtarget,d_eventscope,d_link` (per-hook call deltas that day), plus `# burst day=... rva=... samples=...` rows for the top functions the sim thread sat in during the freeze.

## Decision rule

- LOW (few calls AND < 2-3% of tick wall): NO-GO.
- HOT (> 10-15% of tick wall): GO. A v2 then adds per-job vtable naming + (scope,trigger-id) repeat-rate to confirm cacheability.

## Caveats

- RVAs/signatures are tied to this ck3.exe build; a patch moves them and the probe soft-fails inert on a miss.
- `UpdateTurnTick` has 3 E8 callers; only one is the real day driver. Confirm the day count tracks the in-game date before trusting freeze rows.
