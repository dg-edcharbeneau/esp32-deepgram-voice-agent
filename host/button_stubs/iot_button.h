/* Host stub for espressif/button v4: just enough of the API for boot_button.c
 * to register its callbacks, which fake_button.c records so the test can fire
 * them in the order the real component does. */
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef void *button_handle_t;
typedef enum {
    BUTTON_PRESS_DOWN = 0,
    BUTTON_PRESS_UP,
    BUTTON_SINGLE_CLICK,
    BUTTON_DOUBLE_CLICK,
    BUTTON_LONG_PRESS_START,
    BUTTON_EVENT_MAX,
} button_event_t;
typedef enum { BUTTON_LONG_PRESS_TIME_MS = 0, BUTTON_SHORT_PRESS_TIME_MS } button_param_t;
typedef struct { uint16_t long_press_time; uint16_t short_press_time; } button_config_t;
typedef union { struct { uint16_t press_time; } long_press; } button_event_args_t;
typedef void (*button_cb_t)(void *button_handle, void *usr_data);
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t e, button_event_args_t *args,
                                 button_cb_t cb, void *usr_data);
esp_err_t iot_button_set_param(button_handle_t h, button_param_t p, void *value);
