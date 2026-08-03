# Changelog

## v1.1.0

- **`.sna` snapshots** — 48K `.sna` files load from the SD app folder
  alongside `.z80` (27-byte register header + raw 48 KB RAM dump; the
  program counter is popped RETN-style off the snapshot stack). 128K
  `.sna` images are rejected — this is a 48K machine.
- **Cleaner beeper audio** — fixed three distortion sources: the per-VBL
  refill gate raced the ST VBL, so a lost race replayed a stale sample
  buffer for a frame and overran the beeper ring (the intermittent
  glitches); the decimator silently dropped ~0.4 ms of audio timeline
  every fill (division remainder), phase-jumping sustained tones at
  50 Hz; and duty-cycle volumes were mapped linearly onto the YM2149's
  logarithmic DAC, squashing the filtered edges into near-silence and
  adding harmonics. Windows now tile the fill span exactly and volumes
  go through an inverse-log table, so amplitude tracks duty.

## v1.0.0

First release of **MD/Speccy** — a ZX Spectrum 48K emulator for the
Atari ST / STE / MegaST(E), ported from
[zx2040](https://github.com/antirez/zx2040) (Andre Weissflog's `chips`
core + Salvatore Sanfilippo's RP2040 front-end, MIT) onto the
SidecarTridge framebuffer template.

- **Display** — the Spectrum's 256×192 bitmap+attribute VRAM is decoded
  directly into the chunked framebuffer, centred in the 320×200 screen
  (border fills the margin), with BRIGHT and FLASH honoured. The 16 ZX
  colours are published to the ST shifter palette.
- **Keyboard** — the whole Atari ST keyboard maps positionally onto the
  Spectrum: Shift → Caps Shift, Alt → Symbol Shift, Backspace/Delete →
  Delete, Caps Lock, and punctuation (the matrix supplies Symbol Shift).
- **Joystick / cursor** — the cursor cluster is a Kempston joystick, or
  the Spectrum cursor keys via a menu setting; a real ST joystick works
  as Kempston too, and Insert / Clr-Home are fire.
- **Menu** — ESC opens a game picker + settings (volume, border, cursor
  mode, …) with an about pop-over and an exit-to-GEM item; navigable by
  keyboard or joystick.
- **Audio** — the 1-bit Spectrum beeper is sampled and played out the
  YM2149 via the template's per-VBL fill callback (the RP2040 second
  core, previously used for PWM audio, is freed for the chunky→planar
  worker).
- **Games** — `.z80` snapshots load from the SD app folder (default
  `/speccy`); a small demo is seeded there when the folder is empty.

RAM notes: the Spectrum ROM is mapped from flash (not copied to RAM),
the ROM3 capture ring was shrunk 32 KB→4 KB (it only carries IKBD
bytes), and the ZX ROM dumps were made `const` so they live in flash.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
