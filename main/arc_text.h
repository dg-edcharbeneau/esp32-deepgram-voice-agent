/*
 * Short text laid along the arc of the round panel, at twelve o'clock.
 *
 * WHY IT EXISTS. The status word lives in the middle of the screen, and the
 * middle of the screen is also the 70 px touch target -- so the moment anyone
 * reaches for the control, their thumb covers the very words that told them
 * what the control would do. "forget? hold again" is unreadable at exactly the
 * moment it matters. Moving the text out from under the finger is the fix, and
 * on a 466 px circle the only place with room for eighteen characters is the
 * curve itself.
 *
 * WHY IT IS DRAWN A GLYPH AT A TIME. A straight label at the top of a circle
 * runs out of panel: the chord at that height is far shorter than the arc. Text
 * on the arc fits, but only if each letter is rotated to stand on it -- eighteen
 * characters at radius 196 span about 70 degrees, so the ones at the ends lean
 * more than a third of a right angle and axis-aligned glyphs read as broken.
 *
 * LVGL 9.5 draws a rotated glyph natively (lv_draw_letter's `rotation`, in tenths
 * of a degree); the software renderer routes any non-zero rotation through a
 * transformed image blit, which is why this is a handful of calls rather than a
 * rasteriser. See lv_draw_sw_letter.c.
 *
 * The caller owns when this appears and for how long; this module only knows
 * how to lay a string on a circle and what pixels that touches.
 */
#pragma once

#include "lvgl.h"

/*
 * Where the glyph baselines sit, measured from the centre of the panel.
 *
 * Ascenders point OUTWARD -- that is what "standing on the arc" means -- so the
 * text reaches roughly this plus the font's ascent, 23 px at montserrat_24.
 *
 * 176 puts the capitals at 199 against a 233 px panel radius, so 34 px of margin
 * -- more than the 14 the geometry alone would ask for, and deliberately.
 *
 * A NOTE ON HOW THIS NUMBER WAS ARRIVED AT, because the trail is misleading.
 * The first value was 196, which the arithmetic supports, and on the panel the
 * capitals were clipped. It got lowered twice, and a comment appeared here
 * blaming the bezel or the round mask for eating pixels the numbers could not
 * see. That was wrong: the caption was being drawn 23 px above where it was
 * placed, because of the pivot double-shift documented in draw_visit(). With
 * that fixed the text lands where the radius says, and 176 is simply where it
 * was asked to sit -- not a fudge around a panel quirk.
 *
 * Which is worth knowing before changing it: there is nothing to compensate for
 * any more. Move it because the caption should sit somewhere else.
 */
#define ARC_TEXT_RADIUS 176

/*
 * The box the string would touch at this radius, without drawing anything.
 *
 * Needed twice per frame and for a reason the battery overlay documents at
 * length: whatever an overlay draws, only that overlay knows it drew there, so
 * it has to black its own ground before drawing and invalidate its own box
 * afterwards. Returns false when the string would touch nothing.
 */
bool arc_text_box(const char *text, const lv_font_t *font, lv_area_t *out);

/*
 * Black the ground the string will occupy, glyph by glyph.
 *
 * PER GLYPH, NOT ONE RECTANGLE, and that is a legibility decision as much as a
 * cost one. Both faces paint out past this radius -- the spectrum's bars reach
 * 218 and the orb's shell is wider still -- so something has to be blacked or
 * the text lands on a moving picture and the picture keeps its old pixels where
 * the text moves off them. A rectangle across the top of a round display reads
 * as a slab; a trail of small boxes follows the curve and looks like the ribbon
 * the text is written on.
 */
void arc_text_wipe(lv_layer_t *layer, const char *text, const lv_font_t *font);

/*
 * Draw the string. `opa` fades the whole run, which is what the caller uses to
 * hand the screen back to the centre label without a jump cut.
 */
void arc_text_draw(lv_layer_t *layer, const char *text, const lv_font_t *font,
                   lv_color_t color, lv_opa_t opa);
