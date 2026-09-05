"""Extensions to ck3re.Index: data-pointer xrefs, signature scanning, table dumps, vtable/RTTI helpers."""
import os, struct, re, bisect
import numpy as np
import ck3re
from ck3re import Index, CACHE

def _build_dataptrs(self):
    pe = self.pe
    p = lambda n: os.path.join(CACHE, f"{self.tag}_{n}")
    if os.path.exists(p("dp_val.npy")):
        self.dp_val = np.load(p("dp_val.npy")); self.dp_loc = np.load(p("dp_loc.npy"))
        return
    vals, locs = [], []
    for s in pe.sections:
        if s["name"] not in (".rdata", ".data"): continue
        raw = pe.data[s["raw"]: s["raw"] + min(s["rawsize"], s["vsize"])]
        n = len(raw) // 8
        arr = np.frombuffer(raw[: n*8], dtype=np.uint64).astype(np.int64)
        v = arr - pe.image_base
        ok = (v >= pe.lo_img) & (v < pe.hi_img)
        idx = np.where(ok)[0]
        vals.append(v[idx]); locs.append(idx*8 + s["va"])
    self.dp_val = np.concatenate(vals); self.dp_loc = np.concatenate(locs)
    o = np.argsort(self.dp_val, kind="stable")
    self.dp_val = self.dp_val[o]; self.dp_loc = self.dp_loc[o]
    np.save(p("dp_val.npy"), self.dp_val); np.save(p("dp_loc.npy"), self.dp_loc)

def data_refs_to(self, rva):
    if not hasattr(self, "dp_val"): _build_dataptrs(self)
    lo = np.searchsorted(self.dp_val, rva, "left"); hi = np.searchsorted(self.dp_val, rva, "right")
    return [int(x) for x in self.dp_loc[lo:hi]]

def parse_sig(sig):
    toks = sig.split()
    b = bytes(int(t, 16) if t not in ("?", "??") else 0 for t in toks)
    m = [t not in ("?", "??") for t in toks]
    return b, m

def scan_sig(self, sig, limit=50):
    """Return list of .text RVAs matching an IDA-style signature."""
    pe = self.pe
    t = pe.text
    b, m = parse_sig(sig)
    n = len(t) - len(b)
    first = next(i for i in range(len(b)) if m[i])
    cand = np.where(t[first: first + n] == b[first])[0]
    for i in range(len(b)):
        if not m[i] or i == first: continue
        cand = cand[t[cand + i] == b[i]]
        if len(cand) == 0: break
    return [int(c) + pe.text_va for c in cand[:limit]]

def describe_qword(self, rva):
    pe = self.pe
    try:
        q = pe.u64(rva)
    except Exception:
        return "??"
    if pe.image_base <= q < pe.image_base + 0x10000000:
        qr = q - pe.image_base
        sec = pe.sec_of(qr)
        s = self.str_by_rva.get(qr)
        if s is not None: return f'-> "{s[:60]}"'
        if sec == ".text":
            f = self.func_of(qr)
            return f"-> fn_{qr:08X}" if f and f[0] == qr else f"-> text:{qr:08X}" + (f" (in fn_{f[0]:08X})" if f else "")
        ms = self.str_at(qr)
        return f"-> {sec}:{qr:08X}" + (f' "{ms[:40]}"' if ms else "")
    return f"{q:#x}"

def dump_qwords(self, rva, before=8, after=8):
    lines = []
    for i in range(-before, after + 1):
        a = rva + 8*i
        lines.append(f"  {a:08X}: {describe_qword(self, a)}")
    return "\n".join(lines)

def find_strings(self, pattern, flags=0, limit=200):
    rx = re.compile(pattern, flags)
    out = [(r, s) for r, s in self.strings if rx.search(s)]
    return out[:limit]

