/*
 * Exercises main/boot_button.c on a laptop: what a press of BOOT does while an
 * alarm is ringing, and that it goes back to normal afterwards.
 *
 * The case this exists for cannot be tried casually on the board. Holding BOOT
 * for three seconds is "forget wi-fi", and a sleepy hand holds the button until
 * the noise stops -- so the first version, which dismissed only on a click,
 * erased the credentials of anyone who did that. A finger on the device is
 * still needed to prove the button component's real event timing; this proves
 * the decisions, every sequence, every run.
 *
 * The real boot_button.c is compiled. iot_button, the clock and every module it
 * calls are fakes (host/button_stubs) that record what was asked of them. Each
 * scenario fires events in the order espressif/button emits them: PRESS_DOWN
 * on contact, LONG_PRESS_START at the hold time, SINGLE_CLICK or DOUBLE_CLICK
 * only after release and the repeat window.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_button.h"
#include "button_gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "iot_button.h"

int host_log_enabled;

static int failures;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

/* ---------------- fakes ---------------- */

static int64_t s_now_us;
static button_cb_t s_cb[BUTTON_EVENT_MAX];

/* What boot_button.c asked for. Reset per scenario. */
static struct {
    int wifi_erased;
    int restarts;
    int toggles;
    int ns_sets;
    int dismissals;
} got;

static bool s_ringing;
static bool s_ns_on;

int64_t esp_timer_get_time(void) { return s_now_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *o)
{
    (void)a;
    *o = (void *)1;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    (void)t;
    (void)us;
    /* The forget path reboots from this timer; counting the erase is enough. */
    return ESP_OK;
}
void esp_restart(void) { got.restarts++; }

esp_err_t iot_button_new_gpio_device(const button_config_t *c, const button_gpio_config_t *g,
                                     button_handle_t *o)
{
    (void)c;
    (void)g;
    *o = (void *)1;
    return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t e, button_event_args_t *a,
                                 button_cb_t cb, void *u)
{
    (void)h;
    (void)a;
    (void)u;
    s_cb[e] = cb;
    return ESP_OK;
}
esp_err_t iot_button_set_param(button_handle_t h, button_param_t p, void *v)
{
    (void)h;
    (void)p;
    (void)v;
    return ESP_OK;
}

/*
 * The real alarms_dismiss() stops the chime at once but leaves alarms_ringing()
 * true until the next 1 Hz tick sees it. Modelled the hard way -- ringing ends
 * on the dismiss -- because that is the case where only boot_button.c's own
 * window stands between a second press and a double click.
 */
bool alarms_ringing(void) { return s_ringing; }
void alarms_dismiss(void)
{
    got.dismissals++;
    s_ringing = false;
}

void audio_io_ns_set(bool on) { got.ns_sets++; s_ns_on = on; }
bool audio_io_ns_enabled(void) { return s_ns_on; }
bool audio_io_ns_available(void) { return true; }
void session_ctl_request_toggle(void) { got.toggles++; }
void session_ctl_request_ns_save(void) {}
void ui_set_status(const char *t, bool l) { (void)t; (void)l; }
void ui_set_stopped(bool s) { (void)s; }
void ui_flash_status(const char *t, uint32_t ms) { (void)t; (void)ms; }
void wifi_creds_erase(void) { got.wifi_erased++; }

/* ---------------- driving it ---------------- */

static void at(int64_t ms) { s_now_us = ms * 1000; }

static void fire(button_event_t e)
{
    if (s_cb[e] != NULL) {
        s_cb[e](NULL, NULL);
    }
}

/* One scenario: clean counters, alarm state as given, the clock moved on far
 * enough that nothing from the previous scenario's window carries over. */
static int64_t s_base;
static void scenario(bool ringing)
{
    memset(&got, 0, sizeof(got));
    s_ringing = ringing;
    s_base += 60000;
    at(s_base);
}

/* A single click as the component reports it: contact, then the click after
 * release and the repeat window. */
static void click(int64_t t)
{
    at(t);
    fire(BUTTON_PRESS_DOWN);
    at(t + 120);
    fire(BUTTON_PRESS_UP);
    at(t + 420);
    fire(BUTTON_SINGLE_CLICK);
}

int main(void)
{
    ESP_ERROR_CHECK(boot_button_start());

    /* ---- while ringing ---- */

    scenario(true);
    at(s_base);
    fire(BUTTON_PRESS_DOWN);
    check("ringing, hold: the press itself dismisses", got.dismissals == 1);
    at(s_base + 3000);
    fire(BUTTON_LONG_PRESS_START);
    at(s_base + 5000);
    fire(BUTTON_PRESS_UP);
    check("ringing, hold 5 s: wi-fi is NOT forgotten", got.wifi_erased == 0);
    check("ringing, hold 5 s: no reboot", got.restarts == 0);
    check("ringing, hold 5 s: no session toggle", got.toggles == 0);

    scenario(true);
    click(s_base);
    check("ringing, click: dismissed", got.dismissals == 1);
    check("ringing, click: no session toggle", got.toggles == 0);

    scenario(true);
    at(s_base);
    fire(BUTTON_PRESS_DOWN);
    at(s_base + 100);
    fire(BUTTON_PRESS_UP);
    at(s_base + 250);
    fire(BUTTON_PRESS_DOWN);   /* ringing already over: only the window holds */
    at(s_base + 350);
    fire(BUTTON_PRESS_UP);
    at(s_base + 650);
    fire(BUTTON_DOUBLE_CLICK);
    check("ringing, double click: dismissed once", got.dismissals == 1);
    check("ringing, double click: noise reduction untouched", got.ns_sets == 0);
    check("ringing, double click: no session toggle", got.toggles == 0);

    /* ---- the window after a dismiss ---- */

    scenario(true);
    click(s_base);
    click(s_base + 1000);     /* inside SWALLOW_MS: still the alarm's */
    check("second click 1.0 s after dismiss: still swallowed", got.toggles == 0);
    click(s_base + 2600);     /* past it, nothing ringing: a normal click */
    check("click 2.6 s after dismiss: toggles the session", got.toggles == 1);

    /* ---- nothing ringing: every gesture means what it always did ---- */

    scenario(false);
    click(s_base);
    check("not ringing, click: toggles the session", got.toggles == 1);
    check("not ringing, click: nothing dismissed", got.dismissals == 0);

    scenario(false);
    at(s_base);
    fire(BUTTON_PRESS_DOWN);
    at(s_base + 100);
    fire(BUTTON_PRESS_UP);
    at(s_base + 250);
    fire(BUTTON_PRESS_DOWN);
    at(s_base + 350);
    fire(BUTTON_PRESS_UP);
    at(s_base + 650);
    fire(BUTTON_DOUBLE_CLICK);
    check("not ringing, double click: toggles noise reduction", got.ns_sets == 1);

    scenario(false);
    at(s_base);
    fire(BUTTON_PRESS_DOWN);
    at(s_base + 3000);
    fire(BUTTON_LONG_PRESS_START);
    check("not ringing, hold 3 s: the escape hatch still forgets wi-fi",
          got.wifi_erased == 1);

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    return 0;
}
