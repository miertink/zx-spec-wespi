// WiFi station: credentials from NVS, reconnects on its own (wifi_sta.c).

#ifndef _WIFI_STA_H_
#define _WIFI_STA_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Call once at start-up, after nvs_flash_init() */
void wifi_sta_init(void);

/* True while connected with an IP address */
bool wifi_sta_is_connected(void);

/* IP address as text ("192.168.0.10"), "" until connected */
const char *wifi_get_ip(void);

/* MAC address of the station, "38:3E:51:..." */
const char *wifi_get_MAC_addr(void);

/* Network name in use (from NVS), "" if none */
const char *wifi_sta_ssid(void);

/* Networks in reach, strongest first, names unique */
typedef struct {
    char ssid[33];
    int8_t rssi;            /* dBm */
    bool secure;            /* needs a password */
} wifi_net_t;

/* Scans now (blocks ~2 s, longer while a connection attempt is running) and
 * returns up to `max` networks; if no scan is possible, the last list. The
 * radio is busy meanwhile: not while the Spectrum may be saving. */
int wifi_sta_scan(wifi_net_t *out, int max);

/* Stores a new network in NVS and connects to it (the current connection
 * is dropped). pass "" for an open network. */
esp_err_t wifi_sta_set_network(const char *ssid, const char *pass);

#endif /* _WIFI_STA_H_ */
