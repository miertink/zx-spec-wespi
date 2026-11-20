/* ZX-Spec Wespi: tape server for the ZX Spectrum family on an ESP32.
 *
 * Start-up: log ring, NVS, status LED, tape player, MIC input, WiFi, SPIFFS,
 * web server, then the menu (which starts the loader loop).
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "tap_player.h"
#include "mic_in.h"
#include "zx_menu.h"
#include "weblog.h"
#include "wifi_sta.h"

static const char *TAG = "zxsmain";

esp_err_t start_file_server(const char *base_path);   /* file_server.c */

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS: cannot mount or format (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS: no partition information (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SPIFFS: %u of %u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

/* NVS holds the WiFi credentials (wifi_sta.c) */
static void nvs_sys_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());     /* truncated or old layout */
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Status LED (GPIO 2 on most DevKits, 21 on TTGO boards; both are driven):
 * on during start-up, then fast blinking while the tape plays, otherwise a
 * short flash every 5 s, doubled when WiFi is connected. */
#define LED_GPIO_A      2
#define LED_GPIO_B      21
#define LED_CYCLE_MS    40

static void led_set(bool on)
{
    gpio_set_level(LED_GPIO_A, on);
    gpio_set_level(LED_GPIO_B, on);
}

static void led_init(void)
{
    const gpio_num_t pins[] = { LED_GPIO_A, LED_GPIO_B };
    for (int i = 0; i < 2; i++) {
        gpio_pad_select_gpio(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    }
    led_set(true);
}

static void led_timer(TimerHandle_t t)
{
    static uint16_t count;
    bool on = false;
    ++count;
    if (tap_player_is_busy()) {
        on = (count & 3) == 1;
    } else {
        on = count == 1 || (count == 6 && wifi_sta_is_connected());
        if (count > 5000 / LED_CYCLE_MS) count = 0;
    }
    led_set(on);
}

void app_main(void)
{
    weblog_init(48 * 1024);         /* first, so the whole boot log is kept */

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_boot_partition() != running) {
        ESP_LOGW(TAG, "running from offset 0x%08x, not from the configured boot partition",
                 running->address);
    }
    ESP_LOGI(TAG, "running partition at 0x%08x", running->address);

    nvs_sys_init();
    led_init();
    tap_player_init();
    /* MIC: Spectrum SAVEs, menu replies, load requests, reset detection.
     * I2S0 in ADC mode leaves GPIO0 (BOOT button) alone. */
    mic_in_init();
    wifi_sta_init();                /* needs NVS */
    ESP_ERROR_CHECK(init_spiffs());
    ESP_ERROR_CHECK(start_file_server("/spiffs"));
    zx_menu_init();                 /* needs SPIFFS: builds the menu and loops it */

    TimerHandle_t t = xTimerCreate("led", pdMS_TO_TICKS(LED_CYCLE_MS), pdTRUE, NULL, led_timer);
    xTimerStart(t, 0);
}
