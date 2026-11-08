/**
 * File: fb_core1.h
 * Description: Core 1 job worker (launched by fb_init()).
 */

#ifndef FB_CORE1_H_INCLUDED
#define FB_CORE1_H_INCLUDED

#include <stdint.h>

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Launch Core 1 with the job worker loop. Must be called exactly once,
 * from Core 0. fb_init() does this. A second call would deadlock inside
 * pico-sdk's launch handshake. */
void fb_core1_init(void);

/* Run a job on Core 1, in parallel with Core 0.
 * `fb_core1_dispatch` hands `job(arg)` to the parked Core 1 worker and
 * returns immediately; the caller then calls `fb_core1_wait()` to join.
 * The fn+arg are passed through the inter-core FIFO, whose push/pop
 * carry the cross-core memory barriers, so data prepared before dispatch
 * is visible to the job and the job's writes are visible after wait.
 * Each dispatch MUST be paired with one wait before the next dispatch. */
typedef void (*fb_core1_job_t)(void *arg);
void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg);
void __not_in_flash_func(fb_core1_wait)(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_CORE1_H_INCLUDED */
