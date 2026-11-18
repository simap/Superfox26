// WiFi station. The credentials live in the driver's NVS record: esp_wifi_init reads them back.
// wifi_init then switches the driver to WIFI_STORAGE_RAM, so the station config changes for the fast
// reconnect stay out of NVS. wifi_set_credentials switches to WIFI_STORAGE_FLASH for its one write.
// The event handler reconnects after a disconnect from a 5 s timer while the radio is on.
//
// Fast reconnect: each connection keeps the channel and BSSID of the AP in RTC memory with the SSID
// they belong to. A start with a hint for the configured SSID sets sta.channel, sta.bssid and
// bssid_set. The driver scans the hint channel first and its fast scan ends at the first match
// (IDF docs/en/api-guides/wifi-driver/overview.rst, Station Basic Configuration). A hinted connect
// that ends in a disconnect, or has no IP after HINT_TIMEOUT_MS, clears the hint and connects again
// with a scan of all channels.
//
// Every esp_wifi_get_config, esp_wifi_set_config and esp_wifi_set_storage sequence runs under s_cfg_lock.
// The event task, the timer task, the browser task and the console task all touch the station config.
// wifi_set_credentials switches the storage to flash for its one write, and without the lock a hinted
// config from another task could reach NVS in that window, or an old SSID could be written back. No
// function that holds the lock calls another that takes it. The longest hold is the NVS write of
// wifi_set_credentials, which an event handler can wait for.

#include "wifi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static const int HINT_TIMEOUT_MS = 10 * 1000;

// Event group bits. BIT_STOPPED is set by the STA_STOP event and cleared by wifi_resume.
#define BIT_CONNECTED BIT0
#define BIT_STOPPED   BIT1

static volatile bool       s_connected;
static volatile bool       s_started;
static volatile bool       s_off;      // wifi_stop() is in effect
static volatile bool       s_scanning; // wifi_scan() is running
static volatile bool       s_hinted;   // the driver's station config carries the hint
// Modem sleep between beacons adds up to a DTIM interval to every TCP round trip, so power save is
// off.
static const wifi_ps_type_t s_ps = WIFI_PS_NONE;
static esp_timer_handle_t  s_retry;
static esp_timer_handle_t  s_hint_timer;
static EventGroupHandle_t  s_events;
static SemaphoreHandle_t   s_cfg_lock; // see the top of this file
static esp_netif_t        *s_netif;
static char                s_ip[16];

typedef struct {
    uint8_t ssid[32]; // as in wifi_sta_config_t, no terminator at 32 bytes
    uint8_t bssid[6];
    uint8_t channel;  // 1 to 13, 0 when there is no hint
    uint8_t pad;
} wifi_hint_t;

// A hint channel is valid from 1 to 13.
#define HINT_CHANNEL_MAX 13

// RTC slow memory, not initialised at boot. Deep sleep, a software reset and a panic restart keep it.
// The magic word of main/rtcstate.cpp tells whether it is valid. channel is cleared first and written
// last, so a reset during a write leaves no hint. A change of this layout needs a new layout version
// in main/rtcstate.cpp.
static RTC_NOINIT_ATTR wifi_hint_t s_hint;

// Reason codes from esp_wifi_types, the ones a wrong setting produces.
static const char *reason_name(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "no AP with that SSID found";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout, usually a wrong password";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    case WIFI_REASON_AUTH_EXPIRE:
        return "authentication expired";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout, AP out of range";
    case WIFI_REASON_ASSOC_LEAVE:
        return "left the AP";
    default:
        return "see wifi_err_reason_t";
    }
}

// No effect before wifi_init creates the lock: the driver calls then fail on their own.
static void cfg_lock(void)
{
    if (s_cfg_lock) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
}

static void cfg_unlock(void)
{
    if (s_cfg_lock) {
        xSemaphoreGive(s_cfg_lock);
    }
}

static bool has_ssid(void)
{
    wifi_config_t cfg;
    cfg_lock();
    bool have = esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
    cfg_unlock();
    return have;
}

