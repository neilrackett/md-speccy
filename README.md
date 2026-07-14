# MD/ZX: ZX Spectrum for the Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://x.com/neilrackett)

## Introduction

MD/ZX turns your SidecarT into a **ZX Spectrum 48K** running on your Atari ST — a port of Andre Weissflog's `chips` emulator (via Salvatore Sanfilippo's [zx2040](https://github.com/antirez/zx2040)) onto the SidecarTridge framebuffer template.

The whole Spectrum runs on the RP2040 inside the cartridge:

- The Spectrum's 256×192 screen is drawn 1:1, centred in the ST's 320×200 display, with a coloured border and the full 15-colour palette (BRIGHT and FLASH included).
- The **Atari ST keyboard** drives the games — cursor keys + space, mapped to the Kempston joystick and Spectrum keys through zx2040's per-game keymap system.
- The **1-bit beeper** plays out the YM2149.
- **Games load from the SD card** — drop `.z80` snapshots in a folder and pick them from an on-screen menu.

No display board, no buttons, no soldering: the ST *is* the screen, keyboard and speaker.

## Controls

- **Cursor keys + Space** — play (mapped per-game to Kempston / Spectrum keys).
- **Left + Right held together** — open the game/settings menu.
- **In the menu** — up/down to choose a game or setting, Space/Return to select.
- **ESC** — quit back to GEM.

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) (RP2040-based ROM cartridge emulator)
- Atari ST, STE, MegaST, or MegaSTE (low or medium resolution — high-res falls back to GEM)
- A microSD card for your games
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional, for development)

## Installation

1. Download the latest `.uf2` and `.json` from the [releases page](https://github.com/neilrackett/md-zx/releases).
2. Copy both files to the `/apps` folder of your SidecarT's microSD card.
3. Create a `/zx` folder on the same card and copy your `.z80` Spectrum snapshots into it.
4. On the Booster screen, press ESC for the app list and select MD/ZX.
5. To return to Booster, power on your ST while holding the SELECT button on your SidecarT.

On first boot MD/ZX writes a default `keymaps.txt` into `/zx` (see **Games and keymaps** below). If the folder is empty the emulator still boots to the Spectrum ROM screen so you know it's alive.

## Games and keymaps

Games are `.z80` snapshots (v1, v2 and v3 headers, compressed) placed in the `/zx` folder. Names are cosmetic — the emulator matches keymaps by scanning snapshot memory for known strings, so any snapshot works.

`/zx/keymaps.txt` maps the ST's five controls (cursor keys + space) to Spectrum keys or Kempston moves, per game, with optional frame-triggered macros for auto-selecting the joystick or redefining keys. The file format is inherited from zx2040 — MD/ZX writes a starter file on first boot; edit it on the card to add your own games. No recompile needed.

## How it works

The RP2040 runs the full Spectrum on Core 0 and decodes its video memory straight into the framebuffer; the m68k blits that to the ST screen every VBL. Input and audio ride the cartridge bus in both directions.

```
Atari ST (68000)                         RP2040 (SidecarTridge)
────────────────                         ──────────────────────
VBL: blit cart FB → ST screen  ◄──$FA8300──  Core 0: Z80 emulation (chips core)
                                                  ↓  256×192 VRAM → chunked FB @ (32,4)
IKBD keyboard bytes            ──$FB82xx──►   demux → button mask → Kempston/keys
                                             Core 1: chunky → ST planar (c2p)
Timer-B: write YM volumes      ◄──$FA4100──  beeper → (vA,vB) pairs, filled per VBL
```

The Spectrum is 50 Hz PAL and so is the ST's VBL, so one emulated frame maps to one blit. You develop nothing on the ST — MD/ZX is a self-contained app; the ST just provides the screen, keyboard and YM.

## Building

```bash
# Production build (pico_w) — uses uuid.txt / APP_UUID_KEY
make build

# Debug build (bumps the patch version)
make debug

# Open a UART console on the debug probe
make uart
```

Functionality is split into compile-time **phase gates** so each capability can be tested on its own — pass them to CMake, e.g.:

```bash
cmake --preset pico_w-release -DZX_GAMES_FROM_SD=0   # run the baked-in demo, no SD needed
cmake --preset pico_w-release -DZX_AUDIO_YM=0        # silent
cmake --preset pico_w-release -DZX_INPUT_JOYSTICK=1  # ST joystick (experimental; also needs the m68k rebuilt)
```

With `-DZX_GAMES_FROM_SD=0` the firmware runs an embedded snapshot and needs no card — handy for a first smoke test. For more on coding for the SidecarT, [the docs are here](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

## Credits

MD/ZX stands on the shoulders of:

- **[Andre Weissflog](https://github.com/floooh)** — the [`chips`](https://github.com/floooh/chips) ZX Spectrum emulator and Z80 core this is built on. As zx2040's author puts it, the project "is 90% because of his work."
- **[Salvatore Sanfilippo](https://github.com/antirez)** — [zx2040](https://github.com/antirez/zx2040), the RP2040 port with the on-screen menu, per-game keymap system and 1-bit audio pipeline that MD/ZX adapts.
- The **SidecarTridge Multi-device** framebuffer template it runs on.

The Spectrum, and the joy of it, belongs to Sinclair Research and everyone who ever typed `LOAD ""`.

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text. The vendored emulator core (`rp/src/zx/`) retains its original MIT / zlib licences.
