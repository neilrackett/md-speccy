# AGENTS.md — MD/Speccy playbook

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

**MD/Speccy** — a **ZX Spectrum 48K emulator** for the Atari ST / STE /
MegaST(E), built on the SidecarTridge Multi-device **framebuffer
template**. It is a port of [zx2040](https://github.com/antirez/zx2040)
(Salvatore Sanfilippo's RP2040 port of Andre Weissflog's `chips`
emulator). The Spectrum runs entirely on the RP2040 in the cartridge; the
ST is the screen, keyboard and speaker.

The repo has two layers:

1. **The framebuffer template** (the foundation) — draw a 320×200
   16-colour framebuffer on the RP2040, the firmware blits it to the ST
   each VBL at 50 Hz, with ST keyboard and YM2149 audio for free.
2. **The MD/Speccy port** (what sits on top) — the emulator, its VRAM→FB
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

### Compile-time gates

Display, audio, SD game loading and the ST joystick are all always on
(proven; no build without them). The one remaining knob gates keyboard
input; its default lives in `rp/src/zx/zx_config.h` (`#ifndef` fallback)
and it's read from the environment in `rp/src/CMakeLists.txt`:

| Gate | Default | Effect when off |
| --- | --- | --- |
| `ZX_INPUT_KEYBOARD` | 1 | no keyboard input (joystick only) |

Override from the root Makefile, e.g. `ZX_INPUT_KEYBOARD=0 make build`.

(The earlier `ZX_AUDIO_YM`, `ZX_GAMES_FROM_SD` and `ZX_INPUT_JOYSTICK`
gates were removed once those paths were confirmed working on hardware.
`builtin_game.h` is still used — the embedded demo is seeded into an
empty `/speccy` folder. The old `builtin_keymaps.h` / `keymaps.txt` system
was dropped when input moved to a direct ST→Spectrum keyboard mapping.)

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

`userfw.s` owns the ST IKBD ACIA end-to-end:

1. m68k stubs HBL/Timer-A/C/D to a 1-instruction `rte`; Timer-B is owned
   by audio. **The ACIA IRQ ($118) gets a real handler** (`userfw_acia_irq`)
   and the ACIA MFP interrupt (IERB bit 6) is enabled.
2. IKBD ingest is **interrupt-driven**: on each ACIA RX IRQ the handler
   reads the byte and forwards it via a cart read at `IKBD_WINDOW_BASE +
   byte` (`$FB8200..$FB82FF`). Reading on interrupt (rather than the old
   inline poll) means no byte is lost, so the multi-byte joystick packets
   survive. It drains the MIDI ACIA too (shared MFP IRQ), saves only
   D0/A1, and relies on MFP auto-EOI (no in-service ack). At boot it
   sends `$12` (mouse off) + `$14` (joystick event reporting on).
3. **RP captures** via the commemul ROM3 DMA ring; the main loop drains
   it (`commemul_poll`), filters the `$FB82xx` window, pushes bytes to a
   raw ring.
4. **RP demux** (`ikbd.c` `ikbd_pump`) classifies bytes: `$00..$7F` press,
   `$80..$F1` release; `$FE/$FF` (one stick) / `$FD` (both) joystick
   packets are framed into `s_joy_state` (a small pending-byte counter).
5. **Exit to GEM** — `ikbd_request_boot_gem()` writes `CART_CMD_BOOT_GEM`
   to the sentinel; the m68k VBL loop polls it and exits to GEM. (The
   built-in ESC press+release auto-exit still exists but MD/Speccy disables
   it — see the input section; the menu's Exit item drives this instead.)
   **The sentinel is a one-shot:** the m68k can't write the RP-owned cart
   region and the RP only zeroes it at boot, so a `BOOT_GEM` left set
   would persist across an ST reset and re-trigger the exit (userfw
   re-reads it → straight back to GEM). The main loop calls
   `ikbd_clear_command()` (re-arm to `CART_CMD_NOP`) every iteration;
   `BOOT_GEM` survives the one frame between the exit write and userfw's
   VBL read (`fb_publish` blocks until that read), then the next
   iteration wipes it, so a later reset auto-boots MD/Speccy cleanly.

### Audio pipeline (YM2149)

RP fills a 1 KB cart buffer at `$FA4100` with (vA, vB) volume pairs;
m68k Timer-B (`/4`, TBDR=110 → ~5,585 Hz, ~112 fires/PAL VBL) writes both
YM volume regs per fire. `userfw_vbl` resets the A0 read cursor to the
buffer base every VBL (the resync edge). MD/Speccy installs its own fill
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
- `emul.c` — **boot path + main loop, rewritten for MD/Speccy.** Brings up
  romemul / commemul / fb / palette / audio / SD, calls `zxemu_init()`,
  installs `zxemu_audio_fill`, then loops: `fb_pump_rom3()`,
  `ikbd_pump()`, drain keys → `zxemu_handle_key()`,
  `zxemu_render_frame()`, `audio_render_frame()`. (The template's demo
  dispatcher was removed.)
- `fb.c` / `fb_chunked.c` / `fb_blit.c` / `fb_font.c` — framebuffer +
  draw primitives + the dual-core c2p worker. `fb_publish()` is the
  VBL-synced hand-off. (`fb_render_frame` and the internal demo sprite
  are legacy and now only paint the boot frame; MD/Speccy overwrites it.)
- `commemul.c` — ROM3 cart-bus capture ring. **The ring was shrunk from
  32 KB to 4 KB** for MD/Speccy (`COMM_RING_BITS` 15→12) — it only carries
  IKBD bytes (<1/ms, drained sub-ms), and the RAM was needed for the
  emulator.
- `ikbd.c` / `ikbd.h` — IKBD ingest + demux; `ikbd_pop_key`. Gained a
  gated joystick packet parser + `ikbd_get_joystick()` for the port.
- `romemul.*`, `sdcard.c`, `hw_config.c`, `gconfig.c`, `aconfig.c`,
  `select.c`, `reset.c`, `palette.c`, `audio.c` — unchanged template
  services. `aconfig.c` default folder is `/speccy`.

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

## Architecture — the MD/Speccy port (future-self notes)

### File layout

- `rp/src/zx/` — **vendored emulator core**, kept close to upstream
  (MIT/zlib): `z80.h`, `zx.h`, `mem.h`, `kbd.h`, `chips_common.h`,
  `clk.h`, `zx-roms.h`. Modifications are marked `MODIFIED (md-speccy)`.
- `rp/src/zx/device_config.h` — replaces zx2040's per-board header. The
  button/keymap indirection was dropped (input is applied directly to the
  emulator in `zxemu.c`), so this now only carries `SPEAKER_PIN` and the
  `st77_*` display metrics the core and UI still reference.
- `rp/src/zx/zx_config.h` — input-gate `#ifndef` fallbacks.
- `rp/src/zx/builtin_game.h` — generated: the embedded demo `.z80`,
  seeded into an empty `/speccy`.
- `rp/src/zxemu.c` — **the port**: the emulator front-end (ported from
  zx2040's `zx.c`). Owns the `EMU` state, UI/menu, the ST→Spectrum key
  mapping, the VRAM→FB decode, audio fill, SD loading. Includes the
  emulator core with `#define CHIPS_IMPL`.
- `rp/src/include/zxemu.h` — the 4 entry points `emul.c` calls:
  `zxemu_init`, `zxemu_render_frame`, `zxemu_handle_key`,
  `zxemu_audio_fill`.

### The four seams (how the port maps onto the template)

| zx2040 | Replaced with |
| --- | --- |
| ST77xx display driver | `update_display()` decodes 256×192 VRAM → `fb_chunked_buffer` at (32,4), one palette index/pixel, then `fb_publish()` |
| GPIO buttons | `zxemu_handle_key()` applies ST keys directly via `zx_key_down/up`; the cursor cluster + ST joystick drive `zx_joystick()` (Kempston) |
| PWM beeper on Core 1 | `zxemu_audio_fill` → YM (Core 1 freed for c2p) |
| flash game blob | FatFs enum of `/speccy`, `.z80` loaded via `zx_quickload` |

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

### Input (direct ST→Spectrum mapping — replaced the keymap system)

The zx2040 per-game keymap/macro parser and the `get_device_button`
abstraction were removed. `zxemu_handle_key()` now applies each ST key
event straight to the emulator:

- **ESC** toggles the game/settings menu (we own ESC — `ikbd.c`'s
  auto-exit-to-GEM is disabled at init via `ikbd_set_esc_auto_exit(false)`;
  exit-to-GEM is now the menu's **Exit** item, which calls
  `ikbd_request_boot_gem()`).
- **Menu active** → the cursor cluster navigates (↑↓ move, ←→ adjust a
  setting) and Return/Space/Insert/Clr-Home select; nothing reaches the
  Spectrum. The physical ST joystick navigates too (edge-detected in
  `zxemu_render_frame`).
- **In play** → the positional table `st2zx[128]` maps ST scancodes to
  Spectrum key codes 1:1 (letters/digits), with **Shift→Caps Shift**,
  **Alt→Symbol Shift**, **Backspace/Delete→Delete**. Punctuation
  (`- = ; ' , . /`) maps to the ASCII symbol printed on the ST keycap; the
  matrix registered those codes with the Sym Shift modifier, so it applies
  Sym Shift automatically (one ST key = the combo). Applied via
  `zx_key_down/zx_key_up`. Two combos have no standalone matrix code
  upstream, so `init_emulator` registers them: **Caps Shift** at
  `ZX_KEY_CAPS 0x88` (`kbd_register_key(..., 0,0,0)` — the Caps Shift cell)
  for ST Shift, and **Caps Lock** at `ZX_KEY_CAPSLK 0x89`
  (`kbd_register_key(..., 3,1,1)` — the '2' cell + Caps Shift modifier).
  Sym Shift already has code `0x0F`.
- The **cursor cluster** is the Kempston joystick by default, or the
  Spectrum cursor keys (Caps Shift+5..8) when the **cursor** setting is
  `keys`. Held directions accumulate in `zx_cursor_kempston`.
- **Insert / Clr-Home** are fire (Kempston button), but only in the
  joystick cursor mode — in `keys` mode there's no Kempston, so they're
  ignored (release always clears the bit, so it can't stick across a mode
  change).

`zxemu_render_frame()` composes the Kempston mask each frame as
`zx_cursor_kempston | (physical ST joystick)` and calls
`zx_joystick(&EMU.zx, mask)` (Kempston → `joy_joymask`, OR'd with
`kbd_joymask` by the ULA). The physical joystick is ignored for ~10 ticks
after the menu closes so the fire that picked a game doesn't leak into
play.

### Joystick ingest (always on, hardware-confirmed)

`ikbd.c` runs a small state machine consuming `$FE/$FF` (one stick) /
`$FD` (both) packets into `s_joy_state` (bit0 up,1 down,2 left,3 right,7
fire), exposed via `ikbd_get_joystick()`; `zxemu_render_frame` folds those
bits into the Kempston mask (in play) or into menu navigation (in menu).
The m68k side is the interrupt-driven ACIA handler in the IKBD pipeline
above (`userfw_acia_irq` + `$12`/`$14` sends). This is what made it
reliable: the earlier version *polled* the ACIA inside the blit and lost
bytes, shredding the multi-byte packets — the interrupt handler reads
every byte the instant it arrives, so the framing holds. Works on
hardware (keyboard + joystick together). If the byte loss ever regresses,
the symptom is stuck/phantom directions.

### About pop-over

The **About** menu item sets `EMU.about_active`; `ui_draw_about()` renders
a centred, cell-aligned box (version from the `RELEASE_VERSION` compile
macro + credits) over the menu. It's modal — any key (in
`zxemu_handle_key`) or joystick press (in `zxemu_render_frame`) dismisses
it back to the menu.

### Audio

`zxemu_audio_fill(buf, bytes)` decimates the beeper. The emulator samples
the 1-bit beeper into `zx.audiobuf` during `zx_exec` (enabled because
`SPEAKER_PIN != -1`); the callback takes the bits produced since the last
fill and maps each to full/zero YM volume (`ZX_YM_VOLUME`) on both
channels. Approximate ("recognisable, not hi-fi").

### SD games

`/speccy` (config `ACONFIG_PARAM_FOLDER`). `populate_games_list()` enumerates
`.z80`; `load_game()` reads a snapshot **into the 64 KB
`fb_chunked_buffer`** (borrowed as a transient load buffer — it's
overwritten by the next render, so no permanent allocation) then
`zx_quickload`. No game auto-loads: boot leaves the menu active so the
user always picks. If `/speccy` has no `.z80` files, the embedded
`builtin_game.h` demo is seeded there on first boot. (There is no keymap
file any more — input is the direct mapping described above.)

### RAM budget — CRITICAL, read before adding statics

The 48 KB Spectrum RAM (`zx_t.ram[3][0x4000]`) is irreducible, so the
port only just fits the 192 KB region (links with ~15 KB heap headroom).
`.bss`+heap must not cross `0x20030000`. If you overflow `RAM`, the
reclaims that made it fit were:

1. **ZX ROM `const`** — `zx-roms.h` arrays were `unsigned char` (→ 16 KB
   in `.data` RAM!); made `const` → flash. Keep them const.
2. **ROM mapped from flash** — `zx.h` `zx_t.rom[1][0x4000]` (a 16 KB RAM
   copy) replaced with a `const uint8_t* rom0` pointer into the flash
   array. `MODIFIED (md-speccy)`.
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
| Menu shows only the demo / is empty | SD not mounted or `/speccy` unwritable, so the embedded demo couldn't be seeded and no games were found. The ROM boot screen still shows behind the menu. |
| Final steps fail copying UF2 | An upstream compile failed — scroll back for the first error. |
| Undefined ref to `vram_set_dirty_*` | They must be plain `void` functions (not C99 `inline`, which emits no symbol) since `mem.h` calls them. |

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/` — pinned
  submodules, re-pinned every build. Change FatFs config in
  `rp/src/ff/ffconf.h` (project override wins via `BEFORE PRIVATE`).
- Don't add features to `main.c` — start in `emul.c` / `zxemu.c`.
- Keep vendored `rp/src/zx/*` close to upstream; mark any change
  `MODIFIED (md-speccy)` and preserve the original licence header. New files
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