static bool ssid_equal(const uint8_t *a, const uint8_t *b)
{
    size_t n = strnlen((const char *)a, 32);
    return n == strnlen((const char *)b, 32) && memcmp(a, b, n) == 0;
}

static void hint_clear(void)
{
    ((volatile wifi_hint_t *)&s_hint)->channel = 0;
}

// Keeps the channel and BSSID of the connected AP for the configured SSID.
static void hint_take(void)
{
    wifi_config_t    cfg;
    wifi_ap_record_t ap;
    cfg_lock();
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    cfg_unlock();
    if (err != ESP_OK || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || ap.primary < 1 ||
        ap.primary > HINT_CHANNEL_MAX) {
        return;
    }
    volatile wifi_hint_t *h = &s_hint;
    h->channel              = 0;
    for (int i = 0; i < 32; i++) {
        h->ssid[i] = cfg.sta.ssid[i];
    }
    for (int i = 0; i < 6; i++) {
        h->bssid[i] = ap.bssid[i];
    }
    h->channel = ap.primary;
}

// Sets the station config to the stored credentials, with the hint when use is true and the hint
// is valid for the configured SSID. The driver refuses a config change during a connect
// (ESP_ERR_WIFI_STATE), so this runs only while the station is stopped or disconnected.
// A failed write leaves s_hinted as it was. After a failed clear it stays true, so the next call
// tries the clear again.
static void apply_hint_locked(bool use)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == 0) {
        s_hinted = false;
        return;
    }
    use = use && s_hint.channel >= 1 && s_hint.channel <= HINT_CHANNEL_MAX && ssid_equal(s_hint.ssid, cfg.sta.ssid);
    if (!use && !s_hinted && !cfg.sta.bssid_set && cfg.sta.channel == 0) {
        return;
    }
    if (use) {
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        cfg.sta.channel     = s_hint.channel;
        cfg.sta.bssid_set   = true;
        memcpy(cfg.sta.bssid, s_hint.bssid, sizeof(cfg.sta.bssid));
    } else {
        cfg.sta.channel   = 0;
        cfg.sta.bssid_set = false;
        memset(cfg.sta.bssid, 0, sizeof(cfg.sta.bssid));
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config for the fast reconnect: %s", esp_err_to_name(err));
        return;
    }
    s_hinted = use;
}

static void apply_hint(bool use)
{
    cfg_lock();
    apply_hint_locked(use);
    cfg_unlock();
}

static void connect(void)
{
    esp_timer_stop(s_hint_timer);
    if (s_hinted) {
        esp_timer_start_once(s_hint_timer, HINT_TIMEOUT_MS * 1000LL);
    }
    esp_wifi_connect();
}

static void retry_cb(void *arg)
{
    if (s_started && !s_off && !s_connected && has_ssid()) {
        if (s_hinted && s_hint.channel == 0) {
            apply_hint(false);
        }
        connect();
    }
}

