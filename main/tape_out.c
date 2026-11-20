/*
 * ZX Spectrum tape signal encoder (ESP32 -> Spectrum EAR): a stream of pulses.
 *
 * Why RMT and not I2S/SPI: tape loaders count signal edges. Buffer-based
 * peripherals (I2S, SPI) leave small gaps between DMA transactions where the
 * pin level is not under our control, which can create spurious edges. The
 * RMT peripheral plays a list of (level, duration) items and is refilled by
 * its interrupt while it plays, so a whole stream goes out without a gap.
 *
 * Stream: the player task (any tape format) queues pulses into a ring of
 * 16-bit entries, [level:1][duration in T-states:15], 0 = end of stream.
 * The RMT interrupt (translator below) takes entries from the ring and turns
 * them into RMT items. Entries carry the absolute level, so the producer and
 * the interrupt can never disagree about the polarity.
 *
 * Timing: T-states are converted to RMT ticks of 0.1 us; the fractional
 * remainder is carried over to the next pulse, so there is no accumulated
 * drift. Pulses longer than an RMT item can hold are split into several items
 * at the same level (no edge in between).
 *
 * Ring size: 2048 entries = 4 KB, i.e. ~0.5 s of a standard data block (more
 * in a pilot, less in a fast turbo loader) ahead of the output. The player
 * task only has to keep up on average.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt.h"

#include "tape_out.h"

static const char *TAG = "tape_out";

#define TAPE_RMT_CHANNEL     RMT_CHANNEL_0
#define TAPE_RMT_MEM_BLOCKS  4                      /* 4 x 64 items: refill every 128 items */
#define TAPE_RMT_CLK_DIV     8                      /* 80 MHz APB / 8 = 10 MHz -> 0.1 us per tick */
#define TAPE_RMT_TICK_HZ     (80000000 / TAPE_RMT_CLK_DIV)
#define RMT_MAX_TICKS        32767                  /* 15-bit duration field */
#define UNDERRUN_TICKS       100                    /* 10 us filler when the ring runs dry */

#define RING_SIZE            2048                   /* power of 2 */
#define RING_MASK            (RING_SIZE - 1)
#define E_LEVEL              0x8000
#define E_MAX_T              0x7FFF
#define E_END                0

static uint16_t ring[RING_SIZE];
static volatile uint32_t head;         /* written by the producer only */
static volatile uint32_t tail;         /* written by the interrupt only */

/* Interrupt-side state; reset by the producer before each stream starts. */
static struct {
    uint8_t level;
    uint32_t split;        /* ticks of the current entry still to emit */
    uint32_t acc_q16;      /* fractional ticks carried over, Q16 */
    bool ended;            /* end entry reached */
    uint32_t underruns;
} isr;

/* Producer-side state */
static int prod_level;                /* line level after the last queued entry */
static bool seg_open;                 /* something queued since the last flush */
static bool started;                  /* RMT transmission running */

static uint32_t ticks_per_t_q16;      /* RMT ticks per T-state, Q16 */
static volatile bool abort_req;
static volatile bool sending;
static volatile int64_t last_end_us = -1;
static const uint8_t dummy_src = 0;  /* the driver wants a source buffer; ours is the ring */

/* Next half-item (level, duration in ticks). False at the end of the stream. */
static inline bool IRAM_ATTR next_half(uint32_t *level, uint32_t *ticks)
{
    while (isr.split == 0) {
        uint32_t t = tail;
        if (t == head) {
            /* producer late: hold the line a little longer (distorts this
             * pulse; counted and reported at the flush) */
            isr.underruns++;
            *level = isr.level;
            *ticks = UNDERRUN_TICKS;
            return true;
        }
        uint16_t e = ring[t & RING_MASK];
        __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);
        if (e == E_END) return false;
        isr.level = (e & E_LEVEL) ? 1 : 0;
        uint64_t acc = (uint64_t)(e & E_MAX_T) * ticks_per_t_q16 + isr.acc_q16;
        isr.split = (uint32_t)(acc >> 16);
        isr.acc_q16 = (uint32_t)acc & 0xFFFF;
    }
    uint32_t d = isr.split > RMT_MAX_TICKS ? RMT_MAX_TICKS : isr.split;
    isr.split -= d;
    *level = isr.level;
    *ticks = d;
    return true;
}

/*
 * RMT translator, called from the RMT ISR to refill the item buffer.
 * Contract with the IDF 4.4 legacy driver:
 *  - the driver keeps calling while the source is not reported as fully
 *    translated; our source is a 1-byte dummy, reported done at the end;
 *  - returning fewer items than wanted ends the transmission after them;
 *  - the end is only reported together with fewer items than wanted (else
 *    the driver would replay its buffer), so a stream that ends exactly on
 *    a full buffer reports it at the next call, with no items.
 */
