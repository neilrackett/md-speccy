/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k Timer-B IRQ reads sample bytes from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET (1024 B). The RP refills the buffer
 * once per VBL via audio_render_frame() (paced to ~50 Hz via
 * time_us_32). The library is format-agnostic -- it just dispatches
 * to whatever fill callback the app has installed.
 *
 * See audio.h for the public API and `audio_play_loop` /
 * `audio_set_fill_callback` semantics.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "commemul.h"
#include "fb_chunked.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Per-VBL fill cadence. The gate only exists to reject re-entry from a
 * main loop that spins faster than the VBL; it must never skip a fill
 * in a VBL-locked loop (every fb app blocks on fb_publish). The ST PAL
 * VBL is ~20,032 us, so the old 20,000 us threshold left ~30 us of
 * margin -- one jittery time_us_32() reading skipped a fill, and the
 * m68k then replayed the previous 224 B buffer for a whole frame while
 * the producer overran its ring. 15 ms still rejects sub-VBL re-entry
 * but can never lose the race against a real VBL. */
#define AUDIO_FRAME_PERIOD_US 15000u

/* Audio back-end is chosen at RUNTIME from the m68k's _SND detection,
 * reported over the cart bus (see fb.c / userfw.s). audio_set_mode()
 * switches the per-VBL refill size, and the fill callback keys off
 * audio_get_mode() to produce the matching format:
 *  - AUDIO_MODE_YM: 5,585 Hz, 112 samples/VBL x 2 B (vA,vB) = 224 B
 *    (m68k Timer-B, TBDR=110 /4 prescaler; ~800 B of the cart buffer
 *    stays as overrun headroom).
 *  - AUDIO_MODE_DMA: 25,033 Hz, 500 samples/VBL x 1 B (signed) = 500 B.
 *  - AUDIO_MODE_SILENT: 500 B of zeros until the first report arrives
 *    (zeros are silence for both the DMA and YM readers).
 * Default is SILENT so nothing plays garbage before the m68k reports. */
#define AUDIO_FILL_BYTES_YM  224u
#define AUDIO_FILL_BYTES_DMA 500u

static audio_mode_t s_audio_mode = AUDIO_MODE_SILENT;
static uint32_t s_fill_bytes = AUDIO_FILL_BYTES_DMA;

void audio_set_mode(audio_mode_t mode) {
  if (mode == s_audio_mode) {
    return;
  }
  s_audio_mode = mode;
  s_fill_bytes = (mode == AUDIO_MODE_YM) ? AUDIO_FILL_BYTES_YM
                                         : AUDIO_FILL_BYTES_DMA;
  DPRINTF("audio_set_mode: %s (%u B/VBL)\n",
          mode == AUDIO_MODE_DMA ? "STE DMA" : mode == AUDIO_MODE_YM
                                                   ? "YM"
                                                   : "silent",
          (unsigned)s_fill_bytes);
}

audio_mode_t audio_get_mode(void) { return s_audio_mode; }
uint32_t audio_get_fill_bytes(void) { return s_fill_bytes; }

/* Sound-capability report window (SNDCAP_WINDOW_BASE in userfw.s): the
 * m68k reads $FB8600 + has_dma once per VBL, having probed the _SND
 * cookie at boot. Decoding it here rather than in the ROM3 dispatcher
 * keeps the window address and the bit-to-back-end mapping with the
 * module that owns audio_mode_t and the refill sizes. */
#define AUDIO_SNDCAP_HIBYTE 0x8600u
#define AUDIO_WINDOW_HIMASK 0xFF00u

/* Buffer-length report (SNDLEN_WINDOW_BASE in userfw.s): while STE DMA
 * sound is running the m68k measures how much the chip actually eats
 * per frame -- it is about 501.5, not 500, and it varies by machine
 * because the DMA and the video run off different oscillators -- and
 * sends the length back once a VBL. Producing exactly that many is what
 * stops the chip running a buffer dry and replaying it (a fixed 500
 * drifts dry roughly every 350 frames: the ~7-second crackle). */
#define AUDIO_SNDLEN_HIBYTE 0x8C00u

/* The report is biased by the m68k's minimum so it fits one byte; this
 * must match STE_SND_LEN_MIN in userfw.s. */
#define AUDIO_SNDLEN_BIAS 480u

/* Guard rails on a value that arrives over a bus. The m68k steers
 * within a narrow band around one VBL's worth, so anything well outside
 * it is a corrupt sample rather than a length, and is ignored. */
