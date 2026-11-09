# Start the emulated Zune HD.
#   .\run.ps1              boot with the AVP media coprocessor + audio (music playback with sound)
#   .\run.ps1 -NoAvp       without the AVP (no music decoding; AVP/audio paths patched off)
#   .\run.ps1 -UsbIp       expose the Zune over USB/IP on 127.0.0.1:3240 (music sync, see README)
#   .\run.ps1 -Headless    no window (serial console only)
# Keys (window focused): Home/H = Home button, End/P = power, M = media button,
# left mouse button = finger. Quit: close the window, or Ctrl-A then X in this console.
param([switch]$NoAvp, [switch]$UsbIp, [switch]$Headless, [switch]$NoAudio)
$Avp = -not $NoAvp
$root = $PSScriptRoot
$qemu = Join-Path $root 'bin\qemu-system-arm.exe'
$img = Join-Path $root 'zunehd-emmc16.img'
$nk = Join-Path $root 'firmware\NK.bin'
$ext = Join-Path $root 'firmware\EXT.bin'
foreach ($f in $nk, $ext, $img) {
    if (-not (Test-Path $f)) { Write-Error "$f missing - run setup.ps1 first (see README.md)"; exit 1 }
}

$machine = "zunehd,ext=$ext"
if ($Avp) { $machine += ',avp=on' }
# The guest draws no mouse pointer (touch UI): keep the host cursor visible.
$disp = if ($Headless) { 'none' } else { 'sdl,show-cursor=on' }
$qargs = @('-M', $machine, '-m', '128M', '-kernel', $nk,
           '-display', $disp, '-serial', 'mon:stdio',
           '-drive', "if=sd,index=0,format=raw,file=$img")
if (-not $NoAudio) { $qargs += @('-audiodev', 'dsound,id=snd0') }
if ($UsbIp) { $env:ZUNEHD_USBIP = '3240' } else { Remove-Item Env:ZUNEHD_USBIP -ErrorAction SilentlyContinue }

Write-Host "Starting Zune HD emulator$(if ($Avp) {' (AVP on: music playback)'})..."
Write-Host 'Keys: Home/H = Home, End/P = Power, M = Media, mouse = touch. Quit: Ctrl-A then X.'
& $qemu @qargs
