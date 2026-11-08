// WiFi station. The credentials live in the driver's NVS storage: set once over the
// console, they survive reboots and reflashes of the app.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialises NVS, the netif stack and the driver, starts the station and connects
// when NVS holds an SSID. Returns at once. A lost connection is retried every 5 s.
void wifi_init(void);

// Stores ssid and password through the driver (NVS) and connects. password may be "".
bool wifi_set_credentials(const char *ssid, const char *password);

// The configured SSID, "" when none. Returns false when none.
bool wifi_get_ssid(char *buf, size_t cap);

// True while the station holds an IP address.
bool wifi_is_connected(void);

// Dotted IP address, "" when not connected.
void wifi_get_ip(char *buf, size_t cap);

// Signal strength of the connected AP in dBm and its channel. False when not connected.
bool wifi_get_link(int *rssi, int *channel);

typedef struct {
    char    ssid[33];
    int     channel;
    int     rssi;
    char    auth[12]; // open, WEP, WPA, WPA2, WPA3, ...
} wifi_scan_entry_t;

// Blocking scan of all 2.4 GHz channels, about 3 s. A connect in progress is stopped first and
// restarted after. Returns the number of entries written, negative on a driver error.
int wifi_scan(wifi_scan_entry_t *out, int cap);

#ifdef __cplusplus
}
#endif
