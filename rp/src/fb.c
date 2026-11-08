/**
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and the VBL
 *              frame-sync with the m68k, which copies the single 32 KB
 *              cartridge framebuffer to the ST screen each VBL.
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CART_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "fb_core1.h"
#include "ikbd.h"
#include "pico/time.h"

/* VBL frame-sync. The m68k does a cart-bus read at
 * $FB8400 after each blit (see VBLSYNC_ADDR in userfw.s); the
 * commemul ring captures it with low-16 = 0x84xx. fb_pump_rom3
 * routes ROM3 samples to both the IKBD demux and this detector;
 * fb_wait_vbl() blocks until s_vbl_seen advances. */
#define FB_VBLSYNC_HIBYTE   0x8400u
#define FB_VBLSYNC_HIMASK   0xFF00u
/* Safety net only: must comfortably exceed the worst-case latency from
 * one VBLSYNC ack to the next (one VBL = 20 ms). Firing early would let
 * the caller write the cart FB while the m68k is still copying it -- so
 * keep it generous; it should never fire while the m68k is running. */
#define FB_VSYNC_TIMEOUT_US 60000u

static volatile uint32_t s_vbl_seen;
static uint32_t s_vbl_published;

const struct FB_MODE fb_mode_320x200 = {320, 200, 4};

struct FB_SCREEN fb_screen;

int fb_init(const struct FB_MODE *mode) {
  if (mode == NULL) {
    return -1;
  }
  fb_screen.framebuffer =
      (unsigned int *)((uintptr_t)&__rom_in_ram_start__ +
                       CART_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;

  /* The cart FB is already zeroed (palette index 0 everywhere) by
   * emul_start's ERASE_FIRMWARE_IN_RAM; the app's first frame fills it. */

  /* Launch Core 1 with the job worker (audio starts its timer there). */
  fb_core1_init();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CART_FRAMEBUFFER_SIZE);
  return 0;
}

void fb_clear(void) {
  memset(fb_screen.framebuffer, 0xFF, CART_FRAMEBUFFER_SIZE);
}

/* ROM3 ring dispatch: route each captured cart-bus read to the IKBD
 * demux and to the VBL frame-sync detector. */
static void fb_rom3_dispatch(uint16_t sample) {
  ikbd_consume_rom3_sample(sample);
  if ((sample & FB_VBLSYNC_HIMASK) == FB_VBLSYNC_HIBYTE) {
    s_vbl_seen++;
  }
  /* Sound-capability + buffer-length reports (audio.c owns both
   * windows and the mapping to back-end / refill size). */
  audio_consume_rom3_sample(sample);
}

void fb_pump_rom3(void) { commemul_poll(fb_rom3_dispatch); }

void fb_wait_vbl(void) {
  /* Block until the m68k has finished blitting the previous frame (its
   * VBLSYNC ack) so the cart FB is free to overwrite. Drain the ROM3
   * ring meanwhile so IKBD stays live. */
  uint32_t t_wait = time_us_32();
  while (s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    if (time_us_32() - t_wait > FB_VSYNC_TIMEOUT_US) {
      break;
    }
  }
  s_vbl_published = s_vbl_seen;
}
