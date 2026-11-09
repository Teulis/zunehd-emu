# zunehd-emu: a Zune HD emulator on QEMU

A custom QEMU machine (`zunehd`) that boots the **stock Zune HD firmware** (Windows CE 6, fw 4.5 build 114)
on an emulated NVIDIA Tegra APX 2600. The real UI runs on an emulated Tegra 3D GPU. Touch, buttons, USB
music sync (MTP over USB/IP) and MP3 playback through the AVP media coprocessor all work.

- What works and what doesn't: [`release/CAPABILITIES.md`](release/CAPABILITIES.md)
- Hardware map and emulation status per chip: [`tree.md`](tree.md)
- Next steps: [`plan.md`](plan.md)
- Current state and quick commands: [`CHECKPOINT.md`](CHECKPOINT.md)
- Register-level findings, session by session: [`NOTES.md`](NOTES.md)

**No firmware is included.** You need your own copy of the Zune HD 4.5 firmware (`NK.bin`, `EXT.bin`;
`Recovery.bin` and `ZBoot.bin` are optional).

## Layout

| Path | Contents |
|---|---|
| `qemu/` | The emulator: an **overlay** on QEMU v11.1.3. It holds only the new or changed files (`hw/arm/zunehd*.c/h`, `meson.build`, `Kconfig`, `zbuild.sh`, `scripts/symlink-install-tree.py`). The full QEMU source is unpacked into the same folder locally and ignored by git. |
| `tools/` | Build, boot, debug and analysis scripts (eMMC image builder, boot capture, gdb helpers, MTP client, 3D/audio analysis). |
| `device/` | Non-secret captures from a real 16 GB unit that the eMMC image builder needs (partition tables, exFAT boot regions, up-case table, NVIDIA partition table). |
| `release/` | End-user package scripts (`setup.ps1`, `run.ps1`, `sync-music.ps1`) and their README. |
| `research/` | One-off research scripts from the GPU and audio sessions, kept as-is (see its README). |
| `LICENSES/` | GPL-2.0 and QEMU's license notice. |

## Developer setup (Windows 11)

1. **Toolchain:** install MSYS2 at `C:\msys64` with the UCRT64 toolchain plus QEMU's build dependencies
   (glib2, pixman, SDL2, ninja, meson, python); `gdb-multiarch` is optional. Install Python 3 (`py -3`) on
   the Windows side.
2. **QEMU source:** download `qemu-11.1.3.tar.xz` and unpack it **into `qemu/` without overwriting the
   overlay** (from an MSYS2 UCRT64 shell in the repo root):
   ```sh
   tar -xf qemu-11.1.3.tar.xz --strip-components=1 -k -C qemu
   ```
   (`-k` keeps the existing overlay files.) Then configure and build:
   ```sh
   cd qemu && mkdir -p build && cd build && ../configure --target-list=arm-softmmu && ninja qemu-system-arm.exe
   ```
   After that, incremental builds from PowerShell:
   `$env:MSYSTEM='UCRT64'; & C:\msys64\usr\bin\bash.exe -l <repo>/qemu/zbuild.sh`
3. **Firmware:** put `NK.bin` and `EXT.bin` (and optionally `Recovery.bin`, `ZBoot.bin`) in
   `ZuneHD_Firmware\Bins\`. Convert them to flat images for the eMMC builder:
   ```powershell
   py -3 -I tools\b000ff2nb0.py ZuneHD_Firmware\Bins\NK.bin  extracted\NK.nb0
   py -3 -I tools\b000ff2nb0.py ZuneHD_Firmware\Bins\EXT.bin extracted\EXT.nb0
   ```
   For module-level analysis (rebuilt PE modules, `modules.txt` used by `tools\zsym.py`), also run
   `powershell -NoProfile -File tools\ce\extract.ps1`.
4. **eMMC image:** `py -3 -I tools\mkemmc.py`. This writes `zunehd-emmc16.img`, a 16 GB sparse file
   (~60 MB on disk; needs NTFS).
5. **Run:** `.\run-zunehd.ps1`. The serial console is in the terminal; quit with Ctrl-A then X. Options:
   `-UsbIp` (USB/IP cable for `tools\zmtp.py` or usbip-win2), `-NoAvp`, `-NoAudio`, `-Gdb`, `-Trace`.
   Headless boot + logs: `powershell -NoProfile -File tools\zcapture.ps1 -Seconds 120 -Emmc 0`.

## Optional references (not vendored)

- [grate](https://github.com/grate-driver/grate) (MIT): Tegra 3D ISA docs and disassemblers.
  `tools\fetch-grate.sh` clones it into `ext\`; `tools\g3ddis` builds against it.
- [project-lyra](https://github.com/project-lyra-zune/project-lyra) (MIT): Lyra host tools for reading data
  from a real Zune running [zuneslayer](https://github.com/CUB3D/zuneslayer) (used by `tools\pkv_unwrap.py`).
  Clone into `ext\project-lyra`.
- minimp3 (CC0) in `ext\minimp3`: reference decoder for `tools\mp3pcm.c` and `tools\mp3ref.c`.

## What must never be committed

The `.gitignore` excludes these, but check before pushing:

- firmware and anything extracted or decompiled from it (`ZuneHD_Firmware\`, `extracted\`, traces, RAM dumps);
- device-unique secrets (`device\front_PKV_*.bin`, `devcert.dat`, decrypted key-vault headers);
- eMMC images, logs and screenshots;
- commercial music used for testing.

## License

Everything in this repository is licensed under the **GNU General Public License, version 2 or (at
your option) any later version** (`GPL-2.0-or-later`); see [`LICENSE`](LICENSE). The QEMU overlay in
`qemu/` builds into QEMU, which is GPL-2.0 (QEMU's own notice is in `LICENSES/QEMU-LICENSE`).
`qemu/hw/arm/zunehd-mp3tab.h` holds MP3 Huffman tables taken from minimp3 (CC0).

The Zune HD firmware is **not** covered by this license and is not distributed here.
