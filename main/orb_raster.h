/*
 * Rasterise a finished orb frame into the LVGL canvas.
 *
 * Owns the only two things the geometry does not: how a dot becomes pixels, and
 * which pixels have to be repainted. Everything about the shell's shape lives in
 * orb_geometry.c; this file just draws grey discs.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "orb_geometry.h"

/* Latch the canvas and its pixel buffer. Call once, after the canvas exists. */
esp_err_t orb_raster_init(lv_obj_t *canvas);

/*
 * Clear last frame's marks, blit this frame's, and invalidate just the area that
 * actually changed. Call once per frame on the LVGL task.
 */
void orb_raster_draw(const orb_frame_t *frame);

/*
 * Blank the whole canvas and forget the dirty list.
 *
 * Needed on every switch TO the orb: another face will have painted the canvas,
 * and the incremental clear only knows about boxes the orb itself drew.
 */
void orb_raster_clear(void);
