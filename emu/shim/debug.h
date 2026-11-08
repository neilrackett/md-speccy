/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: debug.h
 * Description: rp/src/include/debug.h for EmuMD: EmuMD's DPRINTF (to
 *              Hatari's log with mdfw run -V), plus the heap probe the
 *              firmware's own debug.h provides, as a no-op.
 */

#ifndef MDSPECCY_EMU_DEBUG_H
#define MDSPECCY_EMU_DEBUG_H

#include_next "debug.h"

#ifndef DPRINT_HEAP
#define DPRINT_HEAP()
#endif

#endif
