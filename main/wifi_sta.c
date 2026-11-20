/* WiFi station (based on ZX-Wespi by holmatic).
 *
 * The network name and password live in NVS (namespace "zxstorage", keys
 * WIFI_n / WIFI_p); on the first start they are seeded from Kconfig, and the
 * Spectrum menu can change them (wifi_sta_set_network). After three quick
 * retries a lost connection is retried every 15 s, for good. Networks are
 * scanned only when asked (the WiFi screen of the menu).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "wifi_sta.h"

static const char *TAG = "wifi_sta";

#define QUICK_RETRIES   3
#define RETRY_US        (15 * 1000 * 1000)

static int quick_left;
static bool connected;
static char ip_text[16];
static char mac_text[18];
static esp_timer_handle_t retry_timer;
static char ssid_now[33];

#define NETS_MAX        12
#define SCAN_TRIES      12              /* busy connecting: try again for ~6 s */
#define SCAN_RETRY_MS   500
static wifi_net_t nets[NETS_MAX];       /* the last scan that worked */
static int nnets;
static SemaphoreHandle_t scan_lock;

bool wifi_sta_is_connected(void)
{
    return connected;
}

const char *wifi_get_ip(void)
{
    return ip_text;
}

const char *wifi_get_MAC_addr(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mac_text;
}

const char *wifi_sta_ssid(void)
{
    return ssid_now;
}

static void retry_cb(void *arg)
{
    if (connected) return;
    ESP_LOGI(TAG, "reconnecting");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

static void retry_later(void)
{
    if (!retry_timer) {
        const esp_timer_create_args_t a = { .callback = retry_cb, .name = "wifi_retry" };
        if (esp_timer_create(&a, &retry_timer) != ESP_OK) return;
    }
    esp_timer_stop(retry_timer);            /* not running: harmless error */
    esp_timer_start_once(retry_timer, RETRY_US);
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        quick_left = QUICK_RETRIES;
        connected = false;
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->event_info.got_ip.ip_info.ip));
        ESP_LOGI(TAG, "connected, IP %s", ip_text);
        quick_left = QUICK_RETRIES;
        connected = true;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "disconnected");
        connected = false;
        if (quick_left > 0) {
            quick_left--;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else {
            retry_later();
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* Network name and password from NVS, seeded from Kconfig the first time */
static esp_err_t config_from_nvs(void)
{
    wifi_config_t cfg = { 0 };
    nvs_handle h;
    esp_err_t err = nvs_open("zxstorage", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(err));
        return err;
    }
    size_t l = sizeof(cfg.sta.ssid);
    err = nvs_get_str(h, "WIFI_n", (char *)cfg.sta.ssid, &l);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no network in NVS: using the build configuration");
        nvs_set_str(h, "WIFI_n", CONFIG_WIFI_SSID);
        nvs_set_str(h, "WIFI_p", CONFIG_WIFI_PASSWORD);
        nvs_commit(h);
        l = sizeof(cfg.sta.ssid);
        err = nvs_get_str(h, "WIFI_n", (char *)cfg.sta.ssid, &l);
    }
    if (err == ESP_OK) {
        l = sizeof(cfg.sta.password);
        err = nvs_get_str(h, "WIFI_p", (char *)cfg.sta.password, &l);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: no WiFi credentials (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "network \"%s\"", (char *)cfg.sta.ssid);
    strlcpy(ssid_now, (char *)cfg.sta.ssid, sizeof(ssid_now));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    return esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
}

esp_err_t wifi_sta_set_network(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !pass || strlen(pass) > 63) return ESP_ERR_INVALID_ARG;
    nvs_handle h;
    esp_err_t err = nvs_open("zxstorage", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "WIFI_n", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "WIFI_p", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "new network \"%s\" (%s)", ssid, pass[0] ? "with password" : "open");
    if (retry_timer) esp_timer_stop(retry_timer);
    err = config_from_nvs();
    if (err != ESP_OK) return err;
    quick_left = QUICK_RETRIES;
    /* the disconnect event connects again, now to the new network */
    if (esp_wifi_disconnect() != ESP_OK) esp_wifi_connect();
    return ESP_OK;
}

/* One blocking scan into `nets`; false if the driver is busy (connecting) */
static bool scan_once(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;
    wifi_ap_record_t *rec = n ? calloc(n, sizeof(*rec)) : NULL;
    if (n && (!rec || esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK)) {
        free(rec);
        return false;
    }
    nnets = 0;
    for (int i = 0; i < n && nnets < NETS_MAX; i++) {        /* records come strongest first */
        const char *s = (const char *)rec[i].ssid;
        if (!s[0]) continue;
        bool dup = false;
        for (int k = 0; k < nnets && !dup; k++) dup = strcmp(nets[k].ssid, s) == 0;
        if (dup) continue;
        strlcpy(nets[nnets].ssid, s, sizeof(nets[nnets].ssid));
        nets[nnets].rssi = rec[i].rssi;
        nets[nnets].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        nnets++;
    }
    free(rec);
    return true;
}

int wifi_sta_scan(wifi_net_t *out, int max)
{
    if (!scan_lock) return 0;
    xSemaphoreTake(scan_lock, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    for (int i = 0; i < SCAN_TRIES && !(ok = scan_once()); i++) vTaskDelay(pdMS_TO_TICKS(SCAN_RETRY_MS));
    if (ok) {
        ESP_LOGI(TAG, "scan: %d network(s) in %d ms", nnets, (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGW(TAG, "scan not possible now: last list (%d network(s))", nnets);
    }
    int n = nnets < max ? nnets : max;
    memcpy(out, nets, n * sizeof(wifi_net_t));
    xSemaphoreGive(scan_lock);
    return n;
}

void wifi_sta_init(void)
{
    scan_lock = xSemaphoreCreateMutex();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(config_from_nvs());
    ESP_ERROR_CHECK(esp_wifi_start());
    /* no power save: it disturbs the I2S DMA reads of the MIC input
     * (https://github.com/espressif/esp-idf/issues/3714) */
    esp_wifi_set_ps(WIFI_PS_NONE);
}
