#include "history_store.h"

#include <inttypes.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "history_store";

/*
 * 'DGH' plus a layout version. Bump the last byte whenever the record's meaning
 * changes and every existing slot stops validating, which is the upgrade path:
 * an old device comes up with no history rather than with misread history.
 */
#define SLOT_MAGIC 0x31484744u /* "DGH1", little-endian */

#define SLOT_SIZE 4096

/*
 * Written LAST, after the payload, and that ordering is the whole design. The
 * magic and CRC only appear once the bytes they describe are already on flash,
 * so a record is either complete or invisible.
 */
typedef struct {
    uint32_t magic;
    uint32_t seq;     /* monotonic; highest valid one wins */
    uint32_t len;     /* payload bytes following this header */
    uint32_t crc;     /* esp_crc32_le over exactly those bytes */
    uint32_t _pad;    /* reserved; keeps the header a round 20 B, which is what
                       * HISTORY_STORE_MAX_PAYLOAD subtracts */
} slot_header_t;

_Static_assert(sizeof(slot_header_t) + HISTORY_STORE_MAX_PAYLOAD <= SLOT_SIZE,
               "a record must fit in one sector");

static const esp_partition_t *s_part;
static uint32_t s_seq;              /* sequence of the newest record we know of */
static int s_slot = -1;             /* slot it lives in, -1 when there is none */

static const esp_partition_t *partition(void)
{
    if (s_part == NULL) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                          "storage");
    }
    return s_part;
}

/*
 * Reads one slot's header and validates it against its payload.
 *
 * The CRC is checked here rather than at read time because a header alone
 * proves nothing: an interrupted write can leave a *previous* record's header
 * intact above a half-erased payload only if the erase failed midway, and the
 * cost of ruling that out is one pass over at most 4 kB.
 */
static bool slot_valid(int index, slot_header_t *out, void *scratch, size_t scratch_len)
{
    slot_header_t h;
    const size_t off = (size_t)index * SLOT_SIZE;

    if (esp_partition_read(s_part, off, &h, sizeof(h)) != ESP_OK) {
        return false;
    }
    if (h.magic != SLOT_MAGIC || h.len > HISTORY_STORE_MAX_PAYLOAD) {
        return false;
    }
    if (h.len > scratch_len) {
        /* A record written by a build with a bigger buffer. Not corrupt, just
         * not ours to read -- treated the same way, which is to ignore it. */
        return false;
    }
    if (h.len > 0 &&
        esp_partition_read(s_part, off + sizeof(h), scratch, h.len) != ESP_OK) {
        return false;
    }
    const uint32_t crc = (h.len > 0)
                             ? esp_crc32_le(0, (const uint8_t *)scratch, h.len)
                             : 0;
    if (crc != h.crc) {
        ESP_LOGW(TAG, "slot %d: crc mismatch, ignoring", index);
        return false;
    }
    *out = h;
    return true;
}

esp_err_t history_store_load(void *buf, size_t cap, size_t *out_len)
{
    *out_len = 0;

    if (partition() == NULL) {
        ESP_LOGW(TAG, "no 'storage' partition -- history will not persist");
        return ESP_ERR_NOT_FOUND;
    }

    int best = -1;
    slot_header_t best_h = {0};

    for (int i = 0; i < HISTORY_STORE_SLOTS; i++) {
        slot_header_t h;
        if (!slot_valid(i, &h, buf, cap)) {
            continue;
        }
        /* Signed difference, so the comparison still means "newer" across the
         * wrap at UINT32_MAX. It takes 800k writes to get there and the ring
         * outlives the flash, but a comparison that is only correct for the
         * first four billion records is not obviously correct at all. */
        if (best < 0 || (int32_t)(h.seq - best_h.seq) > 0) {
            best = i;
            best_h = h;
        }
    }

    if (best < 0) {
        ESP_LOGI(TAG, "no saved history");
        return ESP_OK;
    }

    /* Re-read: the scan above overwrote `buf` with every candidate it examined,
     * and the winner was not necessarily the last one it looked at. */
    if (best_h.len > 0) {
        const size_t off = (size_t)best * SLOT_SIZE + sizeof(slot_header_t);
        esp_err_t err = esp_partition_read(s_part, off, buf, best_h.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "slot %d: re-read failed (%s)", best, esp_err_to_name(err));
            return err;
        }
    }

    s_slot = best;
    s_seq = best_h.seq;
    *out_len = best_h.len;

    /* Zero length is a deliberate clear, not an empty device -- worth being able
     * to tell apart in a capture. */
    ESP_LOGI(TAG, "loaded slot %d seq %" PRIu32 ": %zu bytes%s",
             best, best_h.seq, (size_t)best_h.len,
             (best_h.len == 0) ? " (deliberately cleared)" : "");
    return ESP_OK;
}

