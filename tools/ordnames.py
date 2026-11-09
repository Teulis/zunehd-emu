"""Resolve a WinCE module's ordinal imports to names via the exporting DLLs' export tables,
and print each import thunk's IAT VA (call stubs in the module jump through these).

Usage: py -3 -I tools\\ordnames.py credsvc.dll [--image NK]
"""
import argparse, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pe_info(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    secs = []
    so = opt + optsz
    for i in range(nsec):
        va, vsz, raw, rsz = struct.unpack_from("<IIII", data, so + 40 * i + 8)
        secs.append((struct.unpack_from("<I", data, so + 40 * i + 12)[0],
                     struct.unpack_from("<I", data, so + 40 * i + 8)[0],
                     struct.unpack_from("<I", data, so + 40 * i + 20)[0]))
    dirs = opt + 96
    return base, secs, dirs


def rva2off(secs, rva):
    for va, vsz, raw in secs:
        if va <= rva < va + max(vsz, 1):
            return raw + rva - va
    return None


def exports(path):
    data = open(path, "rb").read()
    base, secs, dirs = pe_info(data)
    erva = struct.unpack_from("<I", data, dirs)[0]
    if not erva:
        return {}
    e = rva2off(secs, erva)
    obase, nfun, nnam, afun, anam, aord = struct.unpack_from("<IIIIII", data, e + 16)
    names = {}
    for i in range(nnam):
        nrva = struct.unpack_from("<I", data, rva2off(secs, anam) + 4 * i)[0]
        idx = struct.unpack_from("<H", data, rva2off(secs, aord) + 2 * i)[0]
        no = rva2off(secs, nrva)
        names[obase + idx] = data[no:data.index(b"\0", no)].decode()
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module")
    ap.add_argument("--image", default="NK")
    a = ap.parse_args()
    mdir = os.path.join(ROOT, "extracted", a.image, "modules")
    data = open(os.path.join(mdir, a.module), "rb").read()
    base, secs, dirs = pe_info(data)
    irva = struct.unpack_from("<I", data, dirs + 8)[0]
    off = rva2off(secs, irva)
    cache = {}
    while True:
        ilt, ts, fwd, name_rva, iat = struct.unpack_from("<IIIII", data, off)
        if not name_rva:
            break
        no = rva2off(secs, name_rva)
        dll = data[no:data.index(b"\0", no)].decode()
        if dll.lower() not in cache:
            cache[dll.lower()] = {}
            # look in this image first, then NK (EXT modules import NK's coredll etc.)
            for d in (mdir, os.path.join(ROOT, "extracted", "NK", "modules")):
                cand = [f for f in os.listdir(d) if f.lower() in (dll.lower(), "k." + dll.lower())]
                if cand:
                    # prefer the user-mode copy over the k. kernel copy
                    cache[dll.lower()] = exports(os.path.join(d, sorted(cand, key=len)[0]))
                    break
        ex = cache[dll.lower()]
        t = rva2off(secs, ilt or iat)
        i = 0
        while True:
            v = struct.unpack_from("<I", data, t + 4 * i)[0]
            if not v:
                break
            if v & 0x80000000:
                nm = ex.get(v & 0xFFFF, f"#{v & 0xFFFF}")
            else:
                ho = rva2off(secs, v)
                nm = data[ho + 2:data.index(b"\0", ho + 2)].decode()
            print(f"IAT 0x{base + iat + 4 * i:08x}  {dll}!{nm}")
            i += 1
        off += 20


if __name__ == "__main__":
    main()
