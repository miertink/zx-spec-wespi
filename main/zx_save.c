/*
 * SAVE to the server: any SAVE on the Spectrum (SAVE "name", ... LINE,
 * CODE, SCREEN$, DATA) is stored as /spiffs/<name>.tap, with no menu and
 * no special command, as with the original ZX-Wespi.
 *
 * The ROM SAVE sends a header block (flag 0x00, 19 bytes with the name)
 * and, about 1 s later, the data block (flag 0xFF). Each header + data pair
 * goes into the .tap named after the header. Pairs that follow within
 * CONFIG_ZXS_SAVE_GROUP_S seconds of the previous one are appended to the
 * same file, so a program saving its loader and its code in sequence ends
 * up as one loadable .tap. Echoes of our own EAR output never get here:
 * mic_in filters them.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "zx_menu.h"
#include "zx_save.h"
#include "tape_status.h"

static const char *TAG = "zx_save";

#define HEADER_LEN       19                 /* flag + 17 + checksum */
#define DATA_WAIT_US     5000000            /* data must start within this after its header */
#define GROUP_US         ((int64_t)CONFIG_ZXS_SAVE_GROUP_S * 1000000)
#define NAME_MAX_CHARS   10

typedef struct {
    uint8_t *data;
    size_t len;
    int64_t start_us;       /* esp_timer time of its sync (start of the data) */
} blk_t;

static QueueHandle_t blk_q;

void zx_save_on_block(const uint8_t *block, size_t len, int64_t start_us)
{
    uint8_t *copy = malloc(len);
    if (!copy) {
        ESP_LOGE(TAG, "out of memory for a %u-byte block: not saved", (unsigned)len);
        return;
    }
    memcpy(copy, block, len);
    blk_t b = { copy, len, start_us };
    if (xQueueSend(blk_q, &b, 0) != pdTRUE) {
        ESP_LOGE(TAG, "queue full: block not saved");
        free(copy);
    }
}

/* File name from the header name: trailing spaces dropped, characters that
 * are awkward in file names or URLs replaced by '_'. */
static void file_name(char *out, size_t size, const uint8_t *hdr)
{
    char n[NAME_MAX_CHARS + 1];
    int len = NAME_MAX_CHARS;
    while (len > 0 && hdr[2 + len - 1] == ' ') len--;
    for (int i = 0; i < len; i++) {
        uint8_t c = hdr[2 + i];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                 || c == '-' || c == '_';
        n[i] = keep ? c : '_';
    }
    n[len] = 0;
    if (len == 0) strcpy(n, "noname");
    if (n[0] == '_') n[0] = '-';            /* '_' files are hidden from the menu */
    snprintf(out, size, "/spiffs/%s.tap", n);
}

static bool put_block(FILE *f, const uint8_t *b, size_t len)
{
    uint8_t lenb[2] = { len & 0xFF, len >> 8 };
    return fwrite(lenb, 1, 2, f) == 2 && fwrite(b, 1, len, f) == len;
}

static void zx_save_task(void *arg)
{
    uint8_t hdr[HEADER_LEN];
    bool have_hdr = false;
    int64_t hdr_time = 0;

    char path[48] = "";
    bool group_open = false;
    int64_t group_last = 0;
    int group_pairs = 0;

    blk_t b;
    while (1) {
        bool got = xQueueReceive(blk_q, &b, pdMS_TO_TICKS(1000)) == pdTRUE;
        int64_t now = esp_timer_get_time();

        /* heap watch (diagnostics): log when it moves, at most every 10 s */
        static int64_t heap_t;
        static size_t heap_last;
        size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (now - heap_t >= 10000000 && (heap + 4096 < heap_last || heap > heap_last + 4096)) {
            ESP_LOGI(TAG, "heap free %u, largest %u, min ever %u", (unsigned)heap,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            heap_t = now;
            heap_last = heap;
        }

        if (group_open && (got ? false : now - group_last >= GROUP_US)) {
            ESP_LOGI(TAG, "saved %s (%d header+data pair(s))", path, group_pairs);
            group_open = false;
            zx_menu_rebuild();
        }
        if (!got) continue;

        if (b.data[0] == 0x00 && b.len == HEADER_LEN) {
            if (have_hdr) ESP_LOGW(TAG, "header without data block, dropped");
            memcpy(hdr, b.data, HEADER_LEN);
            have_hdr = true;
            hdr_time = now;
        } else if (!have_hdr || b.start_us - hdr_time > DATA_WAIT_US) {
            /* measured to the start of the data: a long block ends much later */
            ESP_LOGW(TAG, "block (flag 0x%02x, %u bytes) without a header before it: not saved",
                     b.data[0], (unsigned)b.len);
            ts_rec_error("E_NOHDR");
            have_hdr = false;
        } else {
            have_hdr = false;
            uint16_t expected = hdr[12] | (hdr[13] << 8);
            if (b.len != (size_t)expected + 2) {
                ESP_LOGW(TAG, "data block has %u bytes, header announces %u: saved anyway",
                         (unsigned)b.len - 2, expected);
            }

            bool append = group_open && now - group_last < GROUP_US;
            if (!append) {
                if (group_open) {
                    ESP_LOGI(TAG, "saved %s (%d header+data pair(s))", path, group_pairs);
                    zx_menu_rebuild();
                }
                file_name(path, sizeof(path), hdr);
                group_pairs = 0;
            }
            FILE *f = fopen(path, append ? "ab" : "wb");
            bool ok = f && put_block(f, hdr, HEADER_LEN) && put_block(f, b.data, b.len);
            if (f) ok &= fclose(f) == 0;
            if (ok) {
                static const char *types[] = {"program", "number array", "character array", "bytes"};
                char name[NAME_MAX_CHARS + 1];
                memcpy(name, hdr + 2, NAME_MAX_CHARS);
                name[NAME_MAX_CHARS] = 0;
                ESP_LOGI(TAG, "SAVE \"%s\" (%s, %u bytes) %s %s", name,
                         hdr[1] < 4 ? types[hdr[1]] : "unknown type", expected,
                         append ? "appended to" : "->", path);
                group_open = true;
                group_last = now;
                group_pairs++;
                ts_rec_saved(path);
            } else {
                ESP_LOGE(TAG, "error writing %s (storage full?)", path);
                ts_rec_error("E_UPLOAD");
                if (!append) unlink(path);
                group_open = false;
            }
        }
        free(b.data);
    }
}

void zx_save_init(void)
{
    blk_q = xQueueCreate(4, sizeof(blk_t));
    xTaskCreate(zx_save_task, "zx_save", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready: heap free %u, largest block %u (a 48K SAVE needs ~49K)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