static void IRAM_ATTR tape_translate(const void *src, rmt_item32_t *dest, size_t src_size,
                                     size_t wanted_num, size_t *translated_size, size_t *item_num)
{
    size_t n = 0;
    if (abort_req || isr.ended) {
        *translated_size = src_size;
        *item_num = 0;
        return;
    }
    while (n < wanted_num) {
        uint32_t l0, d0, l1, d1;
        if (!next_half(&l0, &d0)) {
            isr.ended = true;
            break;
        }
        dest[n].level0 = l0;
        dest[n].duration0 = d0;
        if (!next_half(&l1, &d1)) {
            dest[n].level1 = l0;
            dest[n].duration1 = 0;      /* zero duration ends the transmission */
            n++;
            isr.ended = true;
            break;
        }
        dest[n].level1 = l1;
        dest[n].duration1 = d1;
        n++;
    }
    *translated_size = (isr.ended && n < wanted_num) ? src_size : 0;
    *item_num = n;
}

void tape_out_init(void)
{
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX(CONFIG_ZXS_TAPE_OUT_GPIO, TAPE_RMT_CHANNEL);
    cfg.clk_div = TAPE_RMT_CLK_DIV;
    cfg.mem_block_num = TAPE_RMT_MEM_BLOCKS;
    cfg.tx_config.carrier_en = false;
    cfg.tx_config.loop_en = false;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    ESP_ERROR_CHECK(rmt_config(&cfg));
    ESP_ERROR_CHECK(rmt_driver_install(TAPE_RMT_CHANNEL, 0, 0));
    ESP_ERROR_CHECK(rmt_translator_init(TAPE_RMT_CHANNEL, tape_translate));

    ticks_per_t_q16 = (uint32_t)(((uint64_t)TAPE_RMT_TICK_HZ << 16) / CONFIG_ZXS_CPU_CLOCK_HZ);
    prod_level = 0;

    ESP_LOGI(TAG, "init: GPIO %d, Z80 clock %d Hz, %.4f ticks/T-state",
             CONFIG_ZXS_TAPE_OUT_GPIO, CONFIG_ZXS_CPU_CLOCK_HZ, ticks_per_t_q16 / 65536.0);
}

static void start(void)
{
    sending = true;
    started = true;
    ESP_ERROR_CHECK(rmt_write_sample(TAPE_RMT_CHANNEL, &dummy_src, 1, false));
}

static bool push(uint16_t e)
{
    if (!seg_open) {
        /* first entry of a stream: the interrupt is idle, reset its state */
        head = tail = 0;
        memset(&isr, 0, sizeof(isr));
        isr.level = prod_level;
        seg_open = true;
    }
    while (head - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
        if (abort_req) return false;
        if (!started) start();
        vTaskDelay(1);
    }
    ring[head & RING_MASK] = e;
    __atomic_store_n(&head, head + 1, __ATOMIC_RELEASE);
    return true;
}

static bool queue_level(uint32_t t)
{
    if (abort_req) return false;
    uint16_t lv = prod_level ? E_LEVEL : 0;
    while (t > 0) {
        uint32_t d = t > E_MAX_T ? E_MAX_T : t;
        if (!push(lv | d)) return false;
        t -= d;
    }
    return true;
}

bool tape_out_pulse(uint32_t t)
{
    prod_level ^= 1;
    return queue_level(t);
}

bool tape_out_level(int level, uint32_t t)
{
    prod_level = level ? 1 : 0;
    return queue_level(t);
}

int tape_out_get_level(void)
{
    return prod_level;
}

bool tape_out_flush(bool final_edge)
{
    if (!seg_open) return !abort_req;    /* nothing queued: no edge either */
    int idle = final_edge ? prod_level ^ 1 : prod_level;
    /* The idle level only drives the pin once the transmission has ended,
     * so it can be set while the stream is still playing. */
    rmt_set_idle_level(TAPE_RMT_CHANNEL, true, idle ? RMT_IDLE_LEVEL_HIGH : RMT_IDLE_LEVEL_LOW);
    if (!abort_req) push(E_END);         /* on abort the interrupt ends by itself */
    if (!started) start();
    rmt_wait_tx_done(TAPE_RMT_CHANNEL, portMAX_DELAY);

    last_end_us = esp_timer_get_time();
    sending = false;
    started = false;
    seg_open = false;
    prod_level = idle;
    if (isr.underruns) {
        ESP_LOGW(TAG, "output starved %u time(s): timing distorted", (unsigned)isr.underruns);
    }
    return !abort_req;
}

void tape_out_set_idle_level(int level)
{
    prod_level = level ? 1 : 0;
    rmt_set_idle_level(TAPE_RMT_CHANNEL, true, prod_level ? RMT_IDLE_LEVEL_HIGH : RMT_IDLE_LEVEL_LOW);
}

int64_t tape_out_last_activity_us(void)
{
    return sending ? esp_timer_get_time() : last_end_us;
}

void tape_out_abort(void)
{
    abort_req = true;
}

void tape_out_reset_abort(void)
{
    abort_req = false;
}

bool tape_out_aborted(void)
{
    return abort_req;
}