# ---- RTTI ----
def build_rtti(self):
    """Map vtable rva -> class name via MSVC RTTI CompleteObjectLocator. Cached."""
    pe = self.pe
    import pickle
    p = os.path.join(CACHE, f"{self.tag}_rtti.pkl")
    if os.path.exists(p):
        with open(p, "rb") as f:
            self.vt_name, self.name_vts = pickle.load(f)
        return
    # TypeDescriptor: [vfptr][spare][name ".?AV..."]; name at td+16
    tds = {}
    for r, s in self.strings:
        if s.startswith(".?AV") or s.startswith(".?AU"):
            tds[r - 16] = s
    # COL: sig(4) offset(4) cdOffset(4) typeDescRva(4) classHierRva(4) selfRva(4)
    rd = pe.sec[".rdata"]
    raw = pe.data[rd["raw"]: rd["raw"] + min(rd["rawsize"], rd["vsize"])]
    n = len(raw) // 4
    arr = np.frombuffer(raw[: n*4], dtype=np.uint32)
    vt_name = {}
    name_vts = {}
    td_set = set(tds.keys())
    # find COLs: sig==1, offset small, typeDescRva in td_set, selfRva == own rva
    cand = np.where(arr[:-5] == 1)[0]
    for i in cand:
        td = int(arr[i+3]); selfr = int(arr[i+5])
        colrva = rd["va"] + int(i)*4
        if td in td_set and selfr == colrva:
            # vtable: the qword just after a pointer to this COL in .rdata
            for loc in data_refs_to(self, colrva):
                vt = loc + 8
                name = tds[td]
                off = int(arr[i+1])
                vt_name[vt] = (name, off)
                name_vts.setdefault(name, []).append((vt, off))
    self.vt_name, self.name_vts = vt_name, name_vts
    with open(p, "wb") as f:
        pickle.dump((vt_name, name_vts), f)

def vtable_funcs(self, vt, maxn=64):
    pe = self.pe
    out = []
    for i in range(maxn):
        try:
            q = pe.u64(vt + 8*i)
        except Exception:
            break
        qr = q - pe.image_base
        if pe.sec_of(qr) != ".text": break
        out.append(qr)
    return out

def demangle_simple(name):
    # .?AVCCharacter@@ -> CCharacter ; .?AV?$CFoo@VBar@@@@ -> CFoo<Bar>
    n = name[4:]
    n = n.replace("@@", "").replace("@", "::")
    return n

for fn in (data_refs_to, scan_sig, describe_qword, dump_qwords, find_strings, build_rtti, vtable_funcs):
    setattr(Index, fn.__name__, fn)
Index._build_dataptrs = _build_dataptrs

def reg_table(self, fn, want=None):
    """Walk a GUI-data-function registration function: pair each string constant with the
    next function pointer loaded into rdx/r8/rcx before a call. Returns [(name, fnrva, site)]."""
    import capstone
    pe = self.pe
    f = self.func_of(fn)
    b, e = f
    code = pe.read(b, e - b)
    cur = None; out = []
    for ins in self.cs.disasm(code, pe.image_base + b):
        for o in ins.operands:
            if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP:
                tgt = ins.address + ins.size + o.mem.disp - pe.image_base
                s = self.str_by_rva.get(tgt)
                if s is None:
                    s = self.str_at(tgt)
                    if s is not None and not (self.str_by_rva.get(tgt) or (tgt-1 >= 0 and pe.sec_of(tgt) in ('.rdata','.data') and pe.read(tgt-1,1) == b'\x00')):
                        s = None
                if s is not None and ins.mnemonic in ("lea", "movups", "movdqu", "movdqa", "movaps", "mov", "movzx", "vmovdqu", "vmovups"):
                    if len(s) >= 3 and not s.startswith("\\") and ".cpp" not in s:
                        cur = (s, ins.address - pe.image_base)
                elif ins.mnemonic == "lea" and pe.sec_of(tgt) == ".text":
                    ff = self.func_of(tgt)
                    if ff and ff[0] == tgt and cur:
                        if want is None or cur[0] in want:
                            out.append((cur[0], tgt, ins.address - pe.image_base))
    return out
