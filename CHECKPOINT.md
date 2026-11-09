# Zune HD (Tegra APX 2600 / AP16, WinCE 6) on QEMU - checkpoint 2026-11-08 (session 6)

## START HERE (new session)
USER RULE (session 5): keep every QEMU run / timeout to 60 s max for now.

### Session 9 (2026-11-08) - LATEST
- USB -> WINDOWS WORKS (usbip-win2 0.9.7.8 installed by the user, C:\Program Files\USBip\usbip.exe;
  driver "USBip 3.X Emulated Host Controller"). `usbip list -r 127.0.0.1` / `attach -r 127.0.0.1
  -b 1-1` / `detach -p 1`. Windows binds its own Zune driver (oem62.inf, WPD, WUDFRd). Registry
  flags set on this PC (device instance ...063E\00000000-0000-0000-0000-000000000000\Device
  Parameters): PortableDeviceNameSpaceExcludeFromShell=0, ShowInShell=1, EnableLegacySupport=1 ->
  This PC > Zune > Storage in Explorer. Verified byte-exact: download Up_30s.mp3, upload+download
  s70.mp3 (1.1 MB; Windows renames uploads from tags to "01 S70.mp3"). The user's Zune desktop
  software was open meanwhile (behaviour with it not examined; MTPZ is off).
  run-zunehd.ps1 has -UsbIp. Only one USB/IP client at a time (zmtp OR usbip-win2).
- GUI: red tap-highlight / album-row glow FIXED (gr3d spill buffer rt4 + ALU send + timing; NOTES
  "Session 9b UPDATE"). Launchers default to avp=on + dsound audio (-NoAvp / -NoAudio).
- AUDIO WORKS (zunehd-ape.c, NOTES "Session 9: AVP audio vector engine"): recorded PCM ~bit-exact vs
  minimp3; live playback stutters (decode chain ~70 % of real time). NEXT for audio: speed.
- BOOT TIME CORRECTION: avp=on reaches the UI in < 30 s (device ~28 s: logo at 3 s, colour bar at
  15 s, lock at 28 s). The old "165 s" was the screen sleeping after 1 min (Home wakes it).
  AVP mailbox traffic is all within 3.9-7.8 s virtual; AVP idles at 0x060b1906 afterwards.
- RELEASE: C:\rea-work\release (bin\ = stripped exe + 16 MSYS2 DLLs, setup.ps1/run.ps1/sync-music.ps1,
  tools\b000ff2nb0.py + release copy of mkemmc.py, device\ non-secret captures, source\ GPL files,
  README.md, CAPABILITIES.md). No firmware, no PKV. Rebuild the exe + re-strip + re-copy
  source\ after code changes (deps: scratchpad deps.sh logic = ldd | ucrt64).
- FIXED: MTP SendObject of files > 1 MiB (two USB model bugs, NOTES "Session 9"). Up.mp3 10.5 MB syncs
  byte-identical in ~12 s. ZUNEHD_USBDTDLOG=1 traces ep1 dTDs.
- Parallel work: a GUI sub-session fixes rendering vs the user's device photos (work dir C:\rea-work\gui,
  own eMMC image, monitor port 4455, private QEMU copy; results in NOTES "Session 9 (GUI vs device photos)").
  Build lock: qemu\.build.lock (see NOTES Session 9). Stop your own build\qemu-system-arm.exe before building.
- DONE: BSE (MP3 Huffman engine) bit-exact vs minimp3 (NOTES "Session 9 (BSE validation)").
- DONE: APB DMA + I2S1 model -> QEMU audio (zunehd-apbdma.c, NOTES "Session 9: APB DMA"); the AVP mixer
  streams buffers continuously, BUT they are all zeros (silence). CAUSE FOUND: the decoder runs IMDCT +
  polyphase synthesis on an unmodelled AVP "audio vector engine" at 0x6000E000 (+0x74 program addr,
  +0x70 = 0xA0000000|(words-1) kick, +0x78 fence); its PCM staging stays zero. NEXT: write
  qemu\hw\arm\zunehd-ape.c (program format + 30 opcodes partly decoded: tools\apedis.py, NOTES
  "Session 9 (BSE validation)"), validate against minimp3 (tools\mp3pcm.exe, tools\pcmcmp.py).
  BSE output now sign*|x|^(4/3) Q13 (inferred scale; ZUNEHD_BSE_RAW=1 = old raw ints).
