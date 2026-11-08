/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: fb_core1.c
 * Description: rp/src/fb_core1.c for EmuMD. The same Core 1 job worker,
 *              but a job and its argument are 64-bit pointers on the
 *              host, so they cannot travel through the 32-bit inter-core
 *              FIFO as they do on the RP2040: they wait in a slot, and
 *              the FIFO only says when.
 */

#include "fb_core1.h"

#include "pico/multicore.h"

static fb_core1_job_t s_job;
static void *s_job_arg;

static void fb_core1_loop(void) {
  for (;;) {
    (void)multicore_fifo_pop_blocking();
    s_job(s_job_arg);
    multicore_fifo_push_blocking(0);
  }
}

void fb_core1_dispatch(fb_core1_job_t job, void *arg) {
  s_job = job;
  s_job_arg = arg;
  multicore_fifo_push_blocking(1);
}

void fb_core1_wait(void) { (void)multicore_fifo_pop_blocking(); }

void fb_core1_init(void) { multicore_launch_core1(fb_core1_loop); }
