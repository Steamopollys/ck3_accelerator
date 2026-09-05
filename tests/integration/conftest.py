"""Shared fixtures and helpers for the CK3 Accelerator integration harness.

MANUAL / OPT-IN. This harness launches the real Crusader Kings III, so it is
never run in CI (CI runs only the C++ unit tests via `ctest --preset default`).
Run it by hand after deploying the accelerator into the CK3 binaries directory.
See README.md in this directory for the full procedure and how to capture the
golden autosaves with the accelerator UNINSTALLED.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

# --- Documented install paths (overridable via environment variables) --------

# Crusader Kings III executable (Steam, Windows). Build-env documented path.
CK3_EXE = Path(
    os.environ.get(
        "CK3ACCEL_CK3_EXE",
        r"C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III\binaries\ck3.exe",
    )
)

# CK3 user-data save directory; autosaves and named saves land here.
CK3_SAVE_DIR = Path(
    os.environ.get(
        "CK3ACCEL_SAVE_DIR",
        str(Path.home() / "Documents" / "Paradox Interactive" / "Crusader Kings III" / "save games"),
    )
)

# Directory holding the user-provided test fixtures (input saves + goldens).
FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures"

# The autosave CK3 writes; the harness copies a fresh input save in as a named
# save, loads it, advances N days, then snapshots the autosave it produces.
AUTOSAVE_NAME = "autosave.ck3"

# How long to advance in-game before the autosave snapshot, and how long to wait
# for the launch+load+advance to finish before giving up.
ADVANCE_DAYS = 30
LAUNCH_TIMEOUT_SECONDS = 600

# The three reference points the byte-identical gate must hold across.
# Each maps to <stem>_input.ck3 (loaded) and <stem>_golden.ck3 (expected
# autosave captured with the accelerator uninstalled), both under FIXTURE_DIR.
REFERENCE_STEMS = ["early", "mid", "late"]


def input_save_for(stem: str) -> Path:
    """User-provided input save to load for reference point `stem`."""
    return FIXTURE_DIR / f"{stem}_input.ck3"


def golden_save_for(stem: str) -> Path:
    """Golden autosave (captured with the accelerator uninstalled) for `stem`."""
    return FIXTURE_DIR / f"{stem}_golden.ck3"


def require_fixture(path: Path) -> None:
    """Skip (do not fail) the test if a user-provided fixture is missing."""
    if not path.is_file():
        pytest.skip(f"fixture not present: {path} (see tests/integration/README.md)")


def files_identical(a: Path, b: Path) -> bool:
    """Byte-for-byte comparison of two files (shallow=False forces a real read)."""
    import filecmp

    return filecmp.cmp(str(a), str(b), shallow=False)


def first_byte_diff(a: Path, b: Path) -> str:
    """Return a human-readable description of the first differing byte, for diagnostics."""
    size_a = a.stat().st_size
    size_b = b.stat().st_size
    with a.open("rb") as fa, b.open("rb") as fb:
        offset = 0
        while True:
            chunk_a = fa.read(65536)
            chunk_b = fb.read(65536)
            if not chunk_a and not chunk_b:
                break
            limit = min(len(chunk_a), len(chunk_b))
            for i in range(limit):
                if chunk_a[i] != chunk_b[i]:
                    return (
                        f"first byte diff at offset {offset + i}: "
                        f"0x{chunk_a[i]:02x} != 0x{chunk_b[i]:02x} "
                        f"(sizes: {size_a} vs {size_b})"
                    )
            offset += limit
            if len(chunk_a) != len(chunk_b):
                return f"length differs at offset {offset} (sizes: {size_a} vs {size_b})"
    return "files are identical"


def launch_and_snapshot(stem: str, tmp_path: Path) -> Path:
    """Drive CK3 once for reference point `stem`; return the snapshotted autosave path.

    Steps:
      1. Copy the input save into the live CK3 save directory.
      2. Remove any stale autosave so we snapshot only what THIS run produces.
      3. Launch `ck3.exe -debug_mode -gdpr-compliant` pointed at the input save,
         advancing ADVANCE_DAYS days and exiting (CK3 -gamestate test arguments,
         see README "Test-mode arguments", confirm/adjust per build).
      4. Wait for the process to exit (it self-terminates after the scripted run).
      5. Copy the freshly written autosave out to `tmp_path` and return that path.

    NOTE: The scripted-run switches (-save_to_load, -advance_days,
    -exit_after_advance) are ASSUMED, not confirmed for CK3 1.19.0.6. If the
    launch times out, the test fails with a clear message directing you to the
    manual fallback described in README.md and exercised by
    test_manual_compare_only().
    """
    if not CK3_EXE.is_file():
        pytest.skip(f"ck3.exe not found at {CK3_EXE} (set CK3ACCEL_CK3_EXE)")
    if not CK3_SAVE_DIR.is_dir():
        pytest.skip(f"save dir not found at {CK3_SAVE_DIR} (set CK3ACCEL_SAVE_DIR)")

    input_save = input_save_for(stem)
    require_fixture(input_save)

    # 1. Stage the input save where CK3 will find it.
    staged_input = CK3_SAVE_DIR / f"ck3accel_int_{stem}.ck3"
    shutil.copyfile(input_save, staged_input)

    # 2. Clear any stale autosave.
    autosave = CK3_SAVE_DIR / AUTOSAVE_NAME
    if autosave.exists():
        autosave.unlink()

    # 3. Launch CK3 in debug + gdpr-compliant mode, scripted to load, advance, exit.
    #    WARNING: these switches are ASSUMED for 1.19.0.6 (see README "Test-mode
    #    arguments"). If a timeout occurs, the test fails with an explanatory message.
    args = [
        str(CK3_EXE),
        "-debug_mode",
        "-gdpr-compliant",
        f"-save_to_load={staged_input}",
        f"-advance_days={ADVANCE_DAYS}",
        "-exit_after_advance",
    ]
    proc = subprocess.Popen(args, cwd=str(CK3_EXE.parent))

    # 4. Wait for the scripted run to finish.
    try:
        proc.wait(timeout=LAUNCH_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        pytest.fail(
            f"CK3 did not exit within {LAUNCH_TIMEOUT_SECONDS}s for '{stem}'.\n"
            "\n"
            "LIKELY CAUSE: The scripted-run switches (-save_to_load, -advance_days,\n"
            "-exit_after_advance) may not be supported on your CK3 build (1.19.0.6).\n"
            "These switches are assumed, not confirmed.\n"
            "\n"
            "MANUAL FALLBACK: See README.md § 'Manual fallback procedure'.\n"
            "Load the save in-game, advance 30 days, let CK3 autosave, then run:\n"
            "  python -m pytest tests/integration/test_save_load.py"
            "::test_manual_compare_only -v\n"
            "  --snapshot=<path-to-autosave.ck3> --golden=<path-to-golden.ck3>\n"
            "Or call compare_saves_standalone() directly from a Python script."
        )

    # The autosave write can lag the process exit slightly; poll briefly.
    deadline = time.monotonic() + 30
    while not autosave.is_file() and time.monotonic() < deadline:
        time.sleep(0.5)
    if not autosave.is_file():
        pytest.fail(
            f"No autosave produced at {autosave} for '{stem}'.\n"
            "The scripted-run switches may have been ignored (see README.md\n"
            "§ 'Test-mode arguments' and § 'Manual fallback procedure')."
        )

    # 5. Snapshot it out so the next reference point can't clobber the evidence.
    snapshot = tmp_path / f"{stem}_snapshot.ck3"
    shutil.copyfile(autosave, snapshot)

    # Tidy up the staged input.
    staged_input.unlink(missing_ok=True)
    return snapshot


def compare_saves_standalone(snapshot: Path, golden: Path) -> tuple[bool, str]:
    """Standalone byte-comparison of two save files.

    This function does NOT launch the game. It is the comparison half of the
    manual fallback: once you have produced a snapshot by hand (load the save
    in-game, advance ADVANCE_DAYS days, copy autosave.ck3 somewhere), pass both
    files here to check byte-identical equality.

    Returns:
        (identical: bool, detail: str) where detail is either
        "files are identical" or the first-difference description.

    Example (run from the repo root):
        python - <<'EOF'
        from tests.integration.conftest import compare_saves_standalone
        from pathlib import Path
        ok, msg = compare_saves_standalone(
            Path("my_snapshot.ck3"), Path("tests/integration/fixtures/early_golden.ck3")
        )
        print("PASS" if ok else f"FAIL: {msg}")
        EOF
    """
    if not snapshot.is_file():
        return False, f"snapshot not found: {snapshot}"
    if not golden.is_file():
        return False, f"golden not found: {golden}"
    identical = files_identical(snapshot, golden)
    detail = "files are identical" if identical else first_byte_diff(snapshot, golden)
    return identical, detail
