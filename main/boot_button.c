#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "audio_io.h"
#include "boot_button.h"
#include "session_ctl.h"
#include "ui.h"
#include "wifi_creds.h"

static const char *TAG = "boot_btn";

#define BOOT_BUTTON_GPIO   0
#define FORGET_HOLD_MS     3000
#if CONFIG_MIC_NS_ENABLE
/* Long enough to read, short enough that the session's own caption comes back
 * before it is missed. */
#define NS_NOTICE_MS       2000
/* Max press length AND max gap for the two presses to count as one gesture. */
#define NS_DOUBLE_WINDOW_MS 300
#endif

/* Long enough for the ring to repaint and be read. */
#define FORGET_NOTICE_MS   1200

static bool s_forgetting;

static void restart_cb(void *arg)
{
    esp_restart();
}

/*
 * Runs on iot_button's task, so the same rule the LVGL gesture handler follows
 * applies: signal and return, never block. The pause before rebooting is a
 * timer rather than a delay for that reason -- and it exists at all so that an
 * accidental press shows up on screen instead of looking like a spontaneous
 * reboot.
 */
static void on_forget_wifi(void *arg, void *usr_data)
{
    if (s_forgetting) {
        return;
    }
    s_forgetting = true;

    ESP_LOGW(TAG, "BOOT held %d ms -- forgetting the saved network", FORGET_HOLD_MS);
    ui_set_status("forgetting wi-fi", false);
    ui_set_stopped(true);

    wifi_creds_erase();

    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "forget" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, FORGET_NOTICE_MS * 1000);
    } else {
        esp_restart();
    }
}

#if CONFIG_MIC_NS_ENABLE
/*
 * Double click toggles the microphone denoiser.
 *
 * Registered ONLY when the denoiser is compiled in, and that is the whole
 * reason the gesture is conditional rather than always present saying "not
 * built". Asking iot_button for a double click makes it hold every single click
 * back until the double-click window has expired, to see whether a second press
 * is coming -- so a build without the denoiser would be paying a slower session
 * toggle for a feature it does not have. The session toggle is the escape
 * hatch; it does not subsidise anything.
 *
 * Same rule as the other callbacks: this runs on iot_button's task, so it sets
 * a flag and returns. The capture task notices the change on its next block and
 * resets the FIFO there, and session_ctl's worker does the flash write -- an
 * NVS commit runs with the cache off and would stall the audio path from here.
 */
static void on_toggle_ns(void *arg, void *usr_data)
{
    if (!audio_io_ns_available()) {
        ESP_LOGW(TAG, "EVT bootdouble -- denoiser unavailable");
        ui_flash_status("noise reduction n/a", NS_NOTICE_MS);
        return;
    }
    const bool on = !audio_io_ns_enabled();
    ESP_LOGI(TAG, "EVT bootdouble -- noise reduction %s", on ? "on" : "off");
    audio_io_ns_set(on);
    session_ctl_request_ns_save();
    /*
     * flash, not set_status: this is a confirmation, not a session state.
     * ui_set_status(..., false) would ALSO assert the session is down, which
     * this callback has no idea about and which drops the behaviour ladder back
     * to CONNECTING. Static literals either way -- only the pointer is kept.
     */
    ui_flash_status(on ? "noise reduction on" : "noise reduction off",
                    NS_NOTICE_MS);
}
#endif

static void on_click(void *arg, void *usr_data)
{
    /* session_ctl takes requests from any task and has its own debounce and
     * cooldown, so the button needs no gating that the screen does not.
     *
     * Unconditionally a toggle, unlike the screen tap, which interrupts while the
     * agent is speaking. See the header for why the escape hatch stays simple. */
    ESP_LOGI(TAG, "EVT bootclick");
    session_ctl_request_toggle();
}

esp_err_t boot_button_start(void)
{
    const button_config_t btn_cfg = {
        .long_press_time = FORGET_HOLD_MS,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = BOOT_BUTTON_GPIO,
        .active_level = 0,
        /* disable_pull left false: with active_level 0 that selects the
         * internal pull-up, which is what makes the released state read high. */
    };

    button_handle_t btn;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not claim GPIO %d: %s",
                 BOOT_BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

#if CONFIG_MIC_NS_ENABLE
    /*
     * WIDEN THE REPEAT WINDOW, or the double click is not reachable.
     *
     * iot_button counts a second press as a repeat only if BOTH the press and
     * the gap before it come in under short_press_ticks, and the component's
     * default (CONFIG_BUTTON_SHORT_PRESS_TIME_MS) is 180 ms. A deliberate
     * two-finger-tap on a stiff through-hole button does not make that: two
     * attempts on the bench produced no BUTTON_DOUBLE_CLICK at all, only single
     * clicks, which is how this was found.
     *
     * Set per-handle rather than by raising the global Kconfig, so nothing else
     * that uses iot_button inherits it. The cost lands on the single click,
     * which is now held back up to this long to see whether a second press is
     * coming -- 300 ms on the session toggle, still well under the 1.1-6.0 s the
     * WebSocket handshake takes, so it is not the slow part of starting a
     * session.
     */
    ESP_ERROR_CHECK(iot_button_set_param(btn, BUTTON_SHORT_PRESS_TIME_MS,
                                         (void *)(intptr_t)NS_DOUBLE_WINDOW_MS));
#endif

    button_event_args_t hold = { .long_press.press_time = FORGET_HOLD_MS };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &hold,
                                           on_forget_wifi, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL,
                                           on_click, NULL));
#if CONFIG_MIC_NS_ENABLE
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL,
                                           on_toggle_ns, NULL));

    ESP_LOGI(TAG, "BOOT button ready (click: start/stop, double: noise reduction,"
                  " hold %d s: forget wi-fi)", FORGET_HOLD_MS / 1000);
#else
    ESP_LOGI(TAG, "BOOT button ready (click: start/stop, hold %d s: forget wi-fi)",
             FORGET_HOLD_MS / 1000);
#endif
    return ESP_OK;
}
