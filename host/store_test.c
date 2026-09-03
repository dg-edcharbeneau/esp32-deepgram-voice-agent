/*
 * Exercises main/history_store.c against a fake NOR flash, including the case
 * the device cannot be asked to demonstrate: power lost partway through a write.
 *
 * The module's whole claim is that a record is either complete or invisible --
 * that the header goes down last, so an interrupted save costs the new record
 * and never the old one. That is an argument about ordering, and an argument
 * about ordering is worth a test rather than a comment.
 *
 * The real main/history_store.c is compiled here. Only the flash beneath it is
 * fake (host/store_stubs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "history_store.h"

extern int host_log_enabled;

static int failures;

static void check(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

/* Load into a scratch buffer and compare against what was expected. */
static int loads_as(const char *expect)
{
    static char buf[HISTORY_STORE_MAX_PAYLOAD];
    size_t len = 0;

    memset(buf, 0, sizeof(buf));
    if (history_store_load(buf, sizeof(buf), &len) != ESP_OK) {
        return 0;
    }
    if (expect == NULL) {
        return len == 0;
    }
    return len == strlen(expect) && memcmp(buf, expect, len) == 0;
}

/*
 * Phases of the sequence-counter check, each in its OWN PROCESS over the same
 * file-backed flash. history_store.c caches the newest slot in statics, and the
 * bug is what a save does when those were never established -- so the test needs
 * fresh statics over an old flash, which a single process cannot arrange.
 *
 * Without the header scan in the store, phase two restarts at sequence 1, loses
 * to the records phase one left behind, and phase three reads a stale
 * conversation while the fresh one is discarded. Exactly the silent failure the
 * scan exists to prevent, and it gets worse the longer a device has been used.
 */
static int phase_seed(void)
{
    fake_flash_attach();
    for (int i = 0; i < HISTORY_STORE_SLOTS + 2; i++) {
        char s[32];
        snprintf(s, sizeof(s), "old-%d", i);
        if (history_store_save(s, strlen(s)) != ESP_OK) {
            return 1;
        }
    }
    fake_flash_sync();
    return 0;
}

/* No load first -- that is the whole point. */
static int phase_blind_save(void)
{
    fake_flash_attach();
    const int ok = history_store_save("newest", 6) == ESP_OK;
    fake_flash_sync();
    return ok ? 0 : 1;
}

static int phase_verify(void)
{
    fake_flash_attach();
    return loads_as("newest") ? 0 : 1;
}

int main(int argc, char **argv)
{
    host_log_enabled = getenv("HOST_LOG") != NULL;

    if (argc > 1) {
        if (strcmp(argv[1], "--seed") == 0)       return phase_seed();
        if (strcmp(argv[1], "--blind-save") == 0) return phase_blind_save();
        if (strcmp(argv[1], "--verify") == 0)     return phase_verify();
        fprintf(stderr, "unknown phase: %s\n", argv[1]);
        return 2;
    }

    /*
     * FIRST, because history_store.c caches the partition lookup once it
     * succeeds -- correctly, since a partition does not come and go on a real
     * device. Run after any successful load and this would be testing the cache
     * rather than the missing-partition path.
     */
    fake_flash_reset();
    fake_flash_no_partition(1);
    {
        char buf[16];
        size_t len = 0;
        check("a missing partition is reported, not fatal",
              history_store_load(buf, sizeof(buf), &len) == ESP_ERR_NOT_FOUND);
    }
    fake_flash_no_partition(0);

    fake_flash_reset();
    check("a blank partition reads as no history", loads_as(NULL));

    check("a saved record reads back",
          history_store_save("first", 5) == ESP_OK && loads_as("first"));

    check("a second save supersedes the first",
          history_store_save("second", 6) == ESP_OK && loads_as("second"));

    /*
     * Past the ring, so every slot has been used at least twice and the
     * highest-sequence rule is doing real work rather than picking the only
     * candidate.
     */
    for (int i = 0; i < HISTORY_STORE_SLOTS * 3; i++) {
        char s[32];
        snprintf(s, sizeof(s), "turn-%d", i);
        if (history_store_save(s, strlen(s)) != ESP_OK) {
            check("wrapping the ring", 0);
            break;
        }
    }
    {
        char last[32];
        snprintf(last, sizeof(last), "turn-%d", HISTORY_STORE_SLOTS * 3 - 1);
        check("wrapping the ring keeps the newest record", loads_as(last));
    }

    /*
     * THE POINT OF THE HARNESS. Cut the save after 8 bytes: the payload is
     * partly down and the header never lands, so the slot has no magic in it and
     * the previous record must still be the one that loads.
     */
    check("a save cut mid-payload leaves the previous record intact",
          (fake_flash_cut_after(8), history_store_save("interrupted-write", 17),
           fake_flash_cut_after(-1),
           !loads_as("interrupted-write")));

    /* And the device carries on: the next save works and supersedes it. */
    check("the ring still advances after an interrupted write",
          history_store_save("after", 5) == ESP_OK && loads_as("after"));

    /*
     * Cut after the payload but before the header is complete. This is the
     * narrowest window and the one the ordering exists for -- the bytes are
     * there, but nothing says they are.
     */
    check("a save cut mid-header leaves the previous record intact",
          (fake_flash_cut_after(5 + 2), history_store_save("torn", 4),
           fake_flash_cut_after(-1),
           loads_as("after")));

    check("a deliberate clear reads back as empty, not as unwritten",
          history_store_erase() == ESP_OK && loads_as(NULL));

    check("an oversized record is refused rather than truncated",
          history_store_save("x", HISTORY_STORE_MAX_PAYLOAD + 1) ==
              ESP_ERR_INVALID_SIZE);

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
