# Changelog

## Unreleased

Ported the [zx2040](https://github.com/antirez/zx2040) ZX Spectrum 48K
emulator (Andre Weissflog's `chips` core + Salvatore Sanfilippo's RP2040
front-end, MIT) onto the framebuffer template as **MD/ZX**:

- **Display** — the Spectrum's 256×192 bitmap+attribute VRAM is decoded
  directly into the chunked framebuffer, centred at (32, 4) in the
  320×200 screen (border fills the margin), with the BRIGHT and FLASH
  attributes honoured. The 16 ZX colours are published to the ST
  shifter palette.
- **Input** — Atari ST keyboard (cursor keys + space/return) drive the
  emulator through zx2040's per-game keymap/macro system unchanged;
  cursors/space map to Kempston.
- **Audio** — the 1-bit Spectrum beeper is sampled and played out the
  YM2149 via the template's per-VBL fill callback (the RP2040 second
  core, previously used for PWM audio, is freed for the chunky→planar
  worker).
- **Games** — `.z80` snapshots load from the SD app folder (default
  `/zx`); `keymaps.txt` is read from there too, and written from a
  firmware default on first boot.
- **Phase gates** — `ZX_INPUT_KEYBOARD`, `ZX_AUDIO_YM`,
  `ZX_GAMES_FROM_SD` and `ZX_INPUT_JOYSTICK` are CMake build options
  (`cmake … -DZX_GAMES_FROM_SD=0`) so each capability can be tested in
  isolation. With SD off, a single baked-in game runs without a card.
- **ST joystick → Kempston** (`ZX_INPUT_JOYSTICK`, m68k `ZX_JOYSTICK`)
  is implemented but **off by default and untested** — the template's
  IKBD joystick path is documented-fragile; keyboard is the reliable
  route.

RAM notes: the Spectrum ROM is mapped from flash (not copied to RAM),
the ROM3 capture ring was shrunk 32 KB→4 KB (it only carries IKBD
bytes), and the ZX ROM dumps were made `const` so they live in flash.

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
