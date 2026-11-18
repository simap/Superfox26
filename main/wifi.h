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
// kept is the result of rtcstate_begin(). false clears the channel and BSSID kept in RTC memory
// for a fast reconnect. radio_on false initialises the driver but leaves the radio off, in the state
// wifi_stop() leaves it. wifi_resume() then starts it.
void wifi_init(bool kept, bool radio_on);

// Disconnects and turns the radio off. Nothing reconnects until wifi_resume(). Blocks until the
// driver reports the stop, at most 1 s. Call before deep sleep.
void wifi_stop(void);

// Turns the radio on after wifi_stop() and connects. Returns at once. No effect while the radio
// is on.
void wifi_resume(void);

// True from wifi_stop() until wifi_resume().
bool wifi_is_stopped(void);

// Waits up to timeout_ms for an IP address. True when connected. Returns false at once while the
// radio is off.
bool wifi_wait_connected(int timeout_ms);

// Stores ssid and password through the driver (NVS) and connects. password may be "".
// While the radio is off it only stores them and wifi_resume() connects.
bool wifi_set_credentials(const char *ssid, const char *password);

// Erases the stored SSID and password from NVS and disconnects. The browser then shows the welcome
// notice. False when the driver refused the write (see log).
bool wifi_clear_credentials(void);

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
// restarted after. Returns the number of entries written, negative on a driver error and while
// the radio is off.
int wifi_scan(wifi_scan_entry_t *out, int cap);

#ifdef __cplusplus
}
#endif
