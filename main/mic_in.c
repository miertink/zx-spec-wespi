/*
 * Spectrum MIC input: GPIO36 (ADC1 channel 0), sampled continuously through
 * I2S0 in built-in ADC mode at ~60 kHz.
 *
 * Signal path. The TK90X MIC output is the ULA output through 10k with 100R
 * to GND: only ~50 mV, low impedance (tape-recorder microphone level). That
 * sits in the ESP32 ADC dead zone near 0 V, so the input is AC coupled and
 * biased into the linear range:
 *
 *   MIC --[1k]--||-- 100 nF --+-- GPIO36
 *                           +-- 100k -- 3V3
 *                           +-- 22k  -- GND      (bias ~0.6 V)
 *
 * With the ADC at 0 dB (~0..1 V full scale) that is ~2400 counts at rest and
 * roughly +-80 counts of signal. Nothing below depends on the absolute level:
 *   - a slow moving average tracks the bias (time constant ~70 ms);
 *   - an edge is a deviation from it beyond +-MIC_HYST counts, held for
 *     MIC_CONFIRM samples (rejects single-sample spikes);
 *   - the time between edges (half periods) drives the SAVE decoder.
 *
 * SAVE format (ROM, 3.5 MHz T-states):
 *   pilot  2168 T  = 619 us per half      sync 667 T + 735 T = 190 + 210 us
 *   bit 0   855 T  = 244 us per half      bit 1 1710 T = 489 us per half
 * Bits are MSB first; each bit is two equal halves. A block is
 * flag, data..., checksum (XOR of all bytes = 0).
 *
 * Diagnostics: level statistics in the log every MIC_WINDOW_MS (when
 * something changes, plus a heartbeat).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "hal/adc_ll.h"

#include "mic_in.h"
#include "tape_out.h"
#include "tape_status.h"

static const char *TAG = "mic_in";

#define MIC_I2S_NUM        I2S_NUM_0          /* only I2S0 supports the built-in ADC */
#define MIC_ADC_CHANNEL    ADC1_CHANNEL_0     /* GPIO36 */
#define MIC_SAMPLE_RATE    60000              /* requested; actual rate is measured */
#define MIC_READ_SAMPLES   600
#define MIC_WINDOW_MS      250
#define MIC_HEARTBEAT_US   30000000

/* Edge detector */
#define MIC_BASE_SHIFT     12                 /* moving average over 2^12 samples, ~70 ms */
#define MIC_HYST           25                 /* counts from the average (~6 mV at 0 dB) */
#define MIC_CONFIRM        2                  /* samples beyond the threshold to accept an edge */

/* SAVE decoder, in microseconds */
#define DEC_PILOT_MIN_US   500                /* pilot half nominal 619 */
#define DEC_PILOT_MAX_US   760
#define DEC_PILOT_MIN      256                /* pilot halves before a sync is accepted */
#define DEC_PILOT_MISS     6                  /* odd halves a pilot may contain (RF noise:
                                                 WiFi bursts reach the ADC) */
#define DEC_SYNC_MAX_US    350                /* sync halves nominal 190 / 210 */
#define DEC_BIT_SPLIT_US   733                /* bit = two halves: 489 (0) vs 977 (1) */
#define DEC_BIT_MAX_US     1400               /* longer pair: end of block */
#define DEC_END_GAP_US     3000               /* no edge for this long: end of block */
#define DEC_GLITCH_US      110                /* shorter half periods are noise (real: >= ~190) */
#define DEC_BLOCK_MAX      (49152 + 2)        /* flag + 48K + checksum: a full 48K SAVE */
#define DEC_BLOCK_MIN      8192               /* fallback if the heap is short */
#define MIC_RX_HOLD_US     2500000            /* after a SAVE block, keep EAR quiet this long */
#define DEC_FLAG_REPLY     0xA5               /* menu replies and load requests: not a SAVE */

static uint16_t buf[MIC_READ_SAMPLES];

