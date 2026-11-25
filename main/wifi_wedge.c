/* See wifi_wedge.h. Compiled to nothing without CONFIG_WIFI_WEDGE_RESET. */
#include "sdkconfig.h"

#if CONFIG_WIFI_WEDGE_RESET

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

#include "wifi_wedge.h"

static const char *TAG = "wifi";

/* How often the task wakes to look at the refusal run and the check clock. */
#define TICK_MS            10000
/* A failed check is re-asked this soon, rather than a full interval later. */
#define RECHECK_MS         10000
/* Failed checks in a row that make the link dead. */
#define DEAD_AFTER         2
/* A websocket error asks for a check; a flurry of them asks for one. */
#define CHECK_NOW_GAP_MS   10000
#define PINGS              3
/* The n-th reset in a row waits min(5 min << (n-1), 60 min) of uptime. */
#define BACKOFF_BASE_MS    (5u * 60u * 1000u)
#define BACKOFF_MAX_MS     (60u * 60u * 1000u)
/* Connected and passing this long, and the run of resets is over. */
#define HEALTHY_CLEAR_MS   (30u * 60u * 1000u)

enum { WHY_NONE = 0, WHY_DEAD = 1, WHY_REFUSE = 2 };
static const char *const WHY_NAME[] = { "none", "dead", "refuse" };

/*
 * THE RESET COUNT HAS TO OUTLIVE THE RESET IT COUNTS.
 *
 * Otherwise the backoff is meaningless: every boot would think it was the
 * first. RTC memory is not touched by a software reset or by the bootloader,
 * and holds garbage after a power-on -- which is what the magic word and the
 * check word are for, together with the reset reason.
 */
#define RTC_MAGIC 0x57454447u   /* "WEDG" */
typedef struct {
    uint32_t magic;
    uint32_t resets;
    uint32_t pending_why;   /* set just before a wedge reset, cleared by the boot after */
    uint32_t check;
} wedge_rtc_t;
static RTC_NOINIT_ATTR wedge_rtc_t s_rtc;

static uint32_t rtc_check(const wedge_rtc_t *r)
{
    return r->magic ^ r->resets ^ r->pending_why ^ 0xA5A5A5A5u;
}

static void rtc_store(uint32_t resets, uint32_t pending_why)
{
    s_rtc.magic = RTC_MAGIC;
    s_rtc.resets = resets;
    s_rtc.pending_why = pending_why;
    s_rtc.check = rtc_check(&s_rtc);
}

static TaskHandle_t s_task;
static volatile int s_resets;

/* Written from the Wi-Fi event task, read here. Single words; a stale read
 * costs one tick of lateness and nothing else. */
static volatile bool s_have_ip;
static volatile uint32_t s_ip_since_ms;
static volatile bool s_refusing;
static volatile uint32_t s_refuse_since_ms;
static volatile int s_refuse_reason;
static volatile bool s_new_assoc;        /* GOT_IP since the task last looked */
/* Bumped per GOT_IP; a session that reached its server records the one it ran
 * on. Equal means this association has carried real traffic. */
static volatile uint32_t s_assoc_gen;
static volatile uint32_t s_proven_gen;

static const char *volatile s_check_why;
static volatile int s_inject;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- events ---- */

/* The reasons that mean nobody answered at all: the router is off or out of
 * range. Those are what the reconnect loop's patience is for, not a reset. */
static bool reason_no_ap(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return true;
    default:
        return false;
    }
}

void wifi_wedge_note_disconnect(int reason)
{
    s_have_ip = false;
    if (reason_no_ap(reason)) {
        s_refusing = false;
        return;
    }
    s_refuse_reason = reason;
    if (!s_refusing) {
        s_refuse_since_ms = now_ms();
        s_refusing = true;
    }
}

void wifi_wedge_note_got_ip(void)
{
    s_refusing = false;
    s_ip_since_ms = now_ms();
    s_assoc_gen++;
    s_new_assoc = true;
    s_have_ip = true;
}

void wifi_wedge_note_link_ok(void)
{
    s_proven_gen = s_assoc_gen;
}

void wifi_wedge_check_now(const char *why)
{
    if (!s_task || s_check_why) return;
    s_check_why = why;
    xTaskNotifyGive(s_task);
}

