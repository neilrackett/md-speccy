/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Compile-time gates for the ZX Spectrum port.
 *
 * Display, audio and SD game loading are always on -- they are proven
 * and there is no reason to build without them. The two gates that
 * remain toggle the input paths; override either from the build
 * (e.g. -DZX_INPUT_JOYSTICK=1) without editing this file.
 */

#ifndef ZX_CONFIG_H
#define ZX_CONFIG_H

/* ST keyboard input (cursor keys + space) -> Kempston / keys. When 0
 * the emulator runs with no keyboard input. */
#ifndef ZX_INPUT_KEYBOARD
#define ZX_INPUT_KEYBOARD 1
#endif

/* ST joystick -> Kempston. Requires rebuilding the m68k target (IKBD
 * joystick reporting is re-enabled in userfw.s and the RP-side demux
 * parses joystick packets). OFF by default: the template's IKBD
 * joystick path is known-fragile (byte-loss / demux desync), so the
 * reliable keyboard route is the default. */
#ifndef ZX_INPUT_JOYSTICK
#define ZX_INPUT_JOYSTICK 0
#endif

#endif /* ZX_CONFIG_H */
