/* See net_probe.h. Compiled to nothing without CONFIG_BENCH_NET_PROBE. */
#include "sdkconfig.h"

#if CONFIG_BENCH_NET_PROBE

#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#include "net_probe.h"
#include "session_ctl.h"
#include "wifi_sta.h"
#include "wifi_wedge.h"

static const char *TAG = "netprobe";

/* The host the session dials; dg_agent.c's DG_AGENT_URI. */
#define PROBE_HOST       "agent.deepgram.com"
/* Fixed, well-known, and needs no DNS to reach. */
#define PROBE_INET_IP    "8.8.8.8"
#define PROBE_PINGS      3
/* A failing session raises a transport error per retry; one probe per stall
 * is the point, not one per error. */
#define PROBE_HOLDOFF_MS 20000

/* "proto" remembered across a software reset, so a "reboot" (or a watchdog
 * reset) comes back up on the protocol under test. Power-on clears it. */
#define PROTO_MAGIC 0x50524f54u
static RTC_NOINIT_ATTR uint32_t s_proto_magic;
static RTC_NOINIT_ATTR uint8_t s_proto_boot;

uint8_t net_probe_boot_proto(void)
{
    return s_proto_magic == PROTO_MAGIC ? s_proto_boot : 0;
}

static TaskHandle_t s_task;
static const char *volatile s_why;
static volatile uint32_t s_last_ms;
static volatile bool s_forced;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- ping ---- */

typedef struct {
    SemaphoreHandle_t done;
    uint32_t replies;
    uint32_t rtt_max;
} ping_result_t;

static void on_ping_success(esp_ping_handle_t hdl, void *arg)
{
    ping_result_t *r = arg;
    uint32_t rtt = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
    r->replies++;
    if (rtt > r->rtt_max) r->rtt_max = rtt;
}

static void on_ping_end(esp_ping_handle_t hdl, void *arg)
{
    (void)hdl;
    xSemaphoreGive(((ping_result_t *)arg)->done);
}

/* Replies out of PROBE_PINGS; -1 if no session could be made. */
static int ping_ip4(uint32_t addr_be, uint32_t *rtt_max)
{
    *rtt_max = 0;
    if (addr_be == 0) return -1;

    ping_result_t r = { .done = xSemaphoreCreateBinary() };
    if (!r.done) return -1;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = (ip_addr_t)IPADDR4_INIT(addr_be);
    cfg.count = PROBE_PINGS;
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
        /* count * (interval + timeout) is the most it can take; give it slack. */
        if (xSemaphoreTake(r.done, pdMS_TO_TICKS(PROBE_PINGS * 1300 + 2000)) == pdTRUE) {
            out = (int)r.replies;
        }
        esp_ping_stop(hdl);
    }
    if (hdl) esp_ping_delete_session(hdl);
    vSemaphoreDelete(r.done);
    *rtt_max = r.rtt_max;
    return out;
}

/* ---- the probe ---- */

static void probe(const char *why)
{
    wifi_signal_t sig;
    const bool assoc = wifi_sta_get_signal(&sig);

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {0};
    if (nif) {
        esp_netif_get_ip_info(nif, &ip);
        esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns);
    }
    const uint32_t dns_ip = dns.ip.type == ESP_IPADDR_TYPE_V4 ? dns.ip.u_addr.ip4.addr : 0;

    uint32_t gw_rtt, dns_rtt, inet_rtt;
    const int gw = ping_ip4(ip.gw.addr, &gw_rtt);
    /* The gateway is usually the DNS server too; don't ping it twice. */
    const int dn = dns_ip == ip.gw.addr ? gw : ping_ip4(dns_ip, &dns_rtt);
    if (dns_ip == ip.gw.addr) dns_rtt = gw_rtt;
    const int inet = ping_ip4(ipaddr_addr(PROBE_INET_IP), &inet_rtt);

    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    const uint32_t t0 = now_ms();
    const int gai = getaddrinfo(PROBE_HOST, "443", &hints, &res);
    const uint32_t gai_ms = now_ms() - t0;
    char resolved[16] = "-";
    if (gai == 0 && res) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(sa->sin_addr, resolved, sizeof(resolved));
    }
    if (res) freeaddrinfo(res);

    ESP_LOGI(TAG, "EVT netprobe why=%s assoc=%d rssi=%d ch=%d ip=" IPSTR " gw=" IPSTR " dns=" IPSTR
             " ping_gw=%d/%d %lums ping_dns=%d/%d %lums ping_inet=%d/%d %lums name=%d %s %lums",
             why, assoc, assoc ? sig.rssi : 0, assoc ? sig.channel : 0,
             IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR((esp_ip4_addr_t *)&dns_ip),
             gw, PROBE_PINGS, (unsigned long)gw_rtt,
             dn, PROBE_PINGS, (unsigned long)dns_rtt,
             inet, PROBE_PINGS, (unsigned long)inet_rtt,
             gai, resolved, (unsigned long)gai_ms);
}

