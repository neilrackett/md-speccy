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
 *   - the "buttons" are abstract IDs whose state is driven from Atari
 *     ST keyboard / joystick input decoded on the RP2040.
 *
 * The upstream emulator core reaches input solely through the
 * get_device_button(id) macro, so redefining it here to read a
 * software bitmask preserves the entire keymap / macro system
 * unchanged.
 */

#ifndef ZX_DEVICE_CONFIG_H
#define ZX_DEVICE_CONFIG_H

#include <stdint.h>

#include "zx_config.h"

/* ------------------------------------------------------------------ */
/* Buttons                                                            */
/* ------------------------------------------------------------------ */

/* The five gameplay "buttons" are abstract bit positions in
 * zx_input_mask (bit N set => button N held). The upstream keymap
 * parser and handler reference these as KEY_* pin identifiers. Values
 * must stay below 0x80 so they never collide with the keymap control
 * codes KEY_EXT (0x80), RELEASE_AT_TICK (0xFD), PRESS_AT_TICK (0xFE)
 * or KEY_END (0xFF). */
#define ZX_BTN_LEFT  0
#define ZX_BTN_RIGHT 1
#define ZX_BTN_FIRE  2
#define ZX_BTN_UP    3
#define ZX_BTN_DOWN  4

#define KEY_LEFT  ZX_BTN_LEFT
#define KEY_RIGHT ZX_BTN_RIGHT
#define KEY_FIRE  ZX_BTN_FIRE
#define KEY_UP    ZX_BTN_UP
#define KEY_DOWN  ZX_BTN_DOWN

/* Live button state, updated by zxemu_handle_key() from decoded Atari
 * ST input. Defined in zxemu.c. */
extern uint32_t zx_input_mask;

#define get_device_button(id) ((zx_input_mask >> (id)) & 1u)

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
