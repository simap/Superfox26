// WiFi station. esp_wifi_set_config writes the credentials to NVS (WIFI_STORAGE_FLASH, the
// default), and the driver reads them back on the next esp_wifi_init, so nothing here stores
// them itself. The event handler reconnects after a disconnect from a 5 s timer.

#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static volatile bool       s_connected;
static volatile bool       s_started;
static esp_timer_handle_t  s_retry;
static esp_netif_t        *s_netif;
static char                s_ip[16];

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

static bool has_ssid(void)
{
    wifi_config_t cfg;
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != 0;
}

static void retry_cb(void *arg)
{
    if (s_started && !s_connected && has_ssid()) {
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (has_ssid()) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "no SSID stored, set one over the console: wifi SSID PASSWORD");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        if (has_ssid()) {
            ESP_LOGW(TAG, "%s %.*s: reason %d, %s. Retry in 5 s", s_connected ? "lost" : "could not join",
                     ev->ssid_len, (const char *)ev->ssid, ev->reason, reason_name(ev->reason));
        }
        s_connected = false;
        s_ip[0]     = 0;
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, 5 * 1000 * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "ip %s", s_ip);
        s_connected = true;
    }
}

void wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep between beacons adds up to a DTIM interval to every TCP round trip. The badge is
    // on USB power while it browses.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    char ssid[33];
    if (wifi_get_ssid(ssid, sizeof(ssid))) {
        ESP_LOGI(TAG, "stored SSID %s", ssid);
    }
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
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return false;
    }
    esp_timer_stop(s_retry);
    s_connected = false;
    s_ip[0]     = 0;
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s", ssid);
    return true;
}

bool wifi_get_ssid(char *buf, size_t cap)
{
    wifi_config_t cfg;
    if (cap == 0) {
        return false;
    }
    buf[0] = 0;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || cfg.sta.ssid[0] == 0) {
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
    if (!s_started) {
        return -1;
    }
    // The driver refuses a scan while a connect is in progress.
    esp_timer_stop(s_retry);
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
    if (!was_connected && has_ssid()) {
        esp_wifi_connect();
    }
    return n;
}
