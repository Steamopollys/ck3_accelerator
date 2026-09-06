# CK3 Accelerator

A runtime performance patch for **Crusader Kings III**. It hooks a few slow engine routines and swaps in faster ones. No save-format changes, no script edits, no content edits.

Third-party tool, not affiliated with or endorsed by Paradox Interactive.

## What it speeds up

Late-game and large-family saves are where CK3 hurts, so that's the target. Three plugins work today, all single-player + Ironman:

- **`accel_family_lists`** replaces the engine's O(N²) dedup of a character's close/extended family lists with an O(N) hash set that produces the same lists. For an immortal, a very old character, or anyone with thousands of relatives, that's the difference between billions of comparisons and a linear pass, and a big part of why birth events stall.
- **`accel_family_cache`** memoizes those lists per frame. Opening a huge-family character sheet rebuilds the same list a few hundred times per portrait; caching it takes that window from **~15s to ~1.2s** (smaller families ~0.35s).
- **`accel_tick_cache`** caches, during the daily tick, the results of trigger classes that are provably pure functions of their scope (`has_trait`, `is_ruler`, doctrines, family relations, and so on), and re-runs everything else. It serves ~45-50% of in-tick trigger evaluations. Triggers are about a third of the tick, so it saves real work; whether the day *feels* faster is something I'm still measuring rather than claiming.

The release ships with all three on. Turn any off in `config.toml`.

## Installing

The accelerator loads by standing in for a DLL the game already loads (`dxcompiler.dll`) and forwarding the real calls to the original. Everything goes in CK3's `binaries` folder (in Steam: right-click Crusader Kings III, Manage, Browse local files, then open `binaries`).

Close the game first, then:

1. In `binaries`, rename the game's own **`dxcompiler.dll`** to **`dxcompiler_orig.dll`**. The proxy forwards to it, so the shader compiler still works.
2. Copy everything from the release zip into `binaries`: `dxcompiler.dll` (the proxy), `ck3accel_core.dll`, `versions.json`, `config.toml`, the `.conf` files, and the `plugins\` folder. The zip layout already matches.
3. That's it. The bundled `config.toml` has all three optimizations on. To disable one, set it to `false` under `[plugins]`.
4. Launch CK3. To confirm it loaded, press **F10** for the overlay (below), set `console = true` in `config.toml [core]`, or check `binaries\logs\`.

To uninstall, delete the files you added and rename `dxcompiler_orig.dll` back to `dxcompiler.dll`.

On a CK3 build it hasn't been tested against, it loads in observe-only mode: no hooks, just a logged fingerprint so the build can be added. You may see a one-time "untested build" notice.

## In-game overlay

Press **F10** in-game to open a control panel drawn over CK3. It lists every loaded plugin with its live counters (cache hits, comparisons saved, and so on) and a checkbox to turn each one on or off without leaving the game. F10 again hides it. It ships on, but the panel stays hidden until you press F10, so you won't notice it until you ask for it. To leave it out, set `overlay = false` in `overlay.conf`, or `accel_demo_overlay = false` in `config.toml`.

### The family-list fix, concretely

The original dedup does, for every character, a full scan of the output list before appending:

```
for every character:
    scan the whole output list
    if the character isn't already in it:
        append it
```

That's O(N²): the scan grows with the list, so a family of N relatives costs on the order of N² comparisons. The replacement keeps a hash set, so the "already in it?" check is O(1) and the whole build is O(N):

```
if (!set.contains(character)) {
    set.insert(character);
    list.push_back(character);
}
```

Same output list, same order. For a handful of relatives the difference is nothing; for thousands it's the difference between a stall and an instant build.

### The character-window cache, concretely

Opening a character sheet runs the portrait clothing rules, and each of the ~300 clothing modifiers checks the same `any_close_family_member` on the same character. Stock rebuilds that list every time:

```
for each of the ~300 clothing checks on one character:
    build the close-family list from scratch
```

The cache keys the built list on (character, which list, filter) and remembers it for the current frame:

```
key = (character, list type, filter)
if key was already built this frame:
    replay the cached list
else:
    build it once, store it under key
