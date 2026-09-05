# accel_family_lists

Shipping plugin. Fixes the engine's O(N^2) cost of building the composite family scope lists (`close_family_member`, `extended_family_member`, `close_or_extended_family_member`, in every `any_/every_/random_/ordered_` form) for characters with huge families (immortals with thousands of children, alive or dead).

## What it changes

CK3 deduplicates those lists by linearly scanning the whole output list for every relative it adds, in two places: the add-unique helper and the family walker. For N relatives that's N^2/2 compares; at N ~ 40 000 (an immortal's child: siblings, niblings, cousins, dead included) that's ~0.8 s per build, and `birth.8001` builds it twice per birth. The plugin hooks both primitives and reimplements them on a hash set that mirrors the list. The output list is identical, same entries and same order, which `core/test/test_family_dedup.cpp` proves against a literal transcription of the engine's algorithm on random family graphs. Nothing else is touched.

It accounts for ~16% of the child-birth freeze on the big late-game save; the rest is script-evaluation volume.

## Enable

```toml
[plugins]
accel_family_lists = true
```

Single-player and Ironman, checksum-neutral (identical results). Declines multiplayer by policy. Inert (logs a warning, changes nothing) if a signature doesn't resolve on the running build or the decoded engine globals look wrong.

## Observability

Every 60 s while lists are being built it logs `accel_family_lists: builds=… entries=… linear-compares-avoided=…` (and the same as metrics when telemetry is on). `linear-compares-avoided` counts the 16-byte entry compares the stock code would have run.

## Signatures (1.19.0.6, June-2026 rebuild)

- family walker `fn_01A5F8C0`: `4C 89 44 24 18 41 54 41 55 48 83 EC 58 48 8B 82 A0 01 00 00 4C 8B E9 48 85 C0 74 ??`
- add-unique `fn_02688C70`: `48 83 EC 38 48 8B 01 45 33 D2 4C 63 41 0C 44 8B 4A 18 49 C1 E0 04 4C 03 C0 C7 44 24 20 04 00 00 00`
- array push_back is resolved from the E8 call sites inside both (they must agree); the character-db, null-sentinel, and empty-array globals are decoded from the walker's RIP-relative loads.
