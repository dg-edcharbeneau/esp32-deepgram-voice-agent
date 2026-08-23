/*
 * Rasterise a finished orb frame into the LVGL canvas.
 *
 * Owns the only three things the geometry does not: how a dot becomes pixels,
 * which pixels have to be repainted, and what colour the ink is. Everything about
 * the shell's shape lives in orb_geometry.c; this file just draws discs.
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
 *
 * `rgb` is 0xRRGGBB and scales the ink the geometry resolved, per channel. It is
 * a draw parameter rather than raster state because there is nothing to keep:
 * every dot is repainted every frame, so a colour change needs no clear and no
 * invalidation beyond what the frame already does.
 *
 * 0xFFFFFF is the identity -- it reproduces the monochrome shell exactly, which
 * is why the default costs nothing and colour is entirely opt-in.
 */
void orb_raster_draw(const orb_frame_t *frame, uint32_t rgb);

/*
 * Blank the whole canvas and forget the dirty list.
 *
 * Needed on every switch TO the orb: another face will have painted the canvas,
 * and the incremental clear only knows about boxes the orb itself drew.
 */
void orb_raster_clear(void);
