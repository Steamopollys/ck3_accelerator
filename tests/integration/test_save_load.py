"""Byte-identical post-load world-state gate.

For each reference save (early / mid / late):
  - load it with accel_save_load.dll ACTIVE,
  - advance a fixed number of days,
  - snapshot the autosave,
  - assert it is byte-for-byte identical to the golden autosave captured with
    the accelerator UNINSTALLED.

CRC-validated libdeflate decompression must produce identical decompressed
bytes to stock zlib, so the resulting world state, and thus the re-serialized
autosave, must be identical. Any difference is a correctness regression.

MANUAL / OPT-IN: requires a live CK3 install with the accelerator deployed and
user-provided fixtures (see README.md). Not part of CI.

---

Two test strategies are provided:

1. test_post_load_autosave_is_byte_identical   (scripted / automated)
   Launches CK3 with the scripted-run switches (-save_to_load, -advance_days,
   -exit_after_advance), waits for it to exit, and compares the produced
   autosave to the golden. REQUIRES a live CK3 install.

   NOTE: The scripted-run switches are ASSUMED for CK3 1.19.0.6, not
   confirmed. If the launch times out, the test fails with instructions for
   the manual fallback below.

2. test_manual_compare_only   (manual fallback; game-launch-free)
   Compares two files you supply via --snapshot and --golden CLI options.
   Use this after producing a snapshot by hand:
     1. Load the input save in-game (accelerator ACTIVE).
     2. Advance ADVANCE_DAYS (30) days.
     3. Let CK3 autosave; copy save games\\autosave.ck3 somewhere safe.
     4. Run:
          python -m pytest tests/integration/test_save_load.py::test_manual_compare_only -v
            --snapshot=<path-to-your-snapshot.ck3>
            --golden=tests/integration/fixtures/<stem>_golden.ck3
   This test NEVER launches the game, so it works on any machine.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import conftest


# ---------------------------------------------------------------------------
# pytest CLI option registration
# ---------------------------------------------------------------------------

def pytest_addoption(parser: pytest.Parser) -> None:
    """Register --snapshot and --golden options for the manual compare test."""
    parser.addoption(
        "--snapshot",
        default=None,
        help=(
            "Path to the snapshot autosave produced by hand (for "
            "test_manual_compare_only). "
            "Example: --snapshot=C:/path/to/autosave.ck3"
        ),
    )
    parser.addoption(
        "--golden",
        default=None,
        help=(
            "Path to the golden autosave for comparison (for "
            "test_manual_compare_only). "
            "Example: --golden=tests/integration/fixtures/early_golden.ck3"
        ),
    )


# ---------------------------------------------------------------------------
# Test 1: scripted launch + compare (requires live CK3 + accelerator deployed)
# ---------------------------------------------------------------------------

@pytest.mark.integration
@pytest.mark.parametrize("stem", conftest.REFERENCE_STEMS)
def test_post_load_autosave_is_byte_identical(stem: str, tmp_path: Path) -> None:
    """Launch CK3 with the accelerator, snapshot the autosave, assert byte-identical.

    Skipped if ck3.exe is not found or the fixture files are missing.
    Fails with a manual-fallback pointer if the scripted launch times out.
    """
    golden = conftest.golden_save_for(stem)
    conftest.require_fixture(golden)

    snapshot = conftest.launch_and_snapshot(stem, tmp_path)

    identical, detail = conftest.compare_saves_standalone(snapshot, golden)
    assert identical, (
        f"'{stem}' autosave differs from golden: {detail}\n"
        "If this is a scripted-run artefact, re-run the manual fallback:\n"
        "  see README.md § 'Manual fallback procedure' and use\n"
        "  test_manual_compare_only with --snapshot / --golden."
    )


# ---------------------------------------------------------------------------
# Test 2: standalone compare, game-launch-free; works on any machine
# ---------------------------------------------------------------------------

@pytest.mark.integration
def test_manual_compare_only(request: pytest.FixtureRequest) -> None:
    """Byte-compare two user-supplied files without launching the game.

    This test is the always-runnable half of the manual fallback. It does not
    call subprocess, does not touch CK3 paths, and has no CK3 dependency at all.
    Pass the files via CLI options:

        python -m pytest tests/integration/test_save_load.py::test_manual_compare_only -v \\
            --snapshot=<path-to-snapshot.ck3> \\
            --golden=<path-to-golden.ck3>

    If either --snapshot or --golden is omitted the test is skipped.

    This function is also callable as a standalone Python utility:
        from tests.integration.conftest import compare_saves_standalone
        ok, msg = compare_saves_standalone(Path("snap.ck3"), Path("gold.ck3"))
    """
    snapshot_opt = request.config.getoption("--snapshot", default=None)
    golden_opt = request.config.getoption("--golden", default=None)

    if snapshot_opt is None or golden_opt is None:
        pytest.skip(
            "Provide --snapshot=<path> and --golden=<path> to run the manual compare. "
            "See README.md § 'Manual fallback procedure'."
        )

    snapshot = Path(snapshot_opt)
    golden = Path(golden_opt)

    # compare_saves_standalone() is self-contained: no game, no fixtures dir.
    identical, detail = conftest.compare_saves_standalone(snapshot, golden)
    assert identical, f"saves differ: {detail}"
