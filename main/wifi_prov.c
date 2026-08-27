#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

#include "api_key.h"
#include "wifi_creds.h"
#include "wifi_prov.h"

static const char *TAG = "wifi_prov";

/* Long enough to find your phone, short enough that a device left alone after a
 * router hiccup goes back to trying the network it already knows. */
#define PROV_IDLE_TIMEOUT_MS (5 * 60 * 1000)

/* Beyond this the <select> is unreadable on a phone anyway. */
#define SCAN_MAX 20

#define DNS_PORT 53

/*
 * The provisioning AP's WPA2 passphrase: nine characters plus the NUL, random
 * per boot.
 *
 * WPA2 wants at least eight. Nine from the 32-symbol alphabet below is 45 bits,
 * which is far more than an access point that erases itself after five minutes
 * needs, and short enough to read off a round panel if the QR cannot be scanned.
 */
#define AP_PASS_LEN 10

/*
 * Unambiguous alphanumerics only, and that constraint is doing two jobs.
 *
 * A `WIFI:` QR payload treats \ ; : and , as reserved, so an alphanumeric
 * passphrase needs no escaping and cannot produce a code that scans into
 * something subtly different. And 0/O and 1/l/I are the characters people
 * transcribe wrongly, which matters because reading this off the display is the
 * fallback path for anyone whose camera will not act on the QR.
 */
#define AP_PASS_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

static char s_ap_name[24];
static char s_ap_pass[AP_PASS_LEN];
static char s_portal_url[32];
static uint32_t s_portal_ip;          /* network byte order, for the DNS answer */
static httpd_handle_t s_httpd;
static TickType_t s_last_activity;

typedef struct {
    char ssid[WIFI_CREDS_SSID_LEN];
    int8_t rssi;
    bool open;
} scan_entry_t;

static scan_entry_t s_scan[SCAN_MAX];
static size_t s_scan_count;

/* ------------------------------------------------------------------ page */

/*
 * One page, no assets: a second request for a stylesheet is a second chance for
 * a captive-portal webview to wander off. Single quotes throughout the markup
 * so the C string needs almost no escaping.
 */
static const char PORTAL_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Deepgram Agent setup</title><style>"
"body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:24px;"
"background:#111;color:#eee}"
"h1{font-size:20px;margin:0 0 4px}p.sub{margin:0 0 20px;color:#888;font-size:14px}"
"label{display:block;margin:16px 0 6px;font-size:13px;color:#aaa}"
"select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
"padding:12px;font-size:16px;background:#1e1e1e;color:#eee;border:1px solid #333;"
"border-radius:8px}"
"button{width:100%;margin-top:24px;padding:14px;font-size:16px;font-weight:600;"
"background:#13ef95;color:#000;border:0;border-radius:8px}"
"button:disabled{opacity:.5}"
"#msg{margin-top:16px;font-size:14px;color:#13ef95;min-height:20px}"
"p.hint{margin:6px 0 0;font-size:12px;color:#888}"
".chk{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:13px;color:#aaa}"
".chk input{width:auto}"
"</style></head><body>"
"<h1>Deepgram Agent</h1>"
"<p class='sub'>Pick the network this device should join.</p>"
"<form id='f'>"
"<label for='net'>Network</label>"
"<select id='net'><option value=''>scanning...</option></select>"
"<div id='manual' style='display:none'>"
"<label for='ssid'>Network name</label>"
"<input type='text' id='ssid' autocapitalize='none' autocorrect='off' placeholder='SSID'>"
"</div>"
"<label for='pass'>Password</label>"
"<input type='password' id='pass' placeholder='leave empty if open'>"
"<label class='chk'><input type='checkbox' id='show'> Show password</label>"
"<label for='key'>Deepgram API key</label>"
"<input type='password' id='key' autocapitalize='none' autocorrect='off' "
"spellcheck='false' placeholder='paste your key'>"
"<label class='chk'><input type='checkbox' id='showk'> Show key</label>"
"<p class='hint' id='keyhint'></p>"
"<button type='submit' id='go'>Save and connect</button>"
"</form><div id='msg'></div>"
"<script>"
"var sel=document.getElementById('net'),msg=document.getElementById('msg');"
"var man=document.getElementById('manual');"
"function opt(v,t){var o=document.createElement('option');o.value=v;o.textContent=t;"
"sel.appendChild(o);return o}"
"var hint=document.getElementById('keyhint');"
"fetch('/scan').then(function(r){return r.json()}).then(function(d){"
"sel.innerHTML='';"
"d.nets.forEach(function(n){opt(n.ssid,n.ssid+'  ('+n.rssi+' dBm)'+(n.open?'  open':''))});"
"opt('__other__','Other / hidden network...');"
"hint.textContent=d.key_set?'A key is already stored. Leave this blank to keep it.'"
":'No key stored yet - this device cannot talk until one is set.';"
"}).catch(function(){sel.innerHTML='';opt('__other__','scan failed - enter manually');"
"man.style.display='block'});"
"sel.onchange=function(){man.style.display=(sel.value=='__other__')?'block':'none'};"
"document.getElementById('show').onchange=function(){"
"document.getElementById('pass').type=this.checked?'text':'password'};"
"document.getElementById('showk').onchange=function(){"
"document.getElementById('key').type=this.checked?'text':'password'};"
"document.getElementById('f').onsubmit=function(e){"
"e.preventDefault();"
"var s=sel.value=='__other__'?document.getElementById('ssid').value:sel.value;"
"if(!s){msg.textContent='Pick or enter a network.';return}"
"document.getElementById('go').disabled=true;msg.textContent='Saving...';"
"fetch('/save',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(s)+'&pass='+"
"encodeURIComponent(document.getElementById('pass').value)+'&key='+"
"encodeURIComponent(document.getElementById('key').value.trim())})"
".then(function(){msg.textContent='Saved. Restarting to join '+s+'. "
"This page will stop responding, and the setup network will disappear.'})"
".catch(function(){msg.textContent='Save failed. Try again.';"
"document.getElementById('go').disabled=false})};"
"</script></body></html>";