/* ---- Spectrum reset detector ----
 * While the Spectrum is on, the MIC line carries a steady background of
 * short spikes (~400 edges/s, from the machine itself; it goes on during
 * LOAD, BREAK, games). It stops only while the machine is off or in the
 * RAM test after a reset. So "silent for RS_SILENT_US, then active again for
 * RS_ACTIVE_US" = the Spectrum was reset or powered up. (Measured on the
 * TK90X, 2026-11-18. LOAD "" itself leaves no mark on MIC there.)
 * Resets measured on the TK90X were silent for 705 ms or more; programs
 * (Pssst) go quiet for 200-600 ms while they run, hence the 650 ms. */
#define RS_WINDOW_US      100000             /* background measured per 100 ms */
#define RS_SILENT_EDGES   3                  /* fewer edges in a window: silent */
#define RS_SILENT_US      650000
#define RS_ACTIVE_US      300000
#define RS_TAPE_GUARD_US  500000             /* our EAR leaks into MIC: idle this long first */
static mic_in_reset_cb_t reset_cb;

/* ---- SAVE decoder ---- */
typedef enum { DEC_IDLE, DEC_PILOT, DEC_SYNC, DEC_DATA } dec_state_t;
static dec_state_t dec_state = DEC_IDLE;
static uint32_t dec_pilot;                  /* consecutive pilot halves seen */
static uint32_t dec_miss;                   /* odd halves inside the current pilot */
static uint32_t dec_half1;                  /* first half of the current bit, 0 = none */
static uint8_t  dec_byte;
static int      dec_bits;
static uint8_t *dec_buf;
static uint32_t dec_len;                    /* bytes received (may exceed DEC_BLOCK_MAX) */
static uint32_t dec_buf_size;
static int64_t  dec_t0;                     /* esp_timer us: sync, start of the data */
static int64_t  dec_last_rx = -10000000;      /* end of the last real (non-echo) block */
static mic_in_block_cb_t block_cb;

static void dec_reset(void)
{
    dec_state = DEC_IDLE;
    dec_pilot = 0;
    dec_miss = 0;
}

/* Our own EAR output leaks into MIC: a block heard while it plays is an echo */
static bool echo_now(void)
{
    return esp_timer_get_time() - tape_out_last_activity_us() < 200000;
}

static bool dec_shown;                      /* this block is on the web page (a SAVE) */

static void dec_finish(const char *why)
{
    /* EAR leaks into MIC: if our own output was active while this block's
     * data came in, it is the echo of what we sent, not a Spectrum SAVE. */
    bool echo = tape_out_last_activity_us() >= dec_t0;
    if (dec_len > 0 && echo) {
        ESP_LOGD(TAG, "echo of our own block: flag 0x%02x, %u bytes", dec_buf[0], (unsigned)dec_len);
    } else if (dec_len > 0) {
        uint32_t kept = dec_len < dec_buf_size ? dec_len : dec_buf_size;
        uint8_t x = 0;
        for (uint32_t i = 0; i < kept; i++) x ^= dec_buf[i];
        bool ok = (kept == dec_len) && x == 0;
        /* a SAVE may go on (data ~1 s after its header): hold the tape. A
         * menu reply or load request is alone, and the answer is awaited */
        if (dec_buf[0] != DEC_FLAG_REPLY) dec_last_rx = esp_timer_get_time();
        ESP_LOGI(TAG, "SAVE block: flag 0x%02x, %u bytes (incl. flag+checksum), checksum %s, %s%s%s",
                 dec_buf[0], (unsigned)dec_len, ok ? "OK" : "BAD", why,
                 dec_bits ? ", trailing bits dropped" : "",
                 kept < dec_len ? ", TOO LONG for the buffer" : "");
        if (ok && dec_buf[0] == 0x00 && dec_len == 19) {
            char name[11];
            memcpy(name, dec_buf + 2, 10);
            name[10] = 0;
            ESP_LOGI(TAG, "  header: type %u, name \"%s\", length %u, param1 %u, param2 %u",
                     dec_buf[1], name,
                     dec_buf[12] | (dec_buf[13] << 8),
                     dec_buf[14] | (dec_buf[15] << 8),
                     dec_buf[16] | (dec_buf[17] << 8));
        }
        if (dec_shown) {
            if (ok && dec_buf[0] == 0x00 && dec_len == 19) {
                char name[11];
                memcpy(name, dec_buf + 2, 10);
                int l = 10;
                while (l > 0 && name[l - 1] == ' ') l--;
                name[l] = 0;
                for (int k = 0; k < l; k++) if (name[k] < 32 || name[k] > 126) name[k] = '?';
                ts_rec_header(name, dec_buf[1], dec_buf[12] | (dec_buf[13] << 8));
            } else {
                ts_rec_end(ok);
            }
        }
        if (ok && block_cb) block_cb(dec_buf, dec_len, dec_t0);
    } else if (!echo) {
        ESP_LOGW(TAG, "SAVE: sync seen but no complete byte (%s)", why);
    }
    dec_shown = false;
    dec_reset();
}

