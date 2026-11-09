"""Build a raw eMMC image for the zunehd QEMU machine (16 GB Zune HD geometry).

Layout (native 512-byte blocks; DSK1 logical sectors are 2048 bytes):
  native 0 .. 441343        hidden front region (BCT/bootloader/boot images) - zero except
                            the NVIDIA partition table and PKV (FRONT_FILES)
  native 441344 + 4*N       DSK1 sector N
DSK1 (measured on the user's 16 GB unit, 2026-11-07):
  LBA 0       MBR: one extended partition (0x05) LBA 16, 7,803,120 sectors
  LBA 16      EBR -> Part00 ZBINFS (0x21) LBA 24, 65,544 sectors
  LBA 65568   EBR -> Part01 exFAT "Flash" LBA 65576, 65,544 sectors
  LBA 131120  EBR -> Part02 exFAT "Flash2" LBA 131128, 7,672,008 sectors
ZBINFS content = NK.nb0 at partition offset 0 + EXT.nb0 at 0xBC0000 (NK length
rounded up to 64 KiB), zero-filled. Verified sector-for-sector against reads
from the device. The exFAT partitions are formatted empty, reusing the device's
own 12-sector boot regions (device\\dsk1_<lba>_boot.bin) for exact geometry.

Usage: py -3 -I tools\\mkemmc.py [--out zunehd-emmc16.img]
"""
import argparse, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = 512
SEC = 2048
FRONT_BLOCKS = 441344
DSK1_SECTORS = 16 + 7803120             # end of the extended partition
SEC_COUNT = FRONT_BLOCKS + 4 * DSK1_SECTORS  # 31,653,888 = 0x01E30000
ZBINFS_LBA, ZBINFS_SECTORS = 24, 65544
EXT_OFFSET_IN_ZBINFS = 0xBC0000

DEV = os.path.join(ROOT, "device")
# (DSK1 LBA, file) captured from the device: partition tables + ZBINFS samples
DEVICE_SECTORS = [
    (0, "dsk1_0000-0031.bin"), (65568, "dsk1_65568.bin"),
    (131120, "dsk1_131120.bin"), (6013, "dsk1_6013.bin"),
    (12048, "dsk1_12048.bin"), (25369, "dsk1_25369.bin"),
    (65567, "dsk1_65567.bin"),
]
TABLE_LBAS = (0, 16, 65568, 131120)
# (native block, file) captured from the device's hidden front region (Lyra
# read-emmc-front.py, 2026-11-07): the NVIDIA partition table (PT partition,
# located by boot-args +0x1E4/+0x1E8 = {0x600, 0x40}) and KeyVault's PKV
# partition (device-unique secrets - keep device\ private).
FRONT_FILES = [
    (6144, "front_6144_16k.bin"),       # PT: 13 entries, BCT/PT/BTA/BTB/ZRA/ZRB/MTE/ARG/ZAD/PKV/NKV/BLU/COP
    (434688, "front_PKV_434688.bin"),   # PKV, 128 KiB (3 non-zero 2 KiB sectors)
]


def dsk1_off(lba):
    return (FRONT_BLOCKS + 4 * lba) * NATIVE


# EXT modules are paged in from ZBINFS, not run from the RAM copy QEMU loads,
# so EXT code patches must go here too (same as zunehd_avp_patches in zunehd.c).
# (EXT PA, original 8 bytes, replacement) - applied only where the bytes match.
EXT_PA = 0x01790000
EXT_PATCHES = [
    # MtpSvc Initialize FUN_416d2f74: ignore failed KEY1: cert fetch (bpl -> b)
    (0x025B5088, bytes.fromhex("0200005a1631a0e3"), bytes.fromhex("020000ea1631a0e3")),
    # MtpSvc FUN_416cb71c: MTPZ flag forced 0 (strne r8,[r5,#0x28] -> str; r8 == 0)
    (0x025AD808, bytes.fromhex("28808515283095e5"), bytes.fromhex("288085e5283095e5")),
]


def zbinfs_image():
    nk = open(os.path.join(ROOT, "extracted", "NK.nb0"), "rb").read()
    ext = bytearray(open(os.path.join(ROOT, "extracted", "EXT.nb0"), "rb").read())
    for pa, orig, repl in EXT_PATCHES:
        o = pa - EXT_PA
        if ext[o:o + len(orig)] == orig:
            ext[o:o + len(repl)] = repl
            print("EXT patch applied @PA 0x%08x" % pa)
        else:
            print("EXT patch SKIPPED @PA 0x%08x: bytes differ" % pa)
    assert len(nk) <= EXT_OFFSET_IN_ZBINFS
    img = bytearray(EXT_OFFSET_IN_ZBINFS + len(ext))
    img[:len(nk)] = nk
    img[EXT_OFFSET_IN_ZBINFS:] = ext
    assert len(img) <= ZBINFS_SECTORS * SEC
    return img


EXFAT_PARTS = [  # (DSK1 LBA, boot-region capture from the device, volume label)
    (65576, "dsk1_65576_boot.bin", "Flash"),
    (131128, "dsk1_131128_boot.bin", "Flash2"),
]


