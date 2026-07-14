# AGENTS.md — MD/ZX playbook

This is the single source of agent guidance for this repo. `CLAUDE.md`
is just `@AGENTS.md`; keep everything here.

See also: `README.md` (user-facing overview), `CHANGELOG.md` (history),
`programming.md` (shared-region table + budget rules from the template),
and the private `docs/epics/` folder (internal planning notes, **not
committed** — see the hard rule below).

## ⛔ Never reference the epic docs in shipped material

The epic planning docs live in the private `docs/epics/` folder —
internal planning notes that are **not committed** to the repo (one
Markdown file per epic, split into stories). Do NOT reference them — no
links to `docs/epics/*.md`, no "Epic N" / "Story X.Y" citations —
anywhere that ships or is user-facing: `README.md`, this file, the public
docs, code/header comments. Describe the behaviour or the code directly
(e.g. "the dual-core split in `fb_core1_dispatch`", not "the split from
Story N.M"). When you touch a comment that cites an epic/story, rephrase
it. Cross-references *between* epic docs are fine. Hard rule.

## No AI attribution

Never add AI-tool attribution to commits, PRs, code comments, docs, or
any artifact: no `Co-Authored-By: Claude …`, no "Generated with Claude
Code / ChatGPT", no "AI-assisted" notes. Write everything as the human
author. (Also: no `Co-Authored-By` trailers at all, per the repo owner.)

---

## What this repo is

**MD/ZX** — a **ZX Spectrum 48K emulator** for the Atari ST / STE /
MegaST(E), built on the SidecarTridge Multi-device **framebuffer
template**. It is a port of [zx2040](https://github.com/antirez/zx2040)
(Salvatore Sanfilippo's RP2040 port of Andre Weissflog's `chips`
emulator). The Spectrum runs entirely on the RP2040 in the cartridge; the
ST is the screen, keyboard and speaker.

The repo has two layers:

1. **The framebuffer template** (the foundation) — draw a 320×200
   16-colour framebuffer on the RP2040, the firmware blits it to the ST
   each VBL at 50 Hz, with ST keyboard and YM2149 audio for free.
2. **The MD/ZX port** (what sits on top) — the emulator, its VRAM→FB
   decode, ST-keyboard→Kempston input, beeper→YM audio, and SD game
   loading. Lives in `rp/src/zxemu.c` + `rp/src/zx/`.

Public template docs:
<https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

---

## Build

Top-level build is `build.sh`; day-to-day use the `Makefile`:

```bash
make build      # pico_w release; uuid from uuid.txt / APP_UUID_KEY; no version bump
make debug      # pico_w debug; bumps the patch version (tools/bump_version.sh)
make uart       # screen console on the first serial device
make tag        # tag HEAD with version.txt and push (triggers release CI)

# Or directly:  ./build.sh <pico|pico_w|sidecartos_16mb> <debug|release> <uuid>
```

Build flow: `bump_version.sh` (skipped when `SKIP_VERSION_BUMP=1`) syncs
`version.txt` → `rp/`, `target/`; builds the **m68k target** (`stcmd
make` → `BOOT.BIN`, 16 KB hard limit) → padded to 64 KB → `firmware.py` →
`rp/src/include/target_firmware.h` (C byte array); builds the **RP
firmware** (`rp/build.sh` via CMake presets) → `dist/<UUID>-<VER>.uf2` +
`<UUID>.json`.

### Phase gates (compile-time)

Functionality is gated so it can be tested in isolation. Defaults live in
`rp/src/zx/zx_config.h` (`#ifndef` fallbacks) and are set as CMake cache
options in `rp/src/CMakeLists.txt`:

| Gate | Default | Effect when off |
| --- | --- | --- |
| `ZX_INPUT_KEYBOARD` | 1 | no keyboard input |
| `ZX_AUDIO_YM` | 1 | silent |
| `ZX_GAMES_FROM_SD` | 1 | run the single baked-in game; **no SD access** |
| `ZX_INPUT_JOYSTICK` | 0 | no ST joystick (keyboard only) |

Override per build, e.g. `cmake --preset pico_w-release
-DZX_GAMES_FROM_SD=0`. With SD off the firmware runs the embedded
`builtin_game.h` snapshot and needs no card — the fastest smoke test.
`ZX_INPUT_JOYSTICK` also needs the m68k rebuilt with `ZX_JOYSTICK equ 1`
in `userfw.s` (see the port section).

### Build gotchas

- CMake always builds `MinSizeRel` (`-Os`) regardless of the build_type
  arg — a full `Release` previously broke things. `<build_type>` only
  controls `DEBUG_MODE` and the dist filename.
- Harmless VASM warnings (`target data type overflow`, `trailing garbage
  after option -D`) can be ignored.
- `the input device is not a TTY` from `stcmd` → invoked without a PTY.
  `target/atarist/build.sh` exports `STCMD_NO_TTY=1`; export it yourself
  if calling `stcmd` from a non-TTY context. Without it the m68k build
  can fail silently and a stale `target_firmware.h` survives → the ST
  shows garbage while the RP firmware looks fine.
- **Fast RP-only iteration** (skips the m68k/Docker step; reuses the
  existing `target_firmware.h`):
  ```bash
  export PICO_SDK_PATH=$PWD/pico-sdk FATFS_SDK_PATH=$PWD/fatfs-sdk PICO_EXTRAS_PATH=$PWD/pico-extras
  export PICO_BOARD=pico_w DEBUG_MODE=0 APP_UUID_KEY=$(cat uuid.txt)
  export RELEASE_VERSION=$(cat version.txt) RELEASE_DATE="$(date '+%Y-%m-%d %H:%M:%S')"
  cd rp/src && cmake --preset pico_w-release && cmake --build --preset pico_w-release
  ```

### CI / release
- `.github/workflows/build.yml` builds `pico_w` on PR (the trigger is
  commented out locally — check before relying on it).
- `.github/workflows/release.yml` runs on `v*` tags: builds, attaches
  UF2 + JSON to the Release, uploads to `s3://atarist.sidecartridge.com/`.

### Tests / verification
No test suite. Verification is: **build succeeds** (both targets),
**boots on hardware**, manual play + serial console. You usually can't
flash — validate math offline (mirror the exact C in a host script, as
the display decode was validated) and let the owner confirm on device.

---

## Architecture — the framebuffer template (foundation)

Two-target build: m68k assembly runs on the ST, is compiled to a ROM
image, embedded as a C array in the RP2040 firmware, and served back to
the ST over the emulated cartridge bus (PIO + DMA).

### Framebuffer pipeline

Every visible pixel goes through this each VBL:

1. **RP draws into a chunked buffer** — `fb_chunked_buffer` (320×200
   bytes, one palette index per pixel) at RP `0x20000000+`.
2. **RP publishes via chunky→planar** — `fb_chunked_asm.S` (`fb_c2p_half`)
   bit-transposes to ST planar 4 bpp. Split **Core 0 (top 100 rows) /
   Core 1 (bottom 100)** via the inter-core FIFO into `fb_planar_scratch`
   (32 KB RP RAM), then `fb_chunky_to_planar` publishes into the cart FB
   at `$FA8300` with 48-byte MOVEM chunks **pre-reversed** (so the m68k's
   predec store lands each chunk at its natural position). ~1 ms.
3. **m68k blits cart FB → ST screen** in `userfw.s` via `FBDRV_INLINE`
   (unrolled `movem.l` load + predec store, 12 longwords/48 B per iter).
   A0 (audio cursor) and A7 (SP) are kept out of the MOVEM list. Pure
   68000, same code on ST/STE/MegaSTE/TT/Falcon.
4. **m68k flips video base** between `$70000`/`$78000` each frame
   (page-flip, tear-free).

`fb_publish()` (RP) does the transpose + publish and **blocks on the ST
VBL** — one call per loop paces the app to 50 Hz.

### IKBD pipeline (keyboard + ESC exit + joystick)

`userfw.s` owns the ST IKBD ACIA end-to-end so the VBL blit runs
interrupt-free:

1. m68k stubs 5 IRQ vectors (HBL/Timer-A/C/D/ACIA) to a 1-instruction
   `rte`; Timer-B is owned by audio.
2. IKBD draining is **inlined in `FBDRV_INLINE`** — every
   `FBDRV_IKBD_POLL_EVERY` (40) iters it reads the ACIA and forwards the
   byte via a cart read at `IKBD_WINDOW_BASE + byte` (`$FB8200..$FB82FF`).
3. **RP captures** via the commemul ROM3 DMA ring; the main loop drains
   it (`commemul_poll`), filters the `$FB82xx` window, pushes bytes to a
   raw ring.
4. **RP demux** (`ikbd.c` `ikbd_pump`) classifies bytes: `$00..$7F` press,
   `$80..$F1` release. `$FD/$FE/$FF` joystick packets are parsed when the
   port enables joystick (see port section), else discarded.
5. **ESC exit** — ESC press+release within 200 ms writes
   `CART_CMD_BOOT_GEM` to the sentinel; the m68k VBL loop polls it and
   exits to GEM.

### Audio pipeline (YM2149)

RP fills a 1 KB cart buffer at `$FA4100` with (vA, vB) volume pairs;
m68k Timer-B (`/4`, TBDR=110 → ~5,585 Hz, ~112 fires/PAL VBL) writes both
YM volume regs per fire. `userfw_vbl` resets the A0 read cursor to the
buffer base every VBL (the resync edge). MD/ZX installs its own fill
callback (`audio_set_fill_callback`) — see port section.

### Shared 64 KB cartridge region

The ST sees `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). The
single source of truth for cross-target layout — reference named offsets
from `rp/src/include/cart_shared.h` / `target/atarist/src/main.s`, never
hard-code. Key offsets: cartridge image (16 KB), `CMD_MAGIC_SENTINEL`
(`$FA4000`), `AUDIO_BUFFER` (`$FA4100`, 1 KB), `APP_FREE` (`$FA4500`),
`FRAMEBUFFER` (`$FA8300`, 32000 B).

### RP2040 side (`rp/src/`)

- `main.c` — clock/voltage + config init, then `emul_start()`. Don't add
  features here.
- `emul.c` — **boot path + main loop, rewritten for MD/ZX.** Brings up
  romemul / commemul / fb / palette / audio / SD, calls `zxemu_init()`,
  installs `zxemu_audio_fill`, then loops: `fb_pump_rom3()`,
  `ikbd_pump()`, drain keys → `zxemu_handle_key()`,
  `zxemu_render_frame()`, `audio_render_frame()`. (The template's demo
  dispatcher was removed.)
- `fb.c` / `fb_chunked.c` / `fb_blit.c` / `fb_font.c` — framebuffer +
  draw primitives + the dual-core c2p worker. `fb_publish()` is the
  VBL-synced hand-off. (`fb_render_frame` and the internal demo sprite
  are legacy and now only paint the boot frame; MD/ZX overwrites it.)
- `commemul.c` — ROM3 cart-bus capture ring. **The ring was shrunk from
  32 KB to 4 KB** for MD/ZX (`COMM_RING_BITS` 15→12) — it only carries
  IKBD bytes (<1/ms, drained sub-ms), and the RAM was needed for the
  emulator.
- `ikbd.c` / `ikbd.h` — IKBD ingest + demux; `ikbd_pop_key`. Gained a
  gated joystick packet parser + `ikbd_get_joystick()` for the port.
- `romemul.*`, `sdcard.c`, `hw_config.c`, `gconfig.c`, `aconfig.c`,
  `select.c`, `reset.c`, `palette.c`, `audio.c` — unchanged template
  services. `aconfig.c` default folder is `/zx`.

### Memory layout (`rp/src/memmap_rp.ld`)

**The live layout (verify here, not from memory — the RP2040 has 264 KB
SRAM):**

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `RAM` | `0x20000000` | **192 K** | `.data` + `.bss` + heap |
| `ROM_IN_RAM` | `0x20030000` | **64 K** | cart shared-region mirror |
| `SCRATCH_X/Y` | `0x20040000` | 4 K each | core 0/1 stacks |

`__StackLimit` is capped at the RAM/cart boundary (`ORIGIN(RAM) +
LENGTH(RAM)`) so malloc can't hand the cart region to FatFs. Flash: 1 MB
app `FLASH`, plus Booster / config / lookup regions — don't write those.
Core 0 overclocks to 225 MHz @ `VREG_VOLTAGE_1_10`; **Core 1 is owned by
the c2p worker.**

### App identity
`CURRENT_APP_UUID_KEY` (from `APP_UUID_KEY` at CMake time, default
`44444444-4444-4444-8444-444444444444`) must match `desc/app.json`
`uuid`, keyed into `GLOBAL_LOOKUP_FLASH`. Mismatch → jumps to Booster.
`uuid.txt` (git-ignored) holds this app's real UUID.

---

## Architecture — the MD/ZX port (future-self notes)

### File layout

- `rp/src/zx/` — **vendored emulator core**, kept close to upstream
  (MIT/zlib): `z80.h`, `zx.h`, `mem.h`, `kbd.h`, `chips_common.h`,
  `clk.h`, `zx-roms.h`. Modifications are marked `MODIFIED (md-zx)`.
- `rp/src/zx/device_config.h` — replaces zx2040's per-board header:
  buttons are abstract IDs read from `zx_input_mask` via
  `get_device_button(id)`; `SPEAKER_PIN`, `st77_*` display metrics.
- `rp/src/zx/zx_config.h` — phase-gate `#ifndef` fallbacks.
- `rp/src/zx/builtin_game.h`, `builtin_keymaps.h` — generated: a baked-in
  `.z80` (for SD-off) and the default `keymaps.txt`.
- `rp/src/zxemu.c` — **the port**: the emulator front-end (ported from
  zx2040's `zx.c`). Owns the `EMU` state, UI/menu, keymap parser, the
  VRAM→FB decode, input mapping, audio fill, SD loading. Includes the
  emulator core with `#define CHIPS_IMPL`.
- `rp/src/include/zxemu.h` — the 4 entry points `emul.c` calls:
  `zxemu_init`, `zxemu_render_frame`, `zxemu_handle_key`,
  `zxemu_audio_fill`.

### The four seams (how the port maps onto the template)

| zx2040 | Replaced with |
| --- | --- |
| ST77xx display driver | `update_display()` decodes 256×192 VRAM → `fb_chunked_buffer` at (32,4), one palette index/pixel, then `fb_publish()` |
| GPIO buttons | `get_device_button(id)` reads `zx_input_mask`, set from ST keyboard/joystick |
| PWM beeper on Core 1 | `zxemu_audio_fill` → YM (Core 1 freed for c2p) |
| flash game blob | FatFs enum of `/zx`, `.z80` loaded via `zx_quickload` |

### Display decode (validated offline)

`update_display()` (in `zxemu.c`, `__not_in_flash_func`): clears the FB
to the border colour, then for each of 192 rows reads the Spectrum
bitmap byte at `((py&0xC0)<<5)|((py&0x07)<<8)|((py&0x38)<<2)|(px>>3)` and
attribute at `0x1800+((py>>3)<<5)+(px>>3)`, applies BRIGHT
(`(attr&0x40)>>3` → +8 to the index) and FLASH (swap ink/paper when the
frame counter's blink phase is set), and writes the palette index to
`fb_chunked_buffer[(4+py)*320 + 32 + x]`. The 16 ZX colours are pushed to
the ST shifter palette in `zx_set_palette()` (`zxpalette` is `0x00BBGGRR`
→ `PALETTE_RGB` 3-bit channels).

### Input

`zxemu_handle_key()` maps ST scancodes (Up `$48`, Down `$50`, Left `$4B`,
Right `$4D`, Space `$39`, Return `$1C`) to button bits in `zx_kbd_mask`.
`zxemu_render_frame()` composes `zx_input_mask = zx_kbd_mask | joystick`
each frame, so `get_device_button()` — and thus zx2040's entire per-game
keymap/macro parser — works unchanged. Kempston key codes are
`$FB..$FF`.

### Joystick (off by default, UNTESTED)

Fully gated. When `ZX_INPUT_JOYSTICK`: `ikbd.c` runs a small state
machine consuming `$FE/$FF` (one stick) / `$FD` (both) packets into
`s_joy_state` (bit0 up,1 down,2 left,3 right,7 fire), exposed via
`ikbd_get_joystick()`; `zxemu_render_frame` ORs those into
`zx_input_mask`. m68k side: `userfw.s` sends `$14` (enable joystick event
reporting) inside an `ifne ZX_JOYSTICK` block after IRQs come back on.
**Both sides compile/assemble but this path is documented-fragile
(byte-loss / demux desync) and has not run on hardware.** Keyboard is the
reliable route; leave joystick off unless deliberately testing it.

### Audio

`zxemu_audio_fill(buf, bytes)` decimates the beeper. The emulator samples
the 1-bit beeper into `zx.audiobuf` during `zx_exec` (enabled because
`SPEAKER_PIN != -1`); the callback takes the bits produced since the last
fill and maps each to full/zero YM volume (`ZX_YM_VOLUME`) on both
channels. Approximate ("recognisable, not hi-fi").

### SD games / keymaps

`/zx` (config `ACONFIG_PARAM_FOLDER`). `populate_games_list()` enumerates
`.z80`; `load_game()` reads a snapshot **into the 64 KB
`fb_chunked_buffer`** (borrowed as a transient load buffer — it's
overwritten by the next render, so no permanent allocation) then
`zx_quickload`. `keymaps.txt` is read into `zx_keymap_buf` (4 KB), or the
firmware default is written to the card if absent. `EMU.keymap_file`
points at that buffer (SD) or `builtin_keymaps` (SD-off).

### RAM budget — CRITICAL, read before adding statics

The 48 KB Spectrum RAM (`zx_t.ram[3][0x4000]`) is irreducible, so the
port only just fits the 192 KB region (links with ~15 KB heap headroom).
`.bss`+heap must not cross `0x20030000`. If you overflow `RAM`, the
reclaims that made it fit were:

1. **ZX ROM `const`** — `zx-roms.h` arrays were `unsigned char` (→ 16 KB
   in `.data` RAM!); made `const` → flash. Keep them const.
2. **ROM mapped from flash** — `zx.h` `zx_t.rom[1][0x4000]` (a 16 KB RAM
   copy) replaced with a `const uint8_t* rom0` pointer into the flash
   array. `MODIFIED (md-zx)`.
3. **ROM3 ring 32 KB→4 KB** — `commemul.c COMM_RING_BITS` 15→12.
4. Dropped a 50 KB `static zx_t im` from the unused `zx_load_snapshot`
   (it was already `--gc-sections`'d away, so this was cosmetic — the
   real wins were 1–3).

Diagnose overflow with the linker `.map` (`rp/build-*/rp.elf.map`), not
by estimating: `--gc-sections` drops unused statics, and non-`const`
arrays silently land in `.data` RAM. `arm-none-eabi-size rp.elf` shows
the totals. A host `sizeof` probe over `rp/src/zx/*.h` (define
`SPEAKER_PIN`, stub `vram_set_dirty_*`) gives struct sizes; `zx_t` is
~56.7 KB.

### Speed (deferred)

Runs at the template's 225 MHz; the emulator stays in flash (XIP) and
shares Core 0 with the cart bus, so it may run slow. Not yet optimised —
the owner asked to get it working first. Levers if needed: `#pragma GCC
optimize("O3")` on `zxemu.c`, `__not_in_flash_func` on the emulator hot
path (needs `zx.h`/`z80.h` edits), higher clock/voltage (must re-tune the
PIO cart-bus timing — risky).

---

## Environment setup

- **Host tooling**: ARM GNU Toolchain (point `PICO_TOOLCHAIN_PATH` at its
  `arm-none-eabi/bin` — 14.2 per upstream docs, 15.2 also works);
  `atarist-toolkit-docker` (`stcmd`, needs a PTY / `STCMD_NO_TTY=1`);
  `cmake` + `ninja`; a Debug Probe/Picoprobe for flashing (optional).
- **SDK env** (auto-set from the repo by the build if unset):
  ```bash
  export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
  export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
  export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
  ```

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `region RAM overflowed by N bytes` | See **RAM budget** above; check the `.map` for a non-`const` array in `.data` or a big new `.bss`. |
| ST shows garbage but input/logic seems fine | Stale `target_firmware.h` — the m68k build failed silently. Confirm `target/atarist/dist/BOOT.BIN` timestamp matches the rest of `dist/`. |
| `the input device is not a TTY` (stcmd) | Export `STCMD_NO_TTY=1` before invoking `stcmd` from a non-TTY context. |
| `arm-none-eabi-gcc not found` | Point `PICO_TOOLCHAIN_PATH` at the toolchain `bin` dir. |
| `ERROR: cartridge code is N bytes; limit is 16384` | m68k cart grew past 16 KB. Trim `main.s` / `userfw.s` or move data into the shared region. |
| Menu is empty on boot | No `.z80` files in `/zx` on the card (or SD not mounted). The ROM boot screen still shows. Test with `-DZX_GAMES_FROM_SD=0`. |
| Final steps fail copying UF2 | An upstream compile failed — scroll back for the first error. |
| Undefined ref to `vram_set_dirty_*` | They must be plain `void` functions (not C99 `inline`, which emits no symbol) since `mem.h` calls them. |

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/` — pinned
  submodules, re-pinned every build. Change FatFs config in
  `rp/src/ff/ffconf.h` (project override wins via `BEFORE PRIVATE`).
- Don't add features to `main.c` — start in `emul.c` / `zxemu.c`.
- Keep vendored `rp/src/zx/*` close to upstream; mark any change
  `MODIFIED (md-zx)` and preserve the original licence header. New files
  we author get the GPL-3.0-or-later header; don't stamp our copyright on
  vendored/template files we only tweak.
- Match existing C style (`.clang-format` / `.clang-tidy`).

## Working style

- Think before coding: state assumptions; ask when genuinely blocked;
  prefer the simpler approach and say so.
- Simplicity first — minimum code that solves the problem, nothing
  speculative. Surgical changes: touch only what the task needs, match
  surrounding style, don't refactor what isn't broken.
- Goal-driven: define a success check per step and verify (here: builds
  clean + offline math validation, since hardware flashing is the
  owner's step).

Keep this file current as the port evolves — it's the tribal knowledge
every agent starts from.
