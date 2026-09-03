#pragma once

#include <stdio.h>

/* Quiet by default -- the harness prints its own verdicts. Set HOST_LOG=1 to
 * see what the module thinks it is doing. */
extern int host_log_enabled;

#define HOST_LOG(level, tag, fmt, ...) \
    do { if (host_log_enabled) fprintf(stderr, "[" level "] %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG("D", tag, fmt, ##__VA_ARGS__)