static void dec_edge(uint32_t us);

/* Glitch filter in front of the decoder. Noise on the MIC line makes extra
 * pairs of edges a few samples apart, which split a half period or double a
 * transition and put the bit pairs out of step ("trailing bits dropped",
 * checksum BAD). Half periods are passed on one late; a run of short ones
 * (< DEC_GLITCH_US), with the half before and the one after, is regrouped
 * into 1 to 3 half periods: glitches add edges in pairs, so the number of
 * groups keeps the parity of the pieces, and among those groupings the one
 * whose halves look most like tape halves (sync, 0, 1, pilot) wins. */
#define GL_MAX 8
static uint32_t gl_pend;                    /* half period held back, 0 = none */
static uint32_t gl_short[GL_MAX];
static int gl_nshort;

static uint32_t gl_bad(uint32_t x)          /* distance from a tape half, per mille */
{
    static const uint16_t v[] = { 200, 244, 489, 619 };
    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint32_t d = (x > v[i] ? x - v[i] : v[i] - x) * 1000 / v[i];
        if (d < best) best = d;
    }
    return best;
}

static void dec_feed(uint32_t us)
{
    if (us < DEC_GLITCH_US) {
        if (gl_nshort < GL_MAX) gl_short[gl_nshort++] = us;
        else gl_short[GL_MAX - 1] += us;    /* long noise burst: lump it in */
        return;
    }
    if (!gl_nshort) {
        if (gl_pend) dec_edge(gl_pend);
        gl_pend = us;
        return;
    }
    /* pieces: [held back] shorts... us; sum[i] = total of the first i */
    uint32_t sum[GL_MAX + 3];
    int n = 0;
    sum[0] = 0;
    if (gl_pend) { sum[n + 1] = sum[n] + gl_pend; n++; }
    for (int i = 0; i < gl_nshort; i++) { sum[n + 1] = sum[n] + gl_short[i]; n++; }
    sum[n + 1] = sum[n] + us;
    n++;
    gl_nshort = 0;

    uint32_t best = UINT32_MAX;
    int b1 = n, b2 = n;                     /* cuts: groups [0,b1) [b1,b2) [b2,n) */
    for (int c1 = 1; c1 <= n; c1++) {
        for (int c2 = c1; c2 <= n; c2++) {
            int groups = 1 + (c1 < n) + (c2 < n && c2 > c1);
            if (c2 > c1 && c1 == n) continue;
            if (c2 == c1 && c2 < n) continue;           /* two cuts in one place */
            if ((groups & 1) != (n & 1)) continue;
            uint32_t sc = gl_bad(sum[c1] - sum[0]);
            if (c1 < n) {
                uint32_t e = c2 > c1 ? c2 : n;
                uint32_t s2 = gl_bad(sum[e] - sum[c1]);
                if (s2 > sc) sc = s2;
                if (c2 > c1 && c2 < n) {
                    uint32_t s3 = gl_bad(sum[n] - sum[c2]);
                    if (s3 > sc) sc = s3;
                }
            }
            if (sc < best) { best = sc; b1 = c1; b2 = c2; }
        }
    }
    if (b1 < n) {
        dec_edge(sum[b1]);
        if (b2 > b1 && b2 < n) {
            dec_edge(sum[b2] - sum[b1]);
            gl_pend = sum[n] - sum[b2];
        } else {
            gl_pend = sum[n] - sum[b1];
        }
    } else {
        gl_pend = sum[n];
    }
}