- DONE (GUI agent): menu left column, album-row bars, unlock arrow, double-blended quad diagonals
  (NOTES "Session 9 (GUI vs device photos)"); open: faint reddish text in album-row blur.

### Session 6 state - USB
- WORKS: cable plug -> PHY VBUS wake-up -> NvDdkUsbfResume -> bus reset -> full enumeration + SET_CONFIGURATION
  (dQH/dTD engine in zunehd-usb.c, built-in self-test host). Device 045e:063e "Microsoft Corporation"/"Zune",
  1 vendor interface: bulk 0x81 IN / 0x01 OUT (512), intr 0x82 IN (64); MS OS string 0xEE "MSFT100" code 0xFE.
  Test: `$env:ZUNEHD_USB_PLUG_AT='26'; $env:ZUNEHD_USB_SELFTEST='1'; powershell -NoProfile -File tools\zseq.ps1
  -Seconds 58` (descriptors + "usb host:" lines land in zunehd-stderr.log; add ZUNEHD_USBLOG=1 for registers).
- BLOCKER: MTP never reads ep1 OUT because MtpSvc.dll init fails: KEY1: (keyvault.dll) cannot unlock the PKV
  key vault. Done: boot args +0x1E0/+0x1E4/+0x1E8/+0x1F4 from the device, NVIDIA PT + PKV partition read
  from the device and written into the image (mkemmc.py FRONT_FILES) -> KEY_Init passes, KEY1: opens.
  Remaining: the 64-byte PKV header is AES-encrypted with the chip key.
- NEXT STEP (needs the Zune AWAKE, screen on - Lyra RPC <zune-ip>:1337 is unreachable while asleep):
  1. `py -3 tools\pkv_unwrap.py` -> device\pkv_hdr_plain.bin (device decrypts its own header via kcall of
     keyvault FUN_c0b35b48; checks live code bytes first; leaks 2x64 B kernel heap until reboot).
  2. Verify offline: SHA-256(plain[0x30:0x40] || PKV[0x40:size] || "PKV" tag) == plain[0x10:0x30]
     (size = DAT_c0b4e13c at runtime; tag ptr at 0xc0b3901c - confirm exact bytes before trusting).
  3. Emulation: write the plain header into PKV in the image, patch `bl FUN_c0b3b2f4` at 0xc0b38f78 (keyvault)
     into a 64-byte copy (byte-match guarded loader patch like zunehd_avp_patches). Check other callers of
     FUN_c0b3b2f4 / FUN_c0b35b48 (later IOCTLs may need the chip key too).
  4. Re-run self-test: expect MUT_Open + ep1 OUT prime + MTP GetDeviceInfo response.
  5. Then USB/IP server (TCP 3240) on the zune_usb_dev_* API, usbip-win2 on the host (user approved).