```

So ~300 rebuilds collapse to one real build plus cheap replays, which is what takes the window from ~15s to ~1.2s. The cache is thrown away every frame and on any state change, so it never serves a stale list.

### The in-tick trigger cache, concretely

During the daily tick the same pure condition gets evaluated over and over on the same scope. Stock recomputes it every time:

```
each time a trigger is evaluated:
    run the full evaluation, even if identical to a moment ago
```

For trigger classes proven to be pure functions of their scope, the cache returns the previous answer instead:

```
key = (trigger node, this/prev/root scope, saved scopes)
if key was seen this epoch:
    return the cached result
else:
    result = evaluate the trigger; store it under key
```

The epoch is bumped on every script effect and at each tick boundary, so a cached answer is only reused while nothing relevant has changed. Combinators, scripted triggers, and the few impure leaves always re-evaluate, which is why the whole thing stays checksum-neutral.

## Won't it break my Ironman save?

No. That's the rule everything is built around: every cache is **checksum-neutral**, returning exactly what stock CK3 would compute. Cached trigger results are tied to an epoch that's invalidated on every script effect and tick boundary, so nothing stale survives a state change, and any trigger caught mutating its scope is dropped from the cache. If a result could ever diverge from vanilla, it isn't cached.

Multiplayer is a different matter. In theory the same checksum-neutrality keeps a patched and a vanilla client in sync, but I haven't tested it, and MP is unforgiving: it runs in lockstep, so one divergent result on any client desyncs the whole session. The soundness is measured in single-player, not proven, and the runtime assumes single-player anyway. So MP isn't supported: not because I know it breaks, but because the cost if it did is a ruined game for the whole lobby.

Two safety nets:

- **Kill switch:** **Ctrl+Shift+F12** (configurable) disables every hook instantly and falls back to stock CK3, no restart.
- **Per-plugin safe-mode:** if a session crashes inside one of our hooks, only that plugin boots disabled next launch.

## What it doesn't touch (and why)

I profile before optimizing and drop what isn't worth it:

- **Save decompression.** ~4% of load time on a compressed save, and zero on the uncompressed autosaves and large saves CK3 makes.
- **Load time in general.** A late-game load is dominated by disk I/O (game database and mods) plus a serial deserialize where the worker pool idles, not by anything a patch like this can cheaply beat. The one algorithmic win there, the family dedup, is already taken.

## Antivirus / SmartScreen

Windows Defender and some other scanners may flag the core DLL as a false positive. It injects code into a running game, which is what plenty of malware also does, so behavioral heuristics like `Wacatac.B!ml` trip on it even though the code is benign and readable here. ENB, ReShade, and SKSE get flagged for the same reason.

What you can do:

- Verify the download against the published **SHA-256 hashes** before running it.
- Add an exclusion for the install folder if your scanner quarantines it.
- I'm applying for free open-source code signing (SignPath Foundation), which fixes this properly. Until then the hashes are the honest check.

## Supported CK3 version

Only **CK3 1.19.0.6 (Scribe)** on Steam / Windows, including the 2026-06-02 rebuild. Other builds load in observe-only mode.

## Under the hood

The shipping plugins are speed patches, but the mechanism is general: it hooks `ck3.exe` at the machine-code level instead of going through the script layer, so in principle a plugin can reach things script can't. The project is really a small engine-hooking framework that happens to be used for performance.

That mostly matters for the trade-offs the shipping plugins avoid:

- **Game state.** The caches are checksum-neutral and don't change it. A plugin that changed gameplay would.
- **Multiplayer.** Same reason MP is declined: a gameplay change would desync.
- **Game updates.** Hooks are pinned to a specific build, so a patch can break a plugin until it's updated. Script mods weather updates far better.
- **It's low-level.** Getting a hook wrong is a crash or a bad save, not a slow frame.

The shipping plugins stay firmly on the safe side. Anything past that is a different kind of mod with different rules.

## What it is NOT

- Not supported in multiplayer (single-player only).
- Not a script mod, total conversion, or content mod.
- Does not redistribute any bytes from `ck3.exe` or any other Paradox-owned file.

## License

GPLv3. See `LICENSE`.

## Legal posture

This follows the precedent of community engine tools like ENB, ReShade, SKSE, and F4SE: it ships only its own original code plus byte-pattern signatures (facts about a public binary). It redistributes no part of the game executable.
