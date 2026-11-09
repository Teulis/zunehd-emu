# Copy MP3s onto the running emulated Zune (MTP over USB/IP, no drivers needed).
#   1. Start the emulator with:  .\run.ps1 -UsbIp      and wait until the UI is up (~30 s;
#      syncing earlier can leave it on "CONNECTED").
#   2. .\sync-music.ps1 "C:\Music\song1.mp3" "C:\Music\song2.mp3"
# Tracks get title/artist/album from their ID3 tags and appear under music > albums/artists.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)] [string[]]$Files)
$root = $PSScriptRoot
$py = if (Get-Command py -ErrorAction SilentlyContinue) { @('py', '-3') } else { @('python') }
& $py[0] @($py[1..9] | Where-Object { $_ }) -I (Join-Path $root 'tools\zmtp.py') sync @Files