- PC-side reference for USB bringup (session 7): https://github.com/kbhomes/libmtp-zune (LGPL-2.1, libmtp +
  Zune MTPZ). mtpz.md = handshake walkthrough (host->dev app cert msg 02 01, dev->host response 02 02 with
  "Zune Device CA 1"/"Zune Device" certs + RSA sig + CBC-MAC/SHA-256, host->dev confirm 02 03);
  src/mtpz.c, src/mtpz.h, src/music-players.h. src/.mtpz-data = host key material, do not fetch/commit.
  Device side of MTPZ = the certs/keys MtpSvc pulls from KEY1: (keyvault) -> see NOTES "Session 7".
  SESSION 7 DONE: plain MTP WORKS (replaces 1-3 above; no device access needed). MtpSvc patches (init ignores
  KEY1 failure, MTPZ off) in mkemmc.py EXT_PATCHES (EXT is paged from ZBINFS!) + zunehd.c; zbinfs page-hash
  check patched in zunehd.c. Self-test GetDeviceInfo -> 885-byte DeviceInfo ("Microsoft"/"Zune"/04.05.00114).
  Test: `$env:ZUNEHD_USB_PLUG_AT='40'; $env:ZUNEHD_USB_SELFTEST='1'; tools\zseq.ps1 -Seconds 58`.
  GOTCHA: rebuild zunehd-emmc16.img (mkemmc.py) after a crashed boot - guest-written state broke later boots.
  SESSION 7b/c: USB/IP server + tools\zmtp.py: MEDIA SYNC WORKS (albums/artists show in the music hub).
  Lock screen graphics fixed (R/B swap in SB blit, short-texture swizzle, tri_fan vertex index).
  Sync test: `$env:ZUNEHD_USBIP='3240'; tools\zseq.ps1 -Seconds 120 -Steps ...` (background), then
  `py -3 -I tools\zmtp.py sync a.mp3 b.mp3 ls` (tools\mksilentmp3.py makes tagged test MP3s).
  Unlock in zseq: "48:key home; 51:mouse_move -2000 -2000; 51.3:mouse_move 136 420; 51.6:mouse_button 1;
  51.8..52.6: mouse_move 0 -60 (x5); 52.8:mouse_button 0". Music hub ALBUMS tap = 75,123.
  SESSION 8 (paused): VDE bitstream engine (MP3 Huffman) modelled in zunehd-bse.c, NOT yet exercised.
  BLOCKER: MTP objects > ~1 MB never complete SendObject (device scans eMMC sector by sector, see NOTES
  Session 8 "OPEN, NEXT SESSION FIRST"). Small files sync fine. Test audio: "Test audio\Up.mp3" and
  "Test audio\Up_30s.mp3" (tools\mp3cut.py). After that: play with avp=on + ZUNEHD_BSELOG=1, then
  APB DMA + I2S1 + WM8350 -> QEMU audio. Useful new tools: tools\raw2pe.py, mp3cut.py, mksilentmp3.py.
  Device (Lyra) available: user said read as needed (still targeted reads only).
  SESSION 7f: CPU<->AVP RPC WORKS (AVP SDRAM alias 0x90000000); avp=on boots to the UI; playing an MP3
  makes the AVP decoder busy-wait on the VDE (0x6001A000..0x6001DB00, poll 0x6001CC14). NEXT (user
  decision): model the VDE parts the audio decoder uses, then APB DMA + I2S1 + WM8350 -> host audio.
  SESSION 7e: AVP RUNS as a 2nd CPU (zunehd-avp.c, ti925t ARMv4T, cluster 1): boots its kernel, takes
  the CPU's init mailbox message, creates RPC ports, idles waiting for the CPU to connect. NEXT: find the
  CE thread that blocks after "ResetVector = 60abf30" with avp=on (NOTES 7e). Default boot unaffected.
  SESSION 7d AUDIO (user priority now; media-browse UI artifacts parked in NOTES 7c OPEN): avp=on now
  boots to "ResetVector = 60abf30" (divide-by-zero fixed). NEXT: emulate the AVP as a second CPU + its
  MMU/ICTLR/timer/mailbox (plan + all addresses in NOTES Session 7d), then APB DMA/I2S/WM8350 -> host audio.
  Previously: remaining gfx (NOTES 7c OPEN list), then audio (AVP not emulated; WAV_Init
  patched off; nvmm_mp3dec.axf etc. are AVP codecs), usbip-win2 + real host software later.
  Details in NOTES Session 7/7b/7c.
  Alternatives on the host: Zune Explorer (NiceBeard/zune-explorer) or the Zune software via usbip-win2.
- Device access log (session 6, all via Lyra, user-approved "read it from my Zune"): kernel u32 reads of boot
  args (0x80001000+), read-emmc-front.py --block 6144 --count 8 (PT, 16 KiB) and --block 434688 --count 64
  (PKV, 128 KiB). device\front_PKV_434688.bin = device-unique secrets: keep private, never publish.
- Open side issue: once, a SETUP arriving in the same ISR as the previous status-stage completion left the
  driver polling ENDPTPRIME/ENDPTSTATUS (see NOTES). Not reproduced since.

### Session 5 state
THE ZUNE HD MAIN MENU RENDERS (~45 s into boot): music / videos / pictures / radio /
social / internet / settings with sub-items (shots\shot_009.png). Chain of fixes that got there:
 1. host1x sync-point threshold IRQs + indirect INDOFF/INDDATA + output FIFO (3D context save/restore) -> zsplash exits.
 2. eMMC exFAT volumes now formatted like the device (bitmap, standard 2474-byte up-case table copied from the
    device, EMPTY label; mount names come from the partition records). Before, \Flash2 never mounted ->
    zconfig_serv ZCO_Init timed out (waits 30 s for OsPartition=Flash + DataPartition=Flash2) -> no later services
    -> ZAM (zam_serv.dll, launches DefaultApp gemstone.exe) never loaded.
 3. 3D texture DESC2: non-pow2 bit 0x40 -> w/h fields, else log2 w [31:28] / h [27:24]; 1-bpp tiled layout found.
 4. GPIO controller (0x6000D000) + Synaptics RMI4 touch (I2C2 0x2C, ATTN = GPIO U0 active-low level) + QEMU
    absolute pointer (left button = finger). Driver probes it fine; NOT yet tested interactively (user: run
    .\run-zunehd.ps1 and click). Raw->screen axis orientation unverified (may need X/Y flip).