/* End of the signal: pass on what is held back */
static void dec_flush(void)
{
    for (int i = 0; i < gl_nshort; i++) gl_pend += gl_short[i];
    gl_nshort = 0;
    if (gl_pend) dec_edge(gl_pend);
    gl_pend = 0;
}

/* One half period (time between two edges). */
static void dec_edge(uint32_t us)
{
    switch (dec_state) {
    case DEC_IDLE:
    case DEC_PILOT:
        if (us >= DEC_PILOT_MIN_US && us <= DEC_PILOT_MAX_US) {
            dec_pilot++;
            if (dec_state == DEC_IDLE && dec_pilot >= DEC_PILOT_MIN) {
                dec_state = DEC_PILOT;
                ESP_LOGI(TAG, "SAVE: pilot");
                if (!echo_now()) ts_rec_pilot();
            }
        } else if (dec_state == DEC_PILOT && us <= DEC_SYNC_MAX_US) {
            dec_state = DEC_SYNC;               /* first sync half */
        } else if (dec_pilot >= 64 && ++dec_miss <= DEC_PILOT_MISS) {
            /* noise inside a pilot: keep counting it */
        } else {
            dec_reset();
        }
        break;

    case DEC_SYNC:
        if (us <= DEC_SYNC_MAX_US) {            /* second sync half */
            dec_state = DEC_DATA;
            dec_t0 = esp_timer_get_time();
            dec_half1 = 0;
            dec_byte = 0;
            dec_bits = 0;
            dec_len = 0;
        } else if (us >= DEC_PILOT_MIN_US && us <= DEC_PILOT_MAX_US && ++dec_miss <= DEC_PILOT_MISS) {
            dec_state = DEC_PILOT;              /* a noise "sync": the pilot goes on */
            dec_pilot++;
        } else {
            dec_reset();
        }
        break;

    case DEC_DATA: {
        if (dec_half1 == 0) {
            if (us > DEC_BIT_MAX_US) dec_finish("long pulse");
            else dec_half1 = us;
            break;
        }
        uint32_t sum = dec_half1 + us;
        dec_half1 = 0;
        if (sum > DEC_BIT_MAX_US) {
            dec_finish("long pulse");
            break;
        }
        dec_byte = (dec_byte << 1) | (sum > DEC_BIT_SPLIT_US);
        if (++dec_bits == 8) {
            if (dec_len < dec_buf_size) dec_buf[dec_len] = dec_byte;
            dec_len++;
            if (dec_len == 1) {             /* the flag says what this is */
                dec_shown = dec_byte != DEC_FLAG_REPLY && !echo_now();
                if (dec_shown) ts_rec_block(dec_byte);
                else if (dec_byte == DEC_FLAG_REPLY) ts_rec_cancel();
            } else if (dec_shown && (dec_len & 31) == 0) {
                ts_rec_bytes(dec_len);
            }
            if (dec_len > DEC_BLOCK_MAX + 1024) {   /* no SAVE is this long: noise */
                dec_finish("too long");
                break;
            }
            dec_bits = 0;
            dec_byte = 0;
        }
        break;
    }
    }
}

