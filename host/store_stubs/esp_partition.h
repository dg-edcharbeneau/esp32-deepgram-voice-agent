/*
 * A file-backed flash, so history_store.c's A/B ring can be exercised -- and
 * crashed -- on a laptop.
 *
 * The interesting property of that module is what happens when a write does not
 * finish, and that is precisely the thing a device cannot be asked to
 * demonstrate on cue. Here a write can be told to stop after N bytes, which is
 * what a brownout looks like from the flash's point of view.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82 } esp_partition_subtype_t;

typedef struct {
    const char *label;
    size_t size;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len);

/* Test controls, not part of the real API. */
void fake_flash_reset(void);
/* Cut the NEXT n write bytes short: 0 means "fail this write entirely". */
void fake_flash_cut_after(long bytes);
void fake_flash_no_partition(int missing);
/* Start from the image named by $FAKE_FLASH_FILE, if any, and write it back. */
void fake_flash_attach(void);
void fake_flash_sync(void);