Text colour FIXED (white/grey; ALU 'accumulate this' bit). Clipped text on Music/Settings FIXED (pipeline timing + register groups, see NOTES). Mouse works (Y flipped). Buttons all work: Home (H/Home), media (M), power (End/P; sleep/wake).
USB BRIDGE IN PROGRESS (user APPROVED installing usbip-win2 on the host and copying the device's devcert.dat
into \Flash2 of the emulated eMMC for MTPZ). Plan: ChipIdea device controller model + DMA + USB/IP server
(TCP 3240) -> usbip-win2 attach on the host -> Zune software or Zune Explorer (github NiceBeard/zune-explorer,
MIT, JS MTP/MTPZ host; Zune HD = 045e:063e) as the PC side.
Done so far (session 5d): qemu\hw\arm\zunehd-usb.c/.h (stage A: registers, reset, OTGSC/VBUS, IRQ 20;
ZUNEHD_USBLOG=1 logs to stderr; TEMP key U toggles the "cable"). NvDdkUsbfOpen now succeeds (patch: USB PHY
accepts the QuickTurn 8330 kHz oscillator; APB_MISC+0x28 bit7 PHY clock stable). Driver programs the
controller (USBMODE 2, USBINTR 0x157, ep0, RS=1). dQH list at 0xC5001000 = controller-local memory
(session 6: model now maps RAM over the rest of the 1 MiB aperture). Stage B and later: see Session 6 above.
bar orientation now orange-top with OR without the texture flip (flip removed): likely render-target/compositor
orientation question; menu text is upright, so check against device before touching it.
Next: user test of mouse; colours; remaining GPU features; remove g_debug_frag; host-GPU backend later.
Tools: tools\zgdbat.ps1 (attach gdb after N wall seconds). Idle-count breakpoints (IDLE_SKIPS) are EARLY in boot.

Quick commands:
- Build QEMU:  `$env:MSYSTEM='UCRT64'; & C:\msys64\usr\bin\bash.exe -l /c/rea-work/qemu/zbuild.sh`  (log qemu\build-zunehd.log)
- Boot + logs: `powershell -NoProfile -File tools\zcapture.ps1 -Seconds 120 -DebugFlags unimp -Emmc 0 [-Screenshot] [-DumpRam]`
  (serial -> zunehd-serial.log; paste the full serial log to the user after every run)
- Screens during boot: `tools\zshots.ps1`;  interactive window: `.\run-zunehd.ps1` (SDL, Ctrl-A X quits serial)
- 3D trace: set `$env:ZUNEHD_GR3D_TRACE='C:\rea-work\gr3d-trace.bin'` before zcapture; then
  `py -3 -I tools\g3dsum.py gr3d-trace.bin`, `py -3 -I tools\vpedis.py gr3d-trace.bin --draws 0,1`,
  `C:\rea-work\tools\g3ddis\g3ddis.exe gr3d-trace.bin --first 0 --count 2` (needs C:\msys64\ucrt64\bin in PATH;
  rebuild via tools\g3ddis\build.sh - user approved building grate code).
- All-thread dump at idle: `tools\zthreads.ps1`; any gdb script: `tools\zgdb.ps1 -Script x.gdb`;
  breakpoints: `tools\zbreakm.ps1 -Addr "0x..,0x.." -Emmc 0`.
- eMMC image: `py -3 -I tools\mkemmc.py` (rebuilds zunehd-emmc16.img incl. formatted exFAT).

# History below (sessions 1-4). Latest technical detail: "Session 4" section near the end.

Goal: analyse the stock Zune HD firmware and boot it in a custom QEMU machine.
Register-level details live in NOTES.md. User preferences: paste the full serial boot log after every run;
when reading from the real device, do targeted small reads only - no full dumps.