static void mic_in_task(void *arg)
{
    /* The ADC is configured and enabled here rather than in mic_in_init():
     * - enabling takes the ADC lock, a mutex, so any later disable must run
     *   in this same task;
     * - WiFi/PHY start-up reconfigures the SAR ADC, so it is set up only
     *   after WiFi has had time to start. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_12));
    ESP_ERROR_CHECK(adc1_config_channel_atten(MIC_ADC_CHANNEL, ADC_ATTEN_DB_0));
    ESP_ERROR_CHECK(i2s_set_adc_mode(ADC_UNIT_1, MIC_ADC_CHANNEL));
    ESP_ERROR_CHECK(i2s_adc_enable(MIC_I2S_NUM));

    /* In I2S mode IDF 4.4 ignores adc1_config_channel_atten(): its
     * adc_i2s_mode_init() (also run by i2s_adc_enable) writes a pattern table
     * with a hard-coded 12 dB. Overwrite that table entry with 0 dB: ~0..1 V
     * full scale, which suits the ~0.6 V bias and the small MIC signal. */
    adc_digi_pattern_config_t patt = {
        .atten = ADC_ATTEN_DB_0,
        .channel = MIC_ADC_CHANNEL,
        .unit = ADC_NUM_1,
        .bit_width = 12,
    };
    adc_ll_digi_set_pattern_table(ADC_NUM_1, 0, patt);
    adc_ll_digi_clear_pattern_table(ADC_NUM_1);
    ESP_LOGI(TAG, "I2S ADC enabled on GPIO36, 0 dB (~0..1 V), hysteresis +-%d", MIC_HYST);

    /* window statistics */
    uint16_t wmin = 4095, wmax = 0;
    uint64_t wsum = 0;
    uint32_t wcount = 0;
    uint32_t edges = 0;
    uint32_t pulse_min = UINT32_MAX, pulse_max = 0;   /* in samples */

    /* edge detector */
    int32_t base_acc = -1;                  /* moving average << MIC_BASE_SHIFT; -1 = not started */
    int level = -1;                         /* -1 = unknown */
    int confirm = 0;
    uint32_t since_edge = 0;

    /* logging state */
    uint16_t last_mean = 0, last_swing = 0;
    int64_t last_log = 0;

    /* sample rate measurement */
    int64_t rate_t0 = esp_timer_get_time();
    uint32_t rate_count = 0;
    uint32_t rate_hz = MIC_SAMPLE_RATE;
    uint32_t end_gap = (uint64_t)DEC_END_GAP_US * rate_hz / 1000000;

    int64_t win_t0 = esp_timer_get_time();

    /* reset detector state */
    uint32_t rs_edges = 0;                  /* edges in the current window */
    int64_t rs_win_t = 0;
    int64_t rs_silent_since = 0;            /* 0 = line active */
    int64_t rs_active_since = 0;            /* after a long enough silence, 0 = none */

    while (1) {
        size_t bytes = 0;
        i2s_read(MIC_I2S_NUM, buf, sizeof(buf), &bytes, portMAX_DELAY);
        size_t n = bytes / 2;

        for (size_t i = 0; i < n; i++) {
            uint16_t raw = buf[i ^ 1];          /* 16-bit words swapped in pairs; top 4 bits = channel */
            uint16_t v = raw & 0x0FFF;

            if (base_acc < 0) base_acc = (int32_t)v << MIC_BASE_SHIFT;
            base_acc += (int32_t)v - (base_acc >> MIC_BASE_SHIFT);
            int base = base_acc >> MIC_BASE_SHIFT;
            int dev = (int)v - base;

            if (v < wmin) wmin = v;
            if (v > wmax) wmax = v;
            wsum += v;
            wcount++;
            since_edge++;

            /* edge detector: deviation from the average, with hysteresis */
            int want = level;
            if (dev >= MIC_HYST) want = 1;
            else if (dev <= -MIC_HYST) want = 0;
            if (want != level) {
                if (++confirm >= MIC_CONFIRM) {
                    if (level >= 0) {
                        edges++;
                        rs_edges++;
                        if (since_edge < pulse_min) pulse_min = since_edge;
                        if (since_edge > pulse_max) pulse_max = since_edge;
                        dec_feed((uint32_t)((uint64_t)since_edge * 1000000 / rate_hz));
                    }
                    level = want;
                    confirm = 0;
                    since_edge = 0;
                }
            } else {
                confirm = 0;
            }

            /* silence ends a block, or drops a half-seen pilot */
            if (since_edge == end_gap) {
                dec_flush();
                if (dec_state != DEC_IDLE || dec_pilot) {
                    if (dec_state == DEC_DATA) dec_finish("signal stopped");
                    else dec_reset();
                }
            }
        }
        rate_count += n;

        /* reset detector: background silent, then active again */
        int64_t tnow = esp_timer_get_time();
        if (tnow - rs_win_t >= RS_WINDOW_US) {
            bool tape_idle = tnow - tape_out_last_activity_us() > RS_TAPE_GUARD_US
                             && dec_state == DEC_IDLE;
            bool silent = rs_edges < RS_SILENT_EDGES;
            if (!tape_idle) {
                rs_silent_since = 0;
                rs_active_since = 0;
            } else if (silent) {
                if (!rs_silent_since) rs_silent_since = tnow;
                rs_active_since = 0;
            } else {
                if (rs_silent_since && tnow - rs_silent_since >= RS_SILENT_US) {
                    ESP_LOGI(TAG, "MIC line silent for %d ms (Spectrum off or reset)",
                             (int)((tnow - rs_silent_since) / 1000));
                    rs_active_since = tnow;
                }
                rs_silent_since = 0;
                if (rs_active_since && tnow - rs_active_since >= RS_ACTIVE_US) {
                    ESP_LOGI(TAG, "Spectrum reset or powered up");
                    rs_active_since = 0;
                    if (reset_cb) reset_cb();
                }
            }
            rs_edges = 0;
            rs_win_t = tnow;
        }

        int64_t now = esp_timer_get_time();
        if (now - rate_t0 >= 10000000) {
            rate_hz = (uint32_t)((uint64_t)rate_count * 1000000 / (now - rate_t0));
            end_gap = (uint64_t)DEC_END_GAP_US * rate_hz / 1000000;
            ESP_LOGD(TAG, "actual sample rate: %u Hz", (unsigned)rate_hz);
            rate_t0 = now;
            rate_count = 0;
        }

        if (now - win_t0 >= MIC_WINDOW_MS * 1000) {
            uint16_t mean = wcount ? (uint16_t)(wsum / wcount) : 0;
            uint16_t swing = wmax - wmin;
            /* while a block is being decoded, stay quiet: the decoder reports it */
            bool report = dec_state == DEC_IDLE
                       && (edges > 1000                 /* background is ~100 per window */
                           || abs((int)mean - (int)last_mean) > 20
                           || abs((int)swing - (int)last_swing) > 150
                           || now - last_log >= MIC_HEARTBEAT_US);
            if (report) {
                if (edges) {
                    ESP_LOGI(TAG, "min %4u mean %4u max %4u | edges %5u | pulse %u..%u us",
                             wmin, mean, wmax, edges,
                             (unsigned)((uint64_t)pulse_min * 1000000 / rate_hz),
                             (unsigned)((uint64_t)pulse_max * 1000000 / rate_hz));
                } else {
                    ESP_LOGI(TAG, "min %4u mean %4u max %4u | no edges", wmin, mean, wmax);
                }
                last_mean = mean;
                last_swing = swing;
                last_log = now;
            }

            wmin = 4095; wmax = 0; wsum = 0; wcount = 0;
            edges = 0; pulse_min = UINT32_MAX; pulse_max = 0;
            win_t0 = now;
        }
    }
}

