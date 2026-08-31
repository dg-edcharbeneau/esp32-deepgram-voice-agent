#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi_sta.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static int s_retries;
static bool s_stack_ready;

/*
 * Whether we are meant to be associating right now.
 *
 * The driver raises STA_START and STA_DISCONNECTED in AP and APSTA mode too, so
 * without this flag the provisioning portal would be constantly interrupted by
 * a reconnect attempt aimed at the network that just failed.
 */
static bool s_connecting;

#if CONFIG_WIFI_SIGNAL
/* ------------------------------------------------------------- signal
 *
 * WHERE THE BUCKET BOUNDARIES COME FROM
 *
 * These are the conventional 2.4 GHz reception bands, not numbers tuned on this
 * board: -55 and better is as good as the radio gets and the panel should show
 * a full row, -67 is the floor for comfortable streaming, -75 still works with
 * retries, -85 is the edge of association. A number in dBm is meaningless to
 * everyone except a log reader, so this is the only place it is interpreted.
 */
static const int8_t SIGNAL_FLOOR_DBM[4] = { -85, -75, -67, -55 };  /* index = bars-1 */

/*
 * How far a reading has to clear a floor before it is allowed to PROMOTE.
 *
 * Asymmetric on purpose. Losing a bar is reported the instant the reading falls
 * below the floor, because a link that has genuinely gone bad is what the row
 * exists to warn about and delaying that warning is the one failure mode with a
 * cost. Gaining one waits for 3 dB of margin, so a single lucky beacon at the
 * boundary cannot flip the row back and forth while the device sits still.
 */
#define SIGNAL_HYST_DB 3

/*
 * Last bucket reported, which is what makes the hysteresis above possible.
 *
 * Written only from wifi_sta_get_signal() and cleared in wifi_sta_stop(). A
 * torn read costs one frame of one wrong bar count and cannot corrupt anything,
 * so this carries no lock: taking one here would mean a display callback could
 * block on a Wi-Fi event, which is a far worse trade than a stale bar.
 */
static uint8_t s_bars;

/*
 * Whether the weak-signal crossing has already been logged for this excursion.
 *
 * The driver's threshold is one-shot and has to be re-armed (see
 * signal_arm_threshold), but re-arming while the link is STILL below it makes it
 * fire again within about a beacon interval -- measured at roughly one line per
 * second for as long as a hand stayed over the antenna, 13 lines for one
 * ten-second dip. The point of the warning is to put a cause in the log
 * immediately before the dropped audio that follows, and one line does that; a
 * line a second buries the TLM output at the exact moment it is worth reading.
 *
 * So the threshold is always re-armed and the LOG is edge-triggered instead.
 * Cleared by wifi_sta_get_signal() once the reading has recovered, which makes
 * the 1 Hz reader the observer of "has this excursion ended" -- it is already
 * sampling on the cadence that question wants.
 */
static bool s_weak_logged;

static uint8_t bars_from_rssi(int8_t rssi, uint8_t prev)
{
    /* Highest bucket first: the answer is the first floor the reading clears. */
    for (int i = 3; i >= 0; i--) {
        int floor_dbm = SIGNAL_FLOOR_DBM[i];
        if ((uint8_t)(i + 1) > prev) {
            floor_dbm += SIGNAL_HYST_DB;
        }
        if (rssi >= floor_dbm) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

bool wifi_sta_get_signal(wifi_signal_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    wifi_ap_record_t ap;
    /*
     * NOT_CONNECT while unassociated, CONN while the STA interface does not
     * exist at all -- which is the whole time the provisioning AP owns the
     * radio. Neither is logged: the telemetry loop calls this once a second and
     * a device sitting in the portal would fill the log with it.
     */
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        s_bars = 0;
        return false;
    }

    s_bars = bars_from_rssi(ap.rssi, s_bars);

    /* Recovered, so the next crossing is a new excursion and worth a line. The
     * same hysteresis the buckets use, for the same reason: a reading sitting on
     * the threshold must not re-open the excursion once per sample. */
    if (ap.rssi >= CONFIG_WIFI_SIGNAL_WEAK_DBM + SIGNAL_HYST_DB) {
        s_weak_logged = false;
    }

    out->valid   = true;
    out->rssi    = ap.rssi;
    out->bars    = s_bars;
    out->weak    = (s_bars <= 1);
    out->channel = ap.primary;
    return true;
}

/*
 * Arms the driver's own RSSI watchdog.
 *
 * ONE-SHOT. The driver disarms the threshold as it fires, so this has to be
 * called again from the handler -- without that you get exactly one warning per
 * association and a device that has been slowly walking out of range for an
 * hour looks fine in the log.
 *
 * Failure is logged and swallowed. The threshold is a diagnostic nicety; the
 * TLM line still carries the same number once a second.
 */
static void signal_arm_threshold(void)
{
    esp_err_t err = esp_wifi_set_rssi_threshold(CONFIG_WIFI_SIGNAL_WEAK_DBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_rssi_threshold: %s", esp_err_to_name(err));
    }
}
#else /* !CONFIG_WIFI_SIGNAL */

/* Off means absent: no event handler, no code. The empty definition keeps every
 * call site unconditional, the way battery.c does it. */
bool wifi_sta_get_signal(wifi_signal_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

#endif /* CONFIG_WIFI_SIGNAL */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_connecting) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        if (!s_connecting) {
            /* Expected while provisioning, and while shutting the station down
             * to hand the radio over. Nothing to retry. */
            return;
        }
        if (s_retries < CONFIG_WIFI_MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     ev->reason, s_retries, CONFIG_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up after %d attempts (last reason %d)",
                     s_retries, ev->reason);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
        return;
    }