## Environment
- Windows 11, PowerShell 5.1. Python is `py -3` (python.exe is the Store stub). No WSL.
- MSYS2 (C:\msys64, UCRT64 toolchain, gdb-multiarch, git) installed via winget.
- QEMU v11.1.3 source in C:\rea-work\qemu, build dir qemu\build (arm-softmmu, SDL, no docs).
  Build: `$env:MSYSTEM='UCRT64'; & C:\msys64\usr\bin\bash.exe -l /c/rea-work/qemu/zbuild.sh` (log: qemu\build-zunehd.log).
  Do not pass quoted pipes through `bash -lc` from PS 5.1; put commands in a .sh file.
- Ghidra via the REA MCP tools only accepts these ARM PEs after patching the PE machine field 0x1C2 -> 0x1C4
  (copies in extracted\NK\raw\*_armnt.*, e.g. libnvddk_misc_armnt.dll, libnvemmc_armnt.dll).
- Real device: user's 16 GB Zune HD running Lyra (zuneslayer) at <zune-ip>, RPC daemon TCP 1337 (wakes with
  the screen; ARP fails while asleep). Lyra host tools in ext\project-lyra\tools (stdlib python), e.g.
  `py -3 ext\project-lyra\tools\re\read-raw-emmc-range.py <zune-ip> --start N --count K --chunk K --out F`.

## Files created or modified
QEMU tree:
- qemu\hw\arm\zunehd.c (new, ~1800 lines): the `zunehd` machine, `zunehd-hsmmc` device, host1x + sync
  points, display controller + QEMU console, 2D engines (G2 fill/copy, SB blit), AVP/audio image patches
  (machine prop avp=on disables them), 3D method trace (ZUNEHD_GR3D_TRACE).
