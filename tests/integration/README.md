# CK3 Accelerator integration harness

Manual, opt-in harness. It is NOT run in CI. CI (GitHub Actions, `windows-2022`) runs only the C++ unit tests via `ctest --preset default`, which need no game install. This harness drives real Crusader Kings III and must be run by hand on a machine with CK3 installed and the accelerator deployed.

## What it proves

The byte-identical exit gate: a save loaded with `accel_save_load.dll` active must produce an autosave byte-for-byte identical to one from stock CK3. The libdeflate swap is CRC-validated and falls back to stock zlib on any mismatch, so the decompressed bytes, and thus the re-serialized world state, must match. The gate checks this across three reference points: early, mid, and late.

## Fixtures are yours, never checked in

`.ck3` saves contain game state derived from `ck3.exe` and your own playthrough. We never redistribute Paradox-owned bytes, so the input saves and goldens are your fixtures, captured on your machine, living only in the git-ignored `fixtures/`.

## One-time setup

1. Install pytest:
   ```powershell
   python -m pip install pytest
   ```
2. Create the fixtures directory (in case it was cleaned):
   ```powershell
   New-Item -ItemType Directory -Force -Path 'tests\integration\fixtures'
   ```
3. Capture the input saves: pick an early, a mid, and a late save and copy them in as the inputs:
   ```powershell
   $fx = 'tests\integration\fixtures'
   Copy-Item '<your-early-save>.ck3' "$fx\early_input.ck3"
   Copy-Item '<your-mid-save>.ck3'   "$fx\mid_input.ck3"
   Copy-Item '<your-late-save>.ck3'  "$fx\late_input.ck3"
   ```
4. Capture the goldens with the accelerator uninstalled (see the root README). This is the critical step: a golden must reflect stock CK3. For each reference point, load `<stem>_input.ck3`, advance `ADVANCE_DAYS` (30) days, let CK3 autosave, and copy `save games\autosave.ck3` to `fixtures\<stem>_golden.ck3`. The comparison reads `<stem>_golden.ck3`.

A reference point missing its `*_input.ck3` or `*_golden.ck3` is skipped, not failed, so a partial set still runs what it can.

## Running the gate (scripted)

1. Deploy the accelerator per the root README, with `accel_save_load.dll` active.
2. Fully close CK3 first (it locks the DLLs; `Get-Process ck3*` must return nothing).
3. Run:
   ```powershell
   python -m pytest tests\integration -v
   ```
   Expect `test_post_load_autosave_is_byte_identical[early]`, `[mid]`, and `[late]` each PASSED (or SKIPPED if a fixture is absent). A failure prints the first differing byte offset.

## Test-mode arguments

The harness launches `ck3.exe -debug_mode -gdpr-compliant` plus a scripted-run trio (`-save_to_load=<path>`, `-advance_days=<N>`, `-exit_after_advance`) so the game loads, advances, autosaves, and exits unattended. `-debug_mode` enables the debug console; `-gdpr-compliant` suppresses analytics.

> WARNING: these scripted-run switches are assumed, not confirmed for CK3 1.19.0.6. They follow documented Paradox scripted-run conventions and prior-version precedent but aren't verified against the current binary. If unsupported, the launch runs as a normal interactive session (CK3 ignores unknown switches without erroring), the 600-second timeout fires, and the test fails with a clear message, not a cryptic crash.

If the scripted path doesn't work on your build, use the manual fallback.

## Manual fallback

No special game arguments; you drive the steps by hand in the normal interactive game.

### Producing a snapshot by hand

For each reference point (e.g. `early`):

1. Ensure the accelerator is deployed (step 1 above).
2. Launch CK3 normally.
3. Load `fixtures\early_input.ck3` from the in-game load menu.
4. Advance 30 days (fast-forward, or unpause and wait; no cheats that alter determinism).
5. Let CK3 autosave at month rollover, or save as `autosave` explicitly.
6. Copy the autosave out:
   ```powershell
   $saveDir = "$env:USERPROFILE\Documents\Paradox Interactive\Crusader Kings III\save games"
   Copy-Item "$saveDir\autosave.ck3" 'my_early_snapshot.ck3'
   ```

### Running the comparison (no game launch)

```powershell
python -m pytest tests\integration\test_save_load.py::test_manual_compare_only -v `
    --snapshot=my_early_snapshot.ck3 `
    --golden=tests\integration\fixtures\early_golden.ck3
```

Expect PASSED if byte-identical to stock, FAILED with the first differing byte offset otherwise. `test_manual_compare_only` is standalone: no subprocess, no CK3 paths, no deps beyond `pytest`, so it runs on any machine with the two files.

### Calling the comparison directly

```python
from pathlib import Path
import sys
sys.path.insert(0, 'tests/integration')   # or install via pip -e .
from conftest import compare_saves_standalone

ok, detail = compare_saves_standalone(
    Path("my_early_snapshot.ck3"),
    Path("tests/integration/fixtures/early_golden.ck3"),
)
print("PASS" if ok else f"FAIL: {detail}")
```

`compare_saves_standalone()` returns `(identical: bool, detail: str)`. It never calls `pytest.skip`/`pytest.fail`, so it works outside any test runner.

## Configuration

Paths default to the documented install locations; override via environment variables:

| Env var | Default | Meaning |
|---|---|---|
| `CK3ACCEL_CK3_EXE` | `C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III\binaries\ck3.exe` | CK3 executable |
| `CK3ACCEL_SAVE_DIR` | `%USERPROFILE%\Documents\Paradox Interactive\Crusader Kings III\save games` | CK3 save directory |

```powershell
$env:CK3ACCEL_CK3_EXE = 'D:\Games\ck3\binaries\ck3.exe'
$env:CK3ACCEL_SAVE_DIR = 'D:\CK3Saves'
python -m pytest tests\integration -v
```

## Relationship to CI

CI never runs this directory. A green CI run means the C++ unit tests pass (`ctest --preset default`); the byte-identical gate is a separate, deliberate manual step on real hardware before tagging a release.