/* ------------------------------------------------------------------ scan */

static void scan_networks(void)
{
    s_scan_count = 0;

    /* Blocking scan. It briefly starves the AP link -- the radio cannot beacon
     * on our channel while it is listening on someone else's -- which is why
     * this runs once here and every /scan request is answered from the cache. */
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(*records));
    if (records == NULL) {
        /* The driver holds the results until someone takes or clears them. */
        esp_wifi_clear_ap_list();
        return;
    }
    esp_wifi_scan_get_ap_records(&found, records);

    for (uint16_t i = 0; i < found && s_scan_count < SCAN_MAX; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;  /* hidden network: no name to show, use manual entry */
        }

        /* One SSID often appears several times -- multiple APs, or one AP on
         * two channels. Keep the strongest rather than listing it twice. */
        bool dup = false;
        for (size_t j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan[j].ssid, ssid) == 0) {
                if (records[i].rssi > s_scan[j].rssi) {
                    s_scan[j].rssi = records[i].rssi;
                }
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        strlcpy(s_scan[s_scan_count].ssid, ssid, WIFI_CREDS_SSID_LEN);
        s_scan[s_scan_count].rssi = records[i].rssi;
        s_scan[s_scan_count].open = (records[i].authmode == WIFI_AUTH_OPEN);
        s_scan_count++;
    }

    free(records);
    ESP_LOGI(TAG, "scan found %u networks", (unsigned)s_scan_count);
}

/* ------------------------------------------------------------------ util */

/* SSIDs are arbitrary bytes and regularly contain quotes and backslashes. */
static void json_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 7 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            o += snprintf(out + o, outlen - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Pulls one application/x-www-form-urlencoded field out of a body, decoding as
 * it goes. Returns false when the key is absent.
 *
 * Deliberately not httpd_query_key_value(): that works on the URI, and these
 * arrive in a POST body precisely so a password never lands in a request line
 * that something might log.
 */
static bool form_get(const char *body, const char *key, char *out, size_t outlen)
{
    size_t keylen = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            p += keylen + 1;
            size_t o = 0;
            while (*p != '\0' && *p != '&' && o + 1 < outlen) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)((hexval(p[1]) << 4) | hexval(p[2]));
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return false;
}

/*
 * Cap on a /save body. Not tight: it only has to exclude something absurd, and
 * the buffer is heap-allocated per request rather than reserved.
 */
#define BODY_MAX 1024

/*
 * Whether a key is worth storing at all.
 *
 * DELIBERATELY NOT A FORMAT CHECK. Today's keys are 40 hex characters, but
 * Deepgram has issued other shapes and will again, and a device that rejects a
 * valid key looks broken in a way the user cannot argue with. So this only
 * catches what is certainly a mistake -- whitespace from a sloppy paste, or
 * control characters -- and lets the server be the judge of the rest. Getting
 * that judgement back to the user is what the 401 handling in dg_agent.c is for.
 */
