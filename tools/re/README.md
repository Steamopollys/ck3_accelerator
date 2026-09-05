# tools/re: static RE toolkit for ck3.exe

Throwaway-grade helper scripts, kept in the repo but not part of the build. Requires `python -m pip install capstone numpy`.

- `ck3re.py`: PE parse, `.pdata` function table (chained-unwind merge), E8/E9 call graph, RIP-relative xref index, string table, capstone disassembly with annotations. First run builds and caches indexes under `tools/re/cache/`, keyed by PE timestamp (~5 s).
- `ext.py`: absolute data-pointer xrefs, MSVC RTTI map (vtable -> class), signature scan/generation, GUI data-function registration tables.

Usage (from this directory):

    python -c "import ck3re, ext; pe, ix = ck3re.load(); print(ix.disasm(0x01A5F8C0))"
    python -c "import ck3re, ext; pe, ix = ck3re.load(); print(ix.scan_sig('48 83 EC 38 48 8B 01 45 33 D2'))"
