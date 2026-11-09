# Launch the Zune HD WinCE image on the custom "zunehd" QEMU machine.
#   .\run-zunehd.ps1                 # serial (UART instance 0) on this console
#   .\run-zunehd.ps1 -Trace          # also log every stubbed MMIO access to zunehd-mmio.log
#   .\run-zunehd.ps1 -Gdb            # wait for gdb on tcp::1234 (gdb-multiarch, set arch arm)
#   .\run-zunehd.ps1 -Image Recovery # boot Recovery.bin instead of NK.bin
#   .\run-zunehd.ps1 -NoEmmc         # boot without the eMMC image (zunehd-emmc16.img, built by tools\mkemmc.py)
#   AVP (media coprocessor, MP3 playback) and host audio (dsound) are ON by default:
#   .\run-zunehd.ps1 -NoAvp          # old mode: AVP/audio paths patched off
#   .\run-zunehd.ps1 -NoAudio        # no host audio device
#   .\run-zunehd.ps1 -UsbIp          # virtual USB cable via USB/IP on 127.0.0.1:3240 (busid 1-1):
#       py -3 -I tools\zmtp.py sync a.mp3     or   usbip attach -r 127.0.0.1 -b 1-1 (usbip-win2)
param(
    [ValidateSet('NK', 'Recovery')] [string]$Image = 'NK',
    [switch]$Trace,
    [switch]$Gdb,
    [switch]$NoExt,
    [switch]$NoEmmc,
    [string]$EmmcImage = '',
    [string]$Display = 'sdl',         # 'none' for headless
    [switch]$NoAvp,
    [switch]$NoAudio,
    [switch]$UsbIp                    # USB/IP server on 127.0.0.1:3240 (tools\zmtp.py or usbip-win2)
)
if ($UsbIp) { $env:ZUNEHD_USBIP = '3240' } else { Remove-Item Env:ZUNEHD_USBIP -ErrorAction SilentlyContinue }
$root = $PSScriptRoot
$bins = Join-Path $root 'ZuneHD_Firmware\Bins'
$qemu = Join-Path $root 'qemu\build\qemu-system-arm.exe'
# QEMU's DLL dependencies (glib, SDL2, pixman ...) live in the MSYS2 UCRT64 tree.
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$machine = 'zunehd'
if (-not $NoExt) { $machine += ',ext=' + (Join-Path $bins 'EXT.bin') }
if (-not $NoAvp) { $machine += ',avp=on' }

# The guest draws no mouse pointer (touch UI): force the host cursor on in the window.
$disp = $Display
if ($disp -in @('sdl', 'gtk')) { $disp += ',show-cursor=on' }
$qargs = @('-M', $machine, '-m', '128M',
           '-kernel', (Join-Path $bins "$Image.bin"),
           '-display', $disp,
           '-serial', 'mon:stdio',
           '-L', (Join-Path $root 'qemu\pc-bios'))
if (-not $NoAudio) { $qargs += @('-audiodev', 'dsound,id=snd0') }
if ($Trace) { $qargs += @('-d', 'unimp,guest_errors', '-D', (Join-Path $root 'zunehd-mmio.log')) }
if ($Gdb)   { $qargs += @('-s', '-S') }
if (-not $NoEmmc) {
    if (-not $EmmcImage) { $EmmcImage = Join-Path $root 'zunehd-emmc16.img' }
    if (Test-Path $EmmcImage) { $qargs += @('-drive', "if=sd,index=0,format=raw,file=$EmmcImage") }
    else { Write-Warning "No eMMC image at $EmmcImage (run: py -3 -I tools\mkemmc.py)" }
}

Write-Host "qemu-system-arm $($qargs -join ' ')"
Write-Host 'Exit with Ctrl-A then X.'
& $qemu @qargs
