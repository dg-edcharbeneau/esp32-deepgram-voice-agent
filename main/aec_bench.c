#include "aec_bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aec_bench";

#define VEC_ROOT "/vectors"
#define SKIP_SECONDS 2   /* filter convergence is not failure */

/* ---------------- WAV ---------------- */

/*
 * Parses far enough to find the samples. Deliberately not trusting the 44-byte
 * canonical layout: the documentation states no format at all for these files, so
 * the fields are read and reported rather than assumed. A LIST chunk before
 * `data` is common and would break a fixed offset.
 */
typedef struct {
    FILE *f;
    long data_off;
    uint32_t data_len;
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
} wav_t;

static bool wav_open(wav_t *w, const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", VEC_ROOT, name);
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (w->f == NULL) {
        ESP_LOGE(TAG, "%s: cannot open", name);
        return false;
    }

    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), w->f) != sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s: not a RIFF/WAVE file", name);
        fclose(w->f);
        w->f = NULL;
        return false;
    }

    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, sizeof(ck), w->f) != sizeof(ck)) {
            break;
        }
        uint32_t len = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                       ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (fread(fmt, 1, sizeof(fmt), w->f) != sizeof(fmt)) {
                break;
            }
            w->channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            w->rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                      ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            w->bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if (len > 16) {
                fseek(w->f, (long)(len - 16), SEEK_CUR);
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            w->data_off = ftell(w->f);
            w->data_len = len;
            ESP_LOGI(TAG, "%s: %" PRIu32 " Hz, %u ch, %u bit, %" PRIu32 " bytes (%.1f s)",
                     name, w->rate, w->channels, w->bits, w->data_len,
                     (double)w->data_len / (double)(w->rate * w->channels * (w->bits / 8)));
            return true;
        } else {
            fseek(w->f, (long)len + (len & 1), SEEK_CUR);
        }
    }
    ESP_LOGE(TAG, "%s: no data chunk", name);
    fclose(w->f);
    w->f = NULL;
    return false;
}

static void wav_rewind(wav_t *w) { fseek(w->f, w->data_off, SEEK_SET); }
static void wav_close(wav_t *w) { if (w->f) { fclose(w->f); w->f = NULL; } }

/* Returns samples read; short read means end of file. */
static size_t wav_read(wav_t *w, int16_t *dst, size_t samples)
{
    return fread(dst, sizeof(int16_t), samples, w->f);
}

/* ---------------- heap ---------------- */

typedef struct {
    size_t internal;
    size_t largest;
    size_t spiram;
} heap_snap_t;

static heap_snap_t heap_now(void)
{
    heap_snap_t h = {
        .internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    };
    return h;
}

/* Echo-only frame map, filled by segment_vectors() below. Declared up here
 * because bench_reference() has to score the same frames the modes do. */
#define SEG_MAX_FRAMES 2048
static float *s_far_rms;
static float *s_near_rms;
static bool  *s_echo_only;
static size_t s_seg_frames;

/* ---------------- ERLE ---------------- */

static double erle_db(double near_energy, double out_energy)
{
    if (out_energy < 1.0) {
        out_energy = 1.0;
    }
    if (near_energy < 1.0) {
        return 0.0;
    }
    return 10.0 * log10(near_energy / out_energy);
}

/*
 * The reference answer. Espressif's aec_test_*.wav is what THEIR AEC produced
 * from the same near signal, so this is the number ours has to land near. If it
 * does not, the fault is in our integration and nothing measured in the room
 * afterwards would mean anything.
 */