- qemu\hw\arm\zunehd-gr3d.c / zunehd-gr3d.h (new): 3D engine interpreter (class 0x60 / AR20).
- qemu\hw\arm\meson.build (zunehd.c + zunehd-gr3d.c), qemu\hw\arm\Kconfig: CONFIG_ZUNEHD.
- qemu\scripts\symlink-install-tree.py: copies instead of symlinking on Windows (no Developer Mode).
- qemu\zbuild.sh: incremental build helper.
Workspace:
- NOTES.md (detailed findings), CHECKPOINT.md (this file).
- run-zunehd.ps1: interactive launcher; attaches zunehd-emmc16.img by default (-NoEmmc / -EmmcImage).
- zunehd-emmc16.img: sparse 16 GB raw eMMC image (user's geometry), built by tools\mkemmc.py.
  zunehd-emmc.img: old blank 32 GB image, no longer used (can be deleted).
- device\: the only data read from the real device (38 DSK1 sectors = 76 KiB): MBR+EBRs and ZBINFS samples.
  mkemmc.py copies the partition-table sectors from here and self-checks against all of them.
- extracted\: NK/EXT/ZBoot/Recovery .nb0 flat images, rebuilt PE modules (modules\), ROM files (files\),
  modules.txt per image (vbase/entry/sections).
- tools\ce\: CeBin.cs, CeExtract.cs, FindOat.cs + dump/extract/findoat.ps1 (B000FF parser, ROMHDR/TOC dump,
  PE rebuild, OEMAddressTable finder). Run via `powershell -NoProfile -File tools\ce\extract.ps1`.
- qemu-changes\: backup copy of every modified/new QEMU source file (same relative paths as in qemu\),
  refreshed at the end of session 4. Copy back over a fresh QEMU v11.1.3 tree to restore.
- ext\grate (MIT, docs + disassemblers) and ext\grate-mesa (sparse) fetched by tools\fetch-grate.sh.
- tools added in sessions 3-4: zgdb.ps1, zthreads.ps1/.gdb, armnt.py (Ghidra copies), ordnames.py (ordinal
  imports -> names), ppm2png.py, zshots.ps1, g3dsum.py, vpedis.py, g3ddis\ (g3ddis.c, build.sh, g3ddis.exe).
- shots\: boot screenshots (shot_004.png = first rendered frame with the bar).
- gr3d-trace.bin: 3D method trace of a 120 s boot.
- tools\mkemmc.py: builds zunehd-emmc16.img (front reserve zero except FRONT_FILES = NVIDIA PT @native 6144 and
  PKV @native 434688 from device\; MBR/EBRs, ZBINFS = NK.nb0 + EXT.nb0@0xBC0000, formatted exFAT).
- Session 6: qemu\hw\arm\zunehd-usb.c/.h (USB controller + endpoint engine + self-test host; env
  ZUNEHD_USBLOG, ZUNEHD_USB_PLUG_AT, ZUNEHD_USB_SELFTEST), zunehd.c (APB_MISC 0x70/0xA30 hooks into the USB
  model, boot args +0x1E0..+0x1F4 from the device). tools\pkv_unwrap.py (device-side PKV header unwrap).
  device\front_6144_16k.bin (PT), device\front_PKV_434688.bin (PKV, SECRET). qemu-changes\ refreshed.
- tools\zcapture.ps1: boot N s (-DebugFlags, -DFilter, -DumpRam, -Emmc 0 [-EmmcImage]); writes zunehd-serial.log,
  zunehd-mmio.log, zunehd-regs.txt. -Emmc now always attaches the card to the HSMMC (if=sd,index=0); -EmmcSlot obsolete.
- tools\zlog.py (summarise huge -d logs), zsym.py (address -> module+offset), zbreak.ps1 / zbreakm.ps1 (gdb
  breakpoint(s) + symbolised stack), *.gdb helper scripts.
- ext\project-lyra: reference clone (MIT) of community tooling/docs.
Memory: ~/.claude/projects/C--rea-work/memory.

## Firmware facts (short)
- B000FF images: NK.bin PA 0x14000 entry 0x23FB0; EXT.bin PA 0x01790000; ZBoot runs at 0x07000000. VA = PA + 0x80000000.
- OEMAddressTable @0x8004EAD0 (14 entries). OEMGLOBAL @0x80065DF0. OEMInit 0x800241E0.
- Boot-args block at PA 0x1000 (normally left by ZBoot). Machine fills: signatures, debug UART (type 3 = UART0),
  +0x1DC=4, +0x1EC..+0x1F8 = {0x11, HSMMC instance 0, minor 0, DSK1 base 0x1AF00 (2 KiB sectors)}.
- Storage, measured on the user's 16 GB unit: eMMC SEC_COUNT 31,653,888 (0x01E30000) x 512 B. Native blocks
  0..441343 = hidden boot reserve; DSK1 sector N = native 441344 + 4N (7,803,136 sectors of 2 KiB).
  DSK1: MBR ext @16 (7,803,120); Part00 ZBINFS 0x21 @24 (65,544); Part01 exFAT @65576 (65,544);
  Part02 exFAT @131128 (7,672,008). EBRs at 16 / 65568 / 131120.
- ZBINFS = NK.nb0 at partition offset 0 + EXT.nb0 at 0xBC0000 (NK length rounded to 64 KiB), zero padded.
  Verified sector-for-sector against the device. No partition dump is needed.

## What the zunehd machine models (all in zunehd.c)
1. B000FF loader for -kernel NK.bin and machine prop ext=EXT.bin; boot-args blob; initial SP 0x13FF0; PC=entry.
2. arm1136 CPU with ARM_FEATURE_VAPA.  3. 128 MB SDRAM, 256 KB IRAM; logging stubs over all OEMAddressTable regions.
4. APB_MISC HIDREV 0x00011600 + EMU_REVID netlist=1.  5. CAR PLLs pinned BYPASS; module 0x32 cmd self-clear; crypto idle.
6. Flow-controller us countdown; TMRUS; TMR1-4 (IRQ 0/1/41/42).  7. ICTLR + arbitration semaphores (IRQ 29).
8. 16550 UARTs at 0x70006000/6040/6200.  9. I2C DVC (WM8350 + RTC), I2C1, I2C2 - instant ACK.
10. Four generic-sdhci at 0xC8000000+n*0x200 (IRQ 14/15/19/31), no cards (SDMMC1 = Wi-Fi slot).
11. NEW: NVIDIA HSMMC @0x70008500, IRQ 22, with QEMU's TYPE_EMMC card behind it (`-drive if=sd,index=0`).
    Command on +0x10 write with HOLD(0x200) clear; response FIFO +0x34; DMA via +0x44/+0x40; status bits in NOTES.md.

## Session 3 (2026-11-07, later): boot now reaches GWES + display; screen black (needs 3D GPU)
Fixes, in the order they unblocked the boot (all in zunehd.c unless noted):
1. avp=off (default) image patches, applied in the B000FF loader only if bytes match (zunehd_avp_patches):
   NvRmLoadLibraryEx -> NvError 2 (PA 0x00734F3C), NvRmFreeLibrary -> 0 (0x00735044),
   WAV_Init -> 0 (libnvwavedev, PA 0x00658504). Audio is disabled: its mixer left threads holding an NvRm
   power mutex, which deadlocked the eMMC resume (NvRmPowerModuleClockControl) and all ZBINFS page-ins.
2. exFAT partitions formatted empty by mkemmc.py, reusing the device's 12-sector boot regions
   (device\dsk1_65576_boot.bin, dsk1_131128_boot.bin: 2 KiB sectors; Flash 2 KiB clusters, Flash2 16 KiB).
3. SPI/SLINK @0x7000D400/D600/D800 STATUS: flush bits self-clear, RDY forced (panel init over D800).
4. host1x model @0x50000000: channels (DMA PUT/GET/CTRL), command parser (SETCLASS/INCR/NONINCR/MASK/IMM/
   RESTART/GATHER; EXTEND ignored), 32 sync points (+0x3400/3500/3600/3700). Method 0 = INCR_SYNCPT.
   Classes seen: 0x01 host, 0x51 2D (G2), 0x52 stretch-blit, 0x60 3D, 0x70 display A.
5. Display controller A/B @0x54200000/0x54240000 (Tegra2-identical map; banked window regs 0x500-0x8FF via
   0x042), rendered to a QEMU graphic console (272x480 from DISP_ACTIVE 0x409). Vsync sync point via 0x028.
6. 2D engines: class 0x51 solid fill / copy (trigger = reg 0x009 -> 0x03A), class 0x52 copy/stretch
   (trigger 0x038). Works: per frame the compositor clears 0x062CB000 to black (alpha fades), 3D draws
   into it (NOT emulated), 0x52 copies it to the front buffer (0x061CB000 / 0x0624B000, flipped via ch0).
Result: GWES + libnvddi + NvDdkDisp run, window A enabled (B8G8R8A8 272x480 stride 0x440), ~50 fps flips,
but every pixel is black because the UI content is drawn by the 3D engine (class 0x60, ~700 jobs/run).
New tools: tools\zgdb.ps1 (boot + run any gdb script), tools\zthreads.ps1/.gdb (dump all CE threads at idle,
symbolized; NK process only so far), tools\armnt.py (Ghidra copies), tools\ordnames.py (ordinal imports ->
names + IAT VAs), tools\ppm2png.py; zcapture -Screenshot (zunehd-screen.png).
CE6 layout (fw 4.5): KData 0xFFFFC800 +0x08 cur thread id, +0x0C cur proc id, +0x8C cur PROCESS;
PROCESS +0x0C id, +0x10 thread list, +0x20 name; THREAD +0x00 next, +0x3C id, ctx +0x60 psr, +0x64 r0..,
+0x98 sp, +0x9C lr, +0xA0 pc. Process list head 0x80BEE010 (from Lyra).
NEXT: 3D engine (class 0x60) - at least a textured-quad HLE to see the compositor's layers; or real AVP.

## Session 4: 3D GPU (user chose faithful GPU emulation, ideally host-GPU accelerated)
- AP16's 3D engine IS the Tegra 2 "AR20" (class 0x60): every method falls in grate's documented blocks
  (0x100 IDX, 0x200 VPE, 0x300 linker/setup, 0x400 QRast, 0x500 PSEQ, 0x600 MFU, 0x700 TEX, 0x800 ALU,
  0x900 DW, 0xA00 FDC, 0xE00 global/RT).
- References cloned (via tools\fetch-grate.sh, MSYS2 git): ext\grate (MIT: docs\*.md ISA docs, include\tgr_3d.xml.h,
  src\libgrate disassemblers, src\libwrap\disasm.c) and ext\grate-mesa (sparse: src\gallium\drivers\grate|tegra).
- QEMU: ZUNEHD_GR3D_TRACE=<file> env var dumps every class-0x60 method write ({cls|ch<<16, off, val}).
  Trace in gr3d-trace.bin (120 s boot). tools\g3dsum.py summarizes per draw (own code).
- Findings: 99 draws / 120 s, ALL textured quads into RT1 = 0x062CB000 (fmt 13 RGBA8, pitch 1088) = the back
  buffer that the SB 2D copy puts on screen. Only 2 programs: VP 5 insns; FP 4 PSEQ steps, ALU 24 or 32 words,
  TEX 4. Draw 0: tri_strip 4 verts, tex 0x063C2000 272x480 fmt13; draws 1..98: triangles 6 verts, tex
  0x063BC000 1x380 fmt13. Viewport bias=scale=2176 (272*8) / 3840 (480*8, y flipped). RT0 = 0x0634AA00 f11 p576.
- tools\g3ddis\ (g3ddis.c + build.sh): replays the trace through grate's disassembler. NOT BUILT YET: building
  cloned external code needs the user's permission (auto-mode denied it). Ask before building.
- DONE (session 4b): qemu\hw\arm\zunehd-gr3d.c = CPU reference interpreter for class 0x60, hooked from
  host1x_method. FIRST GRAPHICS: the boot bar (shots\shot_004.png) renders: 48px column x112-160, gradient
  texture, rising from y=430. tools\zshots.ps1 takes a screenshot every N s during boot (shots\shot_NNN.png).
  Findings that differ from grate's docs/tools (AP16-specific or grate errors):
   * grate's VERTEX disassembler mis-decodes these programs; the documented bit layout with upload order
     word0=bits127..96 is right (tools\vpedis.py). grate's fragment disassembler is right (tools\g3ddis).
   * TEXTURE_DESC1 bit 4 ("compressed" in grate) = SWIZZLED layout here: 16x16-texel 1 KiB blocks,
     addr bits [2]x0 [3]y0 [4]x1 [5]y1 [6]y2 [7]y3 [8]x2 [9]x3 (y verified, x order inferred).
     Linear textures: pitch = align(w*bpp, 32).
   * DW (data write) executes one batch behind the ALU: DW[i] after ALU[i+1] (needed by the bar program).
   * Linker w1 nibble per export component c: [1:0] TRAM component, [3:2] type (3 fp20, 1/2 fx10 lo/hi).
   * Viewport units are 1/16 pixel (bias = scale = W*8, H*8 with Y flipped).
  Open question: colour channel order on screen (DC depth 12 = B8G8R8A8 shows tex byte0 as blue) - ask user.
- Plan (original): (1) disassemble the 2 programs; (2) CPU interpreter: attrib fetch -> VPE VLIW -> linker/TRAM ->
  raster (quads/tris) -> PSEQ/TEX/ALU/DW -> RT write, in zunehd.c behind a backend interface;
  (3) optional host-GPU backend (VPE/FP -> GLSL, render offscreen, read back).

## Boot status (session 2 run, 120 s, eMMC attached)
Kernel banner, OAL, NvRm clocks; full eMMC init (CMD0/1/2/3/9/7/13/16/6/6/8 + EXT_CSD); libnvemmc mounts DSK1
(reads MBR + EBRs at the right native blocks), ZBINFS walks NK and EXT TOCs, files are loaded from \Windows.
Serial then shows "ResetVector = 60abf30" (NvRm booting the AVP from the carveout) and the system goes idle:
CPU in OAL idle (PC ~0x8002B950), tick IRQ 42 running, polling 0x7000E450 / 0x7000E008, no further disk I/O.
The SDIO Wi-Fi probe no longer appears before this point.

## Next steps
1. Find what the boot waits on after "ResetVector = 60abf30" (likely an AVP handshake: shared semaphores /
   mailbox, IRQ 4 SHR_SEM inbox). Identify the caller (zsym.py on the thread stacks; zbreak on the reset-vector
   print) and fake the AVP's "ready" response.
2. exFAT partitions are unformatted: see whether CE formats them or fails, and format them in mkemmc.py if needed.
3. Audio clock (PLLA divide-by-zero), display controller / host1x, AVP proper.

## How to run
- Build image once: `py -3 -I tools\mkemmc.py` (~1 min, 16 GB sparse).
- Interactive: `.\run-zunehd.ps1` (serial on console, Ctrl-A X to quit).
- Capture: `powershell -NoProfile -File C:\rea-work\tools\zcapture.ps1 -Seconds 120 -DebugFlags unimp,guest_errors -Emmc 0`
  then read zunehd-serial.log; summarise zunehd-mmio.log with `py -3 -I tools\zlog.py zunehd-mmio.log`.
  HSMMC commands are logged as "zunehd: hsmmc CMDn ..." / "zunehd: hsmmc DMA ...".
- Debug: `tools\zbreak.ps1 -Addr 0x...` / `tools\zbreakm.ps1 -Addr "0x..,0x.."`.
