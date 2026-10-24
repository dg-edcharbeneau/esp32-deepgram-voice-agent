#pragma once

/*
 * Screen captures over the serial console (CONFIG_UI_SCREENSHOT_TOUR, bench
 * only). The device has no other way to show anyone what it drew.
 *
 * The LVGL task hands over a finished frame of the face's canvas; it is copied
 * into PSRAM there and then (a few ms), and a low-priority task on the other
 * core run-length encodes it and prints it as base64 lines, the way the audio
 * recorder dumps (audio_rec.c). host/shot2png.py turns a captured log back into
 * PNGs.
 *
 * What is captured is the canvas -- everything the face draws. LVGL labels on
 * top of it (the status word, the sleep clock) are not in it.
 */

#include <stdbool.h>
#include <stdint.h>

/* Allocates the copy buffer and starts the dump task. False if either failed. */
bool screenshot_init(void);

/*
 * LVGL task. Copy `w` x `h` RGB565 pixels (`stride_px` per row) and queue them
 * for printing under `tag`. False, and nothing copied, while the previous
 * capture is still printing.
 */
bool screenshot_capture(const uint16_t *px, uint32_t stride_px, int w, int h, const char *tag);

/* A capture is still being printed. */
bool screenshot_busy(void);
