#pragma once
#include <stdbool.h>
#include <stdint.h>
void ui_set_status(const char *text, bool session_live);
void ui_set_stopped(bool stopped);
void ui_flash_status(const char *text, uint32_t ms);
