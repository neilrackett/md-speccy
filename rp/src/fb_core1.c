/**
 * File: fb_core1.c
 * Description: Core 1 job worker.
 *
 * Core 1 runs a perpetual loop in `fb_core1_loop` that pops a job
 * (function pointer + argument) off the inter-core FIFO, runs it and
 * pushes a completion word back. `fb_core1_init()` launches it once at
 * boot via the pico-sdk's `multicore_launch_core1`.
 *
 * The template used this for the dual-core chunky-to-planar transpose.
 * MD/Speccy decodes the Spectrum screen straight into the cart
 * framebuffer instead (zxemu.c update_display), so the only remaining
 * client is audio.c, which starts its refill timer on Core 1 through it
 * (the alarm pool binds the timer IRQ to the core that creates it).
 */

#include "fb_core1.h"

#include "pico/multicore.h"

/* Pops a job function pointer + arg off the FIFO, runs it, signals
 * completion. The fn/arg travel through the FIFO (not shared memory),
 * so the FIFO's push/pop barriers fully order the handoff. Placed in
 * RAM so the loop doesn't pay XIP cost on every dispatch. */
static void __not_in_flash_func(fb_core1_loop)(void) {
  for (;;) {
    fb_core1_job_t job = (fb_core1_job_t)(uintptr_t)multicore_fifo_pop_blocking();
    void *arg = (void *)(uintptr_t)multicore_fifo_pop_blocking();
    job(arg);
    multicore_fifo_push_blocking(0); /* done */
  }
}

void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg) {
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)job);
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)arg);
}

void __not_in_flash_func(fb_core1_wait)(void) {
  (void)multicore_fifo_pop_blocking(); /* wait for done */
}

void fb_core1_init(void) {
  /* multicore_launch_core1 blocks until Core 1 has entered the user
   * function, so by the time fb_init returns, Core 1 is parked in the
   * FIFO pop and ready for its first job. */
  multicore_launch_core1(fb_core1_loop);
}