#define AUDIO_FILL_BYTES_MIN 448u
#define AUDIO_FILL_BYTES_MAX 576u

void audio_set_fill_bytes(uint32_t bytes) {
  if (s_audio_mode != AUDIO_MODE_DMA) {
    return; /* the YM rate is fixed by the Timer-B divider */
  }
  if (bytes < AUDIO_FILL_BYTES_MIN || bytes > AUDIO_FILL_BYTES_MAX ||
      bytes > CART_AUDIO_BUFFER_SIZE) {
    return;
  }
  s_fill_bytes = bytes;
}

void audio_consume_rom3_sample(uint16_t addr_lsb) {
  const uint16_t window = addr_lsb & AUDIO_WINDOW_HIMASK;
  if (window == AUDIO_SNDCAP_HIBYTE) {
    audio_set_mode((addr_lsb & 1u) ? AUDIO_MODE_DMA : AUDIO_MODE_YM);
  } else if (window == AUDIO_SNDLEN_HIBYTE) {
    audio_set_fill_bytes(AUDIO_SNDLEN_BIAS + (uint32_t)(addr_lsb & 0xFFu));
  }
}

static uint8_t *s_audio_buf;
static uint32_t s_last_frame_us;
static audio_fill_cb_t s_fill_cb;

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE     16u
#define AUDIO_YMS_MODE_DUAL_GHOST 1u
/* Must match TIMERB_COUNT in target/atarist/src/userfw.s:
 *   2.4576 MHz / 4 / 110 = 5,585.45 Hz */
#define AUDIO_NATIVE_RATE_HZ      5585u

static FIL s_yms_file;
static bool s_yms_open;
static FSIZE_t s_yms_data_offset;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_last_frame_us = 0;
  s_fill_cb = NULL;
  s_yms_open = false;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_play_loop() or
   * audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X, %u B/VBL refill\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET, (unsigned)s_fill_bytes);
}

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_fill_cb = cb;
}

static void audio_loop_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t pos = s_loop_pos;
  const uint32_t total = s_loop_bytes;
  const uint8_t *src = s_loop_data;
  for (uint32_t i = 0; i < bytes; i++) {
    buf[i] = src[pos];
    pos++;
    if (pos >= total) {
      pos = 0;  /* loop */
    }
  }
  s_loop_pos = pos;
}

void audio_play_loop(const uint8_t *data, uint32_t bytes) {
  s_loop_data = data;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  s_fill_cb = audio_loop_cb;
}

static void audio_yms_cb(uint8_t *buf, uint32_t bytes) {
  UINT br = 0;
  FRESULT res = f_read(&s_yms_file, buf, bytes, &br);
  if (res != FR_OK) {
    /* I/O error -- silence until the next call. The cursor is in
     * an undefined state, so seek back to the data start for the
     * next attempt. */
    for (uint32_t i = 0; i < bytes; i++) {
      buf[i] = 0;
    }
    f_lseek(&s_yms_file, s_yms_data_offset);
    return;
  }
  if (br < bytes) {
    /* EOF mid-fill: wrap to data start and read the remainder. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    UINT br2 = 0;
    f_read(&s_yms_file, buf + br, bytes - br, &br2);
    /* If the file body is shorter than one VBL's worth, pad the
     * still-unfilled tail with silence rather than reading the
     * header bytes. */
    for (uint32_t i = br + br2; i < bytes; i++) {
      buf[i] = 0;
    }
  }
}

int audio_play_yms_file(const char *path) {
  /* Close any previously open YMS file. Idempotent on a fresh init. */
  if (s_yms_open) {
    f_close(&s_yms_file);
    s_yms_open = false;
  }

  FRESULT res = f_open(&s_yms_file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("audio_play_yms_file: f_open('%s') failed (%d)\n", path, (int)res);
    return -1;
  }

  uint8_t hdr[AUDIO_YMS_HEADER_SIZE];
  UINT br = 0;
  res = f_read(&s_yms_file, hdr, sizeof(hdr), &br);
  if (res != FR_OK || br != sizeof(hdr)) {
    DPRINTF("audio_play_yms_file: short header read (%d, %u/%u)\n",
            (int)res, (unsigned)br, (unsigned)sizeof(hdr));
    f_close(&s_yms_file);
    return -1;
  }

  if (memcmp(hdr, "YMS1", 4) != 0) {
    DPRINTF("audio_play_yms_file: bad magic %02X%02X%02X%02X\n",
            hdr[0], hdr[1], hdr[2], hdr[3]);
    f_close(&s_yms_file);
    return -1;
  }

  uint32_t rate = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (rate != AUDIO_NATIVE_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate mismatch (file %lu, expected %u)\n",
            (unsigned long)rate, (unsigned)AUDIO_NATIVE_RATE_HZ);
    f_close(&s_yms_file);
    return -1;
  }

  if (hdr[12] != AUDIO_YMS_MODE_DUAL_GHOST) {
    DPRINTF("audio_play_yms_file: unsupported mode tag %u (need %u)\n",
            (unsigned)hdr[12], (unsigned)AUDIO_YMS_MODE_DUAL_GHOST);
    f_close(&s_yms_file);
    return -1;
  }

  s_yms_open = true;
  s_yms_data_offset = AUDIO_YMS_HEADER_SIZE;
  s_fill_cb = audio_yms_cb;

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

