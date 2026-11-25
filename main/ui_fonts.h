#pragma once

/*
 * Every font the drawn screens use, by the job it does, at this board's scale.
 * The design's sizes are the S3 column; the P4 gets each at 800/466 the size
 * (see ui_scale.h), rounded to a whole pixel -- or, for LVGL's built-in
 * Montserrat, to the nearest size LVGL ships (24 x 1.72 = 41, so 40).
 *
 *                       S3 (466)   P4 (800)
 *   FONT_PANEL_S        18         31       panel captions, alarm details
 *   FONT_PANEL_M        28         48       panel values, ring labels
 *   FONT_PANEL_L        40         69       colour name, alarm time, Stop, GO
 *   FONT_CLOCK_DIGITS   52         89       the clock's and stopwatch's digits
 *   FONT_CLOCK_AMPM     15         26       AM / PM
 *   FONT_SMALL          14         24       spaced caps, hints, the next alarm
 *   FONT_STATUS         24         40       the status word, and on the arc
 *
 * The font_clock_* and font_panel_* sets are lv_font_conv output over
 * Montserrat-Medium; main/CMakeLists.txt compiles only this board's set.
 */

#include "lvgl.h"

#if PANEL_SIZE == 466
LV_FONT_DECLARE(font_panel_18);
LV_FONT_DECLARE(font_panel_28);
LV_FONT_DECLARE(font_panel_40);
LV_FONT_DECLARE(font_clock_52);
LV_FONT_DECLARE(font_clock_15);
#define FONT_PANEL_S      (&font_panel_18)
#define FONT_PANEL_M      (&font_panel_28)
#define FONT_PANEL_L      (&font_panel_40)
#define FONT_CLOCK_DIGITS (&font_clock_52)
#define FONT_CLOCK_AMPM   (&font_clock_15)
#define FONT_SMALL        (&lv_font_montserrat_14)
#define FONT_STATUS       (&lv_font_montserrat_24)
#elif PANEL_SIZE == 800
LV_FONT_DECLARE(font_panel_31);
LV_FONT_DECLARE(font_panel_48);
LV_FONT_DECLARE(font_panel_69);
LV_FONT_DECLARE(font_clock_89);
LV_FONT_DECLARE(font_clock_26);
#define FONT_PANEL_S      (&font_panel_31)
#define FONT_PANEL_M      (&font_panel_48)
#define FONT_PANEL_L      (&font_panel_69)
#define FONT_CLOCK_DIGITS (&font_clock_89)
#define FONT_CLOCK_AMPM   (&font_clock_26)
#define FONT_SMALL        (&lv_font_montserrat_24)
#define FONT_STATUS       (&lv_font_montserrat_40)
#else
#error "No font set for this PANEL_SIZE. See main/ui_fonts.h."
#endif
