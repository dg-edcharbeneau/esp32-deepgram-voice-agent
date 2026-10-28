#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "iot_button.h"
typedef struct { int32_t gpio_num; uint8_t active_level; bool disable_pull; bool enable_power_save; } button_gpio_config_t;
esp_err_t iot_button_new_gpio_device(const button_config_t *cfg, const button_gpio_config_t *gpio,
                                     button_handle_t *out);
