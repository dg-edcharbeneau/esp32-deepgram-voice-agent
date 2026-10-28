#pragma once
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) { fprintf(stderr, "ESP_ERROR_CHECK failed: %s\n", #x); abort(); } } while (0)
