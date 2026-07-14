/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Compile-time phase gates for the ZX Spectrum port.
 *
 * Each phase can be toggled independently so functionality can be
 * tested in isolation on real hardware. Override any of these from the
 * build (e.g. -DZX_AUDIO_YM=0) without editing this file.
 *
 * Phase 1 (display) is the always-on baseline and has no flag: without
 * it there is nothing to see.
 */

#ifndef ZX_CONFIG_H
#define ZX_CONFIG_H

/* Phase 2: ST keyboard input (cursor keys + space) -> Kempston / keys.
 * When 0 the emulator runs with no input -- useful for bringing up the
 * display / audio pipeline against a self-running demo. */
#ifndef ZX_INPUT_KEYBOARD
#define ZX_INPUT_KEYBOARD 1
#endif

/* Phase 3: Spectrum 1-bit beeper -> YM2149 audio. When 0, silent. */
#ifndef ZX_AUDIO_YM
#define ZX_AUDIO_YM 1
#endif

/* Phase 4: load games (and keymaps.txt) from the SD card app folder.
 * When 0 the single baked-in game (builtin_game.h) is run and no SD
 * access happens at all -- lets Phases 1-3 be exercised without an SD
 * card present. */
#ifndef ZX_GAMES_FROM_SD
#define ZX_GAMES_FROM_SD 1
#endif

/* Phase 5: ST joystick -> Kempston. Requires rebuilding the m68k
 * target (IKBD joystick reporting is re-enabled in userfw.s and the
 * RP-side demux parses joystick packets). OFF by default: the
 * template's IKBD joystick path is known-fragile (byte-loss / demux
 * desync), so the reliable keyboard route is the default. */
#ifndef ZX_INPUT_JOYSTICK
#define ZX_INPUT_JOYSTICK 0
#endif

#endif /* ZX_CONFIG_H */
