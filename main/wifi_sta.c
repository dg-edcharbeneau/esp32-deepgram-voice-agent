#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "net_probe.h"
#include "wifi_sta.h"
#include "wifi_wedge.h"

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

/*
 * WHAT THE RETRY BUDGET IS FOR, AND WHAT IT IS NOT.
 *
 * CONFIG_WIFI_MAX_RETRY exists for the boot-time join: a saved network that
 * cannot be reached at all -- renamed, moved, a changed password -- has to end
 * in the setup portal rather than in endless retries, and main.c's wait is
 * what turns the exhausted budget into the portal.
 *
 * Once this station has had an address, losing the network means something
 * else entirely: a router rebooting, a walk out of range, an access point
 * switched off for a minute. Nothing waits on the budget any more, so spending
 * it used to strand the device -- eight tries in about twenty seconds, then
 * silence until someone power-cycled it. Now the first CONFIG_WIFI_MAX_RETRY
 * tries still go back to back, for the blip, and after that it keeps trying
 * for as long as the network is gone, backing off to one attempt every
 * REJOIN_MAX_MS: a scan is not free on a battery, and a router takes a minute
 * or two to come back anyway.
 */
static bool s_joined;                  /* had an address since wifi_sta_start() */
static volatile bool s_associated;     /* associated right now */
static volatile uint32_t s_gen;        /* bumped by start and stop: stale rejoins drop */
static TaskHandle_t s_rejoin_task;
static volatile uint32_t s_rejoin_ms;
static volatile uint32_t s_rejoin_gen;

#define REJOIN_FIRST_MS 5000
#define REJOIN_MAX_MS   30000

/*
 * The delayed attempts run here, not on a timer: on a board whose radio is a
 * co-processor (the P4's C6, over esp_hosted) esp_wifi_connect() is an RPC that
 * blocks its caller, and neither the esp_timer task nor the event loop should
 * be the one waiting on it.
 */
static void rejoin_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            const uint32_t gen = s_rejoin_gen;
            vTaskDelay(pdMS_TO_TICKS(s_rejoin_ms));
            if (!s_connecting || gen != s_gen || s_associated) {
                break;   /* stopped, restarted, or back by itself meanwhile */
            }
            const esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                break;   /* the attempt's own disconnect, if it fails, schedules the next */
            }
            /* No event will follow a refused call, so the chain would end
             * here: try again after the same wait. */
            ESP_LOGW(TAG, "rejoin: esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
}

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
     *
     * Not even asked while unassociated: through esp_hosted (the P4) the
     * refusal comes back as an RPC, and esp_hosted logs every refused RPC as a
     * warning -- one line a second for as long as the network is gone.
     */
    if (!s_associated || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
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

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_associated = true;
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_associated = false;
        if (!s_connecting) {
            /* Expected while provisioning, and while shutting the station down
             * to hand the radio over. Nothing to retry. */
            return;
        }
        /* The P4's wedge watchdog reads the reasons: a long run of "the AP
         * is there and will not have us" is the C6 wedged, not the network
         * gone. See wifi_wedge.h. */
        wifi_wedge_note_disconnect(ev->reason);
        if (s_retries < CONFIG_WIFI_MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     ev->reason, s_retries, CONFIG_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else if (s_joined && s_rejoin_task != NULL) {
            /* Had a network and lost it: keep at it, further apart (see
             * s_joined). 5, 10, 20, then every 30 s. */
            const int over = s_retries - CONFIG_WIFI_MAX_RETRY;
            s_retries++;
            uint32_t ms = REJOIN_FIRST_MS << (over < 3 ? over : 3);
            if (ms > REJOIN_MAX_MS) {
                ms = REJOIN_MAX_MS;
            }
            if (over == 0) {
                ESP_LOGW(TAG, "EVT wifi lost, still trying every %d s at most",
                         REJOIN_MAX_MS / 1000);
            }
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d in %lu s",
                     ev->reason, s_retries, (unsigned long)(ms / 1000));
            s_rejoin_ms = ms;
            s_rejoin_gen = s_gen;
            xTaskNotifyGive(s_rejoin_task);
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
        if (s_joined && s_retries > 0) {
            ESP_LOGI(TAG, "EVT wifi back after %d attempts", s_retries);
        }
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_associated = true;
        s_joined = true;
        s_retries = 0;
        wifi_wedge_note_got_ip();
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

    /* Small and idle: it only waits out a delay and asks for a connect. If it
     * cannot be had, a lost network is simply not retried, as before. */
    if (xTaskCreate(rejoin_task, "wifi_rejoin", 3072, NULL, 4, &s_rejoin_task) != pdPASS) {
        s_rejoin_task = NULL;
        ESP_LOGW(TAG, "no rejoin task: a lost network will not be retried");
    }

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
            /*
             * THE STRONGEST ACCESS POINT, NOT THE FIRST.
             *
             * The driver's default is a fast scan: channels 1 to 13 in order,
             * joining the first AP with the right SSID. On a network with two
             * APs under one name that is the lower channel, however far away
             * -- and access points move channel on their own. On 2026-11-19
             * the bench's far AP moved to channel 9, below the near one's 11,
             * and every join went to it at -80 dBm, where the uplink died
             * seconds into each session. A full scan costs about a second on
             * a join that already takes several. Reaches the P4's C6 too:
             * esp_hosted forwards both fields (rpc_req.c).
             */
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            /* Under max modem sleep the radio wakes for every third beacon
             * (~300 ms). 3 is also the driver's value for 0; named here because
             * deep idle's light sleep is paced by it. */
            .listen_interval = 3,
        },
    };
    /* strncpy, not assignment: the config fields are fixed-size uint8_t arrays. */
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (!open) {
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    }

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_retries = 0;
    s_joined = false;
    s_gen++;
    s_connecting = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