void mic_in_set_reset_cb(mic_in_reset_cb_t cb)
{
    reset_cb = cb;
}

void mic_in_set_block_cb(mic_in_block_cb_t cb)
{
    block_cb = cb;
}

bool mic_in_is_receiving(void)
{
    /* a sync/data in progress, a pilot already well under way, or a block
     * just received: the ROM SAVE pauses ~1 s between header and data */
    return dec_state != DEC_IDLE || dec_pilot >= 64
        || esp_timer_get_time() - dec_last_rx < MIC_RX_HOLD_US;
}

void mic_in_init(void)
{
    dec_buf_size = DEC_BLOCK_MAX;
    dec_buf = malloc(dec_buf_size);
    if (!dec_buf) {
        dec_buf_size = DEC_BLOCK_MIN;
        dec_buf = malloc(dec_buf_size);
        ESP_LOGW(TAG, "short of memory: SAVE blocks limited to %u bytes", (unsigned)dec_buf_size);
    }
    ESP_ERROR_CHECK(dec_buf ? ESP_OK : ESP_ERR_NO_MEM);

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN,
        .sample_rate = MIC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = MIC_READ_SAMPLES,
        .use_apll = false,
    };
    ESP_ERROR_CHECK(i2s_driver_install(MIC_I2S_NUM, &cfg, 0, NULL));
    xTaskCreate(mic_in_task, "mic_in", 4096, NULL, 8, NULL);
    ESP_LOGI(TAG, "init: GPIO36 (ADC1 ch0) via I2S0, requested %d Hz", MIC_SAMPLE_RATE);
}