// A hinted connect without an IP after HINT_TIMEOUT_MS. The disconnect ends it and the event handler
// connects again without the hint. The retry timer covers a disconnect that raises no event.
static void hint_timeout_cb(void *arg)
{
    if (!s_hinted || s_connected || s_off) {
        return;
    }
    ESP_LOGW(TAG, "fast reconnect: no IP after %d s, scanning all channels", HINT_TIMEOUT_MS / 1000);
    hint_clear();
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, 5 * 1000 * 1000);
    esp_wifi_disconnect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_off) {
            // a start event handled after wifi_stop: no connect
            return;
        }
        if (has_ssid()) {
            connect();
        } else {
            ESP_LOGW(TAG, "no SSID stored, set one over the console: wifi SSID PASSWORD");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_started = false;
        esp_timer_stop(s_retry);
        esp_timer_stop(s_hint_timer);
        xEventGroupSetBits(s_events, BIT_STOPPED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev  = (wifi_event_sta_disconnected_t *)data;
        bool                           was = s_connected;
        s_connected                        = false;
        s_ip[0]                            = 0;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        esp_timer_stop(s_retry);
        esp_timer_stop(s_hint_timer);
        if (was) {
            // The console's event line for a link change, as for the cycle state in browser.cpp.
            printf("EVT wifi connected=0\n");
            fflush(stdout);
        }
        if (s_off) {
            // wifi_stop: no reconnect, and the hint stays for the next start
            return;
        }
        if (s_hinted && !was && !s_scanning) {
            ESP_LOGW(TAG, "fast reconnect to %.*s failed: reason %d, %s. Scanning all channels", ev->ssid_len,
                     (const char *)ev->ssid, ev->reason, reason_name(ev->reason));
            hint_clear();
            apply_hint(false);
            connect();
            return;
        }
        if (has_ssid()) {
            ESP_LOGW(TAG, "%s %.*s: reason %d, %s. Retry in 5 s", was ? "lost" : "could not join",
                     ev->ssid_len, (const char *)ev->ssid, ev->reason, reason_name(ev->reason));
        }
        esp_timer_start_once(s_retry, 5 * 1000 * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_off) {
            return;
        }
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        esp_timer_stop(s_hint_timer);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "ip %s%s", s_ip, s_hinted ? ", fast reconnect" : "");
        hint_take();
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        printf("EVT wifi connected=1 ip=%s\n", s_ip);
        fflush(stdout);
    }
}

void wifi_init(bool kept, bool radio_on)
{
    if (!kept) {
        memset((void *)&s_hint, 0, sizeof(s_hint));
    }
    s_cfg_lock = xSemaphoreCreateMutex();
    assert(s_cfg_lock);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif  = esp_netif_create_default_wifi_sta();
    s_events = xEventGroupCreate();
    assert(s_events);

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));
    const esp_timer_create_args_t hargs = { .callback = hint_timeout_cb, .name = "wifi_hint" };
    ESP_ERROR_CHECK(esp_timer_create(&hargs, &s_hint_timer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    cfg_lock();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    cfg_unlock();

    char ssid[33];
    if (wifi_get_ssid(ssid, sizeof(ssid))) {
        ESP_LOGI(TAG, "stored SSID %s", ssid);
    }
    if (!radio_on) {
        // The state wifi_stop leaves. wifi_resume applies the hint and starts the radio.
        s_off = true;
        xEventGroupSetBits(s_events, BIT_STOPPED);
        ESP_LOGI(TAG, "radio off");
        return;
    }
    apply_hint(true);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(s_ps));
    if (s_hinted) {
        ESP_LOGI(TAG, "fast reconnect on channel %d to %02x:%02x:%02x:%02x:%02x:%02x", s_hint.channel,
                 s_hint.bssid[0], s_hint.bssid[1], s_hint.bssid[2], s_hint.bssid[3], s_hint.bssid[4],
                 s_hint.bssid[5]);
    }
}

void wifi_stop(void)
{
    if (!s_events || s_off) {
        return;
    }
    s_off = true;
    esp_timer_stop(s_retry);
    esp_timer_stop(s_hint_timer);
    s_connected = false;
    s_ip[0]     = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_STOPPED);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop: %s", esp_err_to_name(err));
        return;
    }
    // The events of this stop are handled once BIT_STOPPED is set, so none of them reaches the
    // handler after a later wifi_resume.
    if (!(xEventGroupWaitBits(s_events, BIT_STOPPED, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT_STOPPED)) {
        ESP_LOGW(TAG, "no stop event after 1 s");
    }
    ESP_LOGI(TAG, "radio off");
}

void wifi_resume(void)
{
    if (!s_events || !s_off) {
        return;
    }
    apply_hint(true);
    xEventGroupClearBits(s_events, BIT_STOPPED);
    s_off         = false;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        s_off = true;
        return;
    }
    esp_wifi_set_ps(s_ps);
    ESP_LOGI(TAG, "radio on%s", s_hinted ? ", fast reconnect" : "");
}

bool wifi_is_stopped(void)
{
    return s_off;
}

