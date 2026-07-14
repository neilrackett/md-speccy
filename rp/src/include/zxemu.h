/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * zxemu.h -- app-facing entry points for the ZX Spectrum emulator
 * running on the SidecarTridge Multi-device framebuffer template.
 *
 * emul.c drives these from its main loop the same way it drove the
 * demo dispatcher: init once, feed decoded keyboard events, render a
 * frame per VBL, and (when audio is enabled) hand the beeper->YM fill
 * callback to the audio layer.
 */

#ifndef ZXEMU_H_INCLUDED
#define ZXEMU_H_INCLUDED

#include <stdint.h>

#include "ikbd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up: palette, emulator core, games list, first game.
 * Call after fb_init() / palette_init() / SD mount in emul_start(). */
void zxemu_init(void);

/* Render one Spectrum frame into the framebuffer and publish it
 * (blocks on the ST VBL via fb_publish, pacing the loop to 50 Hz).
 * Call once per main-loop iteration. */
void zxemu_render_frame(void);

/* Feed one decoded Atari ST key event (press/release) to the emulator.
 * Maps cursor keys + space to the gameplay buttons. No-op when
 * keyboard input is compiled out. */
void zxemu_handle_key(const ikbd_key_event_t *k);

/* Per-VBL YM fill callback: converts the emulated 1-bit beeper into
 * (vA, vB) volume pairs. Install via audio_set_fill_callback(). */
void zxemu_audio_fill(uint8_t *buf, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* ZXEMU_H_INCLUDED */
