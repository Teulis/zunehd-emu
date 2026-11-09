# Zune HD hardware tree (Tegra APX 2600) - emulation status

Sources: CHECKPOINT.md, NOTES.md, release\CAPABILITIES.md, qemu\hw\arm\zunehd*.c, plus a live probe of the
user's Zune over Lyra on 2026-11-08 (read-only; see "How this was probed" below).
Addresses and IRQs are the values the stock firmware (fw 4.5) uses.

**Status key:** ✅ emulated and working · ◐ stub or partial model · ✗ not emulated · ? identity unconfirmed

**Part names:** `[...]` gives the chip or IP-core name. Blocks inside the SoC are labelled
`[AP16 internal: ...]`. `ODM "xxx"` is the device's own NVIDIA ODM peripheral GUID for the part.
GPIO pins are written port+pin (port A = 0 ... U = 20, V = 21).

```
Zune HD board  [Microsoft Zune HD, NVIDIA board codename "Pavo"; USB 045e:063e; board type 10]
└── SoC  [NVIDIA Tegra APX 2600 "AP16", chip id 0x16; real HIDREV 0x00031617, emulator reports 0x00011600 (QuickTurn path)]
    │
    ├── CPU complex
    │   ├── Main CPU  [ARM ARM1136J(F)-S; QEMU arm1136 + VAPA] - WinCE 6 / OAL (nk.exe) ........ ✅
    │   └── AVP / COP  [AP16 internal: ARM7TDMI-class media coprocessor; QEMU ti925t, ARMv4T] ... ✅ zunehd-avp.c
    │       ├── AVP MMU/TLB block  [AP16 internal]                 0xF000F000 ........................ ✅
    │       ├── AVP vector RAM @0, IROM page 0xFFFF0000, SDRAM alias 0x90000000 ........................ ✅
    │       └── Audio vector engine "APE"  [AP16 internal: IMDCT + polyphase DSP]  0x6000E000 (+70/74/78) ✅ zunehd-ape.c
    │
    ├── Memory
    │   ├── SDRAM 128 MB  [DRAM part ?]                            0x00000000 ........................ ✅
    │   ├── IRAM 256 KB  [AP16 internal, shared with AVP]          0x40000000 ........................ ✅
    │   ├── GART aperture  [AP16 internal]                         0x58000000 (16 MB) ................ ◐
    │   ├── Boot ROM / high vectors  [AP16 internal IROM]          0xFFF00000 ........................ ◐ no real IROM
    │   └── EMC memory controller  [AP16 internal] (DFS state; QuickTurn path skips it) ............. ◐
    │
    ├── PPSB bus (0x60000000)
    │   ├── RES_SEMA mailbox CPU<->AVP  [AP16 internal]            0x60001000  IRQ 4 (SHR_SEM) ....... ✅
    │   ├── ARB_SEMA arbitration semaphores  [AP16 internal]       0x60002000  IRQ 29 ................ ✅
    │   ├── ICTLR interrupt controller  [AP16 internal, 3 banks, CPU + COP]  0x60004000 ............. ✅
    │   ├── Timers TMR1-4 + TMRUS  [AP16 internal]                 0x60005000  IRQ 0/1/41/42 (tick) .. ✅
    │   ├── CAR clock and reset controller  [AP16 internal] (PLLs pinned to BYPASS)  0x60006000 ...... ◐
    │   ├── Flow controller  [AP16 internal] (µs delay, HALT_COP)  0x60007000 ........................ ✅
    │   ├── APB DMA, 16 ch  [AP16 internal, Tegra 2 layout]        0x6000A000 / ch 0x6000B000+0x20n
    │   │                                                          IRQ 26 (COP 60) ................... ✅ zunehd-apbdma.c
    │   ├── Unknown block  [?] (touched by the AVP)                0x6000C000 ........................ ◐ ?
    │   ├── GPIO, 7 banks x 4 ports  [AP16 internal]               0x6000D000  IRQ 32-35,55,87,89 .... ✅
    │   │   ├── S5  Home key  [ODM "zkeypad0" pin 0] (active low, both edges) ............................. ✅
    │   │   ├── S6  Media/volume overlay key  [ODM "zkeypad0" pin 1] ...................................... ✅
    │   │   ├── O7, D7  touch controller lines  [ODM "tpktouch"] ......................................... ◐ emulator uses U0 (see Touch)
    │   │   ├── U5  accelerometer interrupt  [ODM "accelero"] ............................................. ✗
    │   │   ├── U1, P5, U4, V6  HD Radio control/IRQ lines  [ODM "hd_radio"] ............................. ✗
    │   │   ├── U3, A4, P6  Wi-Fi control lines  [ODM "sdiowlan"] ......................................... ✗
    │   │   ├── U7, D5, T3 (+ T4 L0-L7 T2)  board-type straps  [board resistors] ......................... ✅
    │   │   ├── C4/C5  bit-banged I2C1 fallback (SCL/SDA) ................................................ ◐ ACKs everything, reads 0
    │   │   └── K5, N7  edge-IRQ pins  [not in the ODM table; purpose unknown] .......................... ?
    │   ├── EVP exception vectors  [AP16 internal] (COP reset vector 0x6000F200)  0x6000F000 ......... ✅
    │   └── VDE video decode engine  [AP16 internal]               0x6001A000-0x6001DB00 ............. ◐
    │       ├── BSE bitstream engine  [AP16 internal] (MP3 Huffman, bit-exact)  0x6001C000/0x6001D000 . ✅ zunehd-bse.c
    │       ├── Crypto/BSE status block  [AP16 internal] (idle stub)  0x6001B000 ....................... ◐
    │       └── Video decode paths (H.264 / WMV etc.)  [AP16 internal] ................................ ✗
    │
    ├── host1x  [AP16 internal: NVIDIA host1x]  0x50000000  IRQ 65/67, 32 sync points, channels 0/1/2 .. ✅
    │   └── host1x clients (0x54000000)
    │       ├── Display controller A  [AP16 internal, Tegra 2-compatible DC]  0x54200000 (272x480 win A) ✅
    │       ├── Display controller B  [AP16 internal]              0x54240000 ........................ ◐
    │       ├── 2D "G2" fill/copy  [AP16 internal, host1x class 0x51] ................................... ✅
    │       ├── Stretch-blit "SB"  [AP16 internal, host1x class 0x52] ................................... ✅
    │       ├── 3D GPU  [AP16 internal: NVIDIA "AR20" = Tegra 2 3D core, class 0x60] .................... ✅ zunehd-gr3d.c
    │       ├── HDMI out  [AP16 internal; ODM "ffa_hdmi": DDC I2C1 0xA0 (7-bit 0x50), I2C2 0x74 (0x3A)] ✗ (AV dock output)
    │       ├── TV out / CRT  [AP16 internal; ODM "ffatvout", "ffa__crt"] ................................ ✗
    │       └── VI (camera input, ODM "vipstrea" = rails only), MPE (video encoder)  [AP16 internal] ...... ✗
    │
    ├── APB bus (0x70000000)
    │   ├── APB_MISC (HIDREV, EMU_REVID, pinmux)  [AP16 internal]  0x70000000 ........................ ◐
    │   │   └── USB1/USB2 PHY ctrl 0xA04-0xA34, VBUS wake 0x70/0x74  [AP16 internal UTMI PHY] ......... ✅
    │   ├── DAP audio router  [AP16 internal]                      0x70002400 ........................ ◐
    │   ├── I2S1  [AP16 internal]                                  0x70002800  IRQ 13 ................ ✅ FIFO1 fed by APB DMA ch15
    │   ├── UARTA/B/C  [AP16 internal, NS16550-compatible; UARTA = debug console]
    │   │                                                          0x70006000/6040/6200  IRQ 36/37/46  ✅
    │   ├── HSMMC  [AP16 internal: NVIDIA HSMMC, not SDHCI]        0x70008500  IRQ 22 ................ ✅
    │   │   └── eMMC 4.2 managed NAND  [Phison PS8006 controller + Toshiba TH58NVG7D2ELA80 MLC NAND
    │   │                               (part numbers as reported by the user, not probed)] ............ ✅ QEMU TYPE_EMMC, 16 GB image
    │   │       ├── front reserve: BCT, bootloader, NK x2, NVIDIA PT, PKV key vault
    │   │       └── DSK1 (base 0x1AF00): ZBINFS \Windows, exFAT \Flash, exFAT \Flash2
    │   ├── I2C1  [AP16 internal I2C, NvRm instance 0]             0x7000C000  IRQ 38 ................ ◐ instant ACK
    │   │   └── 0x18  accelerometer  [Kionix KXSD9, ODM "accelero", zaccelerometer.dll] ............... ✗
    │   ├── I2C2  [AP16 internal I2C, NvRm instance 1]             0x7000C400  IRQ 84 ................ ✅
    │   │   ├── 0x33  touch controller  [Avago capacitive touch controller (part ?), avago.dll]
    │   │   │         ← THE ONE ON THE USER'S UNIT (avago.dll loaded, synaptics.dll not) .............. ✗
    │   │   └── 0x2C  touch controller, alternate supplier  [Synaptics, RMI4 protocol (part ?), synaptics.dll]
    │   │             (F01 @0xEE, F11 @0xE8; what the emulator presents) ............................... ✅
    │   ├── DVC I2C (power I2C)  [AP16 internal DVC I2C]           0x7000D000  IRQ 53 ................ ✅
    │   │   └── 0x1A  PMIC + audio codec + RTC  [Wolfson WM8350, ID 0x6143; ODM "wolf8350" / "wm__pmu0"] ◐
    │   │       ├── RTC R16-R19 (BCD; read-only in the model; ODM NVDD_RTC = rail 8) ........................ ◐
    │   │       ├── ON-key = power button (R0xE6/R0x1E/R0x18) -> ext IRQ 86 ................................. ✅
    │   │       ├── Regulators / rails (bypassed by patch; "PMU present" kept clear) ........................ ✗
    │   │       │   rail map (ODM): 1 CORE · 3 NAND, SDIO, VI · 6 OSC, SYS, LCD, AUD, DDR, UART, BB
    │   │       │                   9 VDAC · 10 PLLHD, HDMI · 0 = PLLs, USB, MIPI (no PMU rail) · 8 RTC
    │   │       └── Codec registers (audio goes I2S -> host instead) ......................................... ◐
    │   ├── SPI/SLINK 1-3  [AP16 internal SLINK]                   0x7000D400/D600/D800 .............. ◐ flush + RDY only
    │   │   ├── SPI2 (D600)  HD Radio receiver  [IBOC baseband (part ?), ODM "hd_radio", hdradio.dll;
    │   │   │                 DSP firmware \Windows\hdrfirmware.bin = B000FF image, build "MZ1_2_0_23_1_17034"] ✗
    │   │   ├── SPI3 (D800) cs2  480x272 OLED display panel  [ODM "ffa_lcd0"; panel/driver IC ?] .......... ◐ init accepted and ignored
    │   │   └── SPI1 (D400)  [ODM "enc28j60" = Microchip ENC28J60 Ethernet: NVIDIA dev-board entry,
    │   │                     very likely not fitted on retail units] ........................................ ✗
    │   ├── PMC power management controller  [AP16 internal] (AVP resume vector at +0xA8)  0x7000E400 . ◐
    │   └── NvRm module 0x32  [AP16 internal, maybe fuse/KFUSE ?]  0x7000E800 (+0x30 cmd self-clears) ◐ ?
    │
    ├── AHB devices
    │   ├── USB device controller  [AP16 internal: ChipIdea-style HS USB controller]  0xC5000000  IRQ 20 ✅ zunehd-usb.c
    │   │   └── USB/IP server TCP 3240 -> zmtp.py / usbip-win2 (045e:063e, plain MTP) ................... ✅
    │   ├── Unknown block  [? NAND/AHB]                            0xC3000000 ........................ ◐ log stub
    │   └── SDMMC1-4  [AP16 internal, SDHCI-compatible]            0xC8000000+n*0x200  IRQ 14/15/19/31 ◐ no cards
    │       └── SDMMC1: Wi-Fi  [Atheros AR6002, SDIO; ODM "sdiowlan"; ar6k_ndis_sdio.dll,
    │                           firmware athwlan2_0.bin] ................................................... ✗
    │
    ├── Unmapped OAT windows  [?]: 0xB0000000, 0xB8000000 (2 MB each), 0x80000000 (4 MB) .............. ? log stubs
    │
    └── Board parts without a bus entry in the ODM table
        ├── Battery / charger status  [libzbatt.dll + battdrvr.dll; likely the WM8350 charger/ADC] ...... ✗
        ├── Headphone jack detect  [?] ...................................................................... ✗
        └── Chip-key AES for PKV unwrap  [AP16 internal, NvDdkAes] ......................................... ✗ MTPZ switched off instead
```

