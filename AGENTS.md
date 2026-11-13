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

**MD/Speccy** — a **ZX Spectrum 48K / 128K emulator** for the Atari ST /
STE / MegaST(E), built on the SidecarTridge Multi-device **framebuffer
template**. Both models run: the 128 boots from the menu's `boot` item
or from a 128K `.z80` / `.sna`, and its AY music plays on the ST's own
YM2149. It is a port of [zx2040](https://github.com/antirez/zx2040)
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
make emu        # run it in Hatari on this computer (EmuMD; see Tests / verification)
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
`builtin_game.h` / `builtin_pongwars.h` are still used — the embedded
games are seeded into `/speccy`. The old `builtin_keymaps.h` / `keymaps.txt` system
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
Both workflows are ports of md-doom's and are **live** (the old ones had
their triggers commented out, so nothing ran automatically).
- `.github/workflows/pr.yml` — on PRs to `main`, runs the same bare
  `make` the release does, so a PR that would break the release fails
  here. Tags nothing, publishes nothing, keeps no artifacts. Read-only
  token; no UUID secret, so it works from forks and builds under the
  sample UUID.
- `.github/workflows/release.yml` — **rolling release on every push to
  `main`** (not `v*` tags any more): builds, tags `version.txt`, moves
  the lightweight `latest` tag, and clobbers the two assets on the
  existing `latest` GitHub Release (title and body untouched). The app
  UUID comes from the `APP_UUID_KEY` secret.

The workflows these replaced came from the upstream template and
published to SidecarTridge's own infrastructure; the S3 upload to
`s3://atarist.sidecartridge.com/` went with them deliberately. Releases
are also no longer versioned GitHub Releases with a CHANGELOG body —
there is one `latest` release whose binaries are replaced. Both
workflows track the *latest* atarist-toolkit-docker
installer rather than pinning v1.3.0, because the pinned one derived the
Docker image tag from the ambient `$VERSION` that `build.sh` exports and
so looked for an image named after the app's own version.

### Tests / verification
No test suite. Verification is: **build succeeds** (both targets),
**boots on hardware**, manual play + serial console. You usually can't
flash — validate math offline (mirror the exact C in a host script, as
the display decode was validated) and let the owner confirm on device.

