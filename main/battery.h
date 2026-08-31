/*
 * Battery state, read from the board's AXP2101 power-management IC.
 *
 * WHY A MODULE AND NOT A CALL SITE
 *
 * The AXP2101 sits on the I2C bus that touch, the codec and the RTC also share,
 * and the reading it gives is a slow-moving number that three unrelated places
 * want: the dots on the screen, the TLM line, and the agent's get_battery tool.
 * Sampling it once on a task of its own and handing out copies keeps the bus
 * traffic to one transaction every few seconds no matter how many readers there
 * are, and it keeps I2C off the LVGL frame timer, where a blocking transfer
 * would land straight in the frame budget.
 *
 * READ-ONLY, DELIBERATELY
 *
 * The AXP2101 powers the AMOLED panel and the codec rails. A general-purpose
 * PMU library's init path reconfigures DCDC/LDO outputs, which on this board is
 * a way to brown the display out or worse. This module never writes a register.
 * If the fuel gauge is ever found disabled at boot, enabling it is a separate,
 * reviewed change -- not something to slip into a sampler.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    /* False until the first successful read. Everything else is meaningless
     * while this is false -- do not render 0% during boot. */
    bool valid;
    /* 0-100, from the chip's own fuel gauge, smoothed and step-hysteresed. */
    int percent;
    /* Cell voltage in millivolts, unsmoothed. Diagnostic: it is what tells a
     * log reader whether a percentage is believable. */
    int millivolts;
    /*
     * The cell is still filling. Derived from the AXP2101's charge STATE
     * MACHINE (trickle, pre-charge, CC, CV), not from its current-direction
     * field: the direction falls back to standby as the charge tapers into
     * constant-voltage, so a bolt keyed to it vanished while the charger was
     * still working. Gated on VBUS, so it is never true on a device running off
     * the cell.
     */
    bool charging;
    /* Charge complete, cable still in. Distinct from !charging, which is also
     * true on battery -- "full" and "not charging" mean opposite things to
     * whoever is holding the device. */
    bool full;
    /* The raw charge state machine, 0-5 (trickle, pre, CC, CV, done, not
     * charging), or -1 before the first read. Diagnostic: "charging stopped at
     * 70%" is answered by which state it stopped in, and nothing else. */
    int chg_state;
    /* percent has fallen to CONFIG_BATTERY_LOW_PCT, with hysteresis on the way
     * back up so a cell sitting on the threshold does not chatter. */
    bool low;
} battery_status_t;

/*
 * Starts the sampler. No-op when CONFIG_BATTERY is off.
 *
 * Call AFTER the I2C bus exists -- audio_codecs.c brings it up via
 * bsp_i2c_init() -- though this calls bsp_i2c_init() itself for the same reason
 * the BSP does: it is idempotent and cheaper than an ordering rule.
 */
void battery_start(void);

/*
 * Copies the last sample. Safe from any task; returns false and leaves *out
 * zeroed when the feature is compiled out or nothing has been read yet.
 */
bool battery_get(battery_status_t *out);