#if CONFIG_WIFI_SIGNAL
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
        const wifi_event_bss_rssi_low_t *ev = data;
        if (!s_weak_logged) {
            s_weak_logged = true;
            ESP_LOGW(TAG, "EVT wifi weak rssi=%d thresh=%d",
                     (int)ev->rssi, CONFIG_WIFI_SIGNAL_WEAK_DBM);
        }
        /* Re-armed even when the line was suppressed: the arming is what keeps
         * the NEXT excursion detectable, and it is not what was noisy. */
        signal_arm_threshold();   /* one-shot -- see the note there */
        return;
    }
#endif

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
#if CONFIG_WIFI_SIGNAL
        /* Here rather than in wifi_sta_start(): the threshold is a property of
         * an association, and the driver rejects it before there is one. */
        signal_arm_threshold();
#endif
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        return;
    }
}

esp_err_t wifi_stack_init(void)
{
    if (s_stack_ready) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    s_stack_ready = true;
    return ESP_OK;
}

esp_err_t wifi_sta_start(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(wifi_stack_init());

    const bool open = (pass == NULL || pass[0] == '\0');

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    /* strncpy, not assignment: the config fields are fixed-size uint8_t arrays. */
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (!open) {
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    }

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retries = 0;
    s_connecting = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    /*
     * No modem sleep: it adds tens of ms of jitter to a live audio stream.
     *
     * That is a SESSION-TIME requirement, not an always-on one, and for most of
     * this device's life there is no stream to protect -- 88% of it is spent with
     * the session stopped. wifi_sta_set_power_save() below is what lets the sleep
     * state take advantage of that; this call is the awake default.
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

esp_err_t wifi_sta_wait_connected(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                          WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_stop(void)
{
    /* Before esp_wifi_stop(), so the disconnect it provokes is not retried. */
    s_connecting = false;
    s_retries = 0;
#if CONFIG_WIFI_SIGNAL
    /* So a reconnect -- possibly to a different AP entirely -- does not inherit
     * the bucket of the link that just went away, and get promoted out of it. */
    s_bars = 0;
    s_weak_logged = false;
#endif

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    return err;
}

/*
 * Modem sleep on or off, for the sleep state in main.c.
 *
 * Kept here rather than calling esp_wifi_set_ps() from the caller so that Wi-Fi
 * policy lives in one file and the reasoning above sits next to both settings.
 *
 * MIN_MODEM rather than MAX_MODEM: the station stays associated and keeps
 * listening for beacons, so waking costs about one beacon interval (~100 ms)
 * rather than a re-association. That is invisible behind the 1.1-6.0 s the
 * Deepgram handshake takes -- see the CONNECTING note in main.c.
 *
 * Failure is logged and swallowed: a device that will not modem-sleep should
 * carry on running warm, not refuse to work.
 */
void wifi_sta_set_power_save(bool enabled)
{
    esp_err_t err = esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(%s): %s",
                 enabled ? "MIN_MODEM" : "NONE", esp_err_to_name(err));
    }
}