def exfat_upcase():
    """The standard compressed exFAT up-case table (2474 bytes, checksum 0x8FA6CA0D), exactly as
    on the device (device\\dsk1_65742_flash_upcase.bin, read 2026-11-07). zexfat does not mount
    volumes with a minimal custom table."""
    tbl = open(os.path.join(DEV, "dsk1_65742_flash_upcase.bin"), "rb").read()[:2474]
    chk = 0
    for b in tbl:
        chk = (((chk >> 1) | ((chk & 1) << 31)) + b) & 0xFFFFFFFF
    return tbl, chk


def exfat_writes(boot, label):
    """Empty exFAT volume using the device's own boot region (geometry, serial, checksum).
    Returns [(byte offset within the partition, data)]."""
    (fatoff, fatlen, heapoff, ccount, root) = struct.unpack_from("<IIIII", boot, 80)
    ss = 1 << boot[108]
    cs = ss << boot[109]
    assert len(boot) == 12 * ss
    out = [(0, boot), (12 * ss, boot)]            # main + backup boot region
    bitmap_len = (ccount + 7) // 8
    upcase, upchk = exfat_upcase()
    nbit = -(-bitmap_len // cs)
    nup = -(-len(upcase) // cs)
    bitmap_clus = root + 1
    upcase_clus = bitmap_clus + nbit
    used = 1 + nbit + nup
    # FAT: media + reserved, root EOC, bitmap and up-case chains
    fat = {0: 0xFFFFFFF8, 1: 0xFFFFFFFF, root: 0xFFFFFFFF}
    for start, n in ((bitmap_clus, nbit), (upcase_clus, nup)):
        for k in range(n):
            fat[start + k] = start + k + 1 if k < n - 1 else 0xFFFFFFFF
    fatbuf = bytearray(4 * (max(fat) + 1))
    for k, v in fat.items():
        struct.pack_into("<I", fatbuf, 4 * k, v)
    out.append((fatoff * ss, bytes(fatbuf)))
    bitmap = bytearray(nbit * cs)
    for k in range(used):
        bitmap[k // 8] |= 1 << (k % 8)
    clus_off = lambda c: (heapoff * ss) + (c - 2) * cs
    out.append((clus_off(bitmap_clus), bytes(bitmap)))
    out.append((clus_off(upcase_clus), upcase))
    # Same order as the device's root directories: bitmap, up-case, EMPTY label (the mount
    # names "Flash"/"Flash2" come from the partition records, not from the label).
    rootdir = bytearray(cs)
    struct.pack_into("<BB18xIQ", rootdir, 0, 0x81, 0, bitmap_clus, bitmap_len)  # allocation bitmap
    struct.pack_into("<B3xI12xIQ", rootdir, 32, 0x82, upchk, upcase_clus, len(upcase))  # up-case
    struct.pack_into("<BB", rootdir, 64, 0x83, 0)                            # volume label (empty)
    out.append((clus_off(root), bytes(rootdir)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "zunehd-emmc16.img"))
    a = ap.parse_args()

    dev = {}
    for lba, name in DEVICE_SECTORS:
        if not os.path.exists(os.path.join(DEV, name)):
            if lba in TABLE_LBAS:
                sys.exit("missing partition-table capture device\\%s" % name)
            continue                    # optional self-check sample
        data = open(os.path.join(DEV, name), "rb").read()
        for i in range(len(data) // SEC):
            dev[lba + i] = data[i * SEC:(i + 1) * SEC]

    if os.path.exists(a.out):
        os.remove(a.out)
    open(a.out, "wb").close()
    # Keep the 16 GB file sparse on NTFS (only ~50 MB is ever written).
    subprocess.run(["fsutil", "sparse", "setflag", a.out], check=True,
                   stdout=subprocess.DEVNULL)
    for need in ("NK.nb0", "EXT.nb0"):
        if not os.path.exists(os.path.join(ROOT, "extracted", need)):
            sys.exit("extracted\\%s missing - run setup.ps1 (converts firmware\\NK.bin / EXT.bin)" % need)
    zb = zbinfs_image()
    with open(a.out, "r+b") as f:
        f.truncate(SEC_COUNT * NATIVE)
        for lba in TABLE_LBAS:
            f.seek(dsk1_off(lba))
            f.write(dev[lba])
        f.seek(dsk1_off(ZBINFS_LBA))
        f.write(zb)
        for blk, name in FRONT_FILES:
            p = os.path.join(DEV, name)
            if os.path.exists(p):
                f.seek(blk * NATIVE)
                f.write(open(p, "rb").read())
            else:
                print(f"note: {name} missing, front block {blk} left zero")
        for lba, cap, label in EXFAT_PARTS:
            boot = open(os.path.join(DEV, cap), "rb").read()
            for off, data in exfat_writes(boot, label):
                f.seek(dsk1_off(lba) + off)
                f.write(data)

    # Self-check against every sector read from the device.
    bad = 0
    with open(a.out, "rb") as f:
        for lba, want in sorted(dev.items()):
            f.seek(dsk1_off(lba))
            if f.read(SEC) != want:
                bad += 1
                print(f"MISMATCH DSK1 LBA {lba}")
    print(f"{a.out}: {SEC_COUNT} blocks ({SEC_COUNT * NATIVE} bytes), "
          f"ZBINFS {len(zb)} bytes, {len(dev)} device sectors checked, {bad} mismatches")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