static void bench_reference(const char *expected_name)
{
    wav_t near, exp;
    if (!wav_open(&near, "aec_in_near.wav")) {
        return;
    }
    if (!wav_open(&exp, expected_name)) {
        wav_close(&near);
        return;
    }

    enum { N = 512 };
    int16_t *a = heap_caps_aligned_alloc(16, N * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *b = heap_caps_aligned_alloc(16, N * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (a == NULL || b == NULL) {
        ESP_LOGE(TAG, "no memory for reference buffers");
        free(a); free(b); wav_close(&near); wav_close(&exp);
        return;
    }

    double ne = 0.0, oe = 0.0, se_n = 0.0, se_o = 0.0;
    size_t frame = 0, se_frames = 0;
    const size_t skip = (size_t)(SKIP_SECONDS * 16000) / N;
    for (;;) {
        size_t na = wav_read(&near, a, N);
        size_t nb = wav_read(&exp, b, N);
        size_t n = (na < nb) ? na : nb;
        if (n == 0) {
            break;
        }
        if ((frame % 50) == 0) {
            vTaskDelay(1);
        }
        if (frame >= skip) {
            for (size_t i = 0; i < n; i++) {
                ne += (double)a[i] * a[i];
                oe += (double)b[i] * b[i];
            }
            /* Same frames as the modes are scored on, or the comparison is not
             * like-for-like. N here is 512, which is what chunk turned out to be
             * for every mode. */
            if (frame < s_seg_frames && s_echo_only[frame]) {
                for (size_t i = 0; i < n; i++) {
                    se_n += (double)a[i] * a[i];
                    se_o += (double)b[i] * b[i];
                }
                se_frames++;
            }
        }
        frame++;
    }
    ESP_LOGI(TAG, "REFERENCE %s: erle=%.1f dB over %u echo-only frames "
             "| whole-file energy drop %.1f dB  <-- the number to match",
             expected_name, erle_db(se_n, se_o), (unsigned)se_frames,
             erle_db(ne, oe));

    free(a); free(b);
    wav_close(&near);
    wav_close(&exp);
}


/* ---------------- echo-only segmentation ---------------- */

/*
 * WHY THIS EXISTS
 *
 * The first version of this bench scored 10*log10(sum(near^2)/sum(out^2)) over the
 * whole file and called it ERLE. It is not. Espressif's own output scores 2.3 dB
 * by that measure, and their canceller is plainly better than 2.3 dB -- their
 * vector contains near-end speech, a canceller must not remove the talker, and so
 * whole-file energy reduction conflates removing the echo with removing
 * everything.
 *
 * ERLE is defined over SINGLE-TALK: far end active, near end silent. Nothing
 * labels those frames, so they are inferred, and the inference is the interesting
 * part:
 *
 *   During echo-only, near ~= g * far for the echo path's gain g.
 *   During double-talk, the talker only ADDS energy -- never subtracts.
 *
 * So across far-active frames, the LOW percentile of near/far is the echo path
 * alone; frames near that floor are echo-only, frames well above it contain a
 * voice. Taking the 20th percentile as g and admitting frames within 6 dB of it
 * is deliberately conservative: misclassifying double-talk as echo-only would
 * understate ERLE, which is the safe direction to be wrong in.
 */
static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* One pass over the vectors, no AEC involved. Fills s_echo_only. */
static bool segment_vectors(int chunk)
{
    wav_t near, far;
    if (!wav_open(&near, "aec_in_near.wav")) {
        return false;
    }
    if (!wav_open(&far, "aec_in_far.wav")) {
        wav_close(&near);
        return false;
    }

    int16_t *a = heap_caps_aligned_alloc(16, (size_t)chunk * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *b = heap_caps_aligned_alloc(16, (size_t)chunk * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (a == NULL || b == NULL) {
        free(a); free(b); wav_close(&near); wav_close(&far);
        return false;
    }

    s_seg_frames = 0;
    while (s_seg_frames < SEG_MAX_FRAMES) {
        if (wav_read(&near, a, (size_t)chunk) < (size_t)chunk) break;
        if (wav_read(&far,  b, (size_t)chunk) < (size_t)chunk) break;
        double ne = 0.0, fe = 0.0;
        for (int i = 0; i < chunk; i++) {
            ne += (double)a[i] * a[i];
            fe += (double)b[i] * b[i];
        }
        s_near_rms[s_seg_frames] = (float)sqrt(ne / chunk);
        s_far_rms[s_seg_frames]  = (float)sqrt(fe / chunk);
        s_seg_frames++;
        /* This pass blocks app_main too, and tripped the watchdog once for the
         * same reason the mode loops did. */
        if ((s_seg_frames % 50) == 0) {
            vTaskDelay(1);
        }
    }
    free(a); free(b);
    wav_close(&near); wav_close(&far);

    if (s_seg_frames < 20) {
        return false;
    }

    /* Far-active floor: 5% of the loudest far frame. */
    float far_max = 0.0f;
    for (size_t i = 0; i < s_seg_frames; i++) {
        if (s_far_rms[i] > far_max) far_max = s_far_rms[i];
    }
    const float far_floor = far_max * 0.05f;

    /* The echo path's own gain, as the 20th percentile of near/far. */
    float *ratios = heap_caps_malloc(s_seg_frames * sizeof(float), MALLOC_CAP_SPIRAM);
    if (ratios == NULL) {
        return false;
    }
    size_t nr = 0;
    for (size_t i = 0; i < s_seg_frames; i++) {
        if (s_far_rms[i] > far_floor) {
            ratios[nr++] = s_near_rms[i] / s_far_rms[i];
        }
    }
    if (nr < 10) { free(ratios); return false; }
    qsort(ratios, nr, sizeof(float), cmp_float);
    const float g = ratios[nr / 5];
    free(ratios);

    size_t n_echo = 0;
    for (size_t i = 0; i < s_seg_frames; i++) {
        s_echo_only[i] = (s_far_rms[i] > far_floor) &&
                         (s_near_rms[i] < g * s_far_rms[i] * 2.0f);
        if (s_echo_only[i]) n_echo++;
    }

    ESP_LOGI(TAG, "segmented: %u frames, %u far-active, %u echo-only (%.0f%%), "
             "echo-path gain g=%.4f (ERL %.1f dB)",
             (unsigned)s_seg_frames, (unsigned)nr, (unsigned)n_echo,
             100.0 * n_echo / (double)s_seg_frames, g, -20.0 * log10(g));
    return n_echo >= 10;
}

/* ---------------- one mode ---------------- */

static void bench_mode(const char *label, aec_mode_t mode, uint32_t caps)
{
    wav_t near, far;
    if (!wav_open(&near, "aec_in_near.wav")) {
        return;
    }
    if (!wav_open(&far, "aec_in_far.wav")) {
        wav_close(&near);
        return;
    }

    heap_snap_t before = heap_now();

    aec_config_t cfg = {
        .mic_num       = 1,
        .ref_num       = 1,
        .out_num       = 1,
        .filter_length = 4,
        .sample_rate   = 16000,
        .caps          = caps,
        .mode          = mode,
        .nlp_level     = AEC_NLP_LEVEL_AGGR,
    };
    aec_handle_t *h = aec_create_from_config(&cfg);
    if (h == NULL) {
        ESP_LOGE(TAG, "%s: aec_create_from_config returned NULL", label);
        wav_close(&near); wav_close(&far);
        return;
    }

    heap_snap_t after = heap_now();
    int chunk = aec_get_chunksize(h);

    ESP_LOGI(TAG, "%s: chunk=%d handle.frame_size=%d | internal %+d B, largest %u->%u, "
             "spiram %+d B",
             label, chunk, h->frame_size,
             (int)after.internal - (int)before.internal,
             (unsigned)before.largest, (unsigned)after.largest,
             (int)after.spiram - (int)before.spiram);

    if (chunk <= 0) {
        ESP_LOGE(TAG, "%s: bad chunksize", label);
        aec_destroy(h);
        wav_close(&near); wav_close(&far);
        return;
    }

    size_t bytes = (size_t)chunk * sizeof(int16_t);
    int16_t *in  = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *ref = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (in == NULL || ref == NULL || out == NULL) {
        ESP_LOGE(TAG, "%s: no aligned memory for %d-sample frames", label, chunk);
        free(in); free(ref); free(out);
        aec_destroy(h);
        wav_close(&near); wav_close(&far);
        return;
    }

    /*
     * Does this mode actually have NLP? aec_nlp_process() documents "or 0 if NLP
     * is not applied", which answers it at runtime rather than from a page that
     * says FD-only and a header that recommends SR. One frame through the split
     * path, before the real run, then the filter is reset by rewinding anyway.
     */
    if (wav_read(&near, in, (size_t)chunk) == (size_t)chunk &&
        wav_read(&far, ref, (size_t)chunk) == (size_t)chunk) {
        aec_linear_process(h, in, ref, out);
        int nlp = aec_nlp_process(h, out);
        ESP_LOGI(TAG, "%s: aec_nlp_process -> %d (%s)", label, nlp,
                 nlp > 0 ? "NLP active" : "NLP NOT applied in this mode");
    }
    wav_rewind(&near);
    wav_rewind(&far);

    double ne = 0.0, oe = 0.0, se_n = 0.0, se_o = 0.0;
    size_t frame = 0, se_frames = 0;
    const size_t skip = (size_t)(SKIP_SECONDS * 16000) / (size_t)chunk;
    for (;;) {
        size_t nn = wav_read(&near, in, (size_t)chunk);
        size_t nf = wav_read(&far, ref, (size_t)chunk);
        if (nn < (size_t)chunk || nf < (size_t)chunk) {
            break;   /* partial frames would be fed as silence; stop instead */
        }
        aec_process(h, in, ref, out);
        /* ~53 s of audio per mode, five modes, all on app_main. Without this the
         * task watchdog fires -- observed, not theorised. Every 50 frames is
         * 1.6 s of audio, so the cost to the measurement is nil. */
        if ((frame % 50) == 0) {
            vTaskDelay(1);
        }
        if (frame >= skip) {
            for (int i = 0; i < chunk; i++) {
                ne += (double)in[i] * in[i];
                oe += (double)out[i] * out[i];
            }
            /* The real ERLE: single-talk frames only. The filter still ran on
             * every frame -- only the accounting is selective. */
            if (frame < s_seg_frames && s_echo_only[frame]) {
                for (int i = 0; i < chunk; i++) {
                    se_n += (double)in[i] * in[i];
                    se_o += (double)out[i] * out[i];
                }
                se_frames++;
            }
        }
        frame++;
    }

    ESP_LOGI(TAG, "%s: RESULT erle=%.1f dB over %u echo-only frames "
             "| whole-file energy drop %.1f dB over %u frames",
             label, erle_db(se_n, se_o), (unsigned)se_frames,
             erle_db(ne, oe), (unsigned)frame);

    free(in); free(ref); free(out);
    aec_destroy(h);

    heap_snap_t freed = heap_now();
    int leak = (int)before.internal - (int)freed.internal;
    ESP_LOGI(TAG, "%s: after destroy, internal leak=%d B spiram leak=%d B",
             label, leak, (int)before.spiram - (int)freed.spiram);

    wav_close(&near);
    wav_close(&far);
}

/* ---------------- entry ---------------- */

void aec_bench_run(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = VEC_ROOT,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no vectors: spiffs mount failed (%s). Run `idf.py storage-flash`.",
                 esp_err_to_name(err));
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "vectors mounted: %u of %u bytes used", (unsigned)used, (unsigned)total);
    }

    s_far_rms   = heap_caps_malloc(SEG_MAX_FRAMES * sizeof(float), MALLOC_CAP_SPIRAM);
    s_near_rms  = heap_caps_malloc(SEG_MAX_FRAMES * sizeof(float), MALLOC_CAP_SPIRAM);
    s_echo_only = heap_caps_malloc(SEG_MAX_FRAMES * sizeof(bool),  MALLOC_CAP_SPIRAM);
    if (s_far_rms == NULL || s_near_rms == NULL || s_echo_only == NULL) {
        ESP_LOGE(TAG, "no PSRAM for segmentation");
        goto done;
    }
    /* chunk is 512 for every mode on this chip -- confirmed by aec_get_chunksize()
     * and handle.frame_size in the first run. Segment once, reuse for all modes. */
    if (!segment_vectors(512)) {
        ESP_LOGE(TAG, "segmentation failed; erle figures would be meaningless");
        goto done;
    }

    heap_snap_t base = heap_now();
    ESP_LOGI(TAG, "baseline: internal=%u largest=%u spiram=%u",
             (unsigned)base.internal, (unsigned)base.largest, (unsigned)base.spiram);

    /* Espressif's recommendation first, then the mode this project committed to,
     * then the rest of the family so the published table can be checked. */
    bench_mode("FD_LOW_COST",   AEC_MODE_FD_LOW_COST,   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bench_mode("FD_HIGH_PERF",  AEC_MODE_FD_HIGH_PERF,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bench_mode("SR_HIGH_PERF",  AEC_MODE_SR_HIGH_PERF,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bench_mode("SR_LOW_COST",   AEC_MODE_SR_LOW_COST,   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* Is `caps` honoured at all? Same mode, default allocation, compare the
     * internal delta. AEC-FINDINGS.md carries a claim that only SR_HIGH_PERF
     * respects it, sourced from a disassembly nobody here has checked. */
    bench_mode("FD_LOW_COST/default-caps", AEC_MODE_FD_LOW_COST, MALLOC_CAP_DEFAULT);

    /*
     * FD only. All four vectors are 6.8 MB, which is 93% of the 7 MB partition and
     * more than SPIFFS will accept -- spiffsgen fails outright with "the image
     * size has been exceeded". Three files is 70% and fits. FD is the one that
     * matters, being both Espressif's recommendation and the full-duplex case
     * this device actually has; to check SR against its reference, swap
     * aec_test_sr.wav in for aec_test_fd.wav and reflash the storage image.
     */
    bench_reference("aec_test_fd.wav");

done:
    free(s_far_rms); free(s_near_rms); free(s_echo_only);
    s_far_rms = NULL; s_near_rms = NULL; s_echo_only = NULL;
    esp_vfs_spiffs_unregister("storage");
    ESP_LOGI(TAG, "bench done");
}
