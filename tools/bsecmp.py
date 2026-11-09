"""Validate the Zune HD VDE bitstream engine model (qemu hw/arm/zunehd-bse.c).

Inputs:
  ref  - output of tools/mp3ref.c (minimp3 reference Huffman decode per granule/channel)
  dump - ZUNEHD_BSEDUMP=<file> output of the emulator (per command list: words, bitstream
         bytes, budget/used, values written by the model)
For every decode list the bitstream at the programmed start is matched to a reference granule
(first 48 bits), the list is re-run by an offline copy of the engine (natural or 32-bit
swapped byte order), and the values are compared with the reference and with what the
emulator produced.

Usage: py -3 -I tools/bsecmp.py ref.txt dump.txt [-v N]   (N = print details of N mismatches)
"""
import re
import os
import sys

TAB_H = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "qemu", "hw", "arm", "zunehd-mp3tab.h")
HW2ISO = [0, 0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24] + [0] * 15


def load_tabs():
    src = open(TAB_H).read()
    out = {}
    for name in ("mp3_tabs", "mp3_tab32", "mp3_tab33", "mp3_tabindex"):
        m = re.search(name + r"\[\d*\]\s*=\s*\{(.*?)\};", src, re.S)
        out[name] = [int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]
    return out


T = load_tabs()


class Bits:
    def __init__(self, data, bit):
        self.d = data
        self.pos = bit

    def peek(self, n):
        if n == 0:
            return 0
        v = 0
        p = self.pos
        for i in range(n):
            byte = self.d[(p + i) >> 3] if ((p + i) >> 3) < len(self.d) else 0
            v = (v << 1) | ((byte >> (7 - ((p + i) & 7))) & 1)
        return v

    def get(self, n):
        v = self.peek(n)
        self.pos += n
        return v


def run_engine(words, bits):
    """Offline copy of the engine: returns {line index: value}, bits used, overshoot."""
    reg = {}
    out = {}
    outptr = 0
    base = None
    budget = 0
    start = bits.pos
    over = None
    exhausted = False
    for w in words:
        if w == 0xFFFFFFFF:
            break
        if w >> 24 == 0x7C:
            r = (w >> 16) & 0xFF
            reg[r] = w & 0xFFFF
            if r == 0x64:
                budget = w & 0xFFFF
                start = bits.pos
            elif r == 0x66:
                outptr = w & 0xFFFF
                if base is None:
                    base = outptr
        elif w >> 28 == 3:
            table, count, lin = w & 0x1F, ((w >> 5) & 0x1FF) + 1, (w >> 21) & 0xF
            if reg.get(0x44, 0) & 2:
                cb = T["mp3_tab33"] if table & 1 else T["mp3_tab32"]
                for _ in range(count):
                    if exhausted:
                        break
                    save = bits.pos
                    leaf = cb[bits.peek(4)]
                    if not leaf & 8:
                        extra = leaf & 3
                        leaf = cb[(leaf >> 3) + (bits.peek(4 + extra) & ((1 << extra) - 1))]
                    bits.pos += leaf & 7
                    if bits.pos - start > budget:
                        over = bits.pos - start - budget
                        bits.pos = save
                        exhausted = True
                        break
                    for s in range(4):
                        v = 0
                        if leaf & (128 >> s):
                            v = -1 if bits.get(1) else 1
                        out[outptr] = v
                        outptr += 1
            else:
                cbo = T["mp3_tabindex"][HW2ISO[table]]
                tabs = T["mp3_tabs"]
                for _ in range(count):
                    wd = 5
                    leaf = tabs[cbo + bits.peek(wd)]
                    while leaf < 0:
                        bits.pos += wd
                        wd = leaf & 7
                        leaf = tabs[cbo + bits.peek(wd) - (leaf >> 3)]
                    bits.pos += leaf >> 8
                    for _j in range(2):
                        v = leaf & 15
                        if v == 15 and lin:
                            v += bits.get(lin)
                        if v and bits.get(1):
                            v = -v
                        out[outptr] = v
                        outptr += 1
                        leaf >>= 4
    return out, bits.pos - start, over, budget


def parse_ref(fn):
    grs = []
    cur = None
    for line in open(fn):
        if line.startswith("G "):
            f = line.split()[1:]
            cur = dict(zip("frame gr ch part23 part3 bv t0 t1 t2 r0 r1 c1 bt mixed fo bit".split(),
                           map(int, f)))
            grs.append(cur)
        elif line.startswith("B "):
            cur["bytes"] = bytes.fromhex(line[2:].strip())
        elif line.startswith("V"):
            cur["vals"] = list(map(int, line[1:].split()))
    return grs


