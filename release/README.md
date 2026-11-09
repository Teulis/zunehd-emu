# Zune HD emulator (QEMU "zunehd" machine)

Runs the original Microsoft Zune HD firmware (Windows Embedded CE 6, NVIDIA Tegra APX 2600)
in a custom QEMU machine on Windows. You get the real Zune HD user interface in a 272x480
window: the lock screen, main menu, music library, settings and so on. You can copy MP3s
onto the emulated player.

See **CAPABILITIES.md** for what works and what does not.

## Requirements

- Windows 10 or 11, 64-bit.
- Python 3 (https://www.python.org/; tick "Add python.exe to PATH"). It is only needed for
  setup and for copying music.
- An NTFS drive with about 200 MB free. The emulated 16 GB flash is a sparse file that only
  uses about 60 MB on disk.
- The **Zune HD firmware 4.5 (build 114)** files `NK.bin` and `EXT.bin`. They are **not
  included** (Microsoft copyright). They come from the Zune HD firmware update package.
  SHA-256 of the tested files:
  - NK.bin  `43B4EFCC5718E123D929873F3951E6500564C0733E422D8EC7BFAD4D39DC2963`
  - EXT.bin `C2255DAAB91FBF013A0DD615DA6EBB52865DE511910E57A1700514B123821B63`

## Setup (once)

1. Copy `NK.bin` and `EXT.bin` into the `firmware` folder.
2. Open PowerShell in this folder and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\setup.ps1
   ```

   This converts the firmware and builds `zunehd-emmc16.img`, the emulated internal flash.
   It takes about a minute. Run `.\setup.ps1 -Rebuild` later to reset the player to
   factory state. That deletes synced music and settings.

## Running

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1
```

The UI is up in about 30 seconds (a real Zune HD takes ~28 s). Like the device, the screen
turns off after a minute without input: press **Home** to wake it, then swipe up with the
mouse (hold the left button and drag upwards) if the lock screen shows.

| Zune HD | Keyboard / mouse (window focused) |
|---|---|
| Home button | `Home` or `H` |
| Power button (sleep / wake) | `End` or `P` |
| Media button (volume / now playing overlay) | `M` |
| Touch screen | Left mouse button = finger (click to tap, drag to swipe) |
| Quit | Close the window, or `Ctrl-A` then `X` in the PowerShell console |

Options:

| Option | Effect |
|---|---|
| (default) | Emulates the AVP media coprocessor and plays sound through the Windows audio device, so music plays (see CAPABILITIES.md). |
| `.\run.ps1 -NoAvp` | Without the AVP: no music decoding, slightly lighter on the CPU. |
| `.\run.ps1 -UsbIp` | "Plugs in" a virtual USB cable through USB/IP on 127.0.0.1:3240 (for copying music). |
| `.\run.ps1 -NoAudio` | No host audio device. |
| `.\run.ps1 -Headless` | No window, serial console only. |

The PowerShell console shows the device's serial debug output. Many lines like
`VerifyCardPresence ... CMD5` are normal: the Wi-Fi chip is not emulated.

## Copying music onto the player

1. Start with `.\run.ps1 -UsbIp`. Wait until the UI is up
   (~30 s); connecting earlier can leave the player stuck on "CONNECTED".
2. In a second PowerShell window in this folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\sync-music.ps1 "C:\Music\a.mp3" "C:\Music\b.mp3"
   ```

   Title, artist and album come from the files' ID3 tags. The tracks appear under
   music > albums / artists and in the "New" tile on the main menu.

Only MP3 is tested. The connection is plain MTP over the built-in USB/IP server; no drivers
are needed. The official Zune desktop software is **not** supported (see CAPABILITIES.md).

## Using the player as a real USB device in Windows (Explorer)

With the free **usbip-win2** client the emulated Zune attaches to Windows like a plugged-in
player:

1. Install usbip-win2 once: `winget install --id vadimgrn.usbip-win2 -e` (admin, installs a
   driver).
2. Start the emulator with `.\run.ps1 -UsbIp` and wait for the UI (~30 s).
3. Attach it: `& 'C:\Program Files\USBip\usbip.exe' attach -r 127.0.0.1 -b 1-1`
   Windows installs its own "Zune" portable-device driver. Unplug it with
   `& 'C:\Program Files\USBip\usbip.exe' detach -p 1`.

Windows hides Zune players from Explorer on purpose, so that only the Zune software talks to
them. To browse it in Explorer (This PC > Zune > Storage), run once in an **admin**
PowerShell while it is attached, then detach and re-attach:

```powershell
$k = 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_045E&PID_063E\00000000-0000-0000-0000-000000000000\Device Parameters'
Set-ItemProperty $k PortableDeviceNameSpaceExcludeFromShell 0
Set-ItemProperty $k ShowInShell 1
Set-ItemProperty $k EnableLegacySupport 1
```

This only affects the emulated player's entry (its serial number is all zeros), not a real
Zune. You can then drag MP3s into Storage and copy them back out. Don't use the USB/IP sync
script and an attached usbip-win2 session at the same time: only one client can use the
device.

## Folder contents

| Path | Contents |
|---|---|
| `bin\` | `qemu-system-arm.exe` (QEMU 11.1.3 + zunehd machine) and its runtime DLLs (MSYS2 UCRT64) |
| `firmware\` | Put `NK.bin` / `EXT.bin` here (not included) |
| `device\` | Partition-table and file-system boot sectors captured from a real 16 GB Zune HD, used to lay out the emulated flash exactly like the device. No user data and no keys. |
| `tools\` | `b000ff2nb0.py` (firmware converter), `mkemmc.py` (builds the flash image), `zmtp.py` (MTP client), `mksilentmp3.py` (makes tagged test MP3s) |
| `source\` | GPL source: every QEMU file this project adds or changes, plus licences |
| `setup.ps1`, `run.ps1`, `sync-music.ps1` | The scripts above |

Created by setup: `extracted\` (converted firmware) and `zunehd-emmc16.img`.

## Troubleshooting

- **"running scripts is disabled"**: start the scripts as shown, with
  `powershell -ExecutionPolicy Bypass -File ...`.
- **Black window**: the UI takes ~30 s to appear, and the screen turns off after a minute
  without input, just like the device. Press `Home` to wake it.
- **Odd behaviour after a crash or a killed QEMU**: the guest writes to the flash image and
  a half-written state can break later boots. Run `.\setup.ps1 -Rebuild`.
- **Port already in use**: only one emulator with `-UsbIp` can run at a time (port 3240).
- **Graphics glitches**: see "Known issues" in CAPABILITIES.md.

## Building QEMU yourself

The binary is QEMU v11.1.3 built with MSYS2 UCRT64 (`../configure --target-list=arm-softmmu
--enable-sdl --disable-werror --disable-docs`, then `ninja qemu-system-arm.exe`). Copy the
files from `source\qemu\` over a clean QEMU 11.1.3 source tree first. QEMU and these
changes are licensed under the GNU GPL v2 (`source\COPYING.GPLv2`, `source\QEMU-LICENSE`).
The MP3 Huffman tables in `zunehd-mp3tab.h` are from minimp3 (CC0).
