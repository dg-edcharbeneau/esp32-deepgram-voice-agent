/*
 * The conversation, on flash, so a reboot is not the end of it.
 *
 * dg_agent keeps the last few turns in RAM and replays them into every new
 * Settings message, which is what makes a dropped socket invisible. This module
 * is the other half: it puts that same blob somewhere a brownout cannot reach,
 * so an unplugged cable, a crashed task or the stuck-session reboot in main.c
 * comes back into the conversation rather than out of it.
 *
 * WHY NOT NVS, WHICH IS WHERE EVERY OTHER SETTING LIVES
 *
 * The nvs partition is 0x6000: six 4 kB pages, one always held in reserve for
 * compaction, 126 entries of 32 B each. That is 630 entries for the whole
 * device, and it is already shared with the Wi-Fi driver's calibration blob,
 * the credentials, the API key and the agent settings. A kilobyte-scale blob
 * needs its new copy written before the old one is erased, so a rewrite wants
 * roughly twice its entry count free at that instant, in a five-page arena --
 * which means compaction on most writes. And compaction rewrites its
 * NEIGHBOURS. The erase budget a per-turn history write would spend is the
 * budget holding the Wi-Fi credentials and the API key, on a board whose only
 * recovery from losing them is holding BOOT for three seconds. Depth of
 * transcript is not worth that trade.
 *
 * SPIFFS on `storage` was the other obvious answer and is worse: the component
 * costs flash, CONFIG_SPIFFS_CACHE holds a few kB of INTERNAL RAM for the
 * lifetime of the mount, and the first mount scans a 7 MB partition's object
 * lookup at boot. All to hold one file that is never named, listed or shared.
 *
 * SO: RAW SLOTS IN `storage`
 *
 * `storage` is declared in partitions.csv and mounted by nobody -- not here, and
 * not by spec_analyzer_radial, the sibling firmware the partition table is kept
 * byte-identical to. Seven megabytes, of which this uses thirty-two kilobytes.
 *
 * The layout is a ring of HISTORY_STORE_SLOTS single-sector records. A read
 * scans every slot header and takes the highest sequence number whose magic and
 * CRC both check. A write erases the *next* slot, writes the payload, and writes
 * the header LAST -- so a power loss anywhere in the middle leaves the previous
 * record whole and the new one failing its magic check. That is atomicity
 * without a journal, which is the entire reason for the A/B ring: an in-place
 * rewrite has a window where neither copy is good, and this has none.
 *
 * Wear works out to eight sectors of a hundred thousand cycles each, so roughly
 * eight hundred thousand writes -- and none of them anywhere near the
 * credentials.
 *
 * WHAT SURVIVES AND WHAT DOES NOT. `idf.py flash` writes the bootloader, the
 * partition table and the app; data partitions are untouched, so a reflash and
 * a swap to the sibling firmware both keep the conversation, exactly as NVS
 * already does. `idf.py erase-flash` loses it, along with the credentials and
 * the key, so the device is being re-provisioned anyway. A change to the record
 * layout loses it, cleanly, through the version byte in the magic.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

/*
 * Eight sectors: 32 kB of a 7 MB partition, and eight times the write budget of
 * a single-slot scheme for a rounding error of the space available. There is no
 * reason to be frugal here and every reason not to be.
 */
#define HISTORY_STORE_SLOTS 8

/* One sector per record, minus the header. The caller's blob must fit. */
#define HISTORY_STORE_MAX_PAYLOAD (4096 - 20)

/*
 * Finds the partition and reads the newest valid record. Call once, before
 * anything can save.
 *
 * `buf` is filled with at most `cap` bytes and `*out_len` with how many arrived.
 * Nothing to resume is reported as ESP_OK with *out_len == 0 rather than as an
 * error -- a device that has never held a conversation is not a device with a
 * problem.
 *
 * `buf` IS ALSO THE SCRATCH the scan verifies candidate slots in, so its
 * contents are undefined whenever *out_len is 0. Do not read it back expecting
 * to find what was there before. Making it a private buffer instead would mean
 * holding a second copy of the largest record for the life of the device, which
 * is not worth it for a function called once at init.
 *
 * Returns ESP_ERR_NOT_FOUND when the partition itself is missing, which is the
 * caller's cue to run without persistence rather than to refuse to run.
 */
esp_err_t history_store_load(void *buf, size_t cap, size_t *out_len);

/*
 * Writes a new record and retires the previous one.
 *
 * BLOCKS for the sector erase -- tens of milliseconds with the flash cache
 * disabled on both cores. Never call this from the WebSocket event task, the
 * esp_timer task, or anything holding the LVGL lock. session_ctl's worker exists
 * for work shaped exactly like this.
 *
 * `len` of 0 is legal and meaningful: it records "this conversation was
 * deliberately forgotten", which reads back differently from a partition that
 * has never been written, and the difference is visible in the log.
 */
esp_err_t history_store_save(const void *buf, size_t len);

/*
 * Shorthand for saving a zero-length record. See above for why this is not an
 * erase of the slots.
 *
 * The firmware does not currently call it: dg_agent clears by emptying its arena
 * and letting the ordinary flush write a record that holds a turn count of zero,
 * which is the same "deliberately forgotten" signal one layer up. Kept because
 * it is the store's own way to say it, and because host/store.sh exercises the
 * zero-length path through it.
 */
esp_err_t history_store_erase(void);
