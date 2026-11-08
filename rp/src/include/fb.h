/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Single-FB design: there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * The RP writes ST planar data straight into it (no chunky buffer, no
 * planar scratch copy): call fb_wait_vbl(), then write through an
 * fb_cursor_t, bottom rows first (see fb_wait_vbl for why).
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

#include "cart_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge). */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor and launch the
 *        Core 1 job worker (fb_core1.h).
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/** @brief Fill the cartridge framebuffer with 0xFF (palette index 15
 *         everywhere). */
void fb_clear(void);

/** @brief Block until the m68k has finished copying the cart
 *         framebuffer to the ST screen (its VBLSYNC ack, a cart-bus read
 *         at $FB8400), pacing the caller to the 50 Hz VBL.
 *
 *         On return the cart FB is free: the m68k is idle until the next
 *         VBL. Its next blit then reads the cart FB from offset 0 upward
 *         over most of the frame, and cart offset 0 holds the image's
 *         BOTTOM chunk (the chunks are reversed), so it copies the image
 *         bottom-up. Writing bottom rows first keeps the RP ahead of it
 *         even when the writes run past the VBL. A ~60 ms timeout (safety
 *         net for "m68k not running", e.g. at boot) keeps the RP from
 *         hanging. The ROM3 ring is drained while waiting, so IKBD stays
 *         live. */
void fb_wait_vbl(void);

/** @brief Drain the ROM3 commemul ring once, routing each captured
 *         sample to the IKBD demux, the VBL frame-sync detector and the
 *         audio report windows. Call from the main loop in place of a
 *         bare commemul_poll(); fb_wait_vbl() also calls it while
 *         waiting. */
void fb_pump_rom3(void);

/* ---------------------------------------------------------------------
 * Cart-FB write cursor over consecutive 16-pixel groups.
 *
 * The image is ST low-res planar: byte offset `row * 160 + group * 8`,
 * then one word per plane, plane 0 first. In the cart FB its 48-byte
 * chunks are stored in reverse order for the m68k's predec MOVEM blit,
 * with the last CART_FB_CHUNK_TAIL bytes in natural order -- see
 * "Framebuffer chunk layout for the m68k MOVEM blit" in cart_shared.h.
 * A group (8 bytes) never straddles a chunk (8 divides 48 and 160), so
 * the cursor writes whole groups and only re-aims at a chunk boundary,
 * where the next image chunk sits 48 bytes *below* the current one.
 *
 * Each group is written as two 32-bit stores: `lo` = plane 0 | plane 1
 * << 16 and `hi` = plane 2 | plane 3 << 16, each plane a uint16 whose
 * bit 15 is the leftmost pixel. The cart bus presents each uint16 as
 * the ST's big-endian word, so no byte swapping is needed.
 * ------------------------------------------------------------------- */
typedef struct {
  uint32_t *p;   /* cart-FB address of the next group */
  uint32_t o;    /* its image byte offset */
  uint32_t left; /* groups left in the current chunk, this one included */
} fb_cursor_t;

/* Aim the cursor at image byte offset `o` (a multiple of 8). */
static inline void fb_cursor_seek(fb_cursor_t *c, uint32_t o) {
  uint8_t *fb = (uint8_t *)fb_screen.framebuffer;
  c->o = o;
  if (o >= CART_FB_CHUNK_COVERED) {  // natural-order tail
    c->p = (uint32_t *)(fb + o);
    c->left = UINT32_MAX;
    return;
  }
  const uint32_t k = o / CART_FB_CHUNK_BYTES;
  const uint32_t w = o - k * CART_FB_CHUNK_BYTES;
  c->p = (uint32_t *)(fb + (CART_FB_CHUNK_COUNT - 1u - k) *
                               CART_FB_CHUNK_BYTES + w);
  c->left = (CART_FB_CHUNK_BYTES - w) / 8u;
}

/* Write one 16-pixel group and advance to the next one in image order. */
static inline void fb_cursor_put(fb_cursor_t *c, uint32_t lo, uint32_t hi) {
  c->p[0] = lo;
  c->p[1] = hi;
  c->o += 8u;
  if (--c->left) {
    c->p += 2;
  } else if (c->o < CART_FB_CHUNK_COVERED) {
    // Just wrote the chunk's last group (at +40); the next image chunk
    // starts 48 bytes before this chunk's start.
    c->p -= (40u + CART_FB_CHUNK_BYTES) / 4u;
    c->left = CART_FB_CHUNK_BYTES / 8u;
  } else {
    fb_cursor_seek(c, c->o);
  }
}

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
