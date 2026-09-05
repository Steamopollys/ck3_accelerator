"""Static RE toolkit for ck3.exe (throwaway spike tooling).
PE parse, .pdata function table (chained-unwind aware), E8/E9 call graph,
RIP-relative xrefs (lea/mov/call[rip]), string table, capstone disasm.
Indexes are cached as .npy/.pkl next to this file keyed by PE timestamp.
"""
import struct, os, re, pickle, sys, bisect
import numpy as np
import capstone

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Crusader Kings III\binaries\ck3.exe"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")

class PE:
    def __init__(self, path=EXE):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[pe:pe+4] == b"PE\0\0"
        nsec = struct.unpack_from("<H", d, pe+6)[0]
        self.timestamp = struct.unpack_from("<I", d, pe+8)[0]
        opt_size = struct.unpack_from("<H", d, pe+20)[0]
        opt = pe+24
        self.image_base = struct.unpack_from("<Q", d, opt+24)[0]
        sec_off = opt+opt_size
        self.sections = []
        for i in range(nsec):
            s = d[sec_off+40*i: sec_off+40*(i+1)]
            name = s[:8].rstrip(b"\0").decode(errors="replace")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", s, 8)
            self.sections.append(dict(name=name, va=va, vsize=vsize, raw=rawptr, rawsize=rawsize))
        self.sec = {s["name"]: s for s in self.sections}
        t = self.sec[".text"]
        self.text_va, self.text_size = t["va"], t["vsize"]
        self.text = np.frombuffer(d, dtype=np.uint8, count=min(t["rawsize"], t["vsize"]), offset=t["raw"])
        self.lo_img = min(s["va"] for s in self.sections)
        self.hi_img = max(s["va"] + max(s["vsize"], s["rawsize"]) for s in self.sections)

    def rva_to_off(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
                return rva - s["va"] + s["raw"]
        raise ValueError(f"rva {rva:#x} not in any section")

    def sec_of(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
                return s["name"]
        return None

    def read(self, rva, n):
        o = self.rva_to_off(rva)
        return self.data[o:o+n]

    def u8(self, rva): return self.read(rva, 1)[0]
    def u16(self, rva): return struct.unpack("<H", self.read(rva, 2))[0]
    def u32(self, rva): return struct.unpack("<I", self.read(rva, 4))[0]
    def i32(self, rva): return struct.unpack("<i", self.read(rva, 4))[0]
    def u64(self, rva): return struct.unpack("<Q", self.read(rva, 8))[0]
    def va2rva(self, va): return va - self.image_base
    def cstr(self, rva, maxlen=512):
        b = self.read(rva, maxlen)
        i = b.find(b"\0")
        return b[: i if i >= 0 else maxlen].decode("latin-1")
    def wstr(self, rva, maxlen=512):
        b = self.read(rva, maxlen*2)
        out = []
        for i in range(0, len(b)-1, 2):
            c = b[i] | (b[i+1] << 8)
            if c == 0: break
            out.append(chr(c))
        return "".join(out)


class Index:
    def __init__(self, pe: PE, rebuild=False):
        self.pe = pe
        os.makedirs(CACHE, exist_ok=True)
        tag = f"{pe.timestamp:08x}"
        self.tag = tag
        p = lambda n: os.path.join(CACHE, f"{tag}_{n}")
        if not rebuild and os.path.exists(p("funcs.npy")):
            self.func_begin = np.load(p("funcs.npy"))
            self.func_end = np.load(p("funcs_end.npy"))
            self.call_src = np.load(p("call_src.npy"))
            self.call_dst = np.load(p("call_dst.npy"))
            self.call_order = np.load(p("call_order.npy"))
            self.rip_src = np.load(p("rip_src.npy"))
            self.rip_dst = np.load(p("rip_dst.npy"))
            self.rip_kind = np.load(p("rip_kind.npy"))
            self.rip_order = np.load(p("rip_order.npy"))
            with open(p("strings.pkl"), "rb") as f:
                self.strings = pickle.load(f)
            with open(p("chunks.pkl"), "rb") as f:
                self.chunk_parent = pickle.load(f)
        else:
            self._build_funcs()
            self._build_calls()
            self._build_rip()
            self._build_strings()
            np.save(p("funcs.npy"), self.func_begin); np.save(p("funcs_end.npy"), self.func_end)
            np.save(p("call_src.npy"), self.call_src); np.save(p("call_dst.npy"), self.call_dst); np.save(p("call_order.npy"), self.call_order)
            np.save(p("rip_src.npy"), self.rip_src); np.save(p("rip_dst.npy"), self.rip_dst); np.save(p("rip_kind.npy"), self.rip_kind); np.save(p("rip_order.npy"), self.rip_order)
            with open(p("strings.pkl"), "wb") as f:
                pickle.dump(self.strings, f)
            with open(p("chunks.pkl"), "wb") as f:
                pickle.dump(self.chunk_parent, f)
        self.str_by_rva = {r: s for r, s in self.strings}
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.detail = True
        self.rip_src_order = np.argsort(self.rip_src, kind="stable")

    # ---- .pdata ----
    def _build_funcs(self):
        pe = self.pe
        pd = pe.sec[".pdata"]
        raw = pe.data[pd["raw"]: pd["raw"] + min(pd["rawsize"], pd["vsize"])]
        n = len(raw) // 12
        arr = np.frombuffer(raw[: n*12], dtype=np.uint32).reshape(n, 3)
        begins, ends, unwinds = arr[:, 0].astype(np.int64), arr[:, 1].astype(np.int64), arr[:, 2]
        parent = {}
        for i in range(n):
            u = int(unwinds[i])
            if u == 0: continue
            if u & 1:
                continue
            try:
                off = pe.rva_to_off(u)
            except ValueError:
                continue
            b0 = pe.data[off]
            flags = b0 >> 3
            if flags & 4:
                cnt = pe.data[off+2]
                codes = cnt + (cnt & 1)
                po = off + 4 + codes*2
                pb, pe_, pu = struct.unpack_from("<III", pe.data, po)
                parent[int(begins[i])] = pb
        def root(b):
            seen = 0
            while b in parent and seen < 16:
                b = parent[b]; seen += 1
            return b
        roots = {}
        for i in range(n):
            b, e = int(begins[i]), int(ends[i])
            r = root(b)
            lo, hi = roots.get(r, (r, r))
            roots[r] = (min(lo, b), max(hi, e))
        items = sorted(roots.items())
        self.func_begin = np.array([k for k, _ in items], dtype=np.int64)
        self.func_end = np.array([v[1] for _, v in items], dtype=np.int64)
        self.chunk_parent = parent

    def func_of(self, rva):
        """Return (begin, end) of the .pdata function containing rva (chain-aware), or None."""
        i = bisect.bisect_right(self.func_begin, rva) - 1
        if i < 0: return None
        b, e = int(self.func_begin[i]), int(self.func_end[i])
        if b <= rva < e:
            return b, e
        return None

    def is_func(self, rva):
        i = bisect.bisect_left(self.func_begin, rva)
        return i < len(self.func_begin) and self.func_begin[i] == rva

    # ---- call graph ----
    def _build_calls(self):
        pe = self.pe
        t = pe.text
        base = pe.text_va
        pos = np.where((t[:-4] == 0xE8) | (t[:-4] == 0xE9))[0]
        rel = t[pos+1].astype(np.int64) | (t[pos+2].astype(np.int64) << 8) | (t[pos+3].astype(np.int64) << 16) | (t[pos+4].astype(np.int64) << 24)
        rel = np.where(rel >= 2**31, rel - 2**32, rel)
        src = pos + base
        dst = src + 5 + rel
        fb = self.func_begin
        idx = np.searchsorted(fb, dst)
        ok = (idx < len(fb))
        idx2 = np.where(ok, idx, 0)
        ok &= (fb[idx2] == dst)
        self.call_src = src[ok]
        self.call_dst = dst[ok]
        self.call_order = np.argsort(self.call_dst, kind="stable")

    def callers(self, fn):
        """Code addresses (E8/E9 sites) that target function fn."""
        d = self.call_dst[self.call_order]
        lo = np.searchsorted(d, fn, "left"); hi = np.searchsorted(d, fn, "right")
        return [int(x) for x in self.call_src[self.call_order[lo:hi]]]

    def caller_funcs(self, fn):
        out = {}
        for s in self.callers(fn):
            f = self.func_of(s)
            k = f[0] if f else -1
            out.setdefault(k, []).append(s)
        return out

    def callees(self, fn):
        f = self.func_of(fn)
        if not f: return []
        b, e = f
        lo = np.searchsorted(self.call_src, b); hi = np.searchsorted(self.call_src, e)
        return [(int(self.call_src[i]), int(self.call_dst[i])) for i in range(lo, hi)]

    # ---- rip-relative refs ----
    def _build_rip(self):
        pe = self.pe
        t = pe.text
        base = pe.text_va
        n = len(t)
        srcs, dsts, kinds = [], [], []
        L = n - 12
        modrm = t[1:L+1]
        riprel = (modrm & 0xC7) == 0x05
        def add(p, disp_off, ilen, kind):
            if len(p) == 0: return
            disp = t[p+disp_off].astype(np.int64) | (t[p+disp_off+1].astype(np.int64) << 8) | (t[p+disp_off+2].astype(np.int64) << 16) | (t[p+disp_off+3].astype(np.int64) << 24)
            disp = np.where(disp >= 2**31, disp - 2**32, disp)
            dst = p + base + ilen + disp
            ok = (dst >= pe.lo_img) & (dst < pe.hi_img)
            srcs.append((p + base)[ok]); dsts.append(dst[ok]); kinds.append(np.full(int(ok.sum()), kind, dtype=np.int8))
        head = t[:L]
        for op, kind in ((0x8D, 1), (0x8B, 2), (0x89, 3), (0x3B, 5), (0x39, 5), (0x63, 5), (0x8A, 5), (0x88, 5), (0x03, 5), (0x2B, 5), (0x33, 5), (0x0B, 5), (0x23, 5)):
            p = np.where((head == op) & riprel)[0]
            add(p, 2, 6, kind)
        p = np.where((head == 0xFF) & riprel)[0]
        reg = (t[p+1] >> 3) & 7
        p = p[(reg == 2) | (reg == 4) | (reg == 6)]
        add(p, 2, 6, 4)
        for op in (0x83, 0x80, 0xC6):
            p = np.where((head == op) & riprel)[0]
            add(p, 2, 7, 6)
        p = np.where((head == 0xC7) & riprel)[0]
        add(p, 2, 10, 6)
        p = np.where(head == 0x0F)[0]
        m2 = t[p+2]
        p = p[((m2 & 0xC7) == 0x05)]
        add(p, 3, 7, 7)
        self.rip_src = np.concatenate(srcs); self.rip_dst = np.concatenate(dsts); self.rip_kind = np.concatenate(kinds)
        self.rip_order = np.argsort(self.rip_dst, kind="stable")

    def xrefs_to(self, rva, kinds=None):
        d = self.rip_dst[self.rip_order]
        lo = np.searchsorted(d, rva, "left"); hi = np.searchsorted(d, rva, "right")
        out = []
        for i in self.rip_order[lo:hi]:
            k = int(self.rip_kind[i])
            if kinds and k not in kinds: continue
            out.append((int(self.rip_src[i]), k))
        return out

    def xref_funcs(self, rva, kinds=None):
        """Dict func_begin -> list of xref sites (only sites inside known functions)."""
        out = {}
        for s, k in self.xrefs_to(rva, kinds):
            f = self.func_of(s)
            if f:
                out.setdefault(f[0], []).append((s, k))
        return out

    def refs_in_range(self, b, e):
        s_sorted = self.rip_src[self.rip_src_order]
        lo = np.searchsorted(s_sorted, b); hi = np.searchsorted(s_sorted, e)
        idx = self.rip_src_order[lo:hi]
        return [(int(self.rip_src[i]), int(self.rip_dst[i]), int(self.rip_kind[i])) for i in idx]

    # ---- strings ----
    def _build_strings(self):
        pe = self.pe
        strings = []
        for s in pe.sections:
            if s["name"] not in (".rdata", ".data", ".text"):
                continue
            raw = pe.data[s["raw"]: s["raw"] + min(s["rawsize"], s["vsize"])]
            for m in re.finditer(rb"[\x20-\x7e]{5,}\x00", raw):
                strings.append((s["va"] + m.start(), m.group()[:-1].decode("latin-1")))
        strings.sort()
        self.strings = strings

    def find_strings(self, pattern, flags=0, limit=200):
        rx = re.compile(pattern, flags)
        out = [(r, s) for r, s in self.strings if rx.search(s)]
        return out[:limit]

    def str_at(self, rva):
        """Find the string containing rva (may point mid-string)."""
        rvas = [r for r, _ in self.strings]
        i = bisect.bisect_right(rvas, rva) - 1
        if i >= 0:
            r, s = self.strings[i]
            if r <= rva <= r + len(s):
                return s[rva - r:]
        return None

    # ---- disasm ----
    def annotate(self, ins):
        pe = self.pe
        note = ""
        ops = ins.op_str
        if ins.mnemonic in ("call", "jmp") and ops.startswith("0x"):
            tgt = int(ops, 16) - pe.image_base
            f = self.func_of(tgt)
            note = f"-> fn_{tgt:08X}" + ("" if (f and f[0] == tgt) else f" (mid of fn_{f[0]:08X})" if f else " (no pdata)")
        if "rip +" in ops or "rip -" in ops:
            for o in ins.operands:
                if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP:
                    tgt = ins.address + ins.size + o.mem.disp - pe.image_base
                    sec = pe.sec_of(tgt)
                    s = self.str_by_rva.get(tgt)
                    if s is not None:
                        note = f'-> "{s[:80]}"'
                    else:
                        try:
                            wb = pe.read(tgt, 4)
                            if sec in (".rdata", ".data") and len(wb) == 4 and wb[1] == 0 and wb[3] == 0 and 0x20 <= wb[0] < 0x7f and 0x20 <= wb[2] < 0x7f:
                                note = f'-> L"{pe.wstr(tgt)[:80]}"'
                            elif sec == ".text":
                                f = self.func_of(tgt)
                                note = f"-> fn_{tgt:08X}" if f and f[0] == tgt else f"-> text:{tgt:08X}"
                            elif sec:
                                note = f"-> {sec}:{tgt:08X}"
                                try:
                                    q = pe.u64(tgt)
                                    if pe.image_base <= q < pe.image_base + 0x10000000:
                                        qr = q - pe.image_base
                                        fs = self.str_by_rva.get(qr)
                                        note += f" [={'fn_' if pe.sec_of(qr)=='.text' else ''}{qr:08X}" + (f' "{fs[:40]}"' if fs else "") + "]"
                                except Exception:
                                    pass
                                ms = self.str_at(tgt)
                                if ms: note += f' "{ms[:60]}"'
                            else:
                                note = f"-> {tgt:08X}"
                        except Exception:
                            note = f"-> {tgt:08X}"
        return note

    def disasm(self, rva, nbytes=None, max_ins=None, upto=None):
        pe = self.pe
        f = self.func_of(rva)
        if nbytes is None:
            nbytes = (f[1] - rva) if f else 0x200
        if upto is not None:
            nbytes = upto - rva
        code = pe.read(rva, nbytes)
        lines = []
        for k, ins in enumerate(self.cs.disasm(code, pe.image_base + rva)):
            if max_ins and k >= max_ins: break
            note = self.annotate(ins)
            lines.append(f"{ins.address - pe.image_base:08X}  {ins.mnemonic:8s} {ins.op_str:45s} {note}")
        return "\n".join(lines)

    def fn_strings(self, fn):
        """All string refs made inside function fn."""
        f = self.func_of(fn)
        if not f: return []
        b, e = f
        out = []
        for s, d, k in self.refs_in_range(b, e):
            st = self.str_by_rva.get(d)
            if st: out.append((s, st))
        return sorted(set(out))

    def neighbors_strings(self, rva, span=0x8000, limit=40):
        """Source-path strings referenced by functions near rva (MSVC lays out per-OBJ)."""
        out = []
        for s, d, k in self.refs_in_range(rva - span, rva + span):
            st = self.str_by_rva.get(d)
            if st and (".cpp" in st or ".h" in st):
                out.append((s, st))
        out = sorted(set(out))
        return out[:limit]

    def fn_summary(self, fn, calls=True, strings=True):
        f = self.func_of(fn)
        if not f: return f"fn {fn:08X}: no pdata"
        b, e = f
        lines = [f"fn_{b:08X} size={e-b:#x} callers={len(self.callers(b))}"]
        if strings:
            for s, st in self.fn_strings(b)[:30]:
                lines.append(f"   str @{s:08X}: {st[:100]}")
        if calls:
            for s, d in self.callees(b)[:60]:
                lines.append(f"   call @{s:08X} -> fn_{d:08X}")
        return "\n".join(lines)


_pe = None; _ix = None
def load(rebuild=False):
    global _pe, _ix
    if _ix is None:
        _pe = PE(); _ix = Index(_pe, rebuild=rebuild)
    return _pe, _ix

if __name__ == "__main__":
    import time
    t0 = time.time()
    pe, ix = load(rebuild="--rebuild" in sys.argv)
    print(f"timestamp={pe.timestamp:#x} image_base={pe.image_base:#x}")
    for s in pe.sections:
        print(f"  {s['name']:8s} va={s['va']:#010x} vsize={s['vsize']:#010x} raw={s['raw']:#010x} rawsize={s['rawsize']:#010x}")
    print(f"funcs={len(ix.func_begin)} calls={len(ix.call_src)} riprefs={len(ix.rip_src)} strings={len(ix.strings)} chunks={len(ix.chunk_parent)}")
    print(f"built in {time.time()-t0:.1f}s")
