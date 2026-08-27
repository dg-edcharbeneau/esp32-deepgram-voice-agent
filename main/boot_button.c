#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "boot_button.h"
#include "session_ctl.h"
#include "ui.h"
#include "wifi_creds.h"

static const char *TAG = "boot_btn";

#define BOOT_BUTTON_GPIO   0
#define FORGET_HOLD_MS     3000

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

    button_event_args_t hold = { .long_press.press_time = FORGET_HOLD_MS };
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, &hold,
                                           on_forget_wifi, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL,
                                           on_click, NULL));

    ESP_LOGI(TAG, "BOOT button ready (click: start/stop, hold %d s: forget wi-fi)",
             FORGET_HOLD_MS / 1000);
    return ESP_OK;
}
