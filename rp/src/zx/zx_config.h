/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Compile-time config for the ZX Spectrum port.
 *
 * Display, audio, SD game loading and the ST joystick are all always on
 * -- they are proven and there is no reason to build without them.
 * The one remaining knob gates keyboard input; override it from the
 * build (e.g. -DZX_INPUT_KEYBOARD=0) without editing this file.
 */

#ifndef ZX_CONFIG_H
#define ZX_CONFIG_H

/* ST keyboard input (cursor keys + space) -> Kempston / keys. When 0
 * the emulator runs with no keyboard input (joystick only). */
#ifndef ZX_INPUT_KEYBOARD
#define ZX_INPUT_KEYBOARD 1
#endif

#endif /* ZX_CONFIG_H */
