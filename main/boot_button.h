/*
 * The BOOT button (GPIO 0) as a Wi-Fi escape hatch.
 *
 * WHICH BUTTON
 *
 * The board has two. RESET is wired to EN/CHIP_PU and resets the chip in
 * hardware -- firmware never sees it. BOOT is GPIO 0, active-low with an
 * internal pull-up, and is free once the ROM has finished with it. The board's
 * factory firmware (xiaozhi-esp32) used exactly this pin, which is how the
 * question was finally settled -- see the README.
 *
 * WHY NOT THE TOUCH PANEL
 *
 * A "hold even longer" gesture cannot coexist with the existing one:
 * LV_EVENT_LONG_PRESSED already means restart-the-session, so any longer hold
 * would trip restart on its way past. GPIO 0 is a separate input device, so the
 * screen's tap and hold keep their exact current meaning.
 *
 * (The threshold is 1000 ms, set by lv_indev_set_long_press_time() in ui.c.
 * LVGL's own default is 400 ms, which is well inside an ordinary tap and is the
 * number this comment used to quote.)
 *
 * THE STRAPPING TRAP
 *
 * GPIO 0 decides boot mode. Held low THROUGH a reset, the ROM enters USB
 * download mode and the app never runs -- so this has to be a press *after*
 * boot, and "hold BOOT while pressing RESET" is emphatically not a Wi-Fi reset.
 * Polling the pin after startup, which is what iot_button does, sidesteps this
 * by construction.
 */
#pragma once

#include "esp_err.h"

/*
 * Claims GPIO 0 and wires up:
 *
 *   short click     -> toggle the session
 *   hold for 3 s    -> forget the saved network and reboot into the portal
 *
 * THE CLICK IS A PLAIN TOGGLE, and no longer quite "the same as tapping the
 * screen": a screen tap interrupts instead of toggling while the agent is
 * speaking. That divergence is deliberate. This button is the escape hatch --
 * the thing that still works on a device whose display or session is misbehaving
 * -- so it does exactly one unconditional thing, and what it does cannot depend
 * on what the audio path happens to be doing. If you want the interrupt, the
 * screen is where it lives.
 *
 * Safe to call before the display and the network are up; nothing here depends
 * on either, which is the point -- the escape hatch should still work on a
 * device that is failing to get that far.
 */
esp_err_t boot_button_start(void);