int wifi_wedge_resets(void)
{
    return s_resets;
}

#if CONFIG_BENCH_NET_PROBE
void wifi_wedge_inject(int what)
{
    s_inject = what;
    ESP_LOGW(TAG, "EVT wifi wedge inject %s",
             what == WIFI_WEDGE_INJECT_DEAD ? "dead" :
             what == WIFI_WEDGE_INJECT_REFUSE ? "refuse" : "off");
    if (s_task) xTaskNotifyGive(s_task);
}
#endif

/* ---- the gateway check ---- */

typedef struct {
    SemaphoreHandle_t done;
    uint32_t replies;
} ping_result_t;

static void on_ping_success(esp_ping_handle_t hdl, void *arg)
{
    (void)hdl;
    ((ping_result_t *)arg)->replies++;
}

static void on_ping_end(esp_ping_handle_t hdl, void *arg)
{
    (void)hdl;
    xSemaphoreGive(((ping_result_t *)arg)->done);
}

/* Replies out of PINGS from the station's gateway; -1 if it could not ask
 * (no gateway, no session), which is never taken as a failure. */
static int ping_gateway(void)
{
    if (s_inject == WIFI_WEDGE_INJECT_DEAD) return 0;

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.gw.addr == 0) return -1;

    ping_result_t r = { .done = xSemaphoreCreateBinary() };
    if (!r.done) return -1;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = (ip_addr_t)IPADDR4_INIT(ip.gw.addr);
    cfg.count = PINGS;
    cfg.interval_ms = 200;
    cfg.timeout_ms = 1000;
    cfg.task_stack_size = 3072;

    const esp_ping_callbacks_t cbs = {
        .cb_args = &r,
        .on_ping_success = on_ping_success,
        .on_ping_end = on_ping_end,
    };
    esp_ping_handle_t hdl = NULL;
    int out = -1;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) == ESP_OK && esp_ping_start(hdl) == ESP_OK) {
        if (xSemaphoreTake(r.done, pdMS_TO_TICKS(PINGS * 1300 + 2000)) == pdTRUE) {
            out = (int)r.replies;
        }
        esp_ping_stop(hdl);
    }
    if (hdl) esp_ping_delete_session(hdl);
    vSemaphoreDelete(r.done);
    return out;
}

/* ---- the reset ---- */

static uint32_t backoff_ms(int resets)
{
    if (resets <= 0) return 0;
    if (resets > 5) return BACKOFF_MAX_MS;
    const uint32_t ms = BACKOFF_BASE_MS << (resets - 1);
    return ms > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : ms;
}

/* Resets both chips, unless the backoff says not yet; then it returns, and the
 * caller's condition, still true at the next tick, brings it back here. */
static void wedge_reset(int why, bool *held_logged)
{
    const uint32_t wait = backoff_ms(s_resets);
    const uint32_t up = now_ms();
    if (up < wait) {
        if (!*held_logged) {
            *held_logged = true;
            ESP_LOGW(TAG, "EVT wifi wedge held why=%s n=%d for %lu s",
                     WHY_NAME[why], s_resets, (unsigned long)((wait - up) / 1000));
        }
        return;
    }
    ESP_LOGE(TAG, "EVT wifi wedge reset why=%s n=%d", WHY_NAME[why], s_resets);
    rtc_store((uint32_t)s_resets + 1, (uint32_t)why);
    vTaskDelay(pdMS_TO_TICKS(100));          /* let the line out */
    /* The ROM reset, not esp_restart(): on the P4 the latter can leave the
     * SDMMC DMA running into the next boot (esp-hosted-mcu #240). Boot pulses
     * the C6's reset line, so this resets both chips -- the recovery that
     * cleared the wedge on the bench. */
    esp_rom_software_reset_system();
}

/* ---- the task ---- */

