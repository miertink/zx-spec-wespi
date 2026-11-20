/*
 * Web log: a vprintf hook copies every ESP_LOGx line into a RAM ring buffer
 * (ANSI colour codes removed) while still printing to the UART. The web
 * server sends the buffer on GET /log.
 *
 * Only ESP_LOGx output is captured; plain printf() calls are not.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "weblog.h"

#define LINE_MAX_LEN 160   /* lives on the stack of the task that logs */

static char *ring;
static size_t ring_size;
static size_t head;       /* next write position */
static size_t used;       /* bytes stored, <= ring_size */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t orig_vprintf;
static int mark_count;

static void ring_put(const char *s, size_t n)
{
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < n; i++) {
        ring[head] = s[i];
        head = (head + 1) % ring_size;
    }
    used = (used + n > ring_size) ? ring_size : used + n;
    portEXIT_CRITICAL(&lock);
}

static int weblog_vprintf(const char *fmt, va_list ap)
{
    char line[LINE_MAX_LEN];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);

    if (n > 0) {
        if (n >= (int)sizeof(line)) {              /* cut: keep the line break */
            n = sizeof(line) - 1;
            line[n - 1] = '\n';
        }
        /* strip ANSI colour sequences (ESC [ ... m) in place: k never passes i */
        size_t k = 0;
        for (int i = 0; i < n; i++) {
            if (line[i] == 0x1b && i + 1 < n && line[i + 1] == '[') {
                i += 2;
                while (i < n && line[i] != 'm') i++;
                continue;
            }
            line[k++] = line[i];
        }
        ring_put(line, k);
    }
    return orig_vprintf(fmt, ap);
}

void weblog_init(size_t size)
{
    ring = malloc(size);
    if (!ring) {
        ESP_LOGE("weblog", "cannot allocate %u bytes", (unsigned)size);
        return;
    }
    ring_size = size;
    head = used = 0;
    orig_vprintf = esp_log_set_vprintf(weblog_vprintf);
    ESP_LOGI("weblog", "log ring buffer: %u bytes, served at /log", (unsigned)size);
}

void weblog_mark(void)
{
    ESP_LOGW("MARK", "======== MARK %d ========", ++mark_count);
}

esp_err_t weblog_send(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!ring) return httpd_resp_sendstr(req, "weblog not available\n");

    size_t start, n;
    portENTER_CRITICAL(&lock);
    n = used;
    start = (head + ring_size - used) % ring_size;
    portEXIT_CRITICAL(&lock);

    /* Copy out in small pieces; lines written meanwhile may overwrite the
     * oldest part, which is acceptable for a diagnostic view. */
    char piece[512];
    size_t pos = start;
    while (n > 0) {
        size_t m = n > sizeof(piece) ? sizeof(piece) : n;
        portENTER_CRITICAL(&lock);
        for (size_t i = 0; i < m; i++) {
            piece[i] = ring[pos];
            pos = (pos + 1) % ring_size;
        }
        portEXIT_CRITICAL(&lock);
        if (httpd_resp_send_chunk(req, piece, m) != ESP_OK) return ESP_FAIL;
        n -= m;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