bool wifi_wait_connected(int timeout_ms)
{
    if (!s_events || s_off) {
        return false;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    EventBits_t bits =
        xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_STOPPED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0 && s_connected;
}

bool wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 63)) {
        return false;
    }
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    hint_clear();
    // The one config write that goes to NVS.
    cfg_lock();
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        s_hinted = false;
    }
    cfg_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return false;
    }
    esp_timer_stop(s_hint_timer);
    esp_timer_stop(s_retry);
    s_connected = false;
    s_ip[0]     = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    if (s_off) {
        ESP_LOGI(TAG, "stored %s, the radio is off until wifi_resume", ssid);
        return true;
    }
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s", ssid);
    return true;
}

bool wifi_clear_credentials(void)
{
    // s_hinted false first: the disconnect event handler then does not connect again with the hint.
    // The retry timer and the handler check has_ssid() before a connect.
    hint_clear();
    cfg_lock();
    s_hinted = false;
    cfg_unlock();
    esp_timer_stop(s_hint_timer);
    esp_timer_stop(s_retry);
    if (!s_off) {
        esp_wifi_disconnect();
    }
    // The driver refuses set_config while the station connects (ESP_ERR_WIFI_STATE), so the write
    // tries again until the disconnect has taken effect, for up to 1 s.
    wifi_config_t cfg = { 0 };
    esp_err_t     err;
    for (int i = 0;; i++) {
        cfg_lock();
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        cfg_unlock();
        if (err != ESP_ERR_WIFI_STATE || i == 9) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config to clear: %s", esp_err_to_name(err));
        return false;
    }
    s_connected = false;
    s_ip[0]     = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED);
    ESP_LOGI(TAG, "stored SSID cleared");
    return true;
}

bool wifi_get_ssid(char *buf, size_t cap)
{
    wifi_config_t cfg;
    if (cap == 0) {
        return false;
    }
    buf[0] = 0;
    cfg_lock();
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    cfg_unlock();
    if (err != ESP_OK || cfg.sta.ssid[0] == 0) {
        return false;
    }
    size_t n = strnlen((const char *)cfg.sta.ssid, sizeof(cfg.sta.ssid));
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, cfg.sta.ssid, n);
    buf[n] = 0;
    return true;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

void wifi_get_ip(char *buf, size_t cap)
{
    if (cap == 0) {
        return;
    }
    strncpy(buf, s_connected ? s_ip : "", cap - 1);
    buf[cap - 1] = 0;
}

bool wifi_get_link(int *rssi, int *channel)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi    = ap.rssi;
    *channel = ap.primary;
    return true;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    default:
        return "other";
    }
}

int wifi_scan(wifi_scan_entry_t *out, int cap)
{
    if (!s_started || s_off) {
        ESP_LOGW(TAG, "scan: the radio is off");
        return -1;
    }
    // The driver refuses a scan while a connect is in progress.
    s_scanning = true;
    esp_timer_stop(s_retry);
    esp_timer_stop(s_hint_timer);
    bool was_connected = s_connected;
    if (!was_connected) {
        esp_wifi_disconnect();
    }
    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden        = true;
    esp_err_t err          = esp_wifi_scan_start(&cfg, true);
    int       n            = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
        n = -1;
    } else {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        if (count > cap) {
            count = cap;
        }
        wifi_ap_record_t *recs = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                strncpy(out[i].ssid, (const char *)recs[i].ssid, 32);
                out[i].ssid[32] = 0;
                out[i].channel  = recs[i].primary;
                out[i].rssi     = recs[i].rssi;
                strncpy(out[i].auth, auth_name(recs[i].authmode), sizeof(out[i].auth) - 1);
                out[i].auth[sizeof(out[i].auth) - 1] = 0;
            }
            n = count;
        } else {
            esp_wifi_clear_ap_list();
            n = -1;
        }
        free(recs);
    }
    s_scanning = false;
    if (!was_connected && !s_off && has_ssid()) {
        connect();
    }
    return n;
}
