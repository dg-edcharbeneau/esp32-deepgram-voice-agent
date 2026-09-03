#pragma once

#include <stddef.h>
#include <stdint.h>

/* The same polynomial and bit order as the ROM's esp_crc32_le. */
uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
