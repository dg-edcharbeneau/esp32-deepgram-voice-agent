/*
 * NOR flash, as much of it as history_store.c can tell apart: erase sets bytes
 * to 0xFF, a write only clears bits, and a write can be cut short mid-way.
 *
 * The bit-clearing matters. A write over unerased flash cannot set a bit back
 * to 1, so a header written into a slot that was not erased first comes out as
 * the AND of the two -- which is exactly the corruption the ring is supposed to
 * be immune to, and it would not show up if this stub just memcpy'd.
 */
#include "esp_partition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_SIZE (64 * 1024)

static uint8_t s_flash[FAKE_SIZE];
static long s_cut = -1;           /* < 0: writes complete normally */
static int s_missing;
static const esp_partition_t s_part = { .label = "storage", .size = FAKE_SIZE };

int host_log_enabled;

/*
 * The image can be backed by a FILE, which is what makes the sequence-counter
 * check possible at all. history_store.c caches the newest slot in statics, and
 * the bug being tested is what a save does when those statics were never
 * established -- so the test needs FRESH STATICS over an OLD FLASH, and the only
 * way to get fresh statics is a new process.
 */
static const char *image_path(void) { return getenv("FAKE_FLASH_FILE"); }

static void fake_flash_load(void)
{
    const char *path = image_path();
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    if (fread(s_flash, 1, sizeof(s_flash), f) != sizeof(s_flash)) {
        memset(s_flash, 0xFF, sizeof(s_flash));
    }
    fclose(f);
}

void fake_flash_sync(void)
{
    const char *path = image_path();
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fwrite(s_flash, 1, sizeof(s_flash), f);
    fclose(f);
}

void fake_flash_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_cut = -1;
    s_missing = 0;
}

void fake_flash_attach(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    fake_flash_load();
}

void fake_flash_cut_after(long bytes) { s_cut = bytes; }
void fake_flash_no_partition(int missing) { s_missing = missing; }

const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "ESP_OK" : "ESP_FAIL";
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)type; (void)subtype; (void)label;
    return s_missing ? NULL : &s_part;
}

esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    (void)p;
    if (off + len > FAKE_SIZE) {
        return ESP_FAIL;
    }
    memcpy(dst, s_flash + off, len);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    (void)p;
    if (off + len > FAKE_SIZE) {
        return ESP_FAIL;
    }

    size_t n = len;
    int cut = 0;
    if (s_cut >= 0) {
        if ((size_t)s_cut < n) {
            n = (size_t)s_cut;
            cut = 1;
        }
        s_cut -= (long)n;
    }

    const uint8_t *b = src;
    for (size_t i = 0; i < n; i++) {
        s_flash[off + i] &= b[i];   /* writes clear bits; they never set them */
    }
    /* A cut write reports success, because that is what a brownout does: the
     * call never returns at all, and nothing gets to see an error. */
    return cut ? ESP_OK : ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    (void)p;
    if (off + len > FAKE_SIZE) {
        return ESP_FAIL;
    }
    if (s_cut == 0) {
        return ESP_FAIL;    /* power lost before the erase completed */
    }
    memset(s_flash + off, 0xFF, len);
    return ESP_OK;
}

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}