Index.reg_table = reg_table

def reg_table2(self, fn):
    """Pair string constants with the next .text function pointer loaded via lea (any string-ref mnemonic)."""
    import capstone
    pe = self.pe
    f = self.func_of(fn)
    if not f: return []
    b, e = f
    code = pe.read(b, e - b)
    cur = None; out = []
    for ins in self.cs.disasm(code, pe.image_base + b):
        for o in ins.operands:
            if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP:
                tgt = ins.address + ins.size + o.mem.disp - pe.image_base
                s = self.str_by_rva.get(tgt)
                if s is not None and len(s) >= 3 and ".cpp" not in s and "\\" not in s:
                    cur = s
                elif ins.mnemonic == "lea" and pe.sec_of(tgt) == ".text":
                    ff = self.func_of(tgt)
                    if ff and ff[0] == tgt and cur:
                        out.append((cur, tgt, ins.address - pe.image_base)); cur = None
    return out

def resolve_thunk(self, fn):
    """If fn is a tiny wrapper 'lea rcx,[rip+impl]; call fn_00976280', return impl; else fn."""
    import capstone
    pe = self.pe
    f = self.func_of(fn)
    if not f or f[1] - f[0] > 0x60: return fn
    code = pe.read(f[0], f[1] - f[0])
    impl = None
    for ins in self.cs.disasm(code, pe.image_base + f[0]):
        if ins.mnemonic == "lea":
            for o in ins.operands:
                if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + o.mem.disp - pe.image_base
                    if pe.sec_of(tgt) == ".text" and self.is_func(tgt): impl = tgt
        if ins.mnemonic == "call" and impl is not None:
            return impl
        if ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16) - pe.image_base
            if self.is_func(t): return t
    return fn

def region_regs(self, lo, hi):
    out = []
    for b in self.func_begin[(self.func_begin >= lo) & (self.func_begin < hi)]:
        for name, fn, site in reg_table2(self, int(b)):
            out.append((name, fn, resolve_thunk(self, fn), int(b)))
    return out
Index.reg_table2 = reg_table2; Index.resolve_thunk = resolve_thunk; Index.region_regs = region_regs

def make_sig(self, fn, min_bytes=28, max_bytes=64):
    """Build an IDA-style signature from fn's prologue, wildcarding rel32/disp32 (rip-relative) operand bytes.
    Grows until unique in .text (or max_bytes). Returns (sig, nmatches)."""
    import capstone
    pe = self.pe
    code = pe.read(fn, max_bytes + 16)
    toks = []
    total = 0
    for ins in self.cs.disasm(code, pe.image_base + fn):
        b = list(ins.bytes)
        mask = [True] * len(b)
        # wildcard rip-relative disp32 and rel32 immediates
        if any(o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP for o in ins.operands):
            # disp32 is the 4 bytes at (disp_offset)
            off = ins.disp_offset
            for i in range(off, off + 4): mask[i] = False
        if ins.mnemonic in ("call", "jmp", "je", "jne", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle", "js", "jns") and ins.op_str.startswith("0x"):
            # rel8 or rel32 at the end
            n = 4 if len(b) >= 5 and (b[0] == 0xE8 or b[0] == 0xE9 or b[0] == 0x0F) else 1
            for i in range(len(b) - n, len(b)): mask[i] = False
        toks.extend(f"{x:02X}" if m else "??" for x, m in zip(b, mask))
        total += len(b)
        if total >= min_bytes:
            sig = " ".join(toks)
            n = len(scan_sig(self, sig, limit=5))
            if n == 1 or total >= max_bytes:
                return sig, n
    return " ".join(toks), len(scan_sig(self, " ".join(toks), limit=5))
Index.make_sig = make_sig
