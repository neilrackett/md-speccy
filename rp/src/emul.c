/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: emul.c
 * Description: Boot path + main loop for the ZX Spectrum port. Brings up
 *              the cartridge bus emulator, the 320x200 framebuffer, the
 *              ROM3 command-capture ring, the SD card and the audio
 *              buffer, then hands off to the emulator (zxemu.c): drain
 *              IKBD -> feed keys -> render one Spectrum frame per VBL ->
 *              refill the YM audio buffer.
 */

#include "emul.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aconfig.h"
#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "fb.h"
#include "ff.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "settings/settings.h"
#include "target_firmware.h"
#include "zxemu.h"

/* Cart-corruption canary: the first 16 bytes of the served cart region
 * (the m68k cartridge header + entry) must stay byte-identical to the
 * embedded image after COPY_FIRMWARE_TO_RAM. Any change means something
 * on the RP wrote into the region the m68k is executing from -- which
 * makes the ST bomb / freeze on the boot frame. Logged (debug builds
 * only) with a stage tag so the culprit can be localized over UART. */
static bool cart_check(const char *stage) {
  static bool ok = true;
  if (ok &&
      memcmp((const void *)&__rom_in_ram_start__, target_firmware, 16) != 0) {
    ok = false;
    const volatile uint16_t *c = (const volatile uint16_t *)&__rom_in_ram_start__;
    DPRINTF("CART REGION CORRUPTED (%s): %04X %04X %04X %04X\n", stage, c[0],
            c[1], c[2], c[3]);
  }
  return ok;
}

void emul_start() {
  // Games live in the app folder (per-app config
  // ACONFIG_PARAM_FOLDER, default "/speccy" from aconfig.c). The default is
  // the right value -- a fresh config sector gets "/speccy" and no MD/Speccy
  // build ever stored anything else -- so no runtime "healing" needed.
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/speccy";

  // The .cart_app_free buffers live inside the shared region's unused
  // hole. The linker script hard-codes that window (ld cannot read C
  // headers), so verify it here against cart_shared.h's authoritative
  // offsets -- a drifted script would let the ST-visible layout and the
  // parked buffers collide.
  {
    extern char __cart_app_free_start__[], __cart_app_free_end__[];
    const char *base = (const char *)&__rom_in_ram_start__;
    if (__cart_app_free_start__ < base + CART_APP_FREE_OFFSET ||
        __cart_app_free_end__ > base + CART_FRAMEBUFFER_OFFSET) {
      panic(".cart_app_free outside the shared-region hole");
    }
  }

  // Zero the whole 64 KB shared region so every byte the m68k can see is
  // deterministic, then copy the cartridge image into it.
  ERASE_FIRMWARE_IN_RAM();
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // IKBD ring before commemul (its producer runs from the main loop).
  ikbd_init();

  // Cartridge ROM4 read engine (served by chained DMA -> PIO, no CPU).
  if (init_romemul(false) < 0) {
    panic("init_romemul failed");
  }

  // ROM3 cart-bus capture ring (PIO + DMA) BEFORE fb_init, whose first
  // fb_publish() drains it while waiting for the m68k VBL ack.
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }

  // 320x200 4bpp framebuffer + fb_screen for the draw primitives.
  // (This launches Core 1 for the chunky->planar worker.)
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }
  DPRINTF("fb_init OK\n");

  // Default palette; zxemu_init() overwrites it with the ZX palette.
  palette_init();

  // Cart audio buffer producer. The beeper fill callback is installed
  // after the emulator core is up (it reads emulator state).
  audio_init();

  // Drive the refill from a 1 ms timer on Core 1 rather than the main
  // loop: the m68k drains the cart buffer every VBL whatever the
  // emulated frame costs, and a missed refill replays a stale buffer
  // (audible as distortion). The Core 1 alarm pool binds the interrupt
  // to Core 1 so it never preempts the emulator on Core 0.
  audio_start_vbl_timer(1);

  // SD card -- best effort. Games live in `folderName`.
  FATFS fsys;
  cart_check("pre-sd");
  bool sd_ok = sdcard_initFilesystem(&fsys, folderName) == SDCARD_INIT_OK;
  if (!sd_ok) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }
  cart_check("post-sd");

  // Cartridge SELECT button -- apps can poll select_isPressed().
  select_configure();

  // Bring up the emulator: palette, Z80 core, games list, first game.
  DPRINTF("zxemu_init...\n");
  zxemu_init();
  cart_check("post-zxemu-init");
  DPRINTF("zxemu_init OK\n");

  // Route Spectrum beeper audio out the YM2149.
  audio_set_fill_callback(zxemu_audio_fill);

  // Main loop:
  //   0. Re-arm the command sentinel to NOP (makes an exit's BOOT_GEM a
  //      one-shot so it doesn't re-trigger on the next ST reset).
  //   1. Drain the ROM3 ring -> IKBD demux + VBL frame-sync.
  //   2. Run the IKBD demux.
  //   3. Forward decoded key events to the emulator (button mapping).
  //   4. Render one Spectrum frame into the framebuffer (blocks on VBL).
  //   5. Refill the YM audio buffer.
  DPRINTF("Entering main loop\n");
  while (true) {
    ikbd_clear_command();
    fb_pump_rom3();
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      zxemu_handle_key(&k);
    }

    zxemu_render_frame();
    // No audio_render_frame() here: the Core 1 timer owns the refill.
  }
}