#if CONFIG_ESP_HOSTED_ENABLED
    {
        /* The C6's protocol, set explicitly either way: its Wi-Fi driver may
         * keep the last setting in its own flash, so leaving it alone would not
         * mean 802.11ax. On 802.11ax the factory C6 loses transmit for hours at
         * a time; b/g/n doesn't (A/B in the Kconfig help). Before start, so the first
         * join already uses it; read back because a C6 may ignore it. */
        uint8_t want = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N
#if !CONFIG_WIFI_STA_NO_11AX
                     | WIFI_PROTOCOL_11AX
#endif
                     ;
        /* A bench "proto" before a software reset wins (bench builds only). */
        const uint8_t bench = net_probe_boot_proto();
        if (bench) {
            want = bench;
        }
        uint8_t got = 0;
        esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, want);
        esp_err_t rerr = esp_wifi_get_protocol(WIFI_IF_STA, &got);
        ESP_LOGW(TAG, "EVT wifi protocol set 0x%02x%s: %s, read back 0x%02x (%s)%s",
                 want, bench ? " (bench proto)" : "", esp_err_to_name(err), got, esp_err_to_name(rerr),
                 (rerr == ESP_OK && got != want) ? " -- MISMATCH" : "");
    }
#endif
    /*
     * No modem sleep: it adds tens of ms of jitter to a live audio stream.
     *
     * That is a SESSION-TIME requirement, not an always-on one, and for most of
     * this device's life there is no stream to protect -- 88% of it is spent with
     * the session stopped. wifi_sta_set_power_save_mode() below is what lets the
     * sleep state take advantage of that; this call is the awake default.
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
    s_joined = false;
    s_associated = false;
    s_gen++;   /* a rejoin already waiting out its delay is dropped */
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
 * Modem sleep by level, for the radio policy in main.c and session_ctl.c.
 *
 * Kept here rather than calling esp_wifi_set_ps() from the caller so that Wi-Fi
 * policy lives in one file and the reasoning above sits next to every setting.
 *
 * Either modem sleep keeps the station associated and listening for beacons,
 * so waking costs at most a listen interval (~300 ms at MAX_MODEM) rather than
 * a re-association. That is invisible behind the 1.1-6.0 s the Deepgram
 * handshake takes -- see the CONNECTING note in main.c.
 *
 * Failure is logged and swallowed: a device that will not modem-sleep should
 * carry on running warm, not refuse to work.
 */
static volatile int s_ps_mode = WIFI_STA_PS_NONE;

void wifi_sta_set_power_save_mode(int mode)
{
    static const wifi_ps_type_t PS_TYPE[] = {WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM};
    static const char *const NAME[] = {"NONE", "MIN_MODEM", "MAX_MODEM"};
    if (mode < WIFI_STA_PS_NONE || mode > WIFI_STA_PS_MAX) {
        return;
    }
    esp_err_t err = esp_wifi_set_ps(PS_TYPE[mode]);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(%s): %s", NAME[mode], esp_err_to_name(err));
        return;
    }
    if (mode != s_ps_mode) {
        s_ps_mode = mode;
        /* Logged on success too: the power audit reads the radio's state off the
         * log, and a change that only logs when it fails is invisible. */
        ESP_LOGI(TAG, "EVT wifi ps %s", NAME[mode]);
    }
}

int wifi_sta_power_save_mode(void)
{
    return s_ps_mode;
}