static bool key_is_plausible(const char *key)
{
    for (const char *p = key; *p != '\0'; p++) {
        if (*p <= ' ' || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- handlers */

static void restart_cb(void *arg)
{
    ESP_LOGI(TAG, "restarting into station mode");
    esp_restart();
}

static esp_err_t root_handler(httpd_req_t *req)
{
    s_last_activity = xTaskGetTickCount();
    httpd_resp_set_type(req, "text/html");
    /* The portal only ever serves the current state of the device; a webview
     * that cached it would show a stale scan list on the next provision. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    s_last_activity = xTaskGetTickCount();

    /* Worst case per entry: escaped 32-char SSID plus the rest of the object. */
    size_t cap = 96 + s_scan_count * (WIFI_CREDS_SSID_LEN * 2 + 48);
    char *json = malloc(cap);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }

    /*
     * The list is wrapped in an object so the one request the page already makes
     * can also answer "is a key stored?". A second endpoint would be a second
     * chance for a captive-portal webview to wander off -- the same reasoning
     * that keeps the page down to one document with no assets.
     *
     * The closing "]}" IS RESERVED OUT OF THE BUDGET, not written on trust. The
     * loop below only ever accepts a write that fits strictly inside its limit,
     * so with the full cap it could leave a single byte free -- enough for the
     * old bare-array "]" and not for these two. `lim` is what keeps the JSON
     * well-formed when the scan list is what runs the buffer out.
     */
    const size_t tail = sizeof("]}") - 1;
    const size_t lim = cap - tail;

    size_t n = 0;
    n += snprintf(json + n, lim - n, "{\"key_set\":%s,\"nets\":[",
                  api_key_is_stored() ? "true" : "false");
    for (size_t i = 0; i < s_scan_count && n + 1 < lim; i++) {
        char esc[WIFI_CREDS_SSID_LEN * 2];
        json_escape(s_scan[i].ssid, esc, sizeof(esc));
        int w = snprintf(json + n, lim - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                         i ? "," : "", esc, s_scan[i].rssi,
                         s_scan[i].open ? "true" : "false");
        /* snprintf reports what it WANTED to write, so an unguarded n += w can
         * step past cap and underflow the size_t length on the next call. */
        if (w < 0 || (size_t)w >= lim - n) {
            break;
        }
        n += w;
    }
    /* Direct, not snprintf: the space was reserved above, and snprintf's return
     * value would reintroduce exactly the overrun `lim` exists to prevent. */
    json[n++] = ']';
    json[n++] = '}';

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, n);
    free(json);
    return err;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    s_last_activity = xTaskGetTickCount();

    /*
     * A 32-character SSID and a 63-character password each TRIPLE under
     * percent-encoding, and the API key is up to 128 characters on top, so the
     * old 512 does not cover a legitimate worst case any more.
     */
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }

    /*
     * HEAP, NOT STACK. This handler was already the fourth-largest frame in the
     * image at 688 B before the body grew, and it runs on the HTTP server's
     * task rather than one of ours. See .claude/skills/esp-stack-budget/.
     */
    char *body = malloc(BODY_MAX + 1);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[WIFI_CREDS_SSID_LEN] = {0};
    char pass[WIFI_CREDS_PASS_LEN] = {0};
    char key[DG_API_KEY_LEN] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
    }
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "key", key, sizeof(key));
    /* Nothing else needs the raw body, and it is the only place the key and the
     * password sit together. */
    memset(body, 0, BODY_MAX + 1);
    free(body);

    /*
     * BLANK MEANS KEEP, and only the key can safely mean that: an empty Wi-Fi
     * password is a legitimate open network, while an empty API key is never
     * valid, so there is nothing for blank to be confused with. The page says so
     * when a key is stored, because blank has to look safe before anyone uses it.
     */
    if (key[0] != '\0' && !key_is_plausible(key)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "that key has spaces or control characters in it");
    }
    if (key[0] == '\0' && !api_key_is_stored()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "no api key stored yet, so one is required");
    }

    /* Neither secret is logged -- the whole reason these arrive in a POST body
     * rather than a query string. "passphrase" is the Wi-Fi one; the API key is
     * reported only as set or kept. */
    ESP_LOGI(TAG, "provisioned \"%s\" (%s), api key %s", ssid,
             pass[0] ? "with passphrase" : "open",
             key[0] ? "set" : "kept");

    if (wifi_creds_save(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    if (key[0] != '\0' && api_key_save(key) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "the network saved but the key did not");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "saved");

    /* Answer first, reboot second -- esp_restart() here would cut the response
     * off mid-flight and the page would report a failure it did not have. */
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "prov_restart" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1000 * 1000);
    } else {
        esp_restart();
    }
    return ESP_OK;
}

/*
 * Everything else redirects to the portal.
 *
 * This is half of what makes the sign-in sheet appear on its own: iOS asks for
 * /hotspot-detect.html and Android for /generate_204, and a 302 instead of the
 * expected response is exactly the signal that says "captive portal here".
 */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------- dns */

