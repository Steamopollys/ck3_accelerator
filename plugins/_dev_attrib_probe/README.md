# accel_attrib_probe (dev-only, not shipped)

Attribution probe for the huge-family freezes. Observe-only: every hooked function returns its original result. Single-player only. Inert until enabled.

## Enable

1. `config.toml` `[plugins]`: `accel_attrib_probe = true`
2. `attrib_probe.conf` next to `ck3accel_core.dll`:

```
attrib_probe = true          # master switch
nodes = true                 # per-trigger-node counts + cycles (hooks the trigger evaluator)
repeat = true                # (node, entity) repeat rate + result consistency
freeze_stacks = true         # stack-sample the sim thread while a day overruns
ui_stacks = true             # stack-sample the main thread while the message pump stalls
freeze_threshold_ms = 150
ui_threshold_ms = 150
top_nodes = 100
dump_min_evals = 2000000     # dump the node table for days with >= this many trigger evals
```

Each trigger-evaluating thread uses 8 MiB (+16 MiB with `repeat`). With the usual 6-8 such threads that's ~150-200 MiB.

## Outputs (`logs\`)

- `attrib_nodes.csv`, for long/big days: `day_index, wall_ms, day_evals, node, evals, cycles, trigger_id, class`. `class` is the node's RTTI class name (e.g. `?$CAnyInScriptedListTrigger:VCSiblingListBuilder` = `any_sibling`); `node` is its address (stable for the session). Sort by `evals` to find which script constructs burn the day.
- `attrib_repeat.csv`, per day: `(node, entity)` keyed repeat % and inconsistency %. The entity in the key is what makes this the right measure for the trigger-cache question.
- `attrib_stacks.txt`, one block per burst: `kind=day` (sim thread during an over-long day) or `kind=ui` (main thread while the message pump stalled >= threshold, e.g. opening the character window). Top leaf functions, top inclusive functions, and top full stacks (leaf -> root) as ck3.exe function RVAs (`0xFFFFFFFF` = outside ck3.exe). Resolve names with `tools/re`.

## Safety

Stack unwinding uses only exception tables mapped at init/first tick, never `RtlLookupFunctionEntry`. Reads are SEH-guarded. At most one thread is suspended at a time, resumed right after its register/stack reads. The message-pump hooks (`PeekMessageW`/`GetMessageW`) only timestamp calls from the main thread.

## Bring-up order

1. `nodes=true`, rest false: confirm `attrib_nodes.csv` fills on a birth day.
2. Add `freeze_stacks=true`.
3. Add `ui_stacks=true`, then open the character window / court positions of the big-family ruler while paused.