/* VBL-synced refill. The scan cursor is the timer's own, separate from
 * commemul_poll's read index, so peeking for the ack steals nothing
 * from the main loop's IKBD demux. A fill is never repeated within
 * 5 ms, in case an ack is seen twice across a scan boundary.
 *
 * This exists because the refill cadence must not depend on how long an
 * emulated frame takes. The m68k drains its 224 B (YM) or ~500 B (DMA)
 * every 20 ms regardless; when the main loop ran slower than the VBL it
 * skipped fills and the m68k replayed a stale buffer -- the "farty"
 * distortion. A 1 ms timer that fires right after the m68k's end-of-blit
 * ack refills on every VBL however slow the frame, and can never tear
 * under the m68k's copy. */
#define AUDIO_VBLSYNC_HIBYTE 0x8400u
#define AUDIO_MIN_FILL_GAP_US 5000u

static repeating_timer_t s_vbl_timer;
static uint32_t s_scan_cursor;
static int s_vbl_timer_core = -1; /* -1 off, else the core the IRQ runs on */
static alarm_pool_t *s_core1_pool;

static bool __not_in_flash_func(audio_vbl_timer_cb)(repeating_timer_t *rt) {
  (void)rt;
  const uint32_t t0 = time_us_32();
  if (commemul_scan(&s_scan_cursor, AUDIO_VBLSYNC_HIBYTE) && s_fill_cb) {
    if (t0 - s_last_frame_us >= AUDIO_MIN_FILL_GAP_US) {
      s_last_frame_us = t0;
      s_fill_cb(s_audio_buf, s_fill_bytes);
    }
  }
  return true;
}

/* Runs on Core 1 (as a framework job): a Core 1 alarm pool binds its
 * interrupt to Core 1, so the refill then never interrupts Core 0. */
static void core1_start_timer_job(void *arg) {
  (void)arg;
  if (!s_core1_pool) {
    /* One timer is all this pool ever holds; each spare entry is
       heap the boot-time settings allocations would rather have. */
    s_core1_pool = alarm_pool_create_with_unused_hardware_alarm(1);
  }
  alarm_pool_add_repeating_timer_us(s_core1_pool, -1000, audio_vbl_timer_cb,
                                    NULL, &s_vbl_timer);
}

void audio_start_vbl_timer(int core) {
  if (s_vbl_timer_core >= 0) return;
  s_scan_cursor = 0;
  if (core == 1) {
    fb_core1_dispatch(core1_start_timer_job, NULL);
    fb_core1_wait();
  } else {
    add_repeating_timer_us(-1000, audio_vbl_timer_cb, NULL, &s_vbl_timer);
  }
  s_vbl_timer_core = core ? 1 : 0;
  DPRINTF("audio: VBL-synced refill timer started on core %d\n",
          s_vbl_timer_core);
}

void audio_stop_vbl_timer(void) {
  if (s_vbl_timer_core < 0) return;
  cancel_repeating_timer(&s_vbl_timer);
  s_vbl_timer_core = -1;
  memset(s_audio_buf, 0, CART_AUDIO_BUFFER_SIZE);
  DPRINTF("audio: VBL-synced refill timer stopped\n");
}

void audio_render_frame(void) {
  if (s_fill_cb == NULL) {
    return;
  }

  uint32_t now_us = time_us_32();
  if (s_last_frame_us != 0 &&
      (now_us - s_last_frame_us) < AUDIO_FRAME_PERIOD_US) {
    return;
  }
  s_last_frame_us = now_us;

  s_fill_cb(s_audio_buf, s_fill_bytes);
}