/*
 * Answers every A query with our own address, so whatever hostname the phone
 * reaches for lands on the portal. The other half of the auto-popping sheet.
 *
 * AAAA queries get NOERROR with no answers rather than a bogus record, which is
 * what makes a dual-stack client fall back to IPv4 instead of hanging.
 */
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[256];
    uint8_t tx[sizeof(rx) + 16];

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fromlen);

        /* 12-byte header, and only single-question queries are worth answering. */
        if (len < 12 || (rx[2] & 0x80) != 0 || rx[4] != 0 || rx[5] != 1) {
            continue;
        }

        /* Walk the QNAME labels to find the qtype behind them. */
        int p = 12;
        while (p < len && rx[p] != 0) {
            p += rx[p] + 1;
        }
        if (p + 5 > len) {
            continue;
        }
        const int qtype = (rx[p + 1] << 8) | rx[p + 2];
        const int qend = p + 5;

        memcpy(tx, rx, qend);
        tx[2] = 0x84 | (rx[2] & 0x01);  /* QR=1, AA=1, RD echoed back */
        tx[3] = 0x00;                   /* RA=0, RCODE=NOERROR */
        tx[6] = 0x00;             /* ANCOUNT */
        tx[7] = (qtype == 1) ? 1 : 0;
        tx[8] = tx[9] = tx[10] = tx[11] = 0;  /* no NS, no AR */

        int txlen = qend;
        if (qtype == 1) {
            static const uint8_t answer[] = {
                0xC0, 0x0C,             /* name: pointer back to the question */
                0x00, 0x01, 0x00, 0x01, /* A, IN */
                0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
                0x00, 0x04,             /* rdlength */
            };
            memcpy(tx + txlen, answer, sizeof(answer));
            txlen += sizeof(answer);
            memcpy(tx + txlen, &s_portal_ip, 4);
            txlen += 4;
        }

        sendto(sock, tx, txlen, 0, (struct sockaddr *)&from, fromlen);
    }
}

/* ----------------------------------------------------------------- entry */

const char *wifi_prov_ap_name(void)
{
    if (s_ap_name[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_name, sizeof(s_ap_name), "dg-agent-%02X%02X", mac[4], mac[5]);
    }
    return s_ap_name;
}

const char *wifi_prov_ap_password(void)
{
    if (s_ap_pass[0] == '\0') {
        /* esp_random() is the hardware RNG; by the time the portal starts, Wi-Fi
         * is initialised, which is the condition for it to be properly seeded. */
        for (size_t i = 0; i < AP_PASS_LEN - 1; i++) {
            s_ap_pass[i] = AP_PASS_ALPHABET[esp_random() % (sizeof(AP_PASS_ALPHABET) - 1)];
        }
        s_ap_pass[AP_PASS_LEN - 1] = '\0';
    }
    return s_ap_pass;
}

esp_err_t wifi_prov_start(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .authmode = WIFI_AUTH_WPA2_PSK,   /* see the header for why */
            .max_connection = 4,
            .channel = 1,
        },
    };
    const char *name = wifi_prov_ap_name();
    strncpy((char *)ap_cfg.ap.ssid, name, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(name);
    strncpy((char *)ap_cfg.ap.password, wifi_prov_ap_password(),
            sizeof(ap_cfg.ap.password));

    /* APSTA, not AP: esp_wifi_scan_start() needs a station interface to scan
     * with, and the whole point of the portal is showing the user a list. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip));
    s_portal_ip = ip.ip.addr;
    snprintf(s_portal_url, sizeof(s_portal_url), "http://" IPSTR "/", IP2STR(&ip.ip));

    scan_networks();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    /* Captive-portal probes arrive from several processes at once on a phone,
     * and a webview that cannot get a socket shows a blank page. Kept well
     * under CONFIG_LWIP_MAX_SOCKETS (10), which the DNS responder also draws
     * from. */
    cfg.max_open_sockets = 5;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    const httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_handler);

    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 5, NULL);

    s_last_activity = xTaskGetTickCount();
    ESP_LOGI(TAG, "portal up: join \"%s\", then browse to %s", name, s_portal_url);
    return ESP_OK;
}

void wifi_prov_run(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        uint32_t idle_ms = (xTaskGetTickCount() - s_last_activity) * portTICK_PERIOD_MS;
        if (idle_ms >= PROV_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "portal idle for %u minutes, restarting to retry the "
                          "saved network", (unsigned)(idle_ms / 60000));
            esp_restart();
        }
    }
}
