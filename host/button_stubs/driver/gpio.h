#pragma once
#include "esp_err.h"
/* boot_button.c installs the GPIO interrupt service itself, before the button
 * component would; nothing here has interrupts, so it only has to link. */
static inline esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
