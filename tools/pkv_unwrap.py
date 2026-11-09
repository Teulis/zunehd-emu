"""Have the Zune's own keyvault.dll decrypt the 64-byte PKV header with the
chip's AES key (FUN_c0b35b48(out, in, 0x40, 1), exactly as KEY_Init's
FUN_c0b38f50 does). Read-only w.r.t. storage; leaves two 64-byte kernel heap
allocations (NvOsAlloc) until the next reboot."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ext" / "project-lyra" / "tools" / "general"))
from zune_repl import ZuneREPL  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 else sys.exit("usage: pkv_unwrap.py <zune-ip>")
NVOSALLOC_THUNK = 0xC088D2B0
KV_AES_UNWRAP = 0xC0B35B48          # keyvault.dll FUN_c0b35b48
KV_CODE_EXPECT = 0xC0B35B48         # sanity: keyvault loaded at its ROM vbase 0xC0B30000

pkv = (Path(__file__).resolve().parent.parent / "device" / "front_PKV_434688.bin").read_bytes()
hdr = pkv[:0x40]

r = ZuneREPL(HOST, timeout=60.0)
# Sanity: first instruction of the target must match the ROM image.
rom = (Path(__file__).resolve().parent.parent / "extracted" / "NK" / "modules" / "keyvault.dll").read_bytes()
live = r.kread(KV_AES_UNWRAP, 8)
print("live code @0xC0B35B48:", live.hex())
if live != bytes.fromhex("f0412de950d04de2"):
    sys.exit("keyvault code mismatch - not calling")

buf_in, _ = r.kcall(NVOSALLOC_THUNK, 0x40)
buf_out, _ = r.kcall(NVOSALLOC_THUNK, 0x40)
if not buf_in or not buf_out:
    sys.exit("NvOsAlloc failed")
print(f"in=0x{buf_in:08x} out=0x{buf_out:08x}")
r.kwrite(buf_in, hdr)
r.kwrite(buf_out, b"\0" * 0x40)
rc, err = r.kcall(KV_AES_UNWRAP, buf_out, buf_in, 0x40, 1)
print(f"FUN_c0b35b48 -> {rc} (GetLastError 0x{err:08x})")
plain = r.kread(buf_out, 0x40)
print("plain header:", plain.hex())
if rc == 1:
    (Path(__file__).resolve().parent.parent / "device" / "pkv_hdr_plain.bin").write_bytes(plain)
    print("saved device\\pkv_hdr_plain.bin")