## Identified external chips

| Part | Function | Bus / address | How it was identified |
|---|---|---|---|
| NVIDIA Tegra APX 2600 (AP16) | SoC | - | HIDREV chip id 0x16 (firmware + device read) |
| Wolfson WM8350 | PMIC, audio codec, RTC, power button | DVC I2C 0x1A | ID register R0 = 0x6143; ODM GUID `wolf8350` |
| Avago capacitive touch controller (part number unknown) | touch screen **on the user's unit** | I2C2 0x33 | avago.dll hard-codes 8-bit address 0x66; avago.dll is loaded on the live device, synaptics.dll is not |
| Synaptics touch controller (RMI4; part number unknown) | touch screen, alternate supplier | I2C2 0x2C | synaptics.dll + RMI4 PDT (F01/F11) |
| Kionix KXSD9 | 3-axis accelerometer | I2C1 0x18 | driver protocol: reset key 0xCA -> reg 0x0A, CTRL_REGB 0x0D = 0xC0, CTRL_REGC 0x0C default 0xE3, ±2/4/6/8 g ranges |
| HD Radio (IBOC) receiver (part number unknown) | FM + HD Radio | SPI2 0x7000D600 | ODM GUID `hd_radio`; hdradio.dll loads \Windows\hdrfirmware.bin |
| Atheros AR6002 | Wi-Fi (SDIO) | SDMMC1 0xC8000000 | ODM GUID `sdiowlan`; ar6k_ndis_sdio.dll, athwlan2_0.bin |
| Phison PS8006 + Toshiba TH58NVG7D2ELA80 | eMMC managed NAND | HSMMC 0x70008500 | user-reported, not probed |
| Microchip ENC28J60 | SPI Ethernet | SPI1 | ODM entry only (NVIDIA dev-board leftover; probably not fitted) |

