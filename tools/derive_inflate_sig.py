#!/usr/bin/env python3
"""Derive + verify a unique IDA-style signature for zlib inflate() in ck3.exe.

Dev tooling for save-decompression RE. Disassembles the known inflate
prologue (RVA 0x035C4340), wildcards the only position-dependent field (the
rel32 of the trailing `test rcx,rcx; je`), and confirms the pattern matches
EXACTLY ONCE across ck3.exe .text. Prints the final signature used by the
accel_decompress_probe plugin.

Requires: pefile, capstone (Python 3.11). Run:
    python tools/derive_inflate_sig.py
"""
import sys
import pefile
import capstone
from capstone import x86

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III\binaries\ck3.exe"
TARGET_RVA = 0x035C4340  # inflate(z_stream*, int), MSVC x64: rcx=strm, edx=flush
SIG_LEN = 35             # through `test rcx,rcx; je` (instruction boundary)


def main() -> int:
    pe = pefile.PE(EXE, fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase

    text = next((s for s in pe.sections
                 if s.Name.rstrip(b"\x00") == b".text"), None)
    if text is None:
        print("no .text section", file=sys.stderr)
        return 2
    text_rva = text.VirtualAddress
    text_data = text.get_data()

    foff = TARGET_RVA - text_rva
    prologue = text_data[foff:foff + SIG_LEN]
    base_va = image_base + TARGET_RVA

    # Mask: True = match this byte exactly, False = wildcard ("??").
    mask = [True] * SIG_LEN

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    print("Disassembly of inflate prologue @ RVA 0x%X:" % TARGET_RVA)
    for ins in md.disasm(prologue, base_va):
        off = ins.address - base_va
        if off >= SIG_LEN:
            break
        print("  +0x%02X  %-22s %s %s"
              % (off, ins.bytes.hex(" "), ins.mnemonic, ins.op_str))
        # Wildcard RIP-relative disp32 (none expected here, but be safe).
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                    if off + k < SIG_LEN:
                        mask[off + k] = False
        # Wildcard relative branch/call immediates (the trailing `je` rel32).
        if x86.X86_GRP_JUMP in ins.groups or x86.X86_GRP_CALL in ins.groups:
            if ins.imm_size in (1, 2, 4):
                for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                    if off + k < SIG_LEN:
                        mask[off + k] = False

    sig = " ".join("??" if not mask[i] else "%02X" % prologue[i]
                   for i in range(SIG_LEN))

    # Scan all of .text, counting every match (no early-out).
    pat = [None if t == "??" else int(t, 16) for t in sig.split()]
    plen = len(pat)
    hits = []
    for i in range(len(text_data) - plen + 1):
        if all(pat[k] is None or text_data[i + k] == pat[k] for k in range(plen)):
            hits.append(text_rva + i)

    print("\nSignature (%d bytes):\n%s" % (SIG_LEN, sig))
    print("\nMatches in .text: %d -> %s"
          % (len(hits), ["0x%X" % h for h in hits]))

    ok = len(hits) == 1 and hits[0] == TARGET_RVA
    print("UNIQUE at 0x%X: %s" % (TARGET_RVA, "YES" if ok else "NO"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