static void probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const char *why = s_why ? s_why : "?";
        probe(why);
        s_last_ms = now_ms();
        s_why = NULL;
    }
}

void net_probe_request(const char *why)
{
    if (!s_task || s_why) return;            /* not started, or one queued/running */
    if (!s_forced && s_last_ms && now_ms() - s_last_ms < PROBE_HOLDOFF_MS) return;
    s_forced = false;
    s_why = why;
    xTaskNotifyGive(s_task);
}

/* ---- console commands ---- */

static void run_command(const char *line)
{
    if (strcmp(line, "tap") == 0) {
        ESP_LOGI(TAG, "EVT cmd tap");
        session_ctl_request_toggle();
    } else if (strcmp(line, "stop") == 0) {
        ESP_LOGI(TAG, "EVT cmd stop");
        session_ctl_request_stop();
    } else if (strcmp(line, "probe") == 0) {
        ESP_LOGI(TAG, "EVT cmd probe");
        s_forced = true;                     /* past the hold-off */
        net_probe_request("cmd");
    } else if (strncmp(line, "txpower", 7) == 0 && (line[7] == '\0' || line[7] == ' ')) {
        /*
         * The radio's transmit ceiling, in dBm. Turning it down makes the
         * access point hear this board as if it were further away -- the
         * uplink, which is the direction the stall backs up in -- without
         * moving the board. The driver's unit is 0.25 dBm, range 2..20 dBm.
         */
        if (line[7] == ' ') {
            int dbm = atoi(line + 8);
            if (dbm < 2) dbm = 2;
            if (dbm > 20) dbm = 20;
            const esp_err_t err = esp_wifi_set_max_tx_power((int8_t)(dbm * 4));
            if (err != ESP_OK) ESP_LOGW(TAG, "txpower %d: %s", dbm, esp_err_to_name(err));
        }
        int8_t q = 0;
        const esp_err_t err = esp_wifi_get_max_tx_power(&q);
        ESP_LOGI(TAG, "EVT cmd txpower now=%d.%02d dBm (%s)", q / 4, (q % 4) * 25, esp_err_to_name(err));
    } else if (strcmp(line, "rejoin") == 0) {
        /* Drop the association; wifi_sta.c's disconnect handler joins again
         * at once. If a stall clears on this, it lived in the Wi-Fi session
         * (the C6's, or the access point's), not in the SDIO link. */
        ESP_LOGI(TAG, "EVT cmd rejoin: %s", esp_err_to_name(esp_wifi_disconnect()));
#if CONFIG_ESP_HOSTED_ENABLED
    } else if (strncmp(line, "proto", 5) == 0 && (line[5] == '\0' || line[5] == ' ')) {
        /* The C6's 802.11 protocol, switched live: "proto ax" (b/g/n/ax),
         * "proto bgn", or "proto" to show it. A switch rejoins so the next
         * association uses it. For testing whether 802.11ax is what makes the
         * C6 lose transmit, during a period when it is failing -- no reflash,
         * so the failing conditions are still there. Also kept for the next
         * software reset (net_probe_boot_proto), so "proto bgn" then "reboot"
         * tests b/g/n on a freshly reset C6; power-on goes back to Kconfig. */
        const char *arg = line[5] ? line + 6 : "";
        uint8_t want = 0;
        if (strcmp(arg, "ax") == 0) {
            want = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
        } else if (strcmp(arg, "bgn") == 0) {
            want = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
        } else if (arg[0] != '\0') {
            ESP_LOGW(TAG, "proto: '%s'? use ax, bgn, or nothing to show", arg);
            return;
        }
        esp_err_t err = ESP_OK;
        if (want) {
            err = esp_wifi_set_protocol(WIFI_IF_STA, want);
            s_proto_boot = want;
            s_proto_magic = PROTO_MAGIC;
        }
        uint8_t got = 0;
        esp_err_t rerr = esp_wifi_get_protocol(WIFI_IF_STA, &got);
        ESP_LOGW(TAG, "EVT cmd proto %s: set %s, read back 0x%02x (%s)%s",
                 want ? arg : "show", want ? esp_err_to_name(err) : "-", got,
                 esp_err_to_name(rerr), (want && rerr == ESP_OK && got != want) ? " -- MISMATCH" : "");
        if (want && err == ESP_OK) {
            ESP_LOGI(TAG, "EVT cmd proto rejoin: %s", esp_err_to_name(esp_wifi_disconnect()));
        }
#endif
    } else if (strcmp(line, "reboot") == 0) {
        /* The ROM reset, not esp_restart(): on the P4 the latter can leave the
         * SDMMC DMA running into the next boot (esp-hosted-mcu #240). Boot
         * pulses the C6's reset line, so this resets both chips. */
        ESP_LOGI(TAG, "EVT cmd reboot");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_rom_software_reset_system();
#if CONFIG_WIFI_WEDGE_RESET
    } else if (strncmp(line, "wdtest ", 7) == 0) {
        /* Fault injection for the wedge watchdog (wifi_wedge.h). "dead" and
         * "refuse" fake the two forms without touching the radio; "noap" is
         * real: the station is pointed at an SSID nobody has, so the
         * disconnects carry 201 (no AP found) -- a router switched off --
         * which must NOT reset. "reboot" restores the saved network. */
        const char *arg = line + 7;
        if (strcmp(arg, "dead") == 0) {
            wifi_wedge_inject(WIFI_WEDGE_INJECT_DEAD);
        } else if (strcmp(arg, "refuse") == 0) {
            wifi_wedge_inject(WIFI_WEDGE_INJECT_REFUSE);
        } else if (strcmp(arg, "off") == 0) {
            wifi_wedge_inject(WIFI_WEDGE_INJECT_OFF);
        } else if (strcmp(arg, "noap") == 0) {
            wifi_config_t cfg;
            esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
            if (err == ESP_OK) {
                memset(cfg.sta.ssid, 0, sizeof(cfg.sta.ssid));
                strcpy((char *)cfg.sta.ssid, "wdtest-no-such-ap");
                err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
            }
            if (err == ESP_OK) err = esp_wifi_disconnect();
            ESP_LOGW(TAG, "EVT cmd wdtest noap: %s (\"reboot\" restores)", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "wdtest dead|refuse|noap|off");
        }
#endif
    } else if (line[0]) {
        ESP_LOGW(TAG, "unknown command '%s' (tap, stop, probe, txpower [dBm], rejoin, proto [ax|bgn], reboot, wdtest)", line);
    }
}

/*
 * The console UART's RX, read through the driver. Logging still goes out the
 * VFS's direct path, which writes the TX FIFO itself; the driver here is
 * installed with no TX buffer, so the two never share a queue.
 */
static void console_task(void *arg)
{
    (void)arg;
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    char line[32];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(port, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            run_command(line);
            n = 0;
        } else if (c < 0x20 || c > 0x7e) {
            /* The bridge's first byte after the host opens the port arrives
             * as 0xFF; dropping non-printables keeps it off the command. */
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

bool net_probe_init(void)
{
    if (xTaskCreate(probe_task, "netprobe", 4096, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "no probe task");
        return false;
    }
#if CONFIG_ESP_CONSOLE_UART
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (!uart_is_driver_installed(port) &&
        uart_driver_install(port, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "no console driver; commands off");
        return false;
    }
    if (xTaskCreate(console_task, "netcmd", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no console task; commands off");
        return false;
    }
    ESP_LOGI(TAG, "commands: tap, stop, probe, txpower [dBm], rejoin, proto [ax|bgn], reboot");
#endif
    return true;
}

#endif /* CONFIG_BENCH_NET_PROBE */
