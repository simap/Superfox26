// Manages the badge's WiFi station connection: credentials kept in flash, automatic connect with
// retry, and a fast reconnect hint that survives deep sleep so a wake rejoins the last access point
// on its known channel instead of scanning every channel.
// Call wifi_init once before any other function here.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init(bool kept, bool radio_on);

void wifi_stop(void);

void wifi_resume(void);

bool wifi_is_stopped(void);

bool wifi_wait_connected(int timeout_ms);

bool wifi_set_credentials(const char *ssid, const char *password);

bool wifi_clear_credentials(void);

bool wifi_get_ssid(char *buf, size_t cap);

bool wifi_is_connected(void);

void wifi_get_ip(char *buf, size_t cap);

bool wifi_get_link(int *rssi, int *channel);

typedef struct {
    char    ssid[33];
    int     channel;
    int     rssi;
    char    auth[12];
} wifi_scan_entry_t;

int wifi_scan(wifi_scan_entry_t *out, int cap);

#ifdef __cplusplus
}
#endif
