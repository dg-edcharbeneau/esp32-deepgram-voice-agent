/*
 * Thin on purpose: each control calls what the voice tool or the BOOT button
 * already calls, minus the flash write. See controls.h.
 */
#include "controls.h"

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "audio_io.h"
#include "clock.h"
#include "session_ctl.h"

static const char *TAG = "controls";

#define VOLUME_STEP 10   /* the voice tools' "small change" */

/* LVGL task only: the panel is the only caller. */
static bool s_unsaved;
static uint32_t s_changed_ms;

static void changed(void)
{
    s_unsaved = true;
    s_changed_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

int controls_volume(void)
{
    return audio_io_get_volume();
}

int controls_volume_step(int dir)
{
    /* Snap to the step grid, so a level set by voice (say 65) moves to 70 or 60
     * rather than walking 75, 85 for ever off the marks the panel shows. */
    const int now = audio_io_get_volume();
    int want = (dir > 0) ? ((now / VOLUME_STEP) + 1) * VOLUME_STEP
                         : ((now + VOLUME_STEP - 1) / VOLUME_STEP - 1) * VOLUME_STEP;
    const int level = audio_io_set_volume_live(want);
    if (level != now) {
        changed();
        ESP_LOGI(TAG, "EVT ctl volume %d", level);
    }
    return level;
}

bool controls_mic_gain_available(void)
{
#if CONFIG_MIC_GAIN_TOOL
    return true;
#else
    return false;
#endif
}

int controls_mic_gain(void)
{
    return audio_io_get_mic_gain();
}

int controls_mic_gain_step(int dir)
{
    const int now = audio_io_get_mic_gain();
#if CONFIG_MIC_GAIN_TOOL
    const int g = audio_io_set_mic_gain_live(now + (dir > 0 ? 1 : -1) * AUDIO_MIC_GAIN_STEP_DB);
    if (g != now) {
        changed();
        ESP_LOGI(TAG, "EVT ctl micgain %d", g);
    }
    return g;
#else
    (void)dir;
    return now;
#endif
}

bool controls_ns_available(void)
{
#if CONFIG_MIC_NS_ENABLE
    return true;
#else
    return false;
#endif
}

bool controls_ns_on(void)
{
#if CONFIG_MIC_NS_ENABLE
    return audio_io_ns_enabled();
#else
    return false;
#endif
}

void controls_ns_toggle(void)
{
#if CONFIG_MIC_NS_ENABLE
    /* Exactly the BOOT double-click: set now, save on the worker. */
    const bool on = !audio_io_ns_enabled();
    audio_io_ns_set(on);
    session_ctl_request_ns_save();
    ESP_LOGI(TAG, "EVT ctl ns %s", on ? "on" : "off");
#endif
}

bool controls_clock_available(void)
{
#if CONFIG_CLOCK
    return true;
#else
    return false;
#endif
}

bool controls_clock_12h(void)
{
    return clock_12h();
}

void controls_clock_toggle_12h(void)
{
    const bool h12 = !clock_12h();
    clock_set_12h(h12);   /* clock_tick() applies and saves it */
    ESP_LOGI(TAG, "EVT ctl clock %d-hour", h12 ? 12 : 24);
}

int controls_alarms(alarm_view_t *out, int max)
{
    return alarms_snapshot(out, max);
}

bool controls_alarm_cancel(uint8_t id)
{
    const bool ok = alarms_cancel_id(id);
    ESP_LOGI(TAG, "EVT ctl cancel id=%u %s", id, ok ? "done" : "gone");
    return ok;
}

void controls_commit(uint32_t now_ms, uint32_t quiet_ms)
{
    /* Signed, for the same reason as the panel's idle timer: a change stamped
     * after the frame that calls this began is "in the future", not ancient. */
    if (s_unsaved && (int32_t)(now_ms - s_changed_ms) >= (int32_t)quiet_ms) {
        s_unsaved = false;
        session_ctl_request_settings_save();
    }
}