**EmuMD** runs the whole firmware on the host, inside Hatari: `make emu`
(`emu/emu.mk`; `MACHINE=st` for a plain ST, `RECORD=out.avi` to record
picture and sound). EmuMD is the `emu/emumd` submodule (`git submodule
update --init emu/emumd`, then `make -C emu/emumd hatari` once to build
its Hatari); `mdfw.ini` says how it builds MD/Speccy. Headless, with a
screenshot: `emu/emumd/tools/mdfw run --headless --frames 600
--screenshot out.png`. The emulated microSD is `emu/sd` (git-ignored;
the built-ins are seeded into `emu/sd/speccy` at boot). What it needs
from the firmware: `rp/src/fb_core1.c` is swapped for `emu/fb_core1.c`
(a job and its argument are 64-bit pointers on the host, too wide for the
inter-core FIFO), `emu/shim/` makes the `.cart_app_free` buffers ordinary
variables and stubs `DPRINT_HEAP`, and `emul.c` skips its `.cart_app_free`
linker-window check under `EMUMD`. Pointer casts in shared code must be
`uintptr_t`, not `unsigned int`. The emulated Multi-device is infinitely
fast, so speed (the About box's figures) and the Core 1 audio timer's
cadence are not real — check those on hardware; picture, input, the
cart-bus protocol and the YM/DMA audio paths are.

To drive the menu headless, pass `-- --cmd-fifo FIFO` (Hatari creates
it) and write `hatari-event keydown N` / `keyup N` lines with ST
scancodes (↑ 72, → 77, ← 75, Return 28), a few hundred ms apart.
`keypress N` sends both at once and drops keys.

If Hatari hangs at launch with no output (even `--version`) on macOS,
it's AppKit's "reopen windows?" alert, hidden behind other windows, left
by force-killed Hatari instances: `defaults write org.hatari-emu.Hatari
ApplePersistenceIgnoreState -bool YES`.

---

## Architecture — the framebuffer template (foundation)

Two-target build: m68k assembly runs on the ST, is compiled to a ROM
image, embedded as a C array in the RP2040 firmware, and served back to
the ST over the emulated cartridge bus (PIO + DMA).

### Framebuffer pipeline

Every visible pixel goes through this each VBL:

1. **RP waits for the m68k's end-of-blit ack** — `fb_wait_vbl()` blocks
   until the VBLSYNC read at `$FB8400` shows the m68k has finished copying
   the cart FB; one call per loop paces the app to 50 Hz.
2. **RP writes ST planar data straight into the cart FB** at `$FA8300`
   (`update_display()` in `zxemu.c`, through `fb_cursor_t` in `fb.h`).
   The cart FB's 48-byte chunks are stored **pre-reversed** (so the
   m68k's predec store lands each chunk at its natural position);
   `fb_cursor_t` hides that — a 16-pixel group (8 bytes) never straddles
   a chunk, so it writes whole groups and re-aims at chunk boundaries.
   There is no RP-side chunky buffer or planar scratch copy any more
   (the template's `fb_chunked_buffer` + `fb_planar_scratch` + dual-core
   c2p were removed: 96 KB of RAM, see the RAM budget).
3. **m68k blits cart FB → ST screen** in `userfw.s` via `FBDRV_INLINE`
   (unrolled `movem.l` load + predec store, 12 longwords/48 B per iter),
   unconditionally every VBL. A0 (audio cursor) and A7 (SP) are kept out
   of the MOVEM list. Pure 68000, same code on ST/STE/MegaSTE/TT/Falcon.
   Because the chunks are reversed it copies the **image bottom-up** over
   most of the frame — so the RP writes rows bottom-up too, starting at
   the ack, and stays ahead of it (~5 µs/row against the m68k's ~88).
4. **m68k flips video base** between `$70000`/`$78000` each frame
   (page-flip, tear-free).

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
   VBL read (`fb_wait_vbl` blocks until that read), then the next
   iteration wipes it, so a later reset auto-boots MD/Speccy cleanly.

### Audio pipeline (STE DMA with YM fallback)

Two back-ends, both compiled into `userfw.s`, chosen at boot from the
`_SND` cookie (bit 1 = DMA/PCM sound; design shared with md-mjpeg):

- **STE DMA** — mono 8-bit signed PCM @ 25,033 Hz, loop-mode DMA over a
  double buffer parked in the unused tails of the two screen pages
  (`$77D00`/`$7FD00`, 512 B each — no extra ST RAM). Each VBL
  `.after_copy` copies fresh bytes from the cart audio buffer into
  whichever half the DMA isn't reading (bit 7 of `$FF890B`). Timer-B and
  the YM stay untouched; the exit path stops the DMA and Timer-A before
  returning to GEM. Two details are load-bearing (both learned on
  hardware in md-doom — don't "simplify" either away):
  - **START/END are written by `userfw_snd_irq`, never by the VBL
    loop.** MFP Timer-A runs in event-count mode off the DMA's frame
    end, so the handler re-points the loop at the buffer the chip just
    left, a full frame before the next latch. From the VBL loop the six
    byte writes sat at an arbitrary phase of the DMA frame; when the
    chip's drift brought its frame end into that ~6 µs window it latched
    START from one buffer and END from the other and played the 32 KB
    between them — ~1.3 s of noise, every few minutes.
  - **The buffer length is measured, not assumed.** The DMA and the
    video run off different oscillators, so the chip eats ~501.5 bytes
    per frame, not 500, and it varies by machine. `.after_copy` watches
    the drift of the DMA's within-buffer offset (sampled every frame,
    nudged ±1 every fourth so it can't hunt; jumps >64 are handovers and
    ignored) and steers `UFW_SND_LEN` between `STE_SND_LEN_MIN/MAX`.
    A fixed 500 runs the buffer dry about every 350 frames and replays
    one — the ~7-second crackle. Validated offline: converges and stays
    dry-free for chip rates 498.5–503.2.
- **YM fallback** (plain ST / early TOS) — RP fills the 1 KB cart buffer
  at `$FA4100` with (vA, vB) volume pairs; m68k Timer-B (`/4`, TBDR=110
  → ~5,585 Hz, ~112 fires/PAL VBL) writes both YM volume regs per fire.
  `userfw_vbl` resets the A0 read cursor to the buffer base every VBL.

The m68k reports the detected capability every VBL via a cart read at
`SNDCAP_WINDOW_BASE + UFW_SNDCAP` (`$FB8600..3`: bit 0 DMA sound, bit 1
a TT — see "TT speaker"), and in DMA mode the
measured length at `SNDLEN_WINDOW_BASE + len - STE_SND_LEN_MIN`
(`$FB8C00`). `fb_rom3_dispatch` hands both to
`audio_consume_rom3_sample()`, which owns the windows →
`audio_set_mode()` / `audio_set_fill_bytes()` (`AUDIO_SNDLEN_BIAS` must
match `STE_SND_LEN_MIN`; out-of-band lengths are ignored as bus noise).
`AUDIO_MODE_SILENT` zeros until the first report. **The fill callback
must produce however many bytes it is asked for** — that is what makes
the steering inaudible, and `zxemu_audio_fill` does it by resampling
onto the requested count.

**The refill runs on a 1 ms timer on Core 1, not the main loop**
(`audio_start_vbl_timer(1)` in `emul.c`; a Core 1 alarm pool binds the
IRQ there). The handler peeks the ROM3 ring non-destructively
(`commemul_scan`, its own cursor, so it steals nothing from the IKBD
demux) for the m68k's end-of-blit ack at `$FB8400` and fills right
after it — after the m68k copied the previous buffer, before it copies
the next, so it can never tear under the copy. This decouples audio
from frame rate: the m68k drains its buffer every 20 ms whatever an
emulated frame costs, and when the main loop ran slower than the VBL it
skipped fills and the m68k replayed a stale buffer (the distortion).
Nothing on the emulator path calls `audio_render_frame()` any more.
md-doom measured the same timer on **Core 0** stalling its renderer for
100–200 ms every second or two (mechanism never identified), so keep it
on Core 1. Safe here because md-speccy's only flash write is at boot,
before the timer starts.
`FORCE_NO_DMA=1` (m68k build flag) exercises the YM path on DMA
hardware. **The MFP auto-EOI flip is deliberately common code before the
audio branch** — unlike md-mjpeg (polled ACIA), our interrupt-driven
IKBD handler needs auto-EOI in both modes; putting it in the YM-only
block would wedge the keyboard on STE machines. MD/Speccy installs its
own fill callback (`audio_set_fill_callback`) — see port section.

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
- `fb.c` / `fb.h` — the cart framebuffer (`fb_screen`), the VBL
  frame-sync (`fb_wait_vbl()`, `fb_pump_rom3()`) and the `fb_cursor_t`
  writer for the reversed-chunk layout. The template's chunky buffer,
  c2p, blit/font primitives and boot demo frame were removed.
- `fb_core1.c` — the Core 1 job worker (`fb_core1_dispatch`/`_wait`),
  launched by `fb_init()`. Its only client now is `audio.c`, which
  starts its refill timer on Core 1 through it.
- `commemul.c` — ROM3 cart-bus capture ring. **The ring was shrunk from
  32 KB to 1 KB** for MD/Speccy (`COMM_RING_BITS` 15→10) — it only
  carries IKBD bytes plus a few per-VBL report reads, and the RAM was
  needed for the emulator. `commemul_poll()` consumes; `commemul_scan()`
  peeks non-destructively with a caller-owned cursor, which is how the
  audio refill interrupt watches for the VBL ack without stealing
  samples from the IKBD demux.
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
The system clock is 400 MHz @ `VREG_VOLTAGE_1_30` (see Speed); **Core 1
runs the job worker and the audio refill timer.**

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
- `rp/src/zx/builtin_game.h`, `rp/src/zx/builtin_pongwars.h` — generated
  by `tools/z80_to_header.py`: the embedded `.z80` snapshots, seeded into
  `/speccy`. To add another, run the tool and append a row to
  `BuiltinGames[]` in `zxemu.c`. The arrays are `const` → flash, so they
  cost no RAM.
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
| ST77xx display driver | `fb_wait_vbl()`, then `update_display()` decodes 256×192 VRAM straight into the ST-planar cart FB at (32,4) |
| GPIO buttons | `zxemu_handle_key()` applies ST keys directly via `zx_key_down/up`; the cursor cluster + ST joystick drive `zx_joystick()` (Kempston) |
| PWM beeper on Core 1 | `zxemu_audio_fill` → STE DMA PCM or YM, refilled from a Core 1 timer |
| flash game blob | FatFs enum of `/speccy`, `.z80` via `zx_load_z80`, `.sna` via `zx_load_sna` (both stream from the file) |

### The 128K model

zx2040 had cut chips' `zx.h` down to the 48K; the 128 parts are restored
from upstream (all marked `MODIFIED (md-speccy)`): `ZX_TYPE_128`,
`zx_t.ram[8][0x4000]` (the 48K uses banks 0-2 at `$4000/$8000/$C000`;
the 128 maps 5/2/0), both 128 ROMs as flash pointers (`zx_t.rom[2]`),
3,546,894 Hz, 311 lines / 63 top-border lines, a 36 T interrupt, and the
ports: `$7FFD` (`(A15|A1)==0`, write: RAM bank at `$C000`, screen bank 5
or 7, ROM, lock) and the AY at `$FFFD` (select / read) and `$BFFD`
(write). `_zx_update_memory_map_zx128` only remaps what changed (games
rewrite the same `$7FFD` value a lot; a remap is 16 page-table entries).
**The AY is a register file only** — `ay_addr` + `ay_regs[16]`, masked on
write as upstream / Fuse, port A reads `$BF` in input mode — so software
reads back what it wrote; nothing is synthesised on the RP — the ST's YM
plays it (see Audio, "AY on the YM"). `ay_env_seq` counts envelope-shape
(R13) writes. `zx_exec` stops at `top_border_scanlines + 192`.

Model selection: `zx_start(type)` in `zxemu.c` cold-starts a model
(`zx_init` wipes the machine, so it re-registers the extra key codes,
keeps the menu's `scan-p`, and carries the beeper ring indices across the
wipe for the Core 1 fill, which reads `zx_audiobuf_store` directly). The
menu's `boot` item (left/right: 48K/128K, fire: cold start) calls it;
power-on is the 48K. Loaders call `zx_prepare_model()` after validating
a file and before writing RAM: a cold start if the snapshot's model
differs, else `zx_reset()` (so a 128 left locked by the last game is
unlocked), then all RAM cleared. Note `scan-p` (default 150), not the model, sets T-states per
emulated frame, so the 128 costs the same per frame as the 48K.

Validated offline on the host build of the core: a Z80 program on the
128 checks banking through `$C000`, the lock, the ROM switch, the dirty
hook (only displayed-bank writes, screen switches reported) and AY
read-back; both ROMs boot (48K copyright screen; the 128 menu, 128 BASIC
and 48 BASIC from it, which pages ROM 1 and locks paging).

### Display decode (validated offline)

`update_display()` (in `zxemu.c`, `__not_in_flash_func`, `-O2` region)
runs right after `fb_wait_vbl()` and writes the ST-planar cart FB
directly. It is **dirty-row incremental**: the `mem.h` write hooks
maintain a per-row bitmap (`EMU.dirty_vram`, one bit per scanline;
attribute writes mark all 8 covered rows) and only flagged rows are
re-decoded; the border is repainted only when the border colour changes
(`EMU.last_update_border_color`, `0xff` = forced). The cart FB keeps the
previous frame between calls, which is what makes skipping clean rows
correct. **Anything that touches VRAM behind the hooks must call
`vram_force_dirty()` first** — current callers: the FLASH blink toggle,
the menu/About overlay draw *and* its restore, `load_game()` (snapshots
write VRAM directly), and `init_emulator()`. Missing one shows up as
stale screen regions.

The hook in `mem.h` is keyed on the **host address within the displayed
screen bank** (`vram_base`), not on Z80 `$4000-$5AFF`: on the 128 the
screen can be written through `$C000`, and bank 7 can be the screen.
`vram_set_dirty_*` take the offset in the display file. `zx.h` calls
`vram_set_display()` whenever the displayed bank changes (machine start,
`$7FFD` bit 3), which updates `vram_base` and forces a full repaint.

Per row (bottom-up — see the framebuffer pipeline) it reads the Spectrum
bitmap byte at `((py&0xC0)<<5)|((py&0x07)<<8)|((py&0x38)<<2)|(px>>3)` and
attribute at `0x1800+((py>>3)<<5)+(px>>3)`, applies BRIGHT
(`(attr&0x40)>>3` → +8) and FLASH (swap ink/paper on blink phase), and
writes each pair of cells as one 16-pixel ST group (two 32-bit stores).
Per pair of planes a cell is `paper ^ ((ink ^ paper) & bits)` over the
byte-lane masks in `zx_plane_lanes[4]` (lanes 0/2 = right cell, << 8 =
left cell). Validated offline: byte-identical cart-FB contents to the old
chunky decode → c2p → chunk-reversed publish (mirrored in C from the
asm) over 2,000 random frames of game writes, border/FLASH/overlay
changes, from three different starting cart contents. A full repaint is
~106 Thumb instructions per cell pair (register-starved on M0+), so
~1 ms at 400 MHz; the About pop-over's `dsp` figure reports the mean and
worst decode per second. The 16 ZX colours are pushed to the ST shifter
palette in `zx_set_palette()` (`zxpalette` is `0x00BBGGRR` →
`PALETTE_RGB` 3-bit channels).

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

`ikbd.c` runs a small state machine consuming `$FE` (joystick 0) / `$FF`
(joystick 1) / `$FD` (both, 0 then 1) packets into per-port
`s_joy_state[2]` (bit0 up,1 down,2 left,3 right,7 fire; masked to
`$8F`), with `ikbd_get_joystick()` reporting **port 1 only** — with
event reporting on (`$14`) the port-0 mouse reports as joystick 0, and a
stationary mouse's quadrature lines latch a steady non-zero byte that
would otherwise clobber the real stick (games polling Kempston for a
clear port then never start); `zxemu_render_frame` folds those
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

`zxemu_audio_fill(buf, bytes)` decimates the beeper into whichever
format `audio_get_mode()` reports. The emulator samples the 1-bit beeper
into `zx.audiobuf` during `zx_exec` (enabled because `SPEAKER_PIN !=
-1`; one bit per 16 ticks into a 256×32-bit ring ≈ 218.75 kHz). Each
per-VBL fill box-filters the ~5.5 K bits since the last fill into output
windows whose bounds tile the span exactly (Bresenham — a fixed
`avail/nsamp` step used to drop the division remainder, ~0.4 ms of
timeline per fill, phase-jumping every sustained tone at 50 Hz). In
**DMA mode** each window's duty maps linearly to a signed sample
(−amp..+amp, amp from the menu volume) — no companding, the PCM value
IS the amplitude. In **YM mode** duty goes through `duty_att[]`
(round(−2·log₂ duty) YM steps below the menu-volume peak) so linear
amplitude tracks duty on the YM's ~3 dB/step logarithmic DAC — a plain
`duty*vmax` companded the filtered edges into near-silence. Same level
on both YM channels. The
number of output windows is whatever the m68k asked for that VBL, which
is what lets the DMA length steering work; don't hard-code it.
(`AUDIO_FRAME_PERIOD_US` / `audio_render_frame()` are now only the
fallback path for apps that still pump audio from their main loop —
md-speccy uses the Core 1 timer instead.) The result is approximate on
YM ("recognisable, not hi-fi") and considerably cleaner on STE DMA,
where 25 kHz linear PCM resolves the beeper's square edges ~4.5x finer
than the 5.6 kHz log-DAC path.

**The fill runs in a Core 1 interrupt while `zx_exec` produces on
Core 0.** It reads `audiobuf_byte` then `audiobuf_bit`, in that order,
which is what keeps it safe without a lock: the producer only moves
forward, so a torn read yields a write index at or behind the true one,
never ahead — `avail` can't wrap negative. Keep that order if you touch
it.

### AY on the YM (the 128's music)

The YM2149 is the AY-3-8912's sibling, so the 128's music plays on the
real chip instead of being synthesised. Once per frame, right after
`update_display()` (inside the post-ack window, so it never races the
m68k), `zx_ay_publish()` writes the AY block at `$FA4500`
(`CART_AY_OFFSET`, 16 words: ACTIVE, R13SEQ, R0-R13 as the low byte of
each word) and `ufw_ay_apply` in `userfw.s` copies it into the YM at
the top of the next VBL, at IPL 7 (~100 µs; ~10 µs when idle). On the way:

- tone / noise / envelope periods are rescaled from the AY clock
  (1,773,447 Hz) to the YM's (8,010,613 / 4 = 2,002,653 Hz) and clamped
  (A4 lands 2.6 cents flat — integer periods);
- mixer bits 6/7 are forced on — the YM's ports are the ST's floppy
  select lines and printer port and must stay outputs; R14/R15 are never
  sent;
- fixed volumes follow the menu volume; envelope-driven ones can't;
- R13 is only written when `R13SEQ` moved (or when the AY has just taken
  the YM), because writing it restarts the envelope.

**Who owns the YM.** On a DMA (STE) machine the beeper is DMA sound, so
the YM is the AY's and both play together; ACTIVE going low just zeroes
the three volumes. On a plain ST the beeper *is* the YM, as a DAC driven
by Timer-B, whose handler relies on reg 8 staying latched between fires.
So when ACTIVE rises the m68k masks Timer-B (IMRA bit 0) — the beeper
goes quiet while the AY plays — and when it falls it restores the
boot-time DAC setup (mixer `$FC`, R0-R3 = 0, ch C/B/A volumes 0, **reg 8
latched last**) before unmasking. ACTIVE is held for 50 frames after the
AY was last audible so music rests don't flip the YM back and forth.
The exit to GEM silences the YM (mixer `$FF`, volumes 0).

Limits: registers move once per frame — music drivers update once per
interrupt, but sample playback through the AY volume registers (and
beeper sound effects on a plain ST while music plays) is lost.

Validated offline: a host test of `zx_ay_publish` (rescaling, clamps,
mixer bits, volume, R13 sequence, the hold); the core test checks R13
writes bump `ay_env_seq`; `ufw_ay_apply` was disassembled from
`BOOT.BIN` (capstone) and checked instruction by instruction; 44 of the
248 TOSEC 128K games have AY channels sounding 5 s after loading on the
host core. Run end to end in EmuMD (see Tests / verification) on an STE
and a plain ST: Taipan's music plays at the pitches the host core's AY
registers give, stops on the menu's exit, and on the plain ST the
beeper's key clicks and `BEEP` come back after a 48K is started.

### TT speaker

On a TT only, the menu has a **TT speaker** setting (0 = off, the
default; 1 = on) for the TT's internal speaker.
**Setting bit 6 of the YM's port A switches it off**; TOS leaves the
bit clear, with the speaker on. That is the SidecarTridge Booster's
polarity (`rp2-booster-bootloader`, `term/atarist/src/main.s`), and a
real TT confirmed it: before v2.0.3 this had it the other way round,
so the default, off, left the speaker on. Hatari doesn't emulate the
speaker, so only hardware can tell. Don't reuse this on a Falcon
without checking its polarity.

- `userfw.s` finds a TT from `_MCH` (high word 2) through `ufw_cookie`
  (shared with the `_SND` lookup) and reports it as bit 1 of SNDCAP;
  `audio_has_tt_speaker()` latches it (a list that only grows keeps the
  menu selection valid).
- The item is `SettingsList[0]` and must stay first: elsewhere the menu
  starts one entry later (`settings_first()` / `settings_count()` /
  `settings_id()` map menu positions to entries).
- The RP writes the setting every frame to `CART_SPEAKER_OFFSET`
  (`$FA4520`, just after the AY block); zero, as the erased region
  starts, is off. `ufw_speaker_apply` read-modify-writes port A at IPL 7
  on a TT when the value changes (the other bits are the floppy selects,
  printer strobe, RTS/DTR and LAN select), then re-latches reg 8 for the
  Timer-B beeper. Boot saves port A; the exit to GEM puts bit 6 back.

Run in EmuMD with `make emu MACHINE=tt TOS=<EmuTOS 512k image>`: the
256k EmuTOS EmuMD downloads can't run a TT, and Hatari silently falls
back to an STE. Hatari's `--trace psg_write` shows the port A writes.

### SD games

`/speccy` (config `ACONFIG_PARAM_FOLDER`). `populate_games_list()` enumerates
`.z80` and `.sna` (`snapshot_type()` classifies by extension);
`load_game()` **streams** the snapshot from the file straight into
Spectrum RAM through a 512-byte buffer (`s_load_buf`, in the cart hole);
each snapshot selects its own model (`zx_prepare_model`).

- `zx_load_z80`: versions 1-3. RLE decoded on the fly (a block may end
  in a lone `ED`, whose following byte belongs to the next block header —
  don't consume it), raw `0xFFFF` pages and uncompressed v1 handled.
  `z80_model()` maps the hardware mode: v2 0/1 and v3 0/1/3 are the 48K
  (incl. IF1 / MGT variants, loaded even if their ROM was paged in — some
  then reset to BASIC), v2 3/4 and v3 4/5/6/12 the 128 (incl. +2); SamRam,
  +2A/+3, Pentagon, Timex are rejected before anything is written. The
  extended header is indexed by the spec's byte offsets (`Z80X_*`): the
  vendored `_zx_z80_ext_header` leaves out the 16 AY registers, so its
  later fields are misaligned. 128: pages 3-10 → banks 0-7, then `$7FFD`
  and the AY registers/selection from the extended header.
- `zx_load_sna`: 48K (27-byte header + 48 KB, PC popped RETN-style) or
  128K (131,103 / 147,487 B: banks 5, 2 and the paged bank, then PC,
  `$7FFD`, TR-DOS flag, then the remaining banks ascending). The `$7FFD`
  byte sits after the first 48 KB but decides where its last 16 KB go, so
  it is read first via `zx_rd_seek()` (`f_lseek`). No AY state in `.sna`.
  TR-DOS-paged images are rejected.

Both live in `zxemu.c`; the vendored `zx_quickload` is unused.

Validated offline on the TOSEC set in the git-ignored `tosec/` folder
(4,719 `.z80`): all 4,435 48K files match the vendored `zx_quickload`
except deliberate fixes (raw pages it refused, uncompressed v1 it
mis-decoded or asserted on, `flags0 = 0xFF` read as 1 per the spec); an
independent Python decoder written from the spec agrees on every bank,
PC, `$7FFD`, AY register, screen bank, lock and CPU-visible mapping for
all 4,435 48K and 248 128K files (35 unsupported machines and 1 corrupt
file rejected); all 248 128K games run to title / game screens on the
host core (sampled visually); and 128K `.sna` conversions of all 248
(both sizes) load to the identical machine. No game auto-loads: boot leaves the menu active so the
user always picks. After the scan, each entry in `BuiltinGames[]` is
written to `/speccy` with `FA_CREATE_NEW` — present already, nothing
happens; deleted by the user, it reappears next boot. (There is no keymap
file any more — input is the direct mapping described above.)

### RAM budget — CRITICAL, read before adding statics

The 128 KB of Spectrum RAM (`zx_t.ram[8][0x4000]`, all eight banks,
resident) is irreducible, and the 22 KB Z80 decoder is pinned into RAM as
well. `.bss`+heap must not cross `0x20030000`. It fits because the
framebuffer pipeline stopped using RP RAM (no chunky buffer, no planar
scratch: 96 KB).

**The heap floor is ~9.5 KB — an overflow here does NOT fail the link.**
The boot-time settings library mallocs a **4 KB buffer each for gconfig
and aconfig (held forever)** plus a ~4.2 KB transient per init
(`settings_init`, entries copy), peaking at ~8.4 KB *before `emul_start`
even runs*. On top of the 8.2 KB held, FatFs (LFN=3 + exFAT) adds
~1.1 KB per open file/dir — one context at a time, because
`populate_games_list` closes its dir before the builtin seeding opens a
file — and the Core 1 audio alarm pool ~56 B, so runtime peaks at
~9.4 KB. (Nest a `f_open` inside an open dir and that becomes ~10.5 KB.)
If a boot-time malloc fails, `gconfig_init`/`aconfig_init` fail and
`main.c` **jumps to Booster** — the symptom is "app launches then lands
straight back in Booster", which looks like a crash but is a deliberate
bail. Check heap after every RAM change: `0x20030000 - __bss_end__` in
the `.map` (currently 25,260 B release / 24,948 B debug).

**Byte arithmetic is not enough**: newlib's full malloc grows the heap
in **page-rounded (4 KB) sbrk steps**, so without countermeasures a
malloc can fail on the rounding excursion while the actual bytes fit —
the settings sequence needs a ~12.5 KB window that way, and diagnosing
it from the `.map` shows a seemingly-sufficient heap. The build sets
`PICO_USE_OPTIMISTIC_SBRK=1` (rp/src/CMakeLists.txt) so an over-limit
sbrk grant is clamped to `__StackLimit` instead of failing; newlib's
dlmalloc re-queries the actual break after each grant (verified in the
disassembly), so the clamp is safe and the whole window is usable. Do
not remove that define — every build maps back to Booster without it.

If you overflow `RAM` (or the heap floor), the reclaims that made it
fit were:

1. **ZX ROM `const`** — `zx-roms.h` arrays were `unsigned char` (→ 16 KB
   in `.data` RAM!); made `const` → flash. Keep them const.
2. **ROM mapped from flash** — `zx.h` `zx_t.rom[1][0x4000]` (a 16 KB RAM
   copy) replaced with a `const uint8_t* rom0` pointer into the flash
   array. `MODIFIED (md-speccy)`.
3. **ROM3 ring 32 KB→4 KB→2 KB** — `commemul.c COMM_RING_BITS` 15→12→11.
4. **The cartridge-region hole** — the shared 64 KB region has 15,872
   unused bytes between `CART_APP_FREE_OFFSET` (`$4500`) and
   `CART_FRAMEBUFFER_OFFSET` (`$8300`) — verified unused: the m68k
   defines `APP_FREE_ADDR` but never references it, and every RP-side
   cart writer stays outside it. It is ordinary SRAM, so `memmap_rp.ld`
   maps a `CART_APP_FREE` region over it and a `.cart_app_free` output
   section parks `commRing` (1 KB), `GamesTable` (2.25 KB),
   `s_vram_save` (6.75 KB), the `zx_t` audio ring (1 KB, now a pointer —
   `MODIFIED` in `zx.h`), both `mem.h` dummy pages (2 KB, `MODIFIED`)
   and the snapshot load buffer (512 B) there — **~13 KB reclaimed from
   `RAM`**, ~1.25 KB still free. Tag a buffer into it with
   `__attribute__((section(".cart_app_free.<name>")))`.
   Constraints: the section is `NOLOAD`, so nothing there is zero-inited
   by the CRT — only park buffers first touched **after** `emul_start()`
   wipes the region (`ERASE_FIRMWARE_IN_RAM()`), and never anything a
   pre-`main` consumer (newlib, settings, stdio) relies on being zeroed.
   The DMA ring must come first: an output section inherits its widest
   input alignment, so a naturally-aligned buffer placed later drags the
   whole block up and wastes a full alignment unit. The region starts at
   `$4800`, not `$4500`, for the same reason. Overflow is caught at link
   time by the region length.
5. **ROM3 ring 4 KB→1 KB** (on top of reclaim 3) and
   **`ZX_MAX_GAMES` 128→64** — both sized generously vs. actual use.
6. Dropped a 50 KB `static zx_t im` from the unused `zx_load_snapshot`
   (it was already `--gc-sections`'d away, so this was cosmetic — the
   real wins were 1–5).
7. **No RP-side framebuffer** — the template's 64,000 B chunky buffer
   and 32,000 B planar scratch (plus the c2p and reverse-copy code in
   RAM) went when `update_display()` started writing the cart FB
   directly; snapshot loading, which borrowed the chunky buffer, now
   streams. Heap window 10,772 → 107,616 B (release). The same move is
   what freed md-xt's RAM for its page cache.

Diagnose overflow with the linker `.map` (`rp/build-*/rp.elf.map`), not
by estimating: `--gc-sections` drops unused statics, and non-`const`
arrays silently land in `.data` RAM. `arm-none-eabi-size rp.elf` shows
the totals. A host `sizeof` probe over `rp/src/zx/*.h` (define
`SPEAKER_PIN`, stub `vram_set_dirty_*`, `vram_set_display`, `vram_base`)
gives struct sizes (a 64-bit host inflates the page-table pointers: 137,600
B there); on target the whole `EMU` (mostly `zx_t`) is 135,128 B.

### Frame pacing

`zx_exec()` runs *at least* the requested ticks and then keeps going until
the raster reaches the end of the bitmap (scanline 256), so what a call
actually costs is set by that exit, not by the microseconds asked for.
`zx_frame_usec()` in `zxemu.c` requests 200 scanlines' worth — comfortably
inside the `(1, 256)` scanline window for every `scanline_period` the
`scan-p` menu item allows — so **one call advances exactly one emulated
frame**, matching the 50 Hz VBL that `fb_wait_vbl()` already paces us to.

Upstream's flat `FRAME_USEC` of 25000 µs overshot the first frame's
scanline 256 and ran on to the second: two emulated frames of work per
displayed frame, i.e. 4.70 M ticks/s needed for full speed instead of
2.35 M. That suited zx2040's SPI panel, which redrew far below 50 Hz.
Don't reintroduce a fixed µs figure here. Validated offline by mirroring
the `zx_exec` loop and `_zx_tick`'s raster logic over `scanline_period`
10..500.

### Speed

Runs at the template's 225 MHz, sharing Core 0 with the cart bus.

Measured at 225 MHz with `z80_tick` in flash: **39.5 ms per emulated
frame = 1.19 M ticks/s = 51% of real time**, i.e. 189 CPU cycles per
`z80_tick` call. An interpreter step should be 30–60, so the bulk of that
was XIP stall — `z80_tick` is ~22 KB against a 16 KB XIP cache, so it
missed on essentially every tick. Hence the `__not_in_flash_func` on it
(`z80.h`, `MODIFIED (md-speccy)`) and the RAM reclaims above. Note this
also means **overclocking alone could never have fixed it**: full speed
needs 1.98x and 400 MHz offers at most 1.78x of core, diluted by a slower
flash divider.

**Measuring:** `zxemu_render_frame()` times `zx_exec()` and averages over
a ~1 s window into `EMU.perf_exec_us` / `EMU.perf_fps_x10`, plus the
display decode (`perf_disp_us`, worst frame `perf_disp_max_us`) and the
time blocked in `fb_wait_vbl()` (`perf_wait_us`). The About pop-over
shows them (`emu 13.2ms  fps 50.0`, `dsp 0.3/1.0 wait 5.1ms`), so a
release build on hardware reports its own speed; debug builds also
`DPRINTF` them each window. Under 20 ms of emulation per frame means full speed. The figures
include the pop-over's own compositing cost, so they read slightly below
the in-play rate.

What was tried, and what the hardware measurements showed:

1. **`z80_tick` out of XIP — done, and it bought nothing** (39.5 →
   40.3 ms measured). The hot flash set was effectively cached all
   along; the emulator is **CPU-bound**, not flash-bound. The pinning is
   kept (it makes the hot path immune to the slower flash divider at
   400 MHz) but don't expect residency changes to move the needle.
2. **`-O2` on the per-tick path** (`z80_tick` in `z80.h`; `_zx_tick` +
   `zx_exec` in `zx.h`, via `#pragma GCC push_options` regions — the
   file default stays `-Os`). `-O2` `z80_tick` is slightly *smaller*
   than `-Os` (22,124 B), so no RAM cost.
3. **400 MHz / 1.30 V overclock** (`constants.h`), the only clock target
   that wins: the SSI divisor must be even, so flash drops /2 → /4
   (112.5 → 100 MHz; at 300 MHz sys, /4 = 75 MHz — slower than stock).
   The divider is baked into boot_stage2 via a directory-property
   definition in `rp/src/CMakeLists.txt` (verified in the bs2
   disassembly: BAUDR = 4). The cart-bus PIO keeps its proven 225 MHz
   wall-clock timing via `SAMPLE_DIV_FREQ = RP2040_CLOCK_FREQ_KHZ /
   225000.f` (fractional-divider jitter ≤1 sysclk ≈ 2.5 ns against a
   ~71 ns settle budget). `main.c` raises the voltage **before** the
   clock (never run the higher frequency at the lower voltage, even
   transiently). If a board can't do 400 MHz (silicon lottery), the
   step-down ladder is in `constants.h`; remember /2 flash is only safe
   at 225 MHz.

Expected combined effect: 40.3 ms × ~0.85 (O2) ÷ 1.78 (clock) ≈ 19 ms —
right at the 20 ms budget. If it lands short, the remaining ideas are
per-game `scan-p` tuning (fewer emulated ticks/frame) and shaving
`update_display`.

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
| Menu shows only the built-ins / is empty | SD not mounted or `/speccy` unwritable, so the embedded games couldn't be seeded and no games were found. The ROM boot screen still shows behind the menu. |
| Final steps fail copying UF2 | An upstream compile failed — scroll back for the first error. |
| Undefined ref to `vram_set_dirty_*` | They must be plain `void` functions (not C99 `inline`, which emits no symbol) since `mem.h` calls them. |

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, `fatfs-sdk/`, `emu/emumd/` — pinned
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
