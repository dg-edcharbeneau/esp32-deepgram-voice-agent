#pragma once
/* The real esp_log.h pulls in sdkconfig.h, and boot_button.c relies on that for
 * CONFIG_MIC_NS_ENABLE. Without this the double click compiles OUT and every
 * double-click check passes vacuously -- which is how it was found. */
#include "sdkconfig.h"
#include <stdio.h>
extern int host_log_enabled;
#define HOST_LOG(l, tag, fmt, ...) do { if (host_log_enabled) printf("    %s (%s) " fmt "\n", l, tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO } esp_log_level_t;
static inline void esp_log_level_set(const char *tag, esp_log_level_t l) { (void)tag; (void)l; }