## Emulator differences found by the probe

- **Touch:** the user's Zune uses the **Avago** controller at I2C2 0x33. The emulator models the Synaptics
  part at 0x2C instead, and the "0x33 polled every ~5 s" seen in emulator logs is the Avago driver probing.
  Both drivers ship in the firmware, so the Synaptics model works, but it is not what this unit has.
- **Touch GPIOs:** the type-10 ODM table lists O7 and D7 for `tpktouch`, while the emulator raises the
  Synaptics ATTN on U0. The Synaptics driver evidently picks its own pins; worth checking if the Avago
  path is ever modelled.
- **U1** (previously "edge IRQ, purpose unknown") belongs to the HD Radio receiver.

## Still unidentified

- Exact part numbers: Avago touch controller, Synaptics touch controller, HD Radio baseband, OLED panel and
  its driver IC, SDRAM.
- Blocks at 0x6000C000, 0xC3000000, 0xB0000000, 0xB8000000, 0x80000000 (4 MB window), NvRm module 0x32.
- GPIO K5 and N7.

## How this was probed (2026-11-08, all read-only)

1. Kernel read of the `.data` section of `k.libnvodm_query.dll` (0xC09C6000, 6448 bytes) ->
   `device\probe\odm_query_data.bin`. It holds NVIDIA's ODM peripheral tables for 5 board variants
   (GUIDs stored as little-endian 8-char ASCII; address entries {module, instance, address}:
   module 6 GPIO, 12 I2C (8-bit addr), 13 I2C_PMU, 18 SDIO, 22 SPI, 27 PMU rail).
2. One `kcall` of `NvOdmPeripheralGetGuid` (pure table search) for `tpktouch`, `accelero`, `hd_radio`,
   `blutooth`: the first three resolve into the default table (+0xEE0, used for board type 0 and >= 9),
   and `blutooth` is absent. That confirms the type-10 table is the one this unit uses.
3. Module list of the live kernel: avago.dll, ztouch.dll, zaccelerometer.dll, hdradio.dll, ar6k_ndis_sdio.dll,
   libzbatt.dll, libnvodm_pmu.dll, zkeypad.dll loaded; synaptics.dll not loaded.
4. First 15.5 KB of `\Windows\hdrfirmware.bin` -> `device\probe\hdrfirmware_head.bin`.
5. Offline Ghidra decompilation of libnvodm_query (table selector), avago.dll (I2C address) and
   zaccelerometer.dll (register protocol).

No I2C/SPI transactions were sent to any chip. A direct ID-register read (e.g. KXSD9 reg 0x0C = 0xE3, or the
Avago controller's ID registers) would be the next step if part numbers need hardware confirmation.
