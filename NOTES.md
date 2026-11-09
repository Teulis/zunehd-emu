# Zune HD (Tegra APX 2600, WinCE 6) firmware RE notes

## Images (B000FF), parsed by scratchpad tools CeBin.cs / CeExtract.cs
| file | phys start | len | entry (phys) | ROMHDR VA | VA->PA |
|---|---|---|---|---|---|
| ZBoot.bin | 0x00000000 | 0xA2EE0 | 0x0003AB70 | 0x070A2CAC | physfirst 0x07000000 (runs at top of RAM, MMU off) |
| NK.bin | 0x00014000 | 0xBB2760 | 0x00023FB0 | 0x80BC51A0 | VA-0x80000000 |
| EXT.bin | 0x01790000 | 0x25C160C | â€” | 0x83D50188 | VA-0x80000000 |
| Recovery.bin | same layout as NK.bin, different SHA-256 | | | | |

- CPU type 0x01C2 (ARMV4I/Thumb-interworking); NK RAM 0x80BD0000..0x85000000.
- Flat images: `extracted\*.nb0`. Rebuilt PEs: `extracted\<img>\modules\`, ROM files: `extracted\<img>\files\`.
- Code sections uncompressed; many data sections are CE-ROM-compressed (zero-filled in rebuilt PEs, decompressor not written yet).
- Ghidra (via REA) needs PE machine patched 0x1C2 -> 0x1C4: `extracted\NK\raw\nk_armnt.exe` (decodes ARM mode correctly). PDB name: oal.pdb.

## OAL boot path (nk.exe, observed in Ghidra)
- `0x80023FB0` StartUp: clear SCTLR bits (MMU/caches off), inval I/D cache + DSB (0x8002a5a0), inval TLB (0x8002a594),
  RAM-size fixup (0x80024150 -> size hardcoded 128 MB, patches OAT[0].size), r0 = PA of OEMAddressTable (0x8004ea40),
  `b KernelStart` @ 0x80050a18.
- No MMIO touched before KernelStart.

## OEMAddressTable @ VA 0x8004EAD0 (identical copy in ZBoot @ 0x07071A00)
| cached VA | PA | MB | likely block (inferred from Tegra docs, unverified for AP16) |
|---|---|---|---|
| 0x80000000 | 0x00000000 | 128 | SDRAM |
| 0x9E000000 | 0x58000000 | 16 | GART aperture |
| 0x9F000000 | 0x54000000 | 4 | host1x clients (display, 2D/3D, VI, MPE) |
| 0x9F400000 | 0x40000000 | 1 | IRAM (shared with AVP) |
| 0x9F500000 | 0x50000000 | 1 | host1x / ARM perif |
| 0x9F600000 | 0x60000000 | 1 | PPSB: ICTLR 0x60004000, TMR 0x60005000, CAR 0x60006000, flow 0x60007000, GPIO 0x6000D000, vectors 0x6000F000 |
| 0x9F700000 | 0x70000000 | 1 | APB: pinmux, UARTs ~0x70006000, I2C, PMC ~0x7000E400, fuses |
| 0x9F800000 | 0xB0000000 | 2 | ? |
| 0x9FA00000 | 0xB8000000 | 2 | ? |
| 0x9FC00000 | 0xC5000000 | 1 | USB (inferred) |
| 0x9FD00000 | 0xC8000000 | 1 | SDMMC (inferred) |
| 0x9FE00000 | 0xC3000000 | 1 | ? (NAND/AHB?) |
| 0x9FF00000 | 0xFFF00000 | 1 | IROM / high vectors |
| 0x9DC00000 | 0x80000000 | 4 | ? |

## OAL internals (observed)
- OEMGLOBAL @ 0x80065DF0 (ver 0x00060000): InitDebugSerial 0x80053A18, InitPlatform/OEMInit 0x800241E0,
  WriteDebugByte 0x80053A9C(->0x8002BCF0), WriteDebugString 0x80053AAC, ReadDebugByte 0x80053A8C(->0x8002BC8C).
- Boot args @ PA 0x1000 (FUN_8004e1a8/8004dddc): 'BOOT'@+0x10, size 0x1FC @+0x14, 'ARGS'@+0x110, u16 1 @+0x114,
  u16 ver 0xD @+0x116. Missing => "Invalid or incompatible boot arguments" + hang.
  Debug cfg @+0x154: +0x08 type 1=none (default), 2=JTAG DCC (cp14), 3..7 = NvRm UART inst 0..4 @115200 (16550, stride 4).
  Other fields read by OEMInit: +0x1C, +0xB4, +0xD4, +0xD8 (16-byte device id), +0xE2, +0x188 (cold boot).
- FUN_8002bc24(us): delay via flow controller HALT_CPU_EVENTS (0x60007000) = 0x02000000|n, low byte counts down.
- NvRm: rm struct 0x80BE6B20. HIDREV (0x70000804) decode FUN_80039e20: chip[15:8] major[7:4] minor[19:16] fam[3:0].
  major!=0 => relocation table pointer from IROM @0xFFFF0040 (not available); major==0 => built-in tables for
  0x15/0x16/0x20 (FUN_80030b48). Use HIDREV=0x1600 ("SimulationChip Id: 0x16").
  FUN_80032644/80032680 = NvRm read/write reg (rm, module, inst, off). Module 0x32 @0x7000E800 reg 0x30: cmd low byte self-clears.
- ICTLR @0x60004000: 3 banks x 0x100 (FIR_CLR +0x1C, CPU_IER_CLR +0x28, CPU_IEP_CLASS +0x2C, COP_IER_CLR +0x38, COP_IEP_CLASS +0x3C).
- Needs CP15 c7,c8 VA->PA ops (QEMU: ARM_FEATURE_VAPA on arm1136).

## QEMU machine `zunehd` (C:\rea-work\qemu\hw\arm\zunehd.c, QEMU v11.1.3)
- Build: MSYS2 UCRT64, `bash -l /c/rea-work/qemu/zbuild.sh`. Run: `.\run-zunehd.ps1` (serial on console).
- Debug: tools\zcapture.ps1 (boot N s, log + regs), tools\zlog.py (summarize huge -d logs), tools\zstack.ps1, *.gdb.
- Debug helpers: tools\zbreak.ps1 -Addr X (gdb stop + symbolized stack), tools\zbreakm.ps1 -Addr "a,b,c" (multi-bp
  hit log), tools\zsym.py (addr -> module+off), zcapture.ps1 -DFilter "lo..hi" (with -DebugFlags exec,nochain to see
  the last code a module ran). ICTLR logs "IRQ n enable/disable" and reads while non-tick IRQs are pending.

### Fixes applied (each one unblocked the next crash/hang)
1. Boot args @0x1000 + debug UART (type 3) -> serial console works.
2. Initial SP (ZBoot normally sets it).            3. ARM_FEATURE_VAPA (CP15 c7,c8 VA->PA ops).
4. Flow-ctlr usec countdown (OAL delay).            5. HIDREV 0x00011600: AP16, major 0 (pre-silicon), minor 1 =
   QuickTurn path in libnvrm_impl FUN_c095ce84 (minor 0 = FPGA path NULL-derefs EMC DFS state; silicon path needs IROM).
6. Module 0x32 @0x7000E800 reg 0x30 self-clearing command byte.
7. APB_MISC_EMU_REVID (0x70000860) netlist=1: NvRmOpenNew only inits the DMA HAL if major!=0 || netlist!=0
   (else NvRmDmaAllocate calls NULL).
8. CAR PLL*_BASE 0x60006080..D0 pinned BYPASS: guest enables PLLs with M=0 -> divide by zero in rate calc
   (FUN_c0952bbc / FUN_c0975c30) -> STATUS_INTEGER_DIVIDE_BY_ZERO in audio mixer.
9. ICTLR (3 banks, VIRQ/ISR/FIR/IER/CLASS) -> CPU IRQ/FIQ; timers TMR1/2/3/4 @+0x00/+0x08/+0x50/+0x58 with
   IRQ 0/1/41/42. OAL system tick = TMR4 (IRQ 42), periodic 1 ms then variable one-shot. IRQs confirmed taken.

10. I2C: DVC @0x7000D000 (IRQ 53, regs at +0x40, DVC_STATUS[30] done W1C), I2C1 @0x7000C000 (IRQ 38), I2C2 @0x7000C400
    (IRQ 84; plain I2C, completion cleared by reading STATUS). Transfers complete instantly with ACK.
    NvRm I2C: polling FUN_c094dec8, ISRs FUN_c094de8c (DVC) / FUN_c094de10, xfer FUN_c094e104/e308/e5e8.
11. Slave 0x1A on DVC = Wolfson WM8350-like PMIC+codec+RTC (16-bit regs MSB first; R0 ID 0x6143; R16..R19 BCD RTC
    min:sec / hour:wday / month:date / century:year). Zero RTC -> OAL date conversion FUN_8004a49c loops (month 0) -> fault.
    (The earlier "AVP" diagnosis was wrong: the audio mixer was blocked in NvOdmAudioCodecOpen on I2C.)
12. ARB_SEMA @0x60002000 (GET +4 / PUT +8 / GNT_ST +0) + ICTLR ARBGNT_CPU_STATUS/ENABLE @+0x40/+0x44 -> IRQ 29;
    granted immediately (NvRmXpcInitArbSemaSystem waits for the grant interrupt).
13. Crypto/BSE engines @0x6001B000 / 0x6001D000: STATUS(+0x18)=0x8 idle (libnvddk_misc FUN_c08fc90c polls it).

IRQ numbers seen enabled (Tegra2 numbering fits): 4 SHR_SEM inbox (AVP), 13 I2S1, 20 USB, 26 APB DMA, 29 ARB GNT,
34 GPIO3, 36/37/46 UARTA/B/C, 38 I2C1, 42 TMR4 tick, 53 DVC, 65/67 host1x, 84 I2C2, 87 (?).
I2C devices: DVC 0x1A (WM8350), I2C1 0x18, I2C2 0x33 and 0x2C (touch/accel candidates).

### External references (wiki.zune.moe, project-lyra (MIT), CUB3D/zuneslayer) -- on-device findings, fw 4.5
- Real HIDREV = 0x00031617 (chip 0x16, major 1, minor 3, fam 7). We deliberately report 0x00011600 (pre-silicon path).
- eMMC 4.2 (EXT_CSD_REV 2), SEC_COUNT 0x03C60000 = 63,307,776 x 512 B; no boot/RPMB/GP partitions.
- Front 441,344 native blocks (215.5 MiB) hidden from CE: signed header x24 @0 (every 128 KiB), Tegra BCT @3.0 MiB,
  encrypted bootloader x2 @3.1/6.1 MiB, CE boot image (NK) x2 @9.1/76.8 MiB, CE "networking" image ~30 MiB @144.4 MiB.
- DSK1: (eMMCFlashDisk) = native block 441344 + 4*N, 2048-byte logical sectors, 15,716,608 sectors.
  MBR at DSK1 sector 0: one extended partition (type 0x05) LBA 16 count 15,716,592; logical partitions:
  Part00 zbinfs (type 0x21, vol ZBINFS, 65,544 sec, \Windows\), Part01 zexfat (0x07, "Flash", 65,544 sec, \Flash\),
  Part02 zexfat (0x07, "Flash2", 15,585,480 sec, \Flash2\). FSD chosen from partition record field, not type byte.
- libnvemmc I/O only via DSK_IOControl (DSK_Read stub). IOCTL_DISK_READ 0x75C08 / WRITE 0x79C0C / GETINFO 0x71C00.
- HSMMC (wiki calls it SDMMC; base handle+0x14, phys 0x70008500): command/control +0x04 (write issues cmd),
  cmd index +0x10 (6-bit), argument +0x28, response +0x2C; not SDHCI. NvDdkHsmmcSendCommand struct 32 B:
  +0 cmd index (0x12 = CMD18), +4 block addr, +0x10 logical sector.
- Display: 480x272 OLED, scanout surface 272x480 A8R8G8B8 carveout owned by compositor.exe.

### Storage (2026-11-07)
- Hardware (user): Phison PS8006 managed-NAND controller + 2x Toshiba TH58NVG7D2ELA80 (128 Gbit MLC each = 32 GB),
  appears to the host as MMC. User reports it shows up as ~2 devices (system + user) under "liberate".
- SDMMC1..4 @0xC8000000+n*0x200 are SDHCI (QEMU generic-sdhci, IRQ 14/15/19/31). SDMMC1 = AR6002 Wi-Fi slot
  (libnvsdio: CMD5 probe, then MMC CMD0/CMD1 fallback) -- do NOT put the eMMC there.
- eMMC driver: registry BuiltIn "eMMC" -> libnvemmc.dll (DSK, profile eMMCFlashDisk). DSK_Init gates on OAL IOCTLs
  0x01013E97 (=bootargs+0x1DC, must be 4), 0x01013EA3 (+0x1E0 flag), 0x01013EAF (+0x1EC..+0x1F8 =
  {devtype 0x11, instance, minor, reserved}) then NvDdkBlockDevMgrDeviceOpen -> NvDdkHsmmcBlockDevOpen ->
  NvDdkHsmmcOpen. Per-instance mutexes exist only for instance < NvRm HSMMC instance count -> instance 0.
- HSMMC is NOT SDHCI: NVIDIA HSMMC block @0x70008500 (APB), IRQ 22 (enabled by NvDdkHsmmcOpen right before CMD0).
  Decoded from libnvddk_misc (SendCommand 0xc08e47b4, ISR 0xc08e4d44 (registered via literal 0xc08e5c60),
  data setup 0xc08e4bf0, PreIssueStop 0xc08e4ae0, GetCommandResponse 0xc08e43b4, card init FUN_c08f14b8):
  +00 CTRL (driver writes 1)   +04 STATUS: [5:0] errors, 0x800, 0x1000 write done, 0x2000 cmd done, 0x4000 read
  done, 0x8000 busy end, 0x10000/0x20000; all W1C; [30] xfer active; [31] driver sets before data phase.
  +08 clk  +10 CMD flags: [1:0] resp (0 none,1 R1,2 R2,3 R3), [2] data, [3] WRITE, [5] multi, [7] busy (R1b),
  [8] CMD0, [9] HOLD, [11:10] bus width (read back by Suspend), [13] always. Write with HOLD clear = issue.
  +14 (0x7f) +18 (0xffff) timeouts  +1C blksz  +20 blkcnt  +24 int enable (0x3f/0x7b/0x7d/0x77)
  +28 cmd index  +2C arg  +34 response FIFO: raw bit stream from the start bit, MSB first
  (R1: raw[47:16], raw[15:0]<<16; R2: raw[135:8] in 4 words)  +40 DMA len (bytes-1 | 0x80000000 = go)
  +44 DMA phys addr (64 KiB bounce buffers in carveout, 0x06070000).
  Sequence: cmd -> 0x2000 -> driver arms +40 -> 0x4000 (read) / 0x1000 (write). For CMD18 the ISR issues CMD12
  on read-done; for CMD25 the driver pre-issues CMD12 before arming DMA (model defers it until data written).
  Card init: CMD0, CMD1 0x40FF8080 (poll bit31, bit30 = sector mode), CMD2, CMD3 RCA 1, CMD9, CMD7, CMD13,
  CMD16 512, CMD6 0x03B70200 (8-bit) / 0x03B90100 (HS), CMD8 EXT_CSD (SEC_COUNT @0xD4, 0xAF, 0xB9, 0xDD, 0xE0).
  QEMU TYPE_EMMC works as the card (only needs a 512-multiple size).
- libnvemmc DSK1 mapping: DSK_Init reads OAL IOCTL 0x01013EAF 16-byte record {0x11, inst, minor, base}; base ->
  dev+0x18; FUN_c08c14b0: blockdev sector = sector*scale + base, blockdev sector = 2048 B (FUN_c08f21d0: 512<<2).
  base must be 0x1AF00 (= 441344/4); gemstone.exe also contains 0x1AF00. Read IOCTL handler FUN_c08c188c.
- Image: C:\rea-work\zunehd-emmc16.img, built by tools\mkemmc.py (layout in CHECKPOINT.md). Machine prop
  emmc-instance stays 0.
- Device facts (user's unit, read via Lyra, 2026-11-07): see CHECKPOINT.md "Storage". ZBINFS = NK.nb0 @0 +
  EXT.nb0 @0xBC0000 (B000FF records laid out flat, NK first, EXT 64 KiB aligned after it).

### Current state / open issues (2026-11-07)
- Audio: STATUS_INTEGER_DIVIDE_BY_ZERO (caught) in libnvrm_impl FUN_c0954abc: PLLA output rate 0 because its
  reference (PLLP_OUTx dividers, CAR 0x600060a4..) is stubbed. Needs a small CAR clock model.
- libnvsdio.dll: data abort NULL+4 in libnvddk_misc+0x6de0 (SDIO host); SDMMC @0xC8000000 (Tegra SDMMC is SDHCI-
  compatible) not modelled -> driver unloads with live threads (prefetch aborts at freed code).
- With the eMMC attached (session 2): storage mounts, files load from ZBINFS, serial ends with
  "ResetVector = 60abf30" (AVP boot) and the system idles polling 0x7000E450/0x7000E008 - next: AVP handshake.
- RTC write path: "OALIoCtlHalInitRTC: NvBlSetPmuRtcSeconds Failed" (WM8350 RTC regs are read-only in the model).

### Graphics pipeline (session 3) - see CHECKPOINT.md "Session 3" for the register maps
- Boot chain to display: devmgr BuiltIn drivers -> zdisplay.dll (DSP_Init, thin) ; GWES (gwes.dll) ->
  libnvddi.dll DrvEnableDriver -> libnvddk_disp NvDdkDispOpen -> DC programmed directly by CPU; panel init
  via SPI 0x7000D800; flips + 2D/3D through host1x channels 0 (display), 1 (2D G2 + 3D + host), 2 (SB blit).
- Per frame: G2 fill 0x062CB000 (black, alpha fade) -> 3D renders UI into it -> SB copy to front buffer.
- Blockers found and fixed this session: AVP RPC (RPC_AVP_PORT connect, infinite) from audio driver;
  audio mixer threads holding NvRm power mutex -> eMMC resume deadlock (fixed by disabling WAV_Init);
  exFAT unformatted; SPI flush bits; host1x sync point waits.
- AVP: kernel nvrm_avp.axf + codecs (*.axf) are in the ROM images (CE-compressed, not yet decompressed).

### 3D GPU (session 4) - AP16 3D = Tegra 2 "AR20", class 0x60; interpreter in qemu\hw\arm\zunehd-gr3d.c
- Compositor boot animation: per frame G2 fill back buffer 0x062CB000, 3D draws, SB copy to front buffer.
  2 programs only: VP 5 insns (pixel -> clip), FP 4 batches (PSEQ load RT1, MFU texcoord, TEX, ALU
  premultiplied "over" blend with uniforms u0..u3 = fp20 1.0, DW store RT1).
- Deviations from grate: VP disassembler wrong (doc layout right); desc1 bit4 = swizzled 16x16 blocks;
  DW lags ALU by one batch; linear texture pitch align 32; viewport in 1/16 px.
- User: rendered bar gradient is upside down vs device -> suspect bottom-up texture storage (fix first).

## QEMU direction
- No Tegra APX 2500/2600 machine in QEMU; need a custom machine: arm1136 CPU, 128 MB RAM @0, load NK.nb0 @PA 0x14000
  (EXT.nb0 @PA 0x01790000), PC=0x23FB0, MMU off. Start with unimplemented-device stubs over every OAT region + logging,
  then model ICTLR, TMR/usec counter, UART, CAR as the OAL demands.

### Session 5: host1x sync-point IRQs, indirect access, 3D context save; touch controller
- Textures are stored bottom-up: zunehd-gr3d.c tex_addr() uses row h-1-y.
- Boot hang after the bar was a 3D CONTEXT SWITCH (zsplash -> compositor): channel stream INCR 3D syncpt (20),
  then host class INDOFF(0x2D)/INDDATA(0x2E) reads of all 3D regs (read-mode INDOFF = [27] autoinc,
  [25:18] module 6=3D, [17:2] reg, [0]=1 read; the stream still carries one dummy word 0x77/0x88 per read),
  then WAIT_SYNCPT(20, n+1). NvRm save service (libnvrm_impl FUN_c0967c6c) runs on the syncpt threshold IRQ,
  drains channel output FIFO (FIFOSTAT[28:24] OUTFENTRIES, channel +0x0C INDDATA), then CPU-increments the
  3D syncpt via ch*0x4000+0x3700 (sync block is mirrored in each 0x4000 channel window).
- Syncpt threshold IRQ = ICTLR 65. ISR FUN_c0968a30: status +0x3040 (W1C), enables +0x3050/+0x3054 with
  2 bits per syncpt (bit 2i = CPU0), thresholds +0x3500+4i (16 bit, re-armed by FUN_c09688a8).
  All modelled in zunehd.c now (host1x_syncpt_update, host1x_inddata, output FIFO).
- Host class methods seen: 0x008 WAIT_SYNCPT, 0x009 WAIT_SYNCPT_BASE, 0x00C INCR_SYNCPT_BASE (executed
  immediately / ignored), 0x2D/0x2E indirect. EXTEND opcode 14 still ignored.
- compclient.dll (EGL client): command ring in shared memory (header +4 get, +8 put, +0xC completed fence);
  FUN_41bb11b0 sleeps while the ring is full, FUN_41bba980 waits for the fence.
- Process list: walk PROCESS +0 from NK PROCESS 0x80bee328 (names UTF-16 at *(+0x20)).
- HKLM\Init (from RAM): Launch20 device.dll, 30 gwes.dll, 60 servicesStart.exe, 58 zuncab.exe, 93 zsplash.exe,
  94 compositor.exe. gemstone.exe is launched by ZAM (zam_serv.dll, Services\ZAM DefaultApp).
### Touch (session 5) - Synaptics RMI4 over I2C (synaptics.dll, loaded by ZTouch.dll)
- I2C2 (0x7000C400) 7-bit addr 0x2C (driver holds 8-bit 0x58). NvRm does register write (1 byte) then a
  separate read (<= 8 bytes per transfer; longer reads are split in 8-byte chunks at increasing addresses).
- Init (FUN_c0b01aac): write page 0 to reg 0xFF; scan PDT from 0xEE down in steps of 6 (entry bytes read top-down:
  A=fn#, A-1=int count, A-2=data base, A-3=ctrl base, A-4=cmd base, A-5=query base); needs F01 and F11
  (fn# 0 ends scan). Then F01 cmd <- 1 (reset), sleep, F01 ctrl <- 0x80, read F01 data+1 (int status).
- Report (FUN_c0b01828): read 11 bytes at F11 data base: [0] finger status ([1:0]==1 = finger 0 present),
  [1] X[11:4], [2] Y[11:4], [3] Y[3:0]<<4 | X[3:0], ... Valid only if X!=0 and Y!=0. Raw range X 0..1764,
  Y 0..3136 (scaled <<14 / max); finger status != 1 => pen up.
- ATTN: GPIO interrupt via NvRmGpioInterruptRegister; ODM peripheral DB (libnvodm_query, type 6 = GPIO entries).
- Other I2C: I2C1 0x18 (accelerometer, 6-byte reads), I2C2 0x33 (polled reg 0x8E every ~5 s).
### Session 5b: GPIO, touch, exFAT mounting, UI text
- GPIO @0x6000D000 modelled (zunehd.c): 7 banks x 4 ports, regs CNF/OE/OUT/IN/INT_STA/INT_ENB/INT_LVL/INT_CLR,
  masked copies at +0x800 (val[15:8] mask), bank IRQs 32,33,34,35,55,87,89. Inputs must read 0 by default
  (all-high hangs the OAL before the kernel banner).
- Touch ATTN = GPIO port U (bank 5 port 0) pin 0, level, active low (synaptics.dll registers it via NvRm; the
  gdb read of FUN_c0b01c80 args giving "port 0 pin 2" was wrong). Touch model registers: PDT F01 @0xEE
  (data 0x00, ctrl 0x30, cmd 0x40, query 0x50), F11 @0xE8 (data 0x02, ctrl 0x31, cmd 0x41, query 0x60).
- Services: servicesd loads services in Order; zconfig_serv ZCO_Init waits 30 s (FUN_418f2dd0) for FS mount
  notifications named by HKLM Software\Microsoft\Zune\Directories OsPartition="Flash", DataPartition="Flash2";
  on timeout returns 0x800705B4 and the remaining services (incl. ZAM -> gemstone.exe) never start.
- zexfat only mounts our volumes with the device-identical root dir: 0x81 bitmap, 0x82 standard up-case
  (2474 B, chk 0x8FA6CA0D, device\dsk1_65742_flash_upcase.bin), 0x83 empty label. Device root dirs saved in
  device\dsk1_65737_flash_root.bin (\Flash: zconfig.dat, zver.dat, FWUpdate_LastError.dat, userimg.bmp) and
  device\dsk1_133040_flash2_root.bin (\Flash2: devcert.dat, drmstore.dat, zunedb.dat, ... ).
- 3D TEXTURE_DESC2: bit 0x40 NOT_POW2 -> width [31:20], height [19:8]; else WIDTH_LOG2 [31:28], HEIGHT_LOG2
  [27:24] (grate tgr_3d.xml.h). UI glyph atlases: A8 (fmt 1) 128x128 @0x066C2000, 256x256 @0x066C8000, tiled.
- Tiled layout 1 bpp (64x16 texel tiles, 1 KiB): [0]x0 [1]x1 [2]y0 [3]y1 [4]x2 [5]x3 [6]y2 [7]y3 [8]x4 [9]x5
  (found by exhaustive smoothness search, scratch tool swz.c). 4 bpp unchanged. 2 bpp unverified.
- UI program: VP 11 insns, FP pseq 5 / ALU 40 / TEX 5 words; text drawn as 6-vertex glyph quads into offscreen
  RTs (0x06642000, 0x065C2000) that the compositor composes.
- FP ALU word bit 60 = "accumulate this" (recv): rC := result of the previous ALU in the same instruction
  (ALU1<-ALU0 ... ALU0<-ALU3 of the last instruction); bit 61 = "accumulate other" (send), not modelled.
  The UI text blend needs recv for green (r0.h = (1-a)*dst.g + a*u1): without it white text came out magenta.
  Now implemented in zunehd-gr3d.c alu_packet(); menu text is white, sub-items grey (matches device per user).
- Touch verified (user, SDL): input works; panel Y axis is bottom-up (raw y 0 = bottom) -> flipped in touch_report.
  Touch handler also accepts relative motion (monitor mouse_move) for tools\ztap.ps1 (boot + tap/drag + screenshots).
  Tapping "music" opens Music > Albums; its yellow message text renders garbled (next GPU format/tiling to check).
- ALU "send" (bit 61, 'other') implemented: senders evaluated first, receiver's rC := sum (ALU1/2 -> ALU0,
  ALU3 -> ALU2). Used by a 48-step blur/glow program on 272x64 strips (Music page). Did not change the output.
- OPEN (yellow blocks on Music/Settings pages): clipped-text program (first at draw 651 in a tap-music trace):
  PSEQ 0x0082000A loads rt2 (unset: base 0, fmt 3 - stencil/clip surface?) -> r2/r3; ALU tests -r4..-r7 >= 0
  into cr4/cr5 and writes kill; r4..r7 are never written by the program (hardware-preloaded? frag position /
  clip rect); DW list has several entries (store stencil, 0x00010001, 0x0002008D, store rt1). Our output is
  out-of-range garbage (1.53, -1.80) -> solid yellow glyph boxes. Atlases (0x06400000 512^2, 0x063BC000 256^2,
  0x063D2000 128^2, A8 tiled) are correct.
- FIXED clipped text (Music/Settings pages; was yellow blocks), four interpreter changes in zunehd-gr3d.c:
  1. Pipeline timing: step i = MFU[i], TEX[i]; step i+1 = PSEQ[i] load + ALU[i]; step i+2 = DW[i].
     (A batch's ALU sees the next batch's MFU/TEX; keeps the earlier "DW one batch behind ALU".)
  2. Register groups: the k-th MFU instruction writes var0..3 to r(4k)..r(4k+3); the k-th PSEQ load writes
     r(4k)/r(4k+1) (+2 with bit 1). Clipped-text program: MFU0 texcoords (TRAM1) -> r0/r1, MFU1 clip
     distances (TRAM0, 4 values) -> r4..r7; PSEQ0 stencil rt2 -> r2/r3 (unset, no-op), PSEQ1 colour -> r4/r5.
  3. ALU operand register 31 (lowp vec2(0,1)) = #0/#1 by sub-register select only, regardless of the fixed10
     flag (fp20 decoding gave #0 = 2.0 and made every clip test fire).
  4. Condition registers store fx10 0/1 in the low half (high half with "write high"); grate names
     cr(2*(reg-64)+half).
  Verified: boot bar + cross-fade, main menu, Music > Albums ("ENJOY UNLIMITED ACCESS TO MUSIC WITH ZUNE
  PASS", white), Settings list (white). Menu now appears ~21 s into boot.
### Session 5c: board type, keypad buttons
- Board type = OAL IOCTL 0x01013EBB (handler 0x80025D38 -> FUN_8004b7c8), decoded from GPIO straps at boot:
  U7 sampled with pull-up (high -> table below), then FUN_8004b6b8 reads 12 strap pins (bit i = pin i high):
  T4 D5 L0 L1 L2 L3 L4 L5 L6 L7 T2 T3. With U7 high: 0->2, 1->7, else 0 ... ; real device (Lyra kernel read
  0x80BE8CA0 = strap 0x802, 0x80BE8CA4 = type 10). Emulation now drives U7, D5, T3 high -> type 10.
  Type 0 traps in OAL at 0x8004a0ec (b .). All GPIO inputs high also hangs the OAL (key-held style check).
- ODM peripheral tables (k.libnvodm_query, .data compressed in ROM; read live via gdb): type 1-2 = NVIDIA dev
  board (keyboard, bluetooth, gps; NO zkeypad0) - that is why zkeypad failed before (GetGuid NULL);
  types 3, 4-6/8, 0/>=9 tables contain zkeypad0.
- zkeypad0 pins (type 10): index 0 = GPIO S5 -> VK 0x24 (Home), index 1 = GPIO S6 -> VK 0x74 (media overlay:
  volume, prev/play/next). Active low, both-edge IRQ. Host keys: Home/H = home, M = media.
- GPIO model fix: level-triggered INT_STA latches only for enabled pins (else unconfigured level-low pins
  read pending and the kernel GPIO ISR masked whole ports).
- Other both-edge GPIO IRQ pins (not keypad): U1, K5, N7 (N7 polled by some driver; meaning unknown).
- Power button: not on zkeypad; to find (likely PMU WM8350 ONKEY or another GPIO driver).
- tools\zseq.ps1: boot headless + timed "tap X Y", "key K [ms]", "shot NAME" steps (one ;-separated string).
- Power button: NOT a plain GPIO (U1/K5/N7 presses have no effect). libnvodm_pmu (WM8350) handles it:
  NvOdmPmuInterruptHandler -> FUN_c09b385c: R0x18 & 0x40 -> R0x1E & 0x08 -> level = R0xE6 bit 3 (debounced,
  FUN_c09b3614) -> FUN_c09b34e4 -> PowerPolicyNotify. NvRm PMU thread FUN_c0961cfc waits on a semaphore; the PMU
  interrupt (logical 0x53, registered in FUN_c096252c only if the ODM PMU property says IRQ connected) is NOT
  registered in our boot within 60 s -> next: find why (FUN_c095b420 / ODM PMU property), model WM8350 IRQ
  status regs 0x18/0x1E and level 0xE6, raise the PMU interrupt line.
- POWER BUTTON WORKS (host keys End / P): WM8350 ON-key model in the DVC/PMIC device: R0xE6 bit 3 = level
  (1 = down), R0x1E bit 3 = key interrupt (read-to-clear), R0x18 bit 0x40 = summary; IRQ = Tegra external
  PMU interrupt 86 (logical 0x53). Needed 3 image patches in libnvrm_impl FUN_c096252c, because NvRm only
  registers the PMU interrupt on silicon (chip mode FUN_c095b420: 1 silicon, 2 QuickTurn = ours, 3 FPGA):
    PA 0x0072C55C  bne -> nop        (register the PMU IRQ + thread in emulation mode too)
    PA 0x0072C568  strb [pmu+0x20],1 -> nop  (keep "PMU present" clear: else rail/voltage calls go to our
                   minimal WM8350 and the display never powers up -> black screen)
    PA 0x0072C634  bl FUN_c0956e78 -> nop    (else all 10 rails marked unsupported 0xFFFFFFFF)
  Env ZUNEHD_NOPMUIRQ=1 skips these three (debug). Verified: press -> display off, press -> back to menu.
- NvRmI2cTransaction falls back to bit-banged I2C (FUN_c096331c) on I2C1's pins C4 (SCL) / C5 (SDA) for
  transfers the controller can't do; GPIO model now has pull-ups there and a tiny bit-bang I2C target
  (ACK all, reads 0x00) so those transfers finish instead of timing out per clock.
### Session 5d: USB device controller (in progress)
- Stack: libnvusbf.dll (CE UFN PDD/MDD, BuiltIn USBFN, client MtpClientDrvUsb) -> libnvddk_misc NvDdkUsbf*
  (Open 0xC08ECABC, Start 0xC08EC448, GetEvents 0xC08EC5CC, controller start FUN_c08ee0fc, ISR FUN_c08edac0)
  -> ChipIdea-style controller @0xC5000000 (regs as in zunehd-usb.c header), IRQ 20. Queue heads at regs+0x1000
  (PA 0xC5001000, controller-local memory); 64 KiB DMA buffer area (+0x3A8 = 0x10000).
- NvDdkUsbfOpen failures fixed: (1) NvRmAnalogInterfaceControl(USB, mode 3) -> FUN_c09557ec only knows
  13/19.2/12/26 MHz oscillators, QuickTurn reports 8330 kHz -> patch PA 0x0071F824 (use 13 MHz table);
  (2) mode 2 waits for APB_MISC+0x28 bit 7 (PHY clock) -> forced set in the apb stub.
- NvRm module 0x38 = APB_MISC (0x70000000): USB PHY control regs 0xA04..0xA34, VBUS wake-up 0x70 (bit 16 enable,
  17 clear, 18 status) for USB1, 0x74 for USB2.
- Chip-mode quirks: we run as QuickTurn (mode 2): oscillator 0x208A (8330 kHz) for chips 0x15/0x16.
### Session 6: USB plug -> resume -> enumeration WORKS (zunehd-usb.c stage B)
- Suspend/resume protocol (libnvddk_misc + libnvrm_impl FUN_c0955db8 = NvRmAnalogInterfaceControl backend):
  * APB_MISC+0x70 (USB1): [16] VBUS wake-up enable, [17] write-1 clears [18], [18] latched wake status,
    [2] "PHY suspend done" - polled 100000x by FUN_c0955a14 after setting 0xA30 bit0 (was a long timeout!).
  * APB_MISC+0xA30 bit0 = USB1 PHY suspended (FUN_c0955a14 power-down sets, power-up clears).
  * NvDdkUsbfSuspend: +0x3E4 = 1, PHY down (0xA30|=1, 0x70 EN). ISR FUN_c08edac0 in suspended state only
    calls mode 0 (reads 0x70[18] into +0x3A0 "cable", clears [16]/[18]) and signals the IST (+0x3AC).
  * PDD IST (libnvusbf FUN_c0881b78): GetEvents bit 0x10 (cable, from +0x3A0) -> NvDdkUsbfResume (PHY up,
    controller re-init FUN_c08ee070: RST, USBMODE 2, RS=1).
  * MODEL: controller IRQ masked while 0xA30 bit0 (no clock); plug latches 0x70[18] if [16]; attach
    (BSVIS+PCI+SLI, then 20 ms later URI+PCI) is delivered only when RS=1 and PHY up, and again on every
    RS 0->1 edge. ISR counts URI as reset (event 0x40) only after a suspend (SLI) -> +0x75.
- Endpoint engine: dQH list at ENDPOINTLISTADDR (0xC5001000 = controller-local RAM; model maps RAM over
  the rest of the 1 MiB aperture). On prime, load first dTD into dQH+4 and overlay (+8..+0x24):
  NvDdkUsbfEndpointStatus (FUN_c08ed78c) walks its dTD pool ([+0x7C] + idx*0x80, 0x20 each) until it
  matches dQH+4 - with +4 = 0 it ran off the pool -> Data Abort at libnvddk_misc+0xd8c0.
  API for host models: zune_usb_dev_setup / dev_out / dev_in / dev_ready (zunehd-usb.h).
- Env: ZUNEHD_USB_PLUG_AT=<virt s> auto-plug; ZUNEHD_USB_SELFTEST=1 built-in test host; ZUNEHD_USBLOG=1.
- Enumeration result (self-test): device 12 01 00 02 ff 00 00 40 5e 04 3e 06 00 01 01 02 03 01
  (045e:063e, bcdDevice 1.00, class ff, ep0 64); config 39 bytes: 1 interface ff/ff/ff, ep 0x81 bulk IN 512,
  0x01 bulk OUT 512, 0x82 intr IN 64 (interval 4); strings "Microsoft Corporation", "Zune";
  string 0xEE = "MSFT100" vendor code 0xFE. SET_CONFIGURATION ok, ENDPTCTRL1 = 0x00C80000 (TX bulk).
- OPEN: after SET_CONFIGURATION nobody primes ep1 OUT (MTP GetDeviceInfo not consumed). ROOT CAUSE FOUND:
  * MtpClientDrvUsb.dll (NK 0xC0C50000, stream driver MUT_*): UFN notify FUN_c0c53848; on CONFIGURED opens
    3 pipes and SetEvent(named "MtpClientDrvUsb_Attach", +0x220). Works in emulation.
  * MtpSvc.dll (EXT 0x416C0000, in servicesd): thread FUN_416cb510 = init FUN_416cb71c, then event loop
    FUN_416cccdc (WFMO on +0x5C Attach, +0x60 Detach, ...); on attach it opens MUT1: (FUN_416cd484).
    Init FAILS with E_FAIL -> thread exits -> nobody listens. Failing step: object FUN_416d26b0 ->
    Initialize FUN_416d2f74 -> vtbl call (FUN_416ffcb0) does CreateFile("KEY1:") + DeviceIoControl
    0x220018 (out 0x31C bytes) -> KEY1: does not exist (INVALID_HANDLE) -> size 0 -> E_INVALIDARG.
  * KEY1: = keyvault.dll (NK 0xC0B30000). KEY_Init -> FUN_c0b36a24 fails at check 2 FUN_c0b368e8:
    reads the provisioned key vault "PKV" via FUN_c0b359c0 (store opened by FUN_c0b43520; registry
    "Drivers\BuiltIn\KeyVault" / "VProvision", "FuseBurn"; also \Windows\ZuneKeyPack_NonProd_2009.dat,
    DWX1:, NFS1:) -> device-unique secrets, absent in our image.
  USER CHOSE: read it from the device. Progress:
  * PKV lives on an NVIDIA partition: libnvfsstream.dll (NFS1:, NK 0xC0A60000) NFS_Open asks OAL IOCTLs
    0x01013E97 (boot-args +0x1DC) and 0x01013EAB (= +0x1E4/+0x1E8 {PT start, count} in 2 KiB sectors),
    scans for the PT header (0x8F9E8D8B, -1, 0x100), names "XXX/file" resolve via the PT entries.
  * Device boot args (Lyra kernel reads @0x80001000): +0x1DC 4, +0x1E0 1, +0x1E4 0x600, +0x1E8 0x40,
    +0x1EC 0x11, +0x1F0 0, +0x1F4 0x0E (minor; we had 0), +0x1F8 0x1AF00. zunehd.c now uses all of them.
  * NVIDIA PT (device\front_6144_16k.bin, native 6144; entries 0x40 B: id, name, start u64 @+0x28,
    size u64 @+0x30, type @+0x38; units 2 KiB): BCT 0/0x600, PT 0x600/0x40, BTA 0x640/0x600,
    BTB 0xC40/0x600, ZRA 0x1240/0x8740, ZRB 0x9980/0x8740, MTE 0x120C0/0x8740, ARG 0x1A800/0x40,
    ZAD 0x1A840/0x40, PKV 0x1A880/0x40, NKV 0x1A8C0/0x40, BLU 0x1A900/0x600, COP 0x1AF00/0x771100 (=DSK1).
  * PKV read (device\front_PKV_434688.bin, native 434688, 128 KiB, 3 non-zero 2 KiB sectors). Both files
    are written by mkemmc.py (FRONT_FILES). With them KEY_Init's 4 checks pass and KEY1: opens.
  * Remaining: KEY_IOControl returns ERROR_NOT_READY (0x15) because DAT_c0b4e154 stays 0:
    FUN_c0b38f50 AES-decrypts the 64-byte PKV header with the chip key (FUN_c0b35b48 = NvDdkAes open,
    key type 3 len 16, decrypt) -> plain[0x10:0x30] = SHA-256 digest of plain[0x30:0x40] (16-byte key)
    || vault[0x40:size] || tag ("PKV" string ptr @0xc0b3901c), compared by FUN_c0b44108.
    PLAN: get the 64-byte plaintext from the device by kcall'ing its own FUN_c0b35b48 (script
    tools\pkv_unwrap.py; device must be awake), verify the digest offline, then in emulation store
    the plaintext header in PKV and patch the `bl FUN_c0b3b2f4` at 0xc0b38f78 into a 64-byte copy.
- Extra detail (session 6):
  * NvDdkUsbfGetEvents (0xC08EC5CC) bits: 0x10 cable/connected, 0x40 reset (+0x11), 0x20 setup (+0x12),
    0x80 suspend (+0x74), 0x400 FS / 0x800 HS (+0x39C = PORTSC PSPD), per-endpoint bits from +0x33.. .
    libnvusbf PDD IST FUN_c0881b78 calls GetEvents via IAT 0xC088F118; Resume/Suspend via 0xC088F158/F154.
  * UFN client notify (MtpClientDrvUsb FUN_c0c53848): msg 1 bus event (r2: 1 detach -> Detach event,
    2 attach, 3 ...), 2 CONFIGURED (opens 3 pipes via fn +0x9D4 with the endpoint bytes at +0x8DE/+0x8F2/+0x906,
    SetEvent +0x220), 3 speed,
    4 setup (0x80 std-in / 0xC0 vendor-in / class), 5 preprocessed setup.
  * MtpSvc event loop FUN_416cccdc: WFMO index 0 quit, 1 FUN_416cc250, 2..11 handlers; attach handler opens
    MUT1: in FUN_416cd484 (CreateFile + event +0x298).
  * keyvault.dll map: KEY_Init 0xC0B36B74 -> FUN_c0b36a24 checks c35c78 (selects PKV/NKV fn ptrs by
    DAT_c0b4e148: 1 = PKV), c368e8 (size query + read of PKV via NFS1:), c3696c, c3664c; then
    DAT_c0b4e154 = (c38f50 PKV header verify) || (c36560 NKV path, needs 0x2000-byte record).
    NFS access: FUN_c0b43520 = CreateFile("NFS1:") + IOCTL 0x64 (open name, 0x104-byte in); read via 0x66.
    KEY_IOControl 0xC0B3AA88: codes 0x22000C..0x2200B0; 0x220018 -> FUN_c0b3a444 (MtpSvc asks 0x31C bytes).
    Imports: NvDdkAes* via FUN_c0b437f8 (open), CryptDecrypt/CryptImportKey, KernelIoControl, CeGenRandom.
  * libnvfsstream NFS_IOControl 0xC0A61498: 0x64 open, 0x65 close, 0x66 read, 0x67 seek, 0x68 size,
    0x69..0x6B by-name ops, 0x6C/0x6D/0x6E misc. Error 0x6E = ERROR_OPEN_FAILED (what KeyVault saw with no PT).
  * The SHA-256 / tag interpretation of FUN_c0b38f50 is inferred (32-byte digest, d890/e740/e840 =
    init/update/final) - confirm when verifying pkv_hdr_plain.bin.
- Debug techniques that worked (session 6):
  * NvRegw (libnvrm 0xC0962B84) keeps its caller's return address at [sp+12] at the store; the USB model's
    apb70 log reads it with cpu_memory_rw_debug to name the caller.
  * zbreakm's hook prints only r0..r3 + [sp..sp+8]: to see a value in r4+, break one instruction after it is
    moved into r0..r3. gdb slows the guest a lot: auto-plug earlier (ZUNEHD_USB_PLUG_AT=20) under gdb.
  * Front-region reads on the device: ext\project-lyra\tools\re\read-emmc-front.py HOST --block N --count C
    (C x 2 KiB) --chunk C --out F (temporary SendCommand hook, restored on exit).
- OPEN (race): once, a SETUP arriving in the same ISR as the previous status-stage ep0 OUT completion left
  the driver polling ENDPTPRIME/ENDPTSTATUS (pcs c08ed61c/644/848/858, c08ee750/770/77c). Self-test host
  waits only 10 ms between requests; a real host is similar - investigate if it recurs.
### Session 7: PC-side MTP/MTPZ reference
- https://github.com/kbhomes/libmtp-zune (LGPL-2.1, "Adding Zune support to libmtp"). Key files: mtpz.md
  (handshake write-up), src/mtpz.c + src/mtpz.h (implementation), src/music-players.h (device table),
  src/.mtpz-data (host cert + RSA key blob - do not fetch or commit).
- MTPZ handshake per mtpz.md: host decrypts its 640-byte cert block + 704-byte RSA key block (fixed AES-128
  key + custom InvMixColumns); sends Application Certificate msg (marker 02 01: 2 certs, 16 random bytes,
  RSA/SHA-1 signature); device answers 02 02 (obfuscated key, encrypted response, certs "Zune Device CA 1" +
  "Zune Device", own 16 random bytes + host's echoed, 128-byte RSA signature, 32-byte CBC-MAC/SHA-256);
  host confirms 02 03 (16 bytes derived from that hash).
- Device side therefore needs the device cert chain + device RSA private key = key-vault contents (KEY1:).
  Keyvault analysis vs this handshake (static only; full report was in the session-7 scratchpad
  keyvault-mtpz-report.md):
  * MtpSvc Initialize FUN_416d2f74 ALWAYS does the KEY1: cert fetch (0x416d3064..80, [+0x634] vtbl[3] ->
    FUN_416ffcb0 -> IOCTL 0x220018); failure -> `bpl` @0x416d3088 falls to E_FAIL. Not gated by MTPZ. (verified)
  * MTPZ is optional: REG_DWORD HKLM\Software\Microsoft\Zune\MTP\DeviceInfo "MTPZ" -> svc+0x28 -> dev+0x2dc
    (default 0 if absent). 0: OpenSession mounts storage directly (FUN_416d4038), write gate FUN_416d3be0 open.
    1: storage only after MTPZ (op 0x9212 handler FUN_416e7d38), writes -> 0x200F. Retail value = 1 (inferred;
    registry is compressed in NK.nb0, not read).
  * IOCTL 0x220018 (FUN_c0b3a444): 0x31C = caller buffer size; copies len=*(vault+0xC0) (0x26A in our PKV)
    from vault+0xC4 = cert chain "Zune Device CA 1" (+0xE6) / "Zune Device" (+0x21D), stored plaintext.
    Gated by DAT_c0b4e154: every IOCTL < 0x220055 returns 0x15 until the header verifies. (verified)
  * FUN_c0b3b2f4 (chip-key AES helper) has 7 callers, incl. IOCTL 0x220014 = RSA-1024 private-key op, which
    unwraps a 0x2C0-byte key blob with it -> header unwrap + 0xc0b38f78 patch alone is NOT enough for MTPZ.
  * libmtp-zune: a failed MTPZ handshake only logs "(MTPZ) Failure..." and the device stays open;
    045e:063e = DEVICE_FLAG_NONE. (verified)
  * PLAN (plain MTP, no device access): byte-match-guarded loader patch on EXT MtpSvc.dll
    (EXT\modules\MtpSvc.dll offsets, original bytes checked session 7):
      VA 0x416d3088 file 0x12488: 5a000002 (bpl) -> ea000002 (b)    init ignores KEY1 cert-fetch failure
      VA 0x416cb808 file 0xAC08:  15858028 (strne r8,[r5,#0x28]) -> e5858028 (str)  MTPZ flag forced 0
    (2nd patch assumes r8 == 0 at that point - check in the listing before applying.)
    Then the USB self-test: expect MUT1: open, ep1 OUT primed, GetDeviceInfo + OpenSession answered.
    pkv_unwrap.py only if MTPZ is wanted later, and then the private-key blobs need unwrapping too.
- RESULT (session 7): MTP WORKS. Self-test GetDeviceInfo -> 885-byte DeviceInfo dataset: extensions
  "microsoft.com: 1.0; .../WMDRMPD: 10.1; .../WMPPD: 11.1; .../AAVT: 1.0; .../WMDRMND: 1.0; .../MTPZ: 1.0;
  audible.com: 1.0", 83 operations (incl. 0x9212.. MTPZ ops), manufacturer "Microsoft", model "Zune",
  version "04.05.00114.00-04.05.00114.00-00.00.00000.00", serial all zeros (comes from the key vault).
  Needed beyond the two MtpSvc patches:
  * EXT modules are PAGED IN FROM ZBINFS (eMMC), not executed from the RAM copy QEMU loads -> RAM patches
    on EXT do nothing. tools\mkemmc.py now applies EXT_PATCHES to the EXT.nb0 copy in ZBINFS (byte-guarded).
    (NK modules do run from RAM: NK patches in zunehd.c work.)
  * zbinfs.dll ("secure" build, zbinfs_secure.pdb) checks every page against a SHA hash tree
    (zbinfs_ext.hash; ReadRegionAtOffset_Verified FUN_c0b76070 -> per-page FUN_c0b75a84, hash-block check
    FUN_c0b759a0, memcmp FUN_c0b7ca60). Mismatch -> ZPrintHex of both 32-byte digests on serial + read fails
    -> 'Raised Exception' in the faulting process. zunehd.c patches both `beq ok` after the memcmp into `b`
    (PA 0x008b5f34 / 0x008b5a2c; ZUNEHD_NOZBINFSPATCH=1 disables them).
  * Plug time: MtpSvc (servicesd) is up late; ZUNEHD_USB_PLUG_AT=40 works with a 58 s run (26 is too early:
    the self-test MTP wait is ~15 s virtual).
  * GOTCHA: the guest writes to zunehd-emmc16.img. After a run that crashed in servicesd, every later boot
    raised an exception early in NK.EXE (k.coredll+0x15260, before the SDIO probe) even with the patches off.
    Rebuilding the image (py -3 -I tools\mkemmc.py) fixed it. Rebuild after any crashed/killed boot that
    looks odd. (Which file/state is persisted is not investigated yet.)
  NEXT: OpenSession / GetStorageIDs in the self-test, then the USB/IP server (TCP 3240) + usbip-win2.
### Session 7b: USB/IP + media sync WORKS
- qemu\hw\arm\zunehd-usbip.c: USB/IP 1.1.1 server (ZUNEHD_USBIP=<port>, 127.0.0.1, busid 1-1; chardev
  socket so it works on Windows). Import plugs the cable, waits for the bus reset, sends SET_ADDRESS itself
  (retries once if the first one times out after a replug), then replies. Per-endpoint URB queues pumped by a
  1 ms virtual timer; UNLINK; ep0 STALL -> -EPIPE (zune_usb_ep_stalled; SETUP clears ep0 stall). Disconnect
  unplugs. ZUNEHD_USBIPLOG=1 logs URBs. Only one QEMU at a time (monitor 4445 / usbip 3240 are fixed ports).
- tools\zmtp.py (stdlib USB/IP client + MTP initiator): info / storage / ls / put / putmeta / sync / rm.
  `sync a.mp3 b.mp3` does what zune-explorer does: SendObjectInfo+SendObject per track, SetObjectPropValue
  (0x9804) Name/Artist/Genre/Track/Duration (AlbumName/AlbumArtist on tracks -> Access_Denied, fine), then
  artist objects (0xB218 "<name>.art", Name) and album objects (0xBA03 "<artist>--<album>.alb", Name, Artist,
  ArtistId 0xDAB9), ArtistId on tracks, SetObjectReferences (0x9811) album -> tracks. Result: albums/artists
  show in the music hub; Quickplay "New" lists the songs. A plain SendObjectInfo MP3 shows up only as
  ".mp3" / Unknown Artist (the device does not parse ID3).
- Storage: 0x00010001, 14823 MiB ("Storage"). Playback formats incl. 0x3009 MP3, 0xB901 WMA, 0xB982 MP4,
  0xB981 WMV, 0x3801 JPEG (no WAV 0x3008 -> Invalid_Code_Format 0x2016).
- tools\mksilentmp3.py: silent MPEG-1 L3 + ID3v2.3 (title/artist/album/track/genre) for tests.
- Boot is fast in wall time: MtpSvc answers ~3 s after QEMU start when the client connects early (import
  blocks until ready). Screen sleeps before ~50 s; wake = `key home`, unlock = swipe up (zseq raw
  mouse_move/mouse_button steps, see CHECKPOINT).
### Session 7d: AUDIO - groundwork (AVP)
- Audio path: waveapi -> libnvwavedev (WAV_Init, patched off by default) -> libnvddk_audiomixer_impl /
  libnvaudio_renderer / libnvmm* (CPU) -> NvRmLoadLibraryEx("nvddk_audiomixer_core.axf") on the AVP.
  AVP images (CE-compressed .cecomp: 3-byte uncompressed size + 3-byte block-end table, 4 KiB blocks):
  EXT nvrm_avp.axf (kernel), nvddk_audiomixer_core.axf, nvmm_*dec.axf (NK+EXT). libnvmm_audio.dll (CPU)
  also has decoder code (audecDecode). Codec = WM8350 on DVC I2C 0x1A; I2S1 IRQ 13, APB DMA IRQ 26.
- avp=on now only drops the 3 AVP/audio patches (NvRmLoadLibraryEx, NvRmFreeLibrary, WAV_Init); before it
  dropped ALL patches (zbinfs -> fsdmgr exceptions, MtpSvc...). zseq/zbreakm/zgdb take -Machine "avp=on".
- Fixed: STATUS_INTEGER_DIVIDE_BY_ZERO (0xC0000094) in libnvrm_impl FUN_c0954abc (audio clock): divides by
  PLLA rate = rate cache DAT_c097df78[5]. PLLA (desc 0xc09412a0: id 5, input 0xE = PLLP_OUT1, reg 0xB0)
  is pinned BYPASS -> rate = input; QuickTurn clock init never computes id 0xE (no writer; watchpoint) ->
  0. Clock descriptors: 10 words {id, input id, type, reg, mask, shift, clr, set, disabled-value, fixed}
  (FUN_c095bee0 rate, FUN_c095cc40 configure, FUN_c0952bbc PLL rate). Patch "NvRm audio clock: PLLA rate
  12288 kHz (QuickTurn)" PA 0x0071ebc0 (VA 0xc0954bc0 ldr r4 -> mov r4,#0x3000). Also preset PLLP_OUTA
  0x600060A4 = 0x00070d07 (bootloader-like; not sufficient alone). Debug trick: kernel-mode raises go
  through k.coredll FUN_c008eff4 (trap 0xF101FF8C = kernel API 29): break there, r0 = exception code.
- With avp=on the boot now reaches "ResetVector = 60abf30" and then waits for the AVP (UI stays black).
  NvRm AVP start (MMIO log): CAR CLK_OUT_ENB_L 0x60006010 = 0xffffffff; EVP COP_RESET_VECTOR 0x6000F200 =
  0x060abf30; CAR RST_DEVICES_L 0x60006004 bit1 (COP) pulsed; mailbox MBOX_TO_AVP 0x60001020 = 0 then
  0xa0610b6d (bit31 valid, bit29 int enable, [27:0] = phys>>4 -> msg buffer 0x0610b6d0). Reply expected via
  MBOX_FROM_AVP 0x60001010 + CPU IRQ 4 (SHR_SEM inbox). Same scheme as Linux tegra avp.c / avp_svc.c.
- AVP kernel in RAM: carveout 0x060ab000..0x060b3000 (32 KiB; dumps in extracted\avp\carve_*.bin, entry
  0x060abf30: b; msr cpsr_fc,#0xdf; bl...). Other carveout data at 0x06090000 (16 KiB), 0x06099000.
  Literal MMIO it uses: 0x6000F200 (EVP x7), 0x60004000 (ICTLR/COP), 0x60005000 (timers), 0x60006000 (CAR),
  0x60007000 (flow ctrl), 0x6000C000, 0x7000E400 (PMC), 0xF000xxxx incl. 0xF000F000 (AVP MMU/TLB block).
  Strings: "RPC_AVP_PORT", "RPC_CPU_PORT", "DMA Init Succeeded/Failed", "Assert on %s:%d: %s".
- PLAN: (1) second CPU (ARMv4T-compatible, e.g. arm926 / ti925t) in its own cluster + address space,
  started at COP_RESET_VECTOR on COP reset release; (2) AVP-private regs: 0xF000xxxx MMU/TLB + cache,
  COP side of ICTLR (0x60004000 + COP bank), timers, flow-ctrl HALT_COP; (3) RES_SEMA mailbox both ways
  + IRQ 4 (CPU) / COP IRQ; (4) then the audio data path: APB DMA + I2S1 + WM8350 codec regs -> QEMU audio
  backend. Device (Lyra) is available for targeted register reads to check AVP behaviour (ask first).
### Session 7e: AVP emulated as a second CPU (qemu\hw\arm\zunehd-avp.c/.h)
- Machine is now 2 CPUs: CPU arm1136 in cluster 0 (gdb process 1), AVP in cluster 1 (gdb process 2;
  `add-inferior; inferior 2; attach 2`). mc->default/min/max_cpus = 2 (MTTCG sizes TCG contexts from it).
  With the CPU unclustered, gdb attached to the AVP first and every gdb tool broke.
- AVP core MUST be ARMv4T: QEMU "ti925t". On arm926 (ARMv5) the AVP kernel faulted (prefetch abort,
  IFSR 1 alignment, at 0x060b1906): it resumes Thumb threads by restoring CPSR (T=1) then "ldm {..pc}" with
  bit0 = 0 - ARMv5 interworks on that load, ARMv4T does not.
- AVP address space = system memory + AVP-only overlays: vectors RAM at 0 (8 x "ldr pc,[pc,#0x18]" + copy
  of the EVP COP words 0x6000F200..21C, refreshed on EVP writes: +0 reset, +4 undef, +8 swi, +C pabt,
  +10 dabt, +18 irq, +1C fiq); IROM page at 0xFFFF0000 (+0x40 -> 0xFFFF1000 = copy of the OAL's chip-0x16
  relocation table from guest RAM PA 0x00BD7460 = OAL FUN_80030b48 -> FUN_80037448 -> VA 0x80BD7460;
  the AVP NvRmOpen FUN_060aeb70 always reads IROM+0x40); 0xF0000000 64 KiB private stub (AVP writes
  0xF000F040 = 0, 0xF000F044 = 0xFC0 at init).
- Start: AVP starts parked by HALT_COP (it is the boot CPU on silicon); NvRm sets EVP reset vector,
  pulses CAR RST_DEVICES_L bit1 (also RST_DEV_L_SET/CLR 0x300/0x304 hooked), writes HALT_COP 0 -> AVP runs
  at 0x060abf30. AVP writes HALT_COP 0x90000800 (mode 4) in its idle loop -> CPU_INTERRUPT_HALT (WFI).
- Mailbox RES_SEMA 0x60001000: +0x10 inbox = FROM_AVP, +0x20 outbox = TO_AVP; [31] pend-int-en,
  [30] read-int-en, [29] valid; ICTLR lines 4 inbox full / 5 inbox empty / 6 outbox full / 7 outbox empty.
  AVP COP IER (seen): bank0 0x10000042 (IRQ1 TMR2 = its 1 kHz tick, IRQ6 outbox full, IRQ28 arb grant),
  bank1 bit 28 (IRQ 60). ICTLR now drives the AVP IRQ/FIQ from cop_ier/cop_class. ARB_SEMA per master
  (gnt/req for CPU and COP, told apart by current_cpu; COP grant IRQ 28). Logs: "AVP " prefix on stub
  accesses; ZUNEHD_AVPLOG=1 (start/stop/EVP/mailbox).
- Message area (CPU libnvrm_impl FUN_c0968f58): 0x220-byte NvRmMem at 0x0610b6d0: CPU->AVP at +0,
  AVP->CPU at +0x110; init writes 0xDEADF00D at +0, 0 at +0x110, then MBOX_TO_AVP = 0xA0610B6D
  (FUN_c096da70). CPU registers the inbox IRQ via NvRmGetIrqForLogicalInterrupt(rm, 0x2F, 0/1).
- STATUS: AVP kernel boots (vectors, NvRmOpen with the relocation table, ports "RPC_CPU_PORT"/
  "RPC_AVP_PORT" created by FUN_060af060/FUN_060af720), consumes the init message (clears TO_AVP, writes
  FROM_AVP = 0), then idles at 0x060b1906 (HALT_COP) taking its 1 kHz tick - it is a server waiting for the
  CPU to connect. The CPU never sends a second mailbox message; UI stays black with avp=on. Audio worker
  threads (libnvddk_audiomixer_impl+0x34b4, libnvwavedev+0x35e0/0x9188) are parked normally.
  NEXT: find the blocked CE thread later in boot (zthreads -Machine avp=on, larger -IdleSkips; extend
  zthreads beyond the NK process) - probably NvRmTransportConnect("RPC_AVP_PORT") waiting on something.
- Tools: tools\raw2pe.py (wrap a raw ARM image as PE for REA/Ghidra; AVP kernel ->
  extracted\avp\nvrm_avp_armnt.dll, base 0x060ab000), zseq: `mon CMD` action keeps monitor output in
  zunehd-mon.txt (e.g. "mon cpu 1; mon info registers"), ZSEQ_D / ZSEQ_DFILTER env for -d/-dfilter;
  zgdbat/zthreads/zgdb/zbreakm take -Machine. QEMU here has no disassembler (in_asm prints raw bytes).
### Session 7f: CPU<->AVP RPC works; boot completes with avp=on; MP3 playback reaches the VDE
- AVP sees SDRAM aliased at 0x90000000 (phys + 0x90000000): its mailbox handler FUN_060acebc uses the
  message area at (mbox data << 4) - 0x70000000. Added as an AVP-only alias (256 MiB). Without it the AVP
  never cleared area word 0 (0xDEADF00D) and NvRmTransportConnect (libnvrm_impl 0xc0969120) waited forever.
- Transport protocol (CPU NvRmTransportConnect + AVP FUN_060acebc): CPU->AVP area word 0 = command
  (2 connect, 3 disconnect, 4 message), AVP writes 5 = ACK; CPU sends only when word 0 == 0; first mailbox
  message (0xDEADF00D) just hands the area to the AVP, which zeroes word 0. AVP replies via FROM_AVP
  0xA9610B6D (data 0x09610B6D = alias address >> 4).
- NvRmLoadLibraryEx (0xc096af3c) path: FUN_c09720c0 (AVP boot) -> FUN_c0969824 (area + IRQ) ->
  FUN_c096a238 (open RPC_CPU_PORT) -> FUN_c0969e70 (open RPC_AVP_PORT) -> FUN_c096aa80 -> FUN_c096a0a0
  (SetQueueDepth 5x0x100 + NvRmTransportConnect INFINITE) -> FUN_c096a9b4 (load request).
- RESULT: with avp=on the system boots to the UI (lock screen after Home at ~118 s; slower boot), ~200 RPC
  messages. CPU mixer programs DAP 0x70002400 (0x240C = 0x80808080, CTRL 0x40000000, 0x2540..54) and
  I2S1 0x70002800 (CTRL 0x02000000, 0x808 = 0x1F). AVP writes its resume vector to PMC 0x7000E4A8.
- Playing a (silent) synced MP3 from the UI: song list + player screen work; the AVP decoder (library at
  ~0x0655D000, hot loop in IRAM 0x400173xx..0x4001776c) programs the VDE block (NvRm modules 38/47 @
  0x6001A000, size 0x3B00): 0x6001A0F0, 0x6001B008, 0x6001C050/0A0/214/228/408/600 (init, values 0xA /
  0xA00 / 0x8002), then a command sequence 0x6001D044 = 0x2404, 0x6001CC14 = 0, 0x6001CC00 = 0xE08,
  0x6001CC10 = 0, 0x6001D008 = 0xF3B and BUSY-POLLS 0x6001CC14 (8.3 M reads) -> needs a VDE model
  (undocumented; Linux tegra-vde covers parts of the Tegra20 VDE). No APB DMA / I2S data yet.
### Session 8: MP3 Huffman engine (VDE bitstream engine) model - qemu\hw\arm\zunehd-bse.c
- Test file: "C:\rea-work\Test audio\Up.mp3" (Mr. Ivex - Up, 3:52, MPEG-1 L3 320 kbps 44.1 kHz joint
  stereo; ID3v2 tag is 1.2 MB with embedded art, first frame at 1232644).
- AVP MP3 decoder Huffman step = FUN_400176dc in IRAM (dump extracted\avp\iram_play.bin, wrapped as
  iram_play_armnt.dll with tools\raw2pe.py --imagebase 0x3fff0000 - imagebase must be below the section).
  It builds command lists in IRAM and kicks 0x6001CC14 = 0xA0000000 | (IRAM word index); waits [31] clear;
  then status 0x6001D018 bit 0x800 (done) / 0x10 / !0x1; output end from 0x6001D064 [30:16]. Command
  format and register meanings: header comment of zunehd-bse.c. Constants in the decoder literal pool
  0x40017b90..bbc: 0x6001c000, 0x7c442084 (mode setup), 0x7c560000, 0x7c443084 (mode pairs),
  0x7c660000 (out ptr), 0x3fff (count mask), 0x7c443086 (mode count1), 0xf4240 (timeout), 0x5fc000
  (error mask), 0x2405, 0xf3b, 0x40004100 (reset value -> list "0x7c600041, end" in the error path).
  Output = 576 int32 spectral values per granule (sign + linbits applied); AVP zeroes the rest.
- Model: zunehd-bse.c (+ zunehd-mp3tab.h = minimp3 tables, CC0, ext\minimp3 fetched 2026-11-08).
  ZUNEHD_BSELOG=1 logs per list budget/used/overshoot (validation: overshoot must stay small),
  ZUNEHD_BSE_SWAP=1 tries 32-bit-swapped bitstream byte order. Untested yet (sync of the file pending).
- FIXED (zunehd-usb.c zune_usb_dev_out): a host chunk ending on a full packet no longer retires the
  partly filled dTD (USB short-packet rule); before, every URB end looked like end-of-transfer.
- OPEN, NEXT SESSION FIRST: objects > ~1 MB never finish SendObject. Measured: 0.96 MB (s60.mp3) OK,
  1.12 MB (s70.mp3) hangs; with 1 MiB URBs all data is accepted but no response; with 256 KiB URBs
  (ZMTP_CHUNK=262144) the device stops priming ep1 OUT after the first chunk. During the hang the eMMC
  log shows ONLY single 2 KiB reads of consecutive sectors (CMD18 arg 0xF2C40, 0xF2C60, 0xF2C80 ... each
  + CMD12 + CMD13), no writes -> device-side scan (exFAT bitmap/FAT walk for allocation?) one sector at
  a time, or a loop. Check: which DSK1 range that is (Flash2 starts at DSK1 131128 -> native
  441344 + 4*131128 = 965856 = 0xEBCE0; 0xF2C40 is ~0x6F60 blocks in = Flash2 bitmap/FAT/data?), whether
  mkemmc's exFAT bitmap/FAT are complete for the whole volume, and multi-block (CMD25/18 > 1 block) paths.
  Test files: tools\mksilentmp3.py (s60/s70 in scratchpad\media), tools\mp3cut.py -> "Test audio\
  Up_30s.mp3" (1.2 MB, also hangs). zmtp: ZMTP_CHUNK env, per-URB timing print, timeout 900 s.
- Then: play Up_30s.mp3 with avp=on + ZUNEHD_BSELOG=1 and check the engine's budget/overshoot log.
### Session 9: large MTP SendObject FIXED (two emulator bugs, guest was fine)
- What the "hang" really was: the device did NOT scan; after a corrupted chunk MtpSvc wrote at a bogus
  offset and exFAT zero-filled the gap up to valid-data-length one 16 KiB cluster at a time (8 single-sector
  CMD25 + FAT sector 1 @native 0xebd64 + bitmap @0xedae0 + dir entry @0xedc08 per cluster) -> endless.
  Flash2 geometry (boot region): 2 KiB sectors, 16 KiB clusters, FAT @32 len 1874, heap @1912, 958762
  clusters, root clus 2; heap starts at native 0xEDAC0.
- MtpSvc ep1 OUT pattern: one 12-byte dTD for the container header, then batches of 4 x 16 KiB dTDs
  (0x060b0100/120/140/160, buffers 0x06091000.. 0x0609d000) = 64 KiB file chunks; between batches it does
  FLUSH, FLUSH, PRIME. It also FLUSHes ~25 ms (virtual) after the last completion if the batch stalls.
- Bug 1 (zunehd-usb.c): the short-packet test used the REMAINING chunk length (len % 512) -> at the end of
  a 1 MiB URB the remainder was 16372 bytes (header shift 12) -> dTD retired 12 bytes early. Fixed:
  zune_usb_dev_out_xfer(ep, buf, len, short_end) with short_end decided from the WHOLE URB (usbip passes
  u->len % mp; ep0 OUT data stage too). zune_usb_dev_out() keeps the old behaviour for the self-test.
- Bug 2 (zunehd-usbip.c): while the client waited for "URB n done" before sending URB n+1, the guest
  clock ran on and the driver's ~25 ms flush dropped the partly filled dTD -> 12 stale bytes (from the
  previous 64 KiB buffer) at file offset 1 MiB - 12, rest of the file shifted by 12. Fixed with bulk OUT
  write-behind: OUT URBs are completed to the client as soon as they are queued, up to 8 MiB not yet taken
  by the guest (usbip_write_behind, re-checked whenever a URB drains). Errors on such URBs are not
  reported to the client any more (acceptable: the MTP response phase still reports failures).
- Verified: 3 x Up.mp3 (10,546,991 B) + Up_30s.mp3 byte-identical on the eMMC, ~850 KiB/s, 0 partial
  flushes. Checker: scratchpad verify_img.py logic (find cluster-aligned copies in the Flash2 heap).
- Debug aid: ZUNEHD_USBDTDLOG=1 -> "usbdtd: <virtual ms> ..." lines on stderr for ep>=1 dTD pick/retire,
  PRIME and FLUSH (FLUSH with "pos N>0" = data lost). tools\zseq.ps1 gained -OutDir/-MonPort/-Qemu/
  -EmmcImage for running two QEMUs in parallel; shared build lock file qemu\.build.lock (create
  exclusively before zbuild.sh, delete after; a running build\qemu-system-arm.exe makes the link fail).
- zmtp quirk: options (e.g. --format) must come before the command list; .txt (0x3004) objects are
  refused with 0x2016.
### Session 9: APB DMA + I2S1 -> host audio (qemu\hw\arm\zunehd-apbdma.c)
- Playback recipe (avp=on): boot ~165 s to the UI WITHOUT USB; syncing during boot leaves the UI on
  "CONNECTED"/boot bar. Then zmtp sync, Home, tap the "New" tile (78,282) -> player. Script:
  scratchpad play2.ps1 (zseq -Machine avp=on -QemuArgs "-audiodev wav,id=snd0,path=...").
- APB DMA = Tegra2 layout: global 0x6000A000 (+0 GEN enable; CPU OAL writes all 16 channel bits to
  +0x24 then +0x20 at boot), channels 0x6000B000 + 0x20*n: CSR/STA/AHB_PTR(+0x10)/AHB_SEQ/APB_PTR/APB_SEQ.
  CPU uses ch0/2/4 (UART probes). AVP mixer uses ch15: APB_PTR 0x70002840 (I2S1 FIFO1), AHB_SEQ
  0x25080000 (DBL_BUF), CSR 0xD0220FFC. AVP kernel FUN_060ae090 = 16-entry buffer queue (addresses with
  |0x90000000), FUN_060adbfc = channel start (WCOUNT = size/8 - 1: a buffer of S bytes = 2 halves);
  its EOC handler (pc 0x060adc96) reads STA, W1C, writes the NEXT buffer to AHB_PTR -> model latches
  AHB_PTR at the buffer wrap. Interrupts: CPU enables ICTLR 26, AVP enables ICTLR 60
  (INT_APB_DMA_COP, bank1 bit28): modelled as +0x20 = CPU mask, +0x24 = AVP mask (UNVERIFIED reading,
  but with it the AVP services ch15). I2S1: CTRL 0x02000070 (CPU) -> 0x22000070 (AVP, FIFO1 on),
  TIMING 0x1F, FIFO_SCR 1; left as stub.
- Model: audio channel = enabled, DIR=1, APB_PTR in 0x700028xx; consumed in virtual time at
  ZUNEHD_AUDIO_RATE (default 44100) as S16 stereo words -> ring -> QEMU audio voice. Backend: the
  audiodev with id ZUNEHD_AUDIODEV (default "snd0"), else QEMU's default (dsound when no -audiodev).
  ZUNEHD_DMALOG=1: register log + "apbdma: <ms> ch15 EOC half h (frames, peak)" + ICTLR COP IER changes.
- RESULT: the AVP cycles a ring of 10 x 8 KiB mixer buffers (0x0648d000, 0x064aa320.., 0x064fd000..
  0x06505000) and, with the BSE fix, the decoder runs all the time (~1000 lists / 40 s). BUT the mixer
  buffers are all ZERO (digital silence) - under investigation (decoder synthesis -> mixer path).
  Rate/format (44.1 kHz S16 stereo) not yet verified against real data.
### Session 9 (BSE validation): VDE bitstream engine now decodes bit-exactly (zunehd-bse.c)
- Two model bugs (FUN_400176dc + its descriptor table, confirmed statically):
  1. Codebook numbering: command bits [4:0] are an ENGINE codebook number, not the ISO table. The AVP
     maps ISO table_select through 3-byte descriptors {flag, linbits, codebook} at IRAM 0x40004500
     (index 0..31 = ISO tables, 32/33 = count1 A/B; flag 0 = table 0/4/14 -> AVP zeroes the region,
     no command). Codebooks: 0/1 count1 A/B, 2..4 = ISO 1..3, 5..13 = ISO 5..13, 14 = ISO 15,
     15 = ISO 16..23, 16 = ISO 24..31 (linbits separately in [24:21]). Old model used ISO directly
     (e.g. table 15 -> codebook 14 = empty) -> lists stopped far below the budget.
  2. Byte order: the AVP main-data ring (2 KiB of 32-bit words; head copied past the end before a kick)
     holds the stream big-endian per word (software reader FUN_400170c0 shifts words MSB first, no
     swap); the 0x40 pointer = word address + consumed/8 in that view -> stream byte k at memory k^3.
     Swapped reading is now the DEFAULT (ZUNEHD_BSE_NOSWAP=1 = old plain order, experiments only).
- Confirmed: budget 0x64 = part3 bits (part2_3_length - scalefactor bits); afterwards the AVP advances its
  own reader by the budget (FUN_400170c0), never reads the engine position. "budget 0 ... over 4" lists =
  frame 0 of Up_30s (part2_3_length 0, all-zero granules) = correct. Count1 overshoot 1..6 bits is normal.
- Verification (Up_30s.mp3, playback runs with ZUNEHD_BSEDUMP): 96 of 96 (run 2) and 88 of 88 (run 3)
  non-empty granule lists bit-exact vs minimp3 (all 576 values, budget == part3, emulator output ==
  offline engine), + 4 empty (budget 0) lists. Covers long/start/short/stop blocks, linbits tables.
- Tools: tools\mp3ref.c (+ mp3ref.exe; MSYS2 gcc -O0 -I ext/minimp3 ... -lm; -O2 build segfaults):
  `mp3ref in.mp3 ref.txt` -> per granule/channel side info, part3 bytes, 576 reference values.
  ZUNEHD_BSEDUMP=<file> (QEMU env) -> per list: L words / S start addr+bit+1 KiB raw bytes / U budget used
  over / V values. tools\bsecmp.py: `py -3 -I tools\bsecmp.py ref.txt dump.txt [-v N]` matches each list
  to its granule by the first 48 bitstream bits (tries both byte orders), reruns an offline engine copy
  and compares with the reference and with the emulator. Work dir C:\rea-work\bse (run logs, dumps).
- OPEN (not BSE): in every run the AVP decodes only ~23-25 frames (~92-100 lists, 0.6 s of audio) and
  then idles (AVP PC 0x060b1906 HALT_COP loop), presumably waiting for PCM buffers to drain via the
  APB DMA / I2S path. Also the player screen did not show in runs 2/3 (tap timing under load).
  -> resolved by zunehd-apbdma.c (coordinator); decoder now runs the whole track.
- SILENCE ROOT CAUSE (session 9, follow-up): IMDCT + polyphase synthesis run on an UNMODELLED AVP
  "audio vector engine" at 0x6000E000 (falls into the PPSB register stub). Decoder = AVP library at
  0x06490000 (dump: mem pmemsave 0x06480000..0x06520000 -> bse\avp_0648_armnt.dll, raw2pe base
  0x06480000; IRAM veneers at 0x064a0100.. "ldr pc,[pc,#-4]"). Per granule FUN_064982e8: FUN_064998e8
  scalefactors -> IRAM FUN_400173a4/400176dc Huffman (BSE) -> FUN_06498e24/FUN_06498c08 dequant
  (gain only, see below) -> FUN_0649a97c stereo -> FUN_0649aae4 -> FUN_0649ad74. Frame loop
  FUN_06498784: FUN_0649baa0 (reads 0x6000E078 = wait/fence), FUN_064985c4 builds the hybrid-synthesis
  program (FUN_06497d0c, FUN_064975ec/650/6b4/6f4 per block type, FUN_0649eb70 polyphase,
  FUN_0649bc28 PCM pack, FUN_0649bb6c copy) into IRAM (ctx 0x400117a0: +0x54 program base 0x4000c410,
  +0x58 V-ring 0x40007000 with ring index at +0x100c, +0x5c spectra 0x40008010, +100 PCM staging),
  FUN_0649bab0 kicks: 0x6000E074 = program address, 0x6000E070 = 0xA0000000 | (words - 1); IRAM
  0x40017bf8 then memcpy's the staged PCM into the NvMM output buffer (= the 10 DMA ring buffers,
  valid size 0x1200 per frame). Engine never runs -> staging stays 0 -> digital silence.
  Evidence (-d unimp, 1162 BSE lists): AVP touches ONLY 0x6000E070/74/78 per frame (580 kicks,
  programs 0x4000cc10 len 0xD2A and 0x4000d0c8 len 0xBFC; excerpt bse\ape-kicks-excerpt.txt). No
  other unmodelled block is used per frame.
  Program format (tools\apedis.py, IRAM dump bse\iram215.bin): instruction = header 0x00FFMMLL +
  one word per set bit of MM, of FF&0x23 and of LL&7 + end word (top nibble 8/A, e.g. 0x82001000,
  0xA2002200, 0xA1000088); operands are IRAM addresses (Q31 cosine/window tables at 0x4000532c,
  0x40005834, 0x4000599c...) and descriptors (stride << 16) | (count - 1) (e.g. 0x0001047F = 1152
  contiguous words). 30 distinct opcodes (00401400 x159 most common, 00404500 = 3-address butterfly).
  NEXT: model it in a new qemu\hw\arm\zunehd-ape.c (synchronous interpreter on the 0x6000E070 write;
  semantics to be derived opcode by opcode against minimp3's IMDCT/synthesis on the same granule).
- BSE output format FIXED (static evidence): FUN_06498c08 computes x * T[p & 15] >> (4 * (p >> 4) + 16)
  with T = 2^31 * 2^(-k/4) (IRAM 0x400049e8) and p = 210 - global_gain + scalefactor terms, i.e. only
  2^15 * 2^(-p/4) -> the engine must deliver sign * |x|^(4/3). zunehd-bse.c now writes it in Q13
  (8206^(4/3) * 2^13 just fits int32; result xr in Q28 = libmad format). Q13 is inferred, unverified
  until the vector engine exists (ZUNEHD_BSE_RAW=1 = old raw integers).
- Tools: tools\mp3pcm.c/.exe (minimp3 -> raw s16 reference PCM), tools\pcmcmp.py (align emulator
  wav vs reference: envelope + sample correlation, gain, SNR), tools\apedis.py (engine programs).
### Session 9: AVP audio vector engine (zunehd-ape.c) - MP3 PLAYS, PCM bit-exact vs minimp3
- New qemu\hw\arm\zunehd-ape.c (MMIO 0x6000E000, overlap prio 10 over the PPSB stub) + interpreter
  core qemu\hw\arm\zunehd-ape.h (no QEMU deps, shared with the offline harness). Kick = write
  +0x70 = 0xA0000000 | (words-1) with +0x74 = program address; runs synchronously on the write
  (reads IRAM 256 KiB, executes, writes back only the words written); +0x78 reads 0 (idle).
  Full format/opcode documentation in the zunehd-ape.c header. Summary:
  - insn = header 0x00FFMMLL + operands (LL bits -> L0..L2, MM bits -> slot0..3 addr/desc pairs,
    FF bit0/1 -> P0/P1, bit5 -> SH) + control word. All registers persist; streams restart per insn.
  - desc [31:28] flags (bit2 = 2D: runs of n = P field + 1 at +-1, run start += stride; bit0 =
    reverse), [27:16] stride, [11:0] count-1 (wraps; count 1 = constant). P field [13:12]=3 = order
    0,n-1,1,n-2 (window y[i]/y[17-i]). L override [27:16] n-1, [15:0] outer step; flags 9 =
    circular in n*stride words (V ring), b = plain. SH [15:8] shift, [7:0] saturation bits-1.
  - control: [27:24] class, [23:20] write 0 word / 2 low half / 3 high half (L/R PCM), [15:12] op,
    [11:8] bit1 = round. class 2 element-wise: op0 s2=a*b, op1 s2=a+b, op2 s3=a-b, op3 both
    (butterfly). class 1 MAC: [7:4]+1 data taps (s1), [3:0]+1 coefs (s0), nc/nd outputs per group.
    class 7 = Lee DCT32 recombination (out[2k]=in[k], out[2k+1]=in[n/2+k]+in[n/2+k+1]).
  - Data flow: spectra (0x40008010/0x40009210, already antialiased by the AVP CPU) -> IMDCT
    (pre-differences, 9x9 / 6x6 cosine MACs, twiddle, window butterfly, overlap 0x40012acc) ->
    freq inversion -> x2 transpose 0x400141a4 -> DCT32 -> V ring 0x40007000/0x40007800 -> 16/8-tap
    window -> staging 0x400101a4 (576 words, L low / R high), copied out by IRAM 0x40017bf8.
  - BSE Q13 pow43 assumption CONFIRMED (end-to-end gain 1.0000 vs minimp3).
- Validation: (1) offline, 300 captured kicks chained (tools\apesim.exe run): 99.3 % of samples
  bit-exact vs minimp3 (bse\ref.raw), rest +-1 LSB, SNR 98 dB, gain 1.0000 (long, start/stop and
  short blocks; rounding flag [11:8]&2 needed: 80 dB without). Stage checks with ape\chk_*.py
  (IMDCT 129 dB, DCT32 > 120 dB, synthesis vs transcribed minimp3 synth 84 dB pre-rounding).
  (2) in QEMU: with ZUNEHD_AUDIO_PACE=22050 the DMA stream (ZUNEHD_PCMDUMP) holds the whole track
  as ONE contiguous run of 1,318,288 frames, 98.7 % bit-exact, max diff 1, SNR 99.8 dB (after a
  2048-frame fade-in by the guest mixer; the player skips the first 2160 reference frames =
  Info frame + encoder delay). 44.1 kHz S16 stereo, L = low half confirmed end to end. Final run
  (pace 22050, voice 44100): ape\out.wav (44100 Hz header) = the same gapless bit-exact track,
  DMA "stalls 0"; serial log ape\zunehd-serial.log.
- REAL-TIME LIMIT (open): at the default pace (44100 frames per virtual second) the emulated
  decode pipeline delivers only ~70 % of real time, so the guest mixer underruns: its buffers get
  512-frame silences and ~700-1000 frames of audio are skipped every 50-100 ms (audio is clearly
  recognisable but choppy). Not an engine problem (engine cost is small); it is overall emulation
  speed (AVP Huffman/dequant + CPU NvMM + UI). zunehd-apbdma.c now: (a) never completes a half
  while the previous EOC is pending (fixes replayed buffers on late ticks), (b) keeps up to 0.25 s
  of consumption backlog instead of dropping time, (c) ZUNEHD_AUDIO_PACE = consumption rate
  separate from the voice rate (wav audiodev writes only what it gets -> gapless, pitch-correct
  file), (d) ZUNEHD_PCMDUMP=<file> raw dump of every consumed frame.
- Not exercised: mixed blocks (FUN_064975ec/650 programs), mono programs (0xF45-word layout,
  possibly using the PCM pack sequence FUN_0649bc28 = ctl a2021000/a2321000/a2201000 whose [19:16]
  field is ignored), other AVP codecs.
- Tools (work dir C:\rea-work\ape: run.ps1 playback wrapper, build.ps1 locked build, cap1\ = 300
  kick captures):
  - QEMU env: ZUNEHD_APEDUMP=<dir> (kNNNNN.bin per kick: 'APEK', prog addr, +0x70, kick no, IRAM
    before execution; ZUNEHD_APEDUMP_MAX default 300), ZUNEHD_APELOG=1, ZUNEHD_APE_OFF=1.
  - tools\apesim.c/.exe (gcc -O2 -o tools/apesim.exe tools/apesim.c): `apesim run <dir> 1 300
    sim.raw [-t trace] [-s snapdir]` chains kicks (engine-written words carried over unless the CPU
    changed them) and writes the staged PCM; `apesim one k.bin out.bin [-n N] [-t trace]`.
  - tools\raw2wav.py, tools\pcmcmp2.py (sim.raw vs ref.raw at small offsets: gain/SNR),
    tools\pcmalign.c/.exe (piecewise alignment of a long recording), tools\pcmsegs.py (lists
    bit-exact runs / silences / jumps - shows underruns).
  - Recipe: `$env:ZUNEHD_AUDIO_PACE='22050'; $env:ZUNEHD_PCMDUMP='C:\rea-work\ape\dma.raw';
    powershell -NoProfile -File C:\rea-work\ape\run.ps1 -Tag x -Seconds 300` (boot, sync at 168 s,
    tap New at 196 s), then `py -3 -I tools\pcmsegs.py ape\dma.raw bse\ref.raw`.
### Session 9 (GUI vs device photos) - all changes in qemu\hw\arm\zunehd-gr3d.c
Work dir C:\rea-work\gui (own image/exe/monitor 4455; run.ps1 wrapper, build.ps1/buildretry.ps1 = locked build,
px.py PNG pixel/zoom tool, trscan.py / fpdump.py (per-batch dataflow of a draw's fragment program) /
fpfit.py+fpfit2.py (fit pipeline timing to all programs in a trace)). Shots: gui\shots\before_*.png / after_*.png.
Library: 5 tagged 20 s MP3s synced via zmtp --port 3250 (gui\mp3). Defects vs the device photos:
1. FIXED main-menu left column (Playing/Pins/History/New) = horizontal streaks (7c OPEN #3). Linear texture pitch is
   row bytes rounded up to **64** (was 32): the column is rendered into an 84x354 RT with pitch 384 and sampled back
   (also the album-row 120x64 RTs, pitch 512). 272-wide (1088) unaffected.
2. FIXED music-hub album rows: yellow/green striped bars right of the title (7c OPEN #2). That element is a 2-pass
   separable 9-tap blur (48-batch program, ALU_BUFFER_SIZE 4) of the album's track names rendered in a 6 px font into
   a 120x64 RT. Three root causes:
   a. ALU write mask = destination format (grate assembler-syntax.md): "lh" -> one fp20, "l"/"h" -> fx10 half,
      none -> no write (was: lh = two fx10 halves, none = fp20). Texcoords are fp20 "r2.lh" results.
   b. ALU3 immediates are NOT word-swapped: imm word = alu[w+1] << 32 | alu[w] (verified: blur weights
      .176/.12/.076/.039/.017, tap offsets 1/6..5/6; old read gave imm1 = 573.3).
   c. Pixel-packet rows + pipeline skew (new frag_run): 0xe20[1:0]+1 = N rows; batch b works on row b % N:
      MFU varyings, TEX coords/result, PSEQ load, DW source all address r(4*row)..; ALU regs are absolute r0-r15.
      Unit u of batch b runs at time (b/N)*L + b%N + D[u], L = max(N,6), D = MFU 0, TEX 1, PSEQ 4.25, ALU 4.5,
      DW 8 (events sorted once per draw, fp_schedule). Derived from data-flow constraints of every program in the
      UI (blur ALU b0 uses r14/r15 that MFU b3 interpolates; its rt1 DW at b44 stores the blend of b45-47; 2-row
      programs must read clip distances r4/r5 before MFU b3 reuses them). Old "k-th MFU -> r4k" model is the
      special case. Also MFU SFU ops (rcp/rsq/lg2/ex2/sqrt/frc, result written back to the row register) and MFU
      multipliers with row destinations are now executed.
   Side effects (verified): ring + play icon now drawn left of "music" in the main menu and on every album tile
   (radial-gradient program with SFU sqrt; device photo shows the ring); unlock-arrow right half fixed (7c OPEN #1).
3. FIXED diagonal line across translucent tri_fan quads (now-playing art placeholder, album tiles): no fill rule, the
   shared edge's pixel centres were blended twice. edge_owns(): one of the two edge directions owns on-edge pixels.
4. Debug aids (env, off by default): ZUNEHD_GR3D_DUMP="ADDR,LEN,DIR" (dump rt1 after draws into ADDR; ADDR 0 =
   every offscreen draw, scissor < 272), ZUNEHD_GR3D_FRAG="X,Y,NB" (log the 16 row registers after every unit event
   of fragment X,Y in NB-batch programs; -d unimp), ZUNEHD_FP_TIMING="L,dm,dt,dp,da,dd" (override the schedule).
OPEN / not changed:
- Album-row blur output is still not right: faint reddish 6 px track title under the artist (expected a neutral glow).
  Traced pixel ends with R .37, G .19, B .19, A .01 -> per-channel accumulators still see different taps; the
  program also uses PSEQ words 0x0124xxxx (bit 23 clear) and spill DWs to rt4 (fmt 20) - probably the real
  recirculation/branch semantics. gui\fdbg2.txt has a full register trace of one pixel.
- Big panorama headers ("setti", "ABO", "ZUN", "mus") are 0x66 grey; the program draws white with u3 alpha 0.4, so
  the emulator is right per the shader; the photos look brighter (camera/LCD gamma) - unverified, not changed.
  Same for the now-playing art placeholder (white at 20 % = 0x33; photo looks mid-grey).
- Coordinator's report "now-playing art square turns cyan (avp=on)": not reproduced with this build (avp=on run in
  gui\shots\avp2_*.png: grey placeholder, then the idle zoomed-text animation); may have been the old pipeline.
- About > privacy dialog: the tapped "PRIVACY" item text faintly shows through the top of the dialog (white on the
  0xE6 dialog) - z-order/animation question, unverified.
- Songs list: no "#"/letter jump tiles (device photo has "#"); probably only shown for larger libraries - unverified.
- Device name line ("<OWNER>'S ZUNE HD") and BOOTLOADER 0.0 (0) on About > Zune are content, not rendering.
Session 9b UPDATE: FIXED (held settings item now glows white, album-row glow neutral). Root cause = the
4-row blur programs need (a) the spill buffer and (b) different ALU-send semantics and unit timing:
- Spill buffer (rt4) modelled per fragment (frag_spill_store/load in zunehd-gr3d.c): DW with rt 4 saves the
  batch's whole row (4 regs); DW bit 0 set -> single slot, bit 0 clear -> FIFO. PSEQ with rt field 4 and bit 23
  clear reloads ONE register into row reg (word & 3): bit 8 clear -> slot register bit 6; bit 8 set -> next FIFO
  entry, register bit6 ? 3 : 1. (Blur: slot = centre texel, FIFO = per-channel partial sums.)
- ALU send: sender contributes rA*rB only, added to the receiver's result; the receiver keeps its own rC
  (old: receiver rC := sum of senders' full results -> the accumulator counted 2-3x). ZUNEHD_ALU_SEND=B|C = old.
- Timing refit: L = max(N, 8), D = MFU -5, TEX -3.5, PSEQ -2.5, ALU 0, DW 3.5.
- Derivation: gui\fpsym.py (+ fpgreedy.py / fpalt.py): with these rules every channel of all four blur draws
  (glow pass 1/2, album pass 1/2) is the same 11-tap kernel 0.176 + 2x(.16 .12 .076 .039 .017) = 1.0, every
  spill load has exactly one valid source, and all other UI programs (text, clipped text, compositor, radial
  gradient, solid fill) give the same symbolic result as before (fpcheck.py).
- Verified on screen: gui\shots\after_settings_hold_glow.png (+_zoom; 1500 lit px, max R/G/B spread 1) vs
  before_settings_hold_glow(.png/_zoom.png); after9b_albums.png / after9b_album_row_zoom.png (neutral faint glow of
  the 6 px track title); no regressions: after9b_menu.png, after9b_lock.png (full unlock arrow),
  after9b_nowplaying.png, after9b_songs.png.
- Caveat: the spill field rules are fitted to the only two spill-using programs seen (same shader, two passes);
  bits 3:2 / 7 of the reload word and the DW bits 8..16 are not interpreted.
Earlier analysis (kept for reference): NOT FIXED at first, root cause narrowed down.
- Repro: gui\holdsteps.txt (unlock, scroll, settings, hold DISPLAY). Shot gui\shots\before_settings_hold_glow.png
  (= final9b_hold2.png, unchanged). The glow uses the SAME 48-batch, ALU_BUFFER_SIZE 4 program pair as the album-row
  blur (pass 1 into a 272x48 RT, pass 2 onto the layer; draws 3130/3132 in gui\tr_hold.bin). G == B exactly because
  both come out of near-identical channel chains; R and A pick up extra terms.
- Offline tools: gui\fpsym.py (symbolic run of a program from a trace: which texel channels reach each output
  channel, under the emulator's timing model), fpsearch.py / fpsearch2.py (brute-force the model space: unit
  offsets, pass length L, per-unit row offsets, send/recv variants, ALU register rotation, texel half swaps).
  Result: under the current model pass 2 gives R = 2.0*texB + 0.26*texR, G = 0.32*texB + 0.50*texG + garbage,
  B = 0.82*texB + garbage. NO combination in that model family gives clean per-channel sums of several taps.
  Data-flow contradiction independent of timing: TEX batch 15 (row 3 -> r12/r13) would overwrite the TEX batch 7
  result that ALU batches 12/13 (R, A) and 25 (B, G) both consume. So in these programs values are also moved
  between rows: almost certainly through the spill buffer (rt4, fmt 20, pitch 1216). PSEQ words 0x0124xxxx have
  rt field 4 with bit 23 clear (low bits 0x009/0x045/0x006/0x04a/0x10d/0x18d/0x14d), and the DW words
  0x00013552/0x0001b552/0x00017d52 (bit 0 clear, rt 4) look like spill stores. These are ignored today. NEXT: decode
  the spill slot fields (DW bits 8..16, PSEQ low 9 bits) and model rt4 as per-fragment spill storage, then re-run
  fpsym/fpsearch until the blur comes out as clean weighted sums (should total ~1.0 per channel).
- Also tried in the emulator: ZUNEHD_ALU_SEND=A|B|C (send semantics). No visible improvement; default stays C
  (unchanged behaviour). ZUNEHD_FP_TIMING and ZUNEHD_GR3D_FRAG remain the main debug knobs.

### Session 9: USB/IP with a real Windows host (usbip-win2)
- usbip-win2 0.9.7.8 (winget vadimgrn.usbip-win2; installed by the user - auto mode refused to
  install a kernel driver). Our server answers OP_REQ_DEVLIST: "1-1 Microsoft Corp. Zune HD Media
  Player (045e:063e)", attach -> High Speed, Windows binds its in-box Zune driver (oem62.inf,
  "Zune", class WPD, UMDF WUDFRd) and runs ~1300 normal MTP transactions, no errors. The import
  still shows the known first-SET_ADDRESS timeout + retry (harmless).
- Explorer: the Zune INF sets Device Parameters PortableDeviceNameSpaceExcludeFromShell=1,
  ShowInShell=0 (Microsoft hid Zunes from Explorer). Setting 0/1 (+ EnableLegacySupport=1) and
  re-attaching shows This PC > Zune > Storage (listed twice: one per device interface GUID
  6AC27878... / CA3D7387...). Shell CopyHere both ways verified byte-exact; uploads are renamed by
  the device from tags ("01 S70.mp3"). Shell enumeration is sometimes empty right after attach
  (retry after a second or two).

### Session 9: interactive use - host cursor + 3D speed-up
- run-zunehd.ps1 passes `-display sdl,show-cursor=on` (also gtk): the guest draws no pointer.
- Profiling (ZUNEHD_PROF=1 -> "prof: gr3d X% of wall time, N draws, ms/draw" every 5 s): before, 3D
  draws took up to 95% of wall time during UI animation (12-50 ms per draw, ~300 ns/pixel); QEMU sat at
  exactly 1 host core (machine has 32). Two changes in zunehd-gr3d.c:
  1. rd()/wr() use a direct host pointer into SDRAM (reads anywhere in RAM, writes only in the GPU
     carveout >= 0x06000000) instead of address_space_read/write per texel/pixel. No dirty tracking
     exists in the machine (DC re-reads the framebuffer each refresh), so this is safe.
  2. Raster worker pool: each triangle's rows are interleaved over ZUNEHD_GR3D_THREADS (default 8,
     1 = off; forced off with ZUNEHD_GR3D_FRAG) threads, the vCPU thread is worker 0 and waits for all
     rows before the next triangle -> unchanged draw/blend order. Triangles < 4096 px run inline.
  Result: same UI script, ms/draw 12.6 -> 2.4 (busiest window 95% -> 27% of wall time); screenshot
  pixel-identical to the single-threaded run.
- Remaining CPU: TCG guest execution; the OAL idle loop polls (no WFI) so QEMU always shows 1 busy core.

### Session 7c: lock-screen graphics fixed (user priority; reference photo shots\ref_lockscreen_device.jpg)
- Ground truth: the lock wallpaper is skin_default.gem PNG #2 (274x482; tools carve: extracted\skin_png\).
- R/B swap: textures and the 3D back buffer are RGBA (byte0 = R, verified: PNG px (21,9,9) = texture bytes
  21,9,9,255); the display window is B8G8R8A8 (depth 12). The compositor's class-0x52 stretch blit converts:
  0x01C sbformat = dst<<8|src, guest uses 0x0f0e (src 14, dst 15; grate: 14 BGRA8888, 15 RGBA8888) ->
  g2_execute swaps bytes 0/2 when src/dst are 14/15 or 15/14. Fixed every colour image (the old
  "orange boot bar" question is answered by this).
- Short swizzled textures (gr3d tex_swizzle_offset): the 1 KiB tile shrinks in height: tile h = max(2, h)
  pow2, <= 16; bits x0 y0 x1, then remaining y bits, then x bits (128x1 bar gradients: x0 y0 x1 x2..x6,
  4 KiB apart; verified from memory). Fixed the white line across the top (shadow ramp misread) and the
  missing unlock bar body.
- Vertex index = DRAW_PRIMITIVES.OFFSET + k; DRAW_PARAMS.FIRST is NOT added (grate-mesa writes start into
  both). UI quads are tri_fan FIRST=1 OFFSET=0: adding FIRST read vertex 4 past a 4-vertex buffer ->
  half-drawn wallpaper quad, garbage clip distances (arrow killed), battery-icon line.
- desc2 without 0x40: [31:28] log2 w, [27:24] log2 h (g3dsum/g3ddis print these as "Nx1" - display bug only).
- Layer composition: the UI layer RT is stored y-flipped (vp +3840) and the compositor flips it (vp -3840):
  screen y = 480 - layer y. Only DC window A is used (272x480, depth 12, base 0x061EC000).
- Debug aids added: ZUNEHD_DCLOG=1 (DC window state changes), unimp log now has per-draw TRAM rows 0/1 of
  v0..v3 and the first killed fragment ("kill (x,y) r0..7"); SB sbformat logged once per value.
  tools\raw2png.py (raw RT dump -> PNG, --alpha bg), tools\ppmcrop.py (crop/zoom a screendump),
  tools\carvejpg.py. zseq raw steps: "pmemsave ADDR LEN FILE", "screendump FILE.ppm" (kept, not converted).
- OPEN gfx: (1) unlock arrow right half: left fan triangle black (correct), right one gives per-column
  values 80/206/254/128/0 - in its later DW stores (rt0 f11 / rt3) or the blend; (2) music-hub album rows show
  striped bars right of the title (yellow, were cyan); (3) faint streaks left of the main menu (x<75,
  y 215-320); (4) program DW "store stencil" targets rt2 fmt 3 with base ~0 (px_store ignores fmt 3; a real
  stencil format would write low RAM - add a base check before implementing fmt 3); (5) warn: "render target
  format" (f11, fmt 3) and "MFU special function" (sfu rcp etc. ignored).

### Session 10: board peripheral map probed on the real device (full tree in C:\rea-work\tree.md)
- ODM peripheral DB = .data of k.libnvodm_query.dll (0xC09C6000, 0x1930 B; device\probe\odm_query_data.bin).
  Entries 24 B {u64 GUID (8 ASCII chars, little-endian), addr list ptr, count, class}; addr entry 12 B
  {module, instance, address}: 6 GPIO (port, pin), 12 I2C (8-bit addr), 13 I2C_PMU, 18 SDIO, 22 SPI, 27 PMU rail.
  Table selector FUN_c09c4858 by board type: 1-2 -> +0x058 (0x27 entries), 7 -> +0x400 (0x28), 3 -> +0x7C0
  (0x27), 4-6/8 -> +0xB68 (0x25), else (0, >=9, our type 10) -> +0xEE0 (0x25). Confirmed live: kcall
  NvOdmPeripheralGetGuid (0xC09C4938) -> tpktouch 0xC09C7168, accelero ..7198, hd_radio ..7228, blutooth 0.
- Type 10 entries: wolf8350/wm__pmu0 I2C_PMU 0x34; tpktouch I2C inst 1 (addr from driver), GPIO O7, D7;
  accelero I2C inst 0 0x30 (7-bit 0x18), GPIO U5; zkeypad0 S5, S6; ffa_lcd0 SPI inst 2 cs 2; hd_radio SPI
  inst 1, GPIO U1 P5 U4 V6; sdiowlan SDIO 0, GPIO U3 A4 P6; ffa_hdmi I2C 0xA0 + I2C2 0x74; ffatvout;
  ffa__crt; vipstrea; enc28j60 (SPI 0, GPIO O6, dev-board leftover); NVDD_* rails (see tree.md).
- USER'S UNIT HAS THE AVAGO TOUCH CONTROLLER: live kernel module list has avago.dll, not synaptics.dll.
  avago.dll (FUN_c0af4884) uses 8-bit I2C address 0x66 = 7-bit 0x33 on I2C2 -> this is the "I2C2 0x33 polled
  reg 0x8E" device seen in emulator logs. The emulator's Synaptics model at 0x2C is the alternate supplier.
- Accelerometer = Kionix KXSD9 (zaccelerometer.dll FUN_c0ac1d48/c0ac1cac): reg 0x0D <- 0xC0 (CTRL_REGB),
  read 0x0C expect 0xE3 (CTRL_REGC default), else reset 0x0A <- 0xCA; range 2/4/6/8 g -> 0x0C = 0xE0|fs.
- HD Radio: \Windows\hdrfirmware.bin is a B000FF DSP image (IBOC strings ASIC_CLK, IbocMdAck, AcqSwap);
  first 15.5 KB in device\probe\hdrfirmware_head.bin. Baseband part number still unknown.
