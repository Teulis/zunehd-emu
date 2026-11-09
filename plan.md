# Emulator plan: audio first, then the play-button glow (2026-11-08)

Based on the audio DMA model (`qemu\hw\arm\zunehd-apbdma.c`), the 3D texture sampler
(`qemu\hw\arm\zunehd-gr3d.c`) and the open items in NOTES.md. Each problem has a likely cause, a way to
confirm it, and a fix. Nothing has been changed or run yet.

## 1. Pause doesn't silence the audio (do first; probably a small fix)

**What the model does now:** sound only stops when the guest disables or holds DMA channel 15. Otherwise
it keeps playing whatever is in the buffer it reads from. Several things the player might use to pause
are ignored by the emulator:
- the I2S1 control register (the transmit FIFO enable);
- the DAP audio router;
- the WM8350's DAC mute and volume registers (the WM8350 is the audio chip, reached over I2C).

**Most likely cause:** the player pauses by muting the WM8350 or turning off the I2S transmit FIFO, and
leaves the DMA running. On real hardware that is silent. In the emulator the DMA keeps reading the
mixer's last buffers, so you hear stale audio looping.

**Steps:**
1. Play a track, pause it, then resume. Log every DMA register write (`ZUNEHD_DMALOG=1`), plus every
   I2S1/DAP write and every WM8350 I2C write, timestamped. This shows which mechanism the player uses.
2. Model that mechanism:
   - I2S transmit FIFO off → output silence.
   - WM8350 DAC mute and volume → apply them to the stream. The same fix would probably make the volume
     keys work; they probably don't change the level today.
   - If the player just stops refilling the buffers, check what real hardware outputs and match it.
3. Cut the delay before sound stops. The buffer between the emulator and Windows (`RING_FRAMES`) holds up
   to 0.75 s. Empty it on mute/stop, and shrink it to about 100 ms once playback is fast enough
   (section 2).

**Done when:** pause is silent within about 100 ms, resume continues from the same spot, and the volume
keys change the level.

## 2. Choppy playback (the emulated decoder runs at about 70 % of real time)

Measure before changing anything:

1. **Screen-off test (cheap and decisive):** play a track and press Power to turn the screen off.
   - If the stutter stops, the UI and 3D drawing are using up the time, and the fix is on the graphics side.
   - If it doesn't, the bottleneck is the CPU and the AVP (the media coprocessor that decodes MP3).
2. **Check the two emulated CPUs run in parallel:** the CPU and the AVP are declared as separate clusters
   with 2 CPUs. Confirm QEMU really runs them on separate host threads, by launching with
   `-accel tcg,thread=multi` and watching per-thread CPU use. If they share one thread, the AVP's decoding
   competes with everything else, and that alone could explain the missing 30 %.
3. **Profile during playback:** count how much time goes to each part — the main CPU, the AVP, the two MP3
   hardware models (bitstream engine `zunehd-bse.c`, audio vector engine `zunehd-ape.c`), and the 3D
   worker threads — per virtual second.
4. **Fixes, depending on what that shows:**
   - **CPU idle loop:** the guest's idle loop keeps a host core busy. Make idle actually sleep (waiting
     for an interrupt) so the time goes to the AVP.
   - **MP3 hardware models:** they run synchronously inside the AVP's register writes. If they show up in
     the profile, speed up their inner loops.
   - **Now-playing screen:** if it's the 3D cost, cache or skip frames that haven't changed.
   - **Last resort:** let the audio clock follow the decoder (slightly lower rate, resampled). This is
     gapless but slightly off-pitch, so only if the above isn't enough.

**Done when:** a full track plays with no underruns. `tools\pcmsegs.py` should show one continuous run
with the screen on.

## 3. Play-button press glow draws as a semicircle

**Most likely cause:** the 3D texture sampler handles clamp-to-edge and repeat wrapping, but **not
mirrored repeat**. Mirrored repeat is bits 2 and 3 of the texture descriptor (`TEXTURE_DESC1`) in the
open-source Tegra GPU docs (grate, `ext\grate\include\tgr_3d.xml.h`). A glow texture that stores half a
circle and relies on mirroring to make the full circle would come out exactly as a semicircle. The sqrt
and reciprocal shader functions the radial glow uses are already implemented.

**Steps:**
1. **Get a reference:** capture the real Zune's screen with Lyra's screen-capture tool
   (`ext\project-lyra\tools\capture\zune-screencap.py`) while the play button is held. A photo works too.
2. **Reproduce:** script unlock → Home → press and hold Play, with 3D tracing on (`ZUNEHD_GR3D_TRACE`)
   and screenshots a few frames apart.
3. **Find the glow's draw** in the trace and read its texture descriptor. If bit 2 or 3 is set, implement
   mirrored repeat (a few lines in `zunehd-gr3d.c`, next to the existing clamp/repeat code).
4. If mirroring isn't the cause, check in this order:
   - the blur and spill path that's still wrong on the album rows (`PSEQ 0x0124xxxx`, render target 4) —
     the glow may share it;
   - the scissor or clip distances;
   - the fill rule on the glow's quad.
5. **Regression check:** recapture the screens already fixed (lock screen, menu, album rows, the glow when
   holding a settings item) and compare them against the earlier screenshots.

## Suggested order

1. **Pause/mute investigation:** about one session, and it gives us the audio logging we'll reuse.
2. **Mirrored-repeat check on the glow:** a quick check that may turn out to be a few-line fix.
3. **Playback speed:** the biggest task. The screen-off test and the parallel-thread check show where to
   put the effort.
4. **Remaining graphics items:** the reddish album-row blur, the dialog bleed-through.

## Open questions for the user

- A screenshot or short clip of the real press glow, or OK to capture it over Lyra while the button is held.
- Whether the volume keys change the emulator's volume today.
