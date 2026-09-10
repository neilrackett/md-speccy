/**
 * File: commemul.h
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#ifndef COMMEMUL_H
#define COMMEMUL_H

#include <inttypes.h>
#include <stdbool.h>

#include "pico/stdlib.h"

typedef void (*CommEmulSampleCallback)(uint16_t sample);

// Returns 0 on success, < 0 on failure (PIO program load failed). The
// PIO state-machine and DMA-channel claims call the SDK's "panic on
// exhaustion" variants, so those paths abort the whole boot rather
// than returning here.
int commemul_init(void);
void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback);

/* Non-consuming peek: reports whether any sample whose high byte is
 * `window_hi` landed in the ring since `*cursor`, and advances the
 * cursor to the current DMA write index. The cursor is the caller's
 * own, independent of commemul_poll's read index, so a second reader
 * (the audio refill interrupt) can watch for a window without stealing
 * samples from the main loop's demux. */
bool __not_in_flash_func(commemul_scan)(uint32_t *cursor, uint16_t window_hi);

#endif  // COMMEMUL_H