def parse_dump(fn):
    lists = []
    cur = None
    for line in open(fn):
        tag = line[:1]
        if tag == "L":
            p = line.split()
            cur = {"n": int(p[1]), "words": [int(x, 16) for x in p[2:]]}
            lists.append(cur)
        elif tag == "S":
            p = line.split()
            cur["sa"] = int(p[1], 16)
            cur["sbit"] = int(p[2])
            cur["raw"] = bytes.fromhex(p[3])
        elif tag == "U":
            p = line.split()
            cur["budget"], cur["used"], cur["over"] = int(p[1]), int(p[2]), int(p[3])
        elif tag == "V":
            cur["vals"] = list(map(int, line[1:].split()))
    return lists


def stream(raw, sa, swap):
    o = sa & 3
    if swap:
        return bytes(raw[(o + k) ^ 3] for k in range(len(raw) - 8))
    return raw[o:]


def key48(data, bit):
    return Bits(data, bit).peek(48)


def main():
    ref = parse_ref(sys.argv[1])
    dump = parse_dump(sys.argv[2])
    verbose = int(sys.argv[sys.argv.index("-v") + 1]) if "-v" in sys.argv else 0
    idx = {}
    for g in ref:
        if g["part3"] >= 48:
            idx.setdefault((g["bit"], key48(g["bytes"], g["bit"])), []).append(g)
    stats = {}
    shown = 0
    last_s = None
    for L in dump:
        if "raw" in L:
            last_s = L
        if "budget" not in L or last_s is None:
            continue
        for swap in (False, True):
            st = stream(last_s["raw"], last_s["sa"], swap)
            k = (last_s["sbit"], key48(st, last_s["sbit"]))
            if k in idx:
                break
        else:
            stats["unmatched"] = stats.get("unmatched", 0) + 1
            if L["budget"] == 0:
                stats["budget0"] = stats.get("budget0", 0) + 1
            continue
        g = idx[k][0]
        order = "swap" if swap else "natural"
        stats[order] = stats.get(order, 0) + 1
        out, used, over, budget = run_engine(L["words"], Bits(st, last_s["sbit"]))
        base = min(out) if out else 0
        # line index: model out pointers are IRAM word indexes; base line 0 = lowest pointer
        # minus the first programmed region's start (lines zeroed by the AVP have no output)
        # -> recover base from the count1 pointer: count1 starts at big_values*2.
        c1ptr = None
        mode = 0
        for w in L["words"]:
            if w >> 24 == 0x7C and ((w >> 16) & 0xFF) == 0x44:
                mode = w & 0xFFFF
            if w >> 24 == 0x7C and ((w >> 16) & 0xFF) == 0x66 and mode & 2:
                c1ptr = w & 0xFFFF
        if c1ptr is not None:
            base = c1ptr - 2 * g["bv"]
        mine = [0] * 576
        for p, v in out.items():
            if 0 <= p - base < 576:
                mine[p - base] = v
        ok_vals = mine == g["vals"]
        ok_budget = budget == g["part3"]
        ok_emul = L.get("vals") == [out[p] for p in sorted(out)] if "vals" in L else None
        key = (ok_vals, ok_budget)
        stats[key] = stats.get(key, 0) + 1
        if ok_emul is False:
            stats["emul_differs_from_offline"] = stats.get("emul_differs_from_offline", 0) + 1
        stats.setdefault("over_hist", {}).setdefault(over, 0)
        stats["over_hist"][over] += 1
        if not ok_vals and shown < verbose:
            shown += 1
            first = next(i for i in range(576) if mine[i] != g["vals"][i])
            print("list %d frame %d gr %d ch %d %s budget %d part3 %d used %d over %s bv %d "
                  "tabs %d/%d/%d r %d/%d c1 %d bt %d first diff line %d" % (
                      L["n"], g["frame"], g["gr"], g["ch"], order, budget, g["part3"], used, over,
                      g["bv"], g["t0"], g["t1"], g["t2"], g["r0"], g["r1"], g["c1"], g["bt"], first))
            print("  words", " ".join("%08x" % w for w in L["words"]))
            print("  mine", mine[max(0, first - 4):first + 8])
            print("  ref ", g["vals"][max(0, first - 4):first + 8])
    oh = stats.pop("over_hist", {})
    for k, v in sorted(stats.items(), key=str):
        print(k, v)
    print("overshoot histogram:", sorted(oh.items(), key=lambda x: (x[0] is None, x[0] or 0)))


if __name__ == "__main__":
    main()
