/*
 * Tape player: plays tape images (.tap, .tzx, .pzx, .csw; see tape_file.c)
 * once or in a loop, from a single task, with a BOOT button toggle.
 *
 * Multi-load games: a TZX/PZX can say "stop the tape" (the game then asks
 * for the next part). The player stops there and remembers the position;
 * playing the same file again continues from that point.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "tape_out.h"
#include "tape_file.h"
#include "tap_player.h"

static const char *TAG = "tap_player";

#define TAP_PATH_MAX   128
#define TAP_LOOP_PATH  "/spiffs/" CONFIG_ZXS_LOOP_FILE

typedef struct {
    char path[TAP_PATH_MAX];
    bool loop;
    bool fast;              /* single play: tf_opts_t.fast_start */
    void (*fn)(void *);     /* tap_player_run: a function instead of a file */
    void *arg;
} play_req_t;

static QueueHandle_t play_queue;
static volatile bool busy;
static volatile bool looping;
static volatile bool stop_req;
static char current[TAP_PATH_MAX];

/* "stop the tape" position of the last file, continued by the next play of it */
static char resume_path[TAP_PATH_MAX];
static uint32_t resume_offset;

static void tap_player_task(void *arg)
{
    play_req_t req;
    while (1) {
        if (xQueueReceive(play_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        tape_out_reset_abort();
        tf_opts_t o = { .stop = &stop_req };
        if (req.fn) {
            req.fn(req.arg);
        } else if (req.loop) {
            resume_path[0] = 0;
            ESP_LOGI(TAG, "loop start: %s, pilot %d pulses", req.path, CONFIG_ZXS_LOOP_PILOT_PULSES);
            /* the loop pilot applies to .tap blocks (the menu); other
             * formats keep their own timings */
            o.pilot_pulses = CONFIG_ZXS_LOOP_PILOT_PULSES;
            o.ignore_stops = true;
            uint32_t rounds = 0;
            while (!stop_req) {
                o.verbose = rounds == 0;
                if (tape_file_play(req.path, &o) != TF_DONE) break;
                rounds++;
            }
            ESP_LOGI(TAG, "loop end after %u round(s)", (unsigned)rounds);
        } else {
            if (strcmp(resume_path, req.path) == 0) {
                o.start_offset = resume_offset;
                ESP_LOGI(TAG, "continuing %s from offset %u", req.path, (unsigned)resume_offset);
            } else {
                ESP_LOGI(TAG, "playing %s", req.path);
            }
            resume_path[0] = 0;
            o.verbose = true;
            o.fast_start = req.fast;
            tf_result_t r = tape_file_play(req.path, &o);
            if (r == TF_TAPE_STOP) {
                strlcpy(resume_path, req.path, sizeof(resume_path));
                resume_offset = o.resume_offset;
            }
            static const char *what[] = {"finished", "stopped", "failed", "paused (stop the tape)"};
            ESP_LOGI(TAG, "%s %s", req.path, what[r]);
        }
        current[0] = 0;
        looping = false;
        stop_req = false;
        busy = false;
    }
}

/* Stops what is playing and waits until the player is idle. The encoder
 * notices the abort at its next refill, within ~60 ms. */
static bool wait_idle(void)
{
    if (busy) tap_player_stop();
    for (int i = 0; i < 100 && busy; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return !busy;
}

static esp_err_t start(const char *path, bool loop, bool fast)
{
    if (!path || strlen(path) >= TAP_PATH_MAX) return ESP_ERR_INVALID_ARG;
    if (!wait_idle()) return ESP_ERR_INVALID_STATE;

    play_req_t req;
    strlcpy(req.path, path, sizeof(req.path));
    req.loop = loop;
    req.fast = fast;
    req.fn = NULL;
    req.arg = NULL;
    busy = true;
    looping = loop;
    stop_req = false;
    const char *slash = strrchr(path, '/');
    strlcpy(current, slash ? slash + 1 : path, sizeof(current));
    if (xQueueSend(play_queue, &req, 0) != pdTRUE) {
        busy = false;
        looping = false;
        current[0] = 0;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

#if CONFIG_ZXS_BUTTON_GPIO >= 0
/* BOOT button (active low, pulled up on the board): each press toggles the
 * loop of CONFIG_ZXS_LOOP_FILE. Polled: no ISR, simple debounce. */
static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_ZXS_BUTTON_GPIO;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    int last = 1;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int now = gpio_get_level(pin);
        if (now == last) continue;
        vTaskDelay(pdMS_TO_TICKS(30));               /* debounce */
        if (gpio_get_level(pin) != now) continue;
        last = now;
        if (now == 0) {                              /* pressed */
            if (busy) {
                ESP_LOGI(TAG, "button: stop");
                tap_player_stop();
            } else {
                ESP_LOGI(TAG, "button: loop %s", TAP_LOOP_PATH);
                tap_player_loop(TAP_LOOP_PATH);
            }
        }
    }
}
#endif

void tap_player_init(void)
{
    tape_out_init();
    play_queue = xQueueCreate(1, sizeof(play_req_t));
    xTaskCreate(tap_player_task, "tap_player", 6144, NULL, 9, NULL);
#if CONFIG_ZXS_BUTTON_GPIO >= 0
    xTaskCreate(button_task, "tap_button", 3072, NULL, 5, NULL);
#endif
}

esp_err_t tap_player_run(void (*fn)(void *arg), void *arg, const char *label)
{
    if (!fn || !label) return ESP_ERR_INVALID_ARG;
    if (!wait_idle()) return ESP_ERR_INVALID_STATE;
    play_req_t req = { .fn = fn, .arg = arg };
    strlcpy(req.path, label, sizeof(req.path));
    busy = true;
    looping = false;
    stop_req = false;
    const char *slash = strrchr(label, '/');
    strlcpy(current, slash ? slash + 1 : label, sizeof(current));
    if (xQueueSend(play_queue, &req, 0) != pdTRUE) {
        busy = false;
        current[0] = 0;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t tap_player_play(const char *path)
{
    return start(path, false, false);
}

esp_err_t tap_player_play_fast(const char *path)
{
    return start(path, false, true);
}

esp_err_t tap_player_resume(void)
{
    if (!resume_path[0]) return ESP_ERR_NOT_FOUND;
    char path[TAP_PATH_MAX];
    strlcpy(path, resume_path, sizeof(path));
    return start(path, false, false);
}

esp_err_t tap_player_loop(const char *path)
{
    return start(path, true, false);
}

bool tap_player_is_looping(void)
{
    return looping;
}

void tap_player_stop(void)
{
    if (!busy) return;
    stop_req = true;
    tape_out_abort();
}

bool tap_player_is_busy(void)
{
    return busy;
}

const char *tap_player_current(void)
{
    return current;
}

bool tap_player_has_resume(void)
{
    return resume_path[0] != 0;
}
