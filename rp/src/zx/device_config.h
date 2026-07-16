/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * SidecarTridge Multi-device "device" configuration for the ZX
 * Spectrum port. This replaces the per-board device header from
 * upstream zx2040 (which described a Pico + ST77xx + GPIO buttons).
 *
 * On this target there is no ST77xx display and no GPIO buttons:
 *   - the "display" is the 320x200 framebuffer that the firmware blits
 *     to the Atari ST each VBL (see fb_chunked.h), and
 *   - input is decoded from the Atari ST keyboard / joystick on the
 *     RP2040 and applied directly to the emulator in zxemu.c, via
 *     zx_key_down/zx_key_up (Spectrum keys) and zx_joystick (Kempston).
 *
 * The abstract-button / keymap indirection from upstream zx2040 was
 * dropped in favour of that direct mapping, so this header only carries
 * the audio and display metrics the core and UI still reference.
 */

#ifndef ZX_DEVICE_CONFIG_H
#define ZX_DEVICE_CONFIG_H

#include <stdint.h>

#include "zx_config.h"

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */

/* The upstream emulator samples the 1-bit beeper into zx_t.audiobuf
 * only when SPEAKER_PIN != -1; the YM fill callback in zxemu.c
 * consumes that buffer. There is no real PWM pin -- this just keeps the
 * upstream beeper-sampling path enabled. */
#define SPEAKER_PIN 1

/* ------------------------------------------------------------------ */
/* Display metrics (VRAM coordinate space)                            */
/* ------------------------------------------------------------------ */

/* The UI primitives (ui_fill_box / ui_draw_menu / ...) draw directly
 * into the 256x192 Spectrum VRAM, so the "display" the UI lays out
 * against is the Spectrum bitmap itself. */
#define st77_width  256
#define st77_height 192

/* Kept only to satisfy the settings table / init references carried
 * over from upstream; brightness has no effect on this target. */
#define ST77_MAX_BRIGHTNESS 100

/* Upstream display defaults. On this target the Spectrum bitmap is
 * shown 1:1, centred in the 320x200 framebuffer, so scaling is always
 * 100% and the surrounding margin is the Spectrum border. */
#define DEFAULT_DISPLAY_SCALING        100
#define DEFAULT_DISPLAY_BORDERS        1
#define DEFAULT_DISPLAY_PARTIAL_UPDATE 0

#endif /* ZX_DEVICE_CONFIG_H */
