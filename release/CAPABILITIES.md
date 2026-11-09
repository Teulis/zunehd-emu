# What the emulator can and cannot do (status: 2026-11-08)

The machine models the parts of the Tegra APX 2600 SoC and the Zune HD board that the stock
firmware needs. Some firmware paths that depend on missing hardware are patched at load time
(listed at startup on stderr as "patched ...").

## Works

- **Boot**: Windows CE kernel, NVIDIA drivers, file systems and services start from an
  emulated 16 GB eMMC laid out like the real device. The UI is up within ~30 s
  (measured with the AVP on, the default; the real device takes ~28 s).
- **User interface**: the real Zune HD UI, drawn by an emulation of the Tegra 3D GPU
  (fragment/vertex shader interpreter, multi-threaded) plus the 2D blitter and display
  controller. Lock screen with wallpaper and unlock swipe, main menu with
  Pins/New/History tiles, music hub (albums, artists, songs, genres, playlists),
  videos/pictures/radio/social/internet menus, settings, About pages, dialogs.
- **Input**: capacitive touch screen (Synaptics, via the mouse), Home, Power (sleep / wake)
  and Media buttons.
- **Music sync over USB**: the device's USB controller is emulated and exposed through a
  built-in USB/IP server. Plain MTP works: storage info, file transfer of any size
  (10 MB+ files are verified byte-exact), track metadata, artists and albums. The synced
  tracks show up in the music library. Use `sync-music.ps1`.
- **Real USB device in Windows (usbip-win2)**: with the free usbip-win2 client the emulated
  player attaches to Windows as a real USB device (high speed). Windows loads its own
  Zune driver (portable device "Zune"). After a registry tweak (README) it appears in
  Explorer under This PC > Zune > Storage. Copying files in both directions through
  Explorer works and was verified byte-exact (1.1-1.2 MB MP3s). Windows names uploaded MP3s
  from their tags ("01 Title.mp3").
- **Music playback with sound (default; off with `-NoAvp`)**: the AVP media coprocessor runs as a second
  emulated CPU with its own firmware, and the MP3 decoding hardware it drives is emulated:
  the Huffman bitstream engine, the audio vector engine (IMDCT + polyphase filterbank) and
  the audio DMA / I2S output to the Windows audio device. A recorded track comes out
  essentially bit-exact against a reference MP3 decoder (98.7 % of samples identical, the
  rest off by 1 LSB; 44.1 kHz 16-bit stereo, full track, no gaps).
  **Live playback stutters**: the emulated decode chain currently runs at ~70 % of real
  time, so the player's mixer underruns (short silences / skips every 50-100 ms). The
  music is recognisable but choppy. Only MP3 is supported.

## Not working / not emulated

- **Wi-Fi** (SDIO card not emulated): no internet, marketplace, social or Wi-Fi sync.
  The boot log shows harmless "card not found" retries.
- **FM radio** (tuner chip not emulated).
- **Video playback** (the video parts of the VDE decoder are not emulated) and probably
  most other codecs (WMA, AAC); only MP3 has been worked on.
- **The official Zune desktop software / MTPZ**: real sync with the Zune software needs the
  device's private keys and certificates (MTPZ handshake). Those are unique to each
  device and are not included, so MTPZ is switched off. Use `sync-music.ps1` or Explorer
  (usbip-win2) instead.
- **Accelerometer** (tilt), battery / charging status and the headphone jack are not
  emulated.
- **Games / apps**: not tested.

## Known issues

- Large panorama headers ("settings", "music"...) look a bit darker than on a real
  screen. The emulator draws exactly what the shader asks for (white at 40 %); the
  difference is probably display gamma.
- Connecting USB before the UI is up can leave the player on "CONNECTED".
- The guest's idle loop keeps one host CPU core busy even when the player is idle.
- After a crash, rebuild the flash image (`setup.ps1 -Rebuild`).
- The device name shown under Settings > About is empty and the bootloader version reads 0.0.

## Performance

QEMU uses about one host core plus up to 8 threads for 3D drawing during animations. The UI
is usable on a modern desktop CPU. Boot: ~30 s to the UI. MP3 decoding runs at ~70 % of real
time (see above).