/*
 * Establish s_seq/s_slot from the headers alone, for the case where load() never
 * got to.
 *
 * WITHOUT THIS, a save after a failed or skipped load starts again at sequence
 * 1 -- and loses. The old records are still in the ring with much higher
 * sequence numbers, so the next boot picks one of them and resumes a stale
 * conversation while discarding the fresh one. Silently, and more convincingly
 * the longer the device has been in use.
 *
 * Headers only, no CRC: this is picking a slot to write and a number to count
 * from, not a record to trust. A slot whose payload is corrupt still occupied a
 * sequence number, and stepping past it is the whole point.
 */
static void ensure_scanned(void)
{
    if (s_slot >= 0) {
        return;
    }
    for (int i = 0; i < HISTORY_STORE_SLOTS; i++) {
        slot_header_t h;
        if (esp_partition_read(s_part, (size_t)i * SLOT_SIZE, &h, sizeof(h)) != ESP_OK) {
            continue;
        }
        if (h.magic != SLOT_MAGIC) {
            continue;
        }
        if (s_slot < 0 || (int32_t)(h.seq - s_seq) > 0) {
            s_slot = i;
            s_seq = h.seq;
        }
    }
}

esp_err_t history_store_save(const void *buf, size_t len)
{
    if (partition() == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > HISTORY_STORE_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    ensure_scanned();

    const int next = (s_slot < 0) ? 0 : (s_slot + 1) % HISTORY_STORE_SLOTS;
    const size_t off = (size_t)next * SLOT_SIZE;

    esp_err_t err = esp_partition_erase_range(s_part, off, SLOT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slot %d: erase failed (%s)", next, esp_err_to_name(err));
        return err;
    }

    if (len > 0) {
        err = esp_partition_write(s_part, off + sizeof(slot_header_t), buf, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "slot %d: payload write failed (%s)", next,
                     esp_err_to_name(err));
            return err;
        }
    }

    /*
     * LAST. Until this lands the slot is an erased sector with no magic in it,
     * so a power loss between here and the line above costs the new record and
     * leaves the old one exactly where it was.
     */
    const slot_header_t h = {
        .magic = SLOT_MAGIC,
        .seq = s_seq + 1,
        .len = (uint32_t)len,
        /* len 0 is the deliberate-clear record; esp_crc32_le must not be
         * handed a null pointer even with nothing to read. */
        .crc = (len > 0) ? esp_crc32_le(0, (const uint8_t *)buf, len) : 0,
        ._pad = 0,
    };
    err = esp_partition_write(s_part, off, &h, sizeof(h));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slot %d: header write failed (%s)", next, esp_err_to_name(err));
        return err;
    }

    s_slot = next;
    s_seq = h.seq;
    /*
     * LOGI, not LOGD: this build sets CONFIG_LOG_MAXIMUM_LEVEL=3, so a debug
     * line is compiled out -- and a store that can only ever be seen to FAIL is
     * a poor fit for a feature whose verification story is a serial capture.
     * One line per exchange, against a TLM line every second.
     */
    ESP_LOGI(TAG, "saved slot %d seq %" PRIu32 ": %zu bytes", next, h.seq, len);
    return ESP_OK;
}

esp_err_t history_store_erase(void)
{
    return history_store_save(NULL, 0);
}