static void wedge_task(void *arg)
{
    (void)arg;
    int fails = 0;
    bool gw_answered = false;      /* this association: is the check meaningful */
    bool gw_silent_logged = false;
    bool held_logged = false;
    uint32_t last_check_ms = now_ms();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        const uint32_t now = now_ms();

        if (s_new_assoc) {
            s_new_assoc = false;
            fails = 0;
            gw_answered = false;
            gw_silent_logged = false;
            held_logged = false;
        }

        /* ---- refuse ---- */
        if (s_inject == WIFI_WEDGE_INJECT_REFUSE && !s_refusing) {
            s_refuse_reason = WIFI_REASON_AUTH_EXPIRE;
            s_refuse_since_ms = now;
            s_refusing = true;
        }
        if (s_refusing &&
            now - s_refuse_since_ms >= (uint32_t)CONFIG_WIFI_WEDGE_REFUSE_S * 1000u) {
            ESP_LOGW(TAG, "EVT wifi wedge refused for %lu s (last reason %d)",
                     (unsigned long)((now - s_refuse_since_ms) / 1000), s_refuse_reason);
            wedge_reset(WHY_REFUSE, &held_logged);
        }

        /* ---- dead ---- */
        if (!s_have_ip) {
            fails = 0;
            s_check_why = NULL;
            continue;
        }
        if (!gw_answered && s_proven_gen == s_assoc_gen && s_assoc_gen != 0) {
            /* The session reached its server on this association, so the
             * path worked: a gateway that stops answering now is news even if
             * it never answered a ping yet -- the 2026-11-19 failures went
             * dead seconds after joining, before the first check ran. */
            gw_answered = true;
            ESP_LOGI(TAG, "EVT wifi wedge check armed: session reached its server");
        }
        const char *why = s_check_why;
        const uint32_t interval = fails ? RECHECK_MS : (uint32_t)CONFIG_WIFI_WEDGE_CHECK_S * 1000u;
        const bool asked = why && now - last_check_ms >= CHECK_NOW_GAP_MS;
        s_check_why = NULL;
        if (asked || now - last_check_ms >= interval) {
            last_check_ms = now;

            const int replies = ping_gateway();
            if (replies > 0) {
                if (fails) {
                    ESP_LOGI(TAG, "EVT wifi wedge check ok %d/%d after %d failed", replies, PINGS, fails);
                }
                fails = 0;
                if (!gw_answered) {
                    gw_answered = true;
                    ESP_LOGI(TAG, "EVT wifi wedge check armed: gateway answered %d/%d", replies, PINGS);
                }
                held_logged = false;
            } else if (replies == 0 && gw_answered) {
                fails++;
                if (fails <= DEAD_AFTER) {
                    ESP_LOGW(TAG, "EVT wifi wedge check failed 0/%d (%s) %d/%d",
                             PINGS, asked ? why : "timer", fails, DEAD_AFTER);
                }
                if (fails >= DEAD_AFTER) {
                    wedge_reset(WHY_DEAD, &held_logged);
                }
            } else if (replies == 0 && !gw_silent_logged) {
                gw_silent_logged = true;
                ESP_LOGW(TAG, "EVT wifi wedge gateway has not answered a ping yet; "
                         "failures not counted until it does");
            }
        }

        if (s_resets && fails == 0 && s_have_ip && now - s_ip_since_ms >= HEALTHY_CLEAR_MS) {
            ESP_LOGI(TAG, "EVT wifi wedge healthy %lu min, reset count %d cleared",
                     (unsigned long)(HEALTHY_CLEAR_MS / 60000u), s_resets);
            s_resets = 0;
            rtc_store(0, WHY_NONE);
        }
    }
}

bool wifi_wedge_init(void)
{
    const bool valid = s_rtc.magic == RTC_MAGIC && s_rtc.check == rtc_check(&s_rtc) &&
                       esp_reset_reason() != ESP_RST_POWERON;
    if (valid) {
        s_resets = (int)s_rtc.resets;
        const uint32_t why = s_rtc.pending_why;
        if (why == WHY_DEAD || why == WHY_REFUSE) {
            /* Called after the first join, so "back" is already true. */
            ESP_LOGW(TAG, "EVT wifi wedge rebooted why=%s n=%d, network back %lu ms after boot",
                     WHY_NAME[why], s_resets, (unsigned long)now_ms());
        }
        rtc_store((uint32_t)s_resets, WHY_NONE);
    } else {
        s_resets = 0;
        rtc_store(0, WHY_NONE);
    }

    if (xTaskCreate(wedge_task, "wifi_wedge", 3072, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "no wedge watchdog task: a wedged C6 will not be reset");
        return false;
    }
    return true;
}

#endif /* CONFIG_WIFI_WEDGE_RESET */
