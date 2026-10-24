#pragma once

/*
 * The selected orb colour, applied to everything else on the screen.
 *
 * The UI's colours are designed in white and neutral greys (Ed's reference
 * sheets), and every one of them is derived from the orb colour by one of three
 * rules. Under white -- the default -- each rule returns its input unchanged, so
 * the out-of-box look is exactly the reference.
 *
 *   ui_tone_mul    Light things, and ink on them: white button fills, the
 *                  pressed fill, the dark ink on a white button. Per channel
 *                  times the colour, so white becomes the colour itself.
 *   ui_tone_grey   Dark surfaces and grey text: the dark buttons, the meter, the
 *                  hints. Re-hued at the SAME brightness, because a straight
 *                  multiply halves it on purple or blue and the dark buttons
 *                  vanish into the black ground.
 *   ui_tone_ink    Text that is white on a dark surface: the colour, lifted
 *                  towards white until it is at least this bright, so it keeps
 *                  its contrast against the re-hued surfaces on the darker
 *                  colours. The light colours pass through untouched.
 *
 * Plain 0xRRGGBB and no LVGL, so host/panel.sh checks it as is.
 */

#include <stdint.h>

/* How bright ui_tone_ink keeps text, on luma's 0..255. White, yellow, lime and
 * teal pass through; cyan and vira (181) are lifted a touch, purple (128) most.
 * Measured against the meter's fill, the darkest surface text sits on, that
 * keeps every catalog colour at 3.6:1 or better (white: 7.6). */
#define UI_TONE_INK_LUMA 190

/* The small grey of the screen's hints and qualifiers, as designed on white. */
#define UI_HINT_RGB 0x8B96A0

/* Rec. 601 luma on the sRGB values, 0..255: cheap, and only ever compared. */
static inline uint32_t ui_tone_luma(uint32_t rgb)
{
    return (77u * ((rgb >> 16) & 0xFF) + 150u * ((rgb >> 8) & 0xFF) + 29u * (rgb & 0xFF)) >> 8;
}

static inline uint32_t ui_tone_mul(uint32_t tint, uint32_t rgb)
{
    uint32_t out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        const uint32_t c = ((rgb >> s) & 0xFF) * ((tint >> s) & 0xFF);
        out |= ((c + 127) / 255) << s;
    }
    return out;
}

static inline uint32_t ui_tone_grey(uint32_t tint, uint32_t grey)
{
    /* Luma of white comes out as 255, so under white this is the identity. */
    const uint32_t y = ui_tone_luma(tint);
    if (y == 0) {
        return grey;
    }
    uint32_t out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        uint32_t c = (((grey >> s) & 0xFF) * ((tint >> s) & 0xFF) + y / 2) / y;
        out |= (c > 255 ? 255 : c) << s;
    }
    return out;
}

static inline uint32_t ui_tone_ink(uint32_t tint)
{
    const uint32_t y = ui_tone_luma(tint);
    if (y >= UI_TONE_INK_LUMA) {
        return tint;
    }
    /* Mix towards white by t = (want - y) / (255 - y): luma is linear in the
     * channels, so that lands on UI_TONE_INK_LUMA. */
    const uint32_t num = UI_TONE_INK_LUMA - y, den = 255 - y;
    uint32_t out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        const uint32_t c = (tint >> s) & 0xFF;
        out |= (c + ((255 - c) * num + den / 2) / den) << s;
    }
    return out;
}
