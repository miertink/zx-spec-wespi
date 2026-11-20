/*
 * Tape image formats, played to the Spectrum EAR through the tape_out pulse
 * stream.
 *
 *  .tap  blocks of [length:2][flag][data][checksum], ROM standard timings.
 *  .tzx  the community standard (v1.20): every signal block (standard,
 *        turbo, pure tone, pulse sequence, pure data, direct recording, CSW,
 *        generalized data), pauses, set level, loops, jumps, call sequences
 *        and "stop the tape". Information blocks are logged or skipped.
 *  .pzx  PULS, DATA, PAUS, STOP (the format tools like tzx2pzx produce).
 *  .csw  v1 and v2, RLE and Z-RLE (zlib, inflated with the ROM's tinfl).
 *
 * Times in all of them are T-states at 3.5 MHz, the unit of tape_out.
 *
 * A stream of pulses is played without gaps until a pause: there tape_out is
 * flushed, the line is held, and before the next block the player waits
 * while the Spectrum is saving (EAR leaks into MIC). A data block is read
 * into RAM before it is played when the heap allows it, so a slow SPIFFS read
 * (garbage collection during a write) cannot starve the output; otherwise it
 * is streamed from the file.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp32/rom/miniz.h"

#include "tape_out.h"
#include "tape_status.h"
#include "tape_file.h"
#include "mic_in.h"

static const char *TAG = "tape_file";

/* ROM standard timings, in T-states */
#define T_PILOT          2168
#define T_SYNC1           667
#define T_SYNC2           735
#define T_BIT0            855
#define T_BIT1           1710
#define PILOT_HEADER     8063   /* pulses, flag byte < 128 */
#define PILOT_DATA       3223   /* pulses, flag byte >= 128 */
#define TAPE_HZ          3500000
#define T_PER_MS         (TAPE_HZ / 1000)

#define HEADER_LEN       19     /* .tap header block: flag + 17 + checksum */
/* fast_start: the ROM loader waits ~1 s after the first edge, then needs 256
 * pilot cycles (~0.32 s): 2600 pulses (~1.6 s) leave margin. After a header
 * the ROM only prints the name before it listens again. */
#define FAST_PILOT       2600
#define FAST_HDR_PAUSE_MS 300
#define TZX_MAX_BLOCKS   4096
#define PRELOAD_MARGIN   (24 * 1024)   /* heap left free when a block is read into RAM */

typedef struct {
    FILE *f;
    tf_opts_t *o;
    bool streaming;      /* pulses queued since the last flush */
    bool error;          /* file truncated / corrupt */
    int nblocks;         /* blocks in the file, 0 = unknown (web status) */
} ctx_t;

static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t le24(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16); }
static inline uint32_t le32(const uint8_t *p) { return le24(p) | ((uint32_t)p[3] << 24); }

static bool stopped(ctx_t *c)
{
    return (c->o->stop && *c->o->stop) || tape_out_aborted();
}

/* Reads exactly n bytes; flags the context on a short read. */
static bool get(ctx_t *c, void *buf, size_t n)
{
    if (fread(buf, 1, n, c->f) == n) return true;
    c->error = true;
    return false;
}

/* Waits `ms`, returning false if a stop was requested. */
static bool pause_ms(ctx_t *c, uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(step) ? pdMS_TO_TICKS(step) : 1);
        ms -= step;
        if (stopped(c)) return false;
    }
    return true;
}

/* Called before queueing a block: if nothing is playing, this is where a new
 * stream starts, so wait while the Spectrum is saving (EAR leaks into MIC:
 * the menu reply goes out in a gap). */
static bool begin(ctx_t *c)
{
    if (c->streaming) return !stopped(c);
    if (mic_in_is_receiving()) {
        ESP_LOGI(TAG, "Spectrum is saving: tape on hold");
        for (int i = 0; i < 300 && !stopped(c) && mic_in_is_receiving(); i++)
            vTaskDelay(pdMS_TO_TICKS(50));
    }
    c->streaming = true;
    return !stopped(c);
}

/* Ends the stream (if any) and plays the pause that follows a block.
 * TZX rule: the last edge is completed by ~1 ms of the opposite level
 * (final edge), then the line goes low for the rest of the pause. */
static bool pause_after(ctx_t *c, uint32_t ms, bool final_edge, bool go_low)
{
    if (ms == 0) return !stopped(c);
    bool ok = tape_out_flush(final_edge && c->streaming);
    c->streaming = false;
    if (!ok) return false;
    if (go_low && tape_out_get_level()) {
        if (!pause_ms(c, 1)) return false;
        tape_out_set_idle_level(0);
        ms = ms > 1 ? ms - 1 : 0;
    }
    return pause_ms(c, ms);
}

static bool flush(ctx_t *c, bool final_edge)
{
    bool ok = tape_out_flush(final_edge && c->streaming);
    c->streaming = false;
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Data reader: a block read into RAM when possible, else streamed. */

typedef struct {
    ctx_t *c;
    uint8_t *mem;
    uint32_t len, pos;
    uint8_t buf[128];
    uint16_t n, i;
    uint8_t xor;
} rd_t;

static void rd_open(rd_t *r, ctx_t *c, uint32_t len)
{
    memset(r, 0, sizeof(*r));
    r->c = c;
    r->len = len;
    if (len > 0 && len + PRELOAD_MARGIN < heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) {
        r->mem = malloc(len);
        if (r->mem && !get(c, r->mem, len)) {
            free(r->mem);
            r->mem = NULL;
            r->len = 0;
        }
    }
}

/* Next byte, -1 at the end (or on a read error). */
static int rd_byte(rd_t *r)
{
    if (r->pos >= r->len) return -1;
    uint8_t b;
    if (r->mem) {
        b = r->mem[r->pos];
    } else {
        if (r->i == r->n) {
            uint32_t want = r->len - r->pos;
            if (want > sizeof(r->buf)) want = sizeof(r->buf);
            if (!get(r->c, r->buf, want)) {
                r->len = r->pos;
                return -1;
            }
            r->n = want;
            r->i = 0;
        }
        b = r->buf[r->i++];
    }
    r->pos++;
    r->xor ^= b;
    if ((r->pos & 31) == 0 || r->pos == r->len) ts_bytes(r->pos);
    return b;
}

static void rd_close(rd_t *r)
{
    free(r->mem);
    r->mem = NULL;
}

/* ------------------------------------------------------------------------ */
/* Signal generators */

static bool gen_tone(uint32_t t, uint32_t count)
{
    while (count--) {
        if (!tape_out_pulse(t)) return false;
    }
    return true;
}

/* Two equal pulses per bit, MSB first; the last byte sends `last_bits`. */
static bool gen_data(rd_t *r, uint32_t t0, uint32_t t1, int last_bits)
{
    for (uint32_t i = 0; i < r->len; i++) {
        int b = rd_byte(r);
        if (b < 0) return false;
        int bits = (i == r->len - 1) ? last_bits : 8;
        for (int k = 0; k < bits; k++, b <<= 1) {
            uint32_t t = (b & 0x80) ? t1 : t0;
            if (!tape_out_pulse(t) || !tape_out_pulse(t)) return false;
        }
    }
    return true;
}

/* Pilot, sync and data of a standard or turbo block. Reads `len` bytes. */
static bool gen_block(ctx_t *c, uint32_t len, uint32_t t_pilot, uint32_t pilot, uint32_t t_s1,
                      uint32_t t_s2, uint32_t t0, uint32_t t1, int last_bits, uint8_t *xor)
{
    rd_t r;
    rd_open(&r, c, len);
    bool ok = begin(c) && gen_tone(t_pilot, pilot) && tape_out_pulse(t_s1) && tape_out_pulse(t_s2)
              && gen_data(&r, t0, t1, last_bits);
    if (xor) *xor = r.xor;
    rd_close(&r);
    return ok && !c->error;
}

static void log_header(const uint8_t *b, uint32_t len, const char *what, int index)
{
    if (len >= HEADER_LEN && b[0] == 0x00) {
        static const char *types[] = {"Program", "Number array", "Character array", "Bytes"};
        char name[11];
        memcpy(name, &b[2], 10);
        name[10] = 0;
        ESP_LOGI(TAG, "block %d: %sheader, %s: \"%s\", %u bytes", index, what,
                 b[1] < 4 ? types[b[1]] : "unknown type", name, le16(b + 12));
    } else {
        ESP_LOGI(TAG, "block %d: %sflag 0x%02X, %u bytes", index, what, len ? b[0] : 0, (unsigned)len);
    }
}

/* Web status: block `index` starts here, `len` data bytes. `desc` is a code
 * the page translates: H|type|name header, F|flag data, G game turbo,
 * U pure data, W direct recording, C CSW, Q generalized data, A data */
static void mark(ctx_t *c, int index, const char *desc, uint32_t len)
{
    ts_block(index, c->nblocks, (uint32_t)ftell(c->f), desc, len);
}

/* Web status for a standard block, from its first bytes */
static void mark_std(ctx_t *c, int index, const uint8_t *head, uint32_t len, const char *what)
{
    char d[40];
    if (len >= HEADER_LEN && head[0] == 0x00) {
        char name[11];
        memcpy(name, &head[2], 10);
        int l = 10;
        while (l > 0 && name[l - 1] == ' ') l--;
        name[l] = 0;
        for (int k = 0; k < l; k++) if (name[k] < 32 || name[k] > 126 || name[k] == '|') name[k] = '?';
        snprintf(d, sizeof(d), "%sH|%u|%s", what, head[1] & 3, name);
    } else {
        snprintf(d, sizeof(d), "%sF|%02X", what, len ? head[0] : 0);
    }
    mark(c, index, d, len);
}

/* Menu plays (fast_start): the menu's own LOAD "" loads the program. One
 * without an autostart line then returns into the menu's line 10, and BASIC
 * reads on in the new program as if it were the menu ("C Nonsense in BASIC,
 * 10:2"). Such a header gets the line after its last one as autostart: the
 * program loads and stops with "0 OK", as after LOAD "" typed by hand.
 * `hdr` = the 19-byte header block (flag first), the program data block
 * (flag first) at file offset `data`. True if changed. */
static bool autostart_fix(ctx_t *c, uint8_t *hdr, long data)
{
    if (hdr[0] != 0x00 || hdr[1] != 0 || le16(hdr + 14) < 32768) return false;
    uint32_t plen = le16(hdr + 16);             /* program, without the variables */
    long back = ftell(c->f);
    int last = -1;
    uint8_t l[4];
    for (uint32_t p = 0; p + 4 <= plen; p += 4 + le16(l + 2)) {
        if (fseek(c->f, data + 1 + p, SEEK_SET) != 0 || fread(l, 1, 4, c->f) != 4) break;
        int num = (l[0] << 8) | l[1];
        if (num > 9999) break;
        last = num;
    }
    fseek(c->f, back, SEEK_SET);
    if (last < 0 || last >= 9999) return false;
    hdr[14] = (last + 1) & 0xFF;
    hdr[15] = (last + 1) >> 8;
    uint8_t x = 0;
    for (int i = 0; i < HEADER_LEN - 1; i++) x ^= hdr[i];
    hdr[HEADER_LEN - 1] = x;
    ESP_LOGI(TAG, "program without autostart: LINE %d added for the menu", last + 1);
    return true;
}

/* A standard block from memory (`len` bytes, flag first, checksum included) */
static bool gen_mem(ctx_t *c, const uint8_t *b, uint32_t len, uint32_t pilot)
{
    rd_t r;
    memset(&r, 0, sizeof(r));
    r.c = c;
    r.mem = (uint8_t *)b;                       /* not freed: rd_close is not called */
    r.len = len;
    return begin(c) && gen_tone(T_PILOT, pilot) && tape_out_pulse(T_SYNC1) && tape_out_pulse(T_SYNC2)
           && gen_data(&r, T_BIT0, T_BIT1, 8);
}

/* ------------------------------------------------------------------------ */
/* .tap */

static tf_result_t play_tap(ctx_t *c)
{
    int index = 0;
    bool after_header = false;   /* the previous block was a header */
    uint8_t lenb[2];
    while (!stopped(c) && fread(lenb, 1, 2, c->f) == 2) {
        uint16_t len = le16(lenb);
        if (len < 2) {
            ESP_LOGE(TAG, "block %d: invalid length %u", index, len);
            return TF_ERROR;
        }
        /* peek at the start of the block for the log */
        uint8_t head[HEADER_LEN];
        uint32_t n = len < HEADER_LEN ? len : HEADER_LEN;
        long pos = ftell(c->f);
        if (!get(c, head, n)) break;
        fseek(c->f, pos, SEEK_SET);
        if (c->o->verbose) log_header(head, len, "", index);
        mark_std(c, index, head, len, "");

        bool is_header = len == HEADER_LEN && head[0] == 0x00;
        bool fast = c->o->fast_start && (index == 0 || (after_header && !is_header));
        uint32_t pilot = c->o->pilot_pulses ? c->o->pilot_pulses
                       : fast ? FAST_PILOT
                       : head[0] < 128 ? PILOT_HEADER : PILOT_DATA;
        uint8_t x = 0;
        bool ok;
        if (c->o->fast_start && index == 0 && is_header
            && autostart_fix(c, head, pos + HEADER_LEN + 2)) {
            ok = fseek(c->f, pos + HEADER_LEN, SEEK_SET) == 0 && gen_mem(c, head, HEADER_LEN, pilot);
        } else {
            ok = gen_block(c, len, T_PILOT, pilot, T_SYNC1, T_SYNC2, T_BIT0, T_BIT1, 8, &x);
        }
        /* A bad checksum is played anyway: some protection schemes rely on
         * it, and the ROM reports "R Tape loading error" if it matters. */
        if (ok && x != 0) ESP_LOGW(TAG, "block %d: checksum error (played anyway)", index);
        if (!ok || !flush(c, true)) break;
        index++;
        after_header = is_header;
        /* after a header the ROM only prints the name: a short pause when it
         * is known to be listening (menu play) or the tape is ours (loop) */
        uint32_t pause = (c->o->fast_start || c->o->pilot_pulses) && is_header ? FAST_HDR_PAUSE_MS
                       : CONFIG_ZXS_TAPE_BLOCK_PAUSE_MS;
        if (!pause_ms(c, pause)) break;
    }
    if (c->error) {
        ESP_LOGE(TAG, "file truncated after %d block(s)", index);
        return TF_ERROR;
    }
    if (stopped(c)) return TF_STOPPED;
    return index > 0 ? TF_DONE : TF_ERROR;
}

/* ------------------------------------------------------------------------ */
/* CSW pulses (TZX block 0x18 and .csw files): RLE, optionally zlib-packed */

typedef struct {
    ctx_t *c;
    uint32_t left;                 /* compressed/raw bytes left in the file */
    bool z;
    tinfl_decompressor *inf;
    uint8_t *dict;                 /* TINFL_LZ_DICT_SIZE output window */
    uint32_t dict_wr, dict_rd;     /* positions in the window, wrapping */
    uint8_t in[256];
    size_t in_n, in_i;
    bool z_done;
} csw_t;

static bool csw_open(csw_t *s, ctx_t *c, uint32_t len, bool z)
{
    memset(s, 0, sizeof(*s));
    s->c = c;
    s->left = len;
    s->z = z;
    if (!z) return true;
    s->inf = malloc(sizeof(tinfl_decompressor));
    s->dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!s->inf || !s->dict) {
        ESP_LOGE(TAG, "CSW Z-RLE: not enough memory to inflate (needs ~44 KB)");
        free(s->inf);
        free(s->dict);
        return false;
    }
    tinfl_init(s->inf);
    return true;
}

static void csw_close(csw_t *s)
{
    if (s->z) {
        free(s->inf);
        free(s->dict);
    }
}

static int csw_byte(csw_t *s)
{
    if (!s->z) {
        if (s->in_i == s->in_n) {
            if (s->left == 0) return -1;
            size_t want = s->left > sizeof(s->in) ? sizeof(s->in) : s->left;
            if (!get(s->c, s->in, want)) return -1;
            s->left -= want;
            s->in_n = want;
            s->in_i = 0;
        }
        return s->in[s->in_i++];
    }
    while (s->dict_rd == s->dict_wr) {
        if (s->z_done) return -1;
        if (s->in_i == s->in_n && s->left > 0) {
            size_t want = s->left > sizeof(s->in) ? sizeof(s->in) : s->left;
            if (!get(s->c, s->in, want)) return -1;
            s->left -= want;
            s->in_n = want;
            s->in_i = 0;
        }
        size_t in_bytes = s->in_n - s->in_i;
        size_t ofs = s->dict_wr & (TINFL_LZ_DICT_SIZE - 1);
        size_t out_bytes = TINFL_LZ_DICT_SIZE - ofs;
        tinfl_status st = tinfl_decompress(s->inf, s->in + s->in_i, &in_bytes, s->dict, s->dict + ofs,
                                           &out_bytes, TINFL_FLAG_PARSE_ZLIB_HEADER
                                           | (s->left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        s->in_i += in_bytes;
        s->dict_wr += out_bytes;
        if (st < TINFL_STATUS_DONE) {
            ESP_LOGE(TAG, "CSW Z-RLE: corrupt data (%d)", st);
            s->c->error = true;
            return -1;
        }
        if (st == TINFL_STATUS_DONE) s->z_done = true;
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && s->left == 0 && in_bytes == 0 && out_bytes == 0) {
            s->z_done = true;
        }
    }
    return s->dict[s->dict_rd++ & (TINFL_LZ_DICT_SIZE - 1)];
}

/* Plays CSW RLE pulses: each a run of samples at `rate` Hz, alternating
 * levels, the first one at `first_level` (or an edge if -1). */
static bool gen_csw(ctx_t *c, csw_t *s, uint32_t rate, int first_level)
{
    uint64_t acc = 0;           /* sample count -> T-states, remainder carried */
    bool first = true;
    while (1) {
        int b = csw_byte(s);
        if (b < 0) break;
        uint32_t n = b;
        if (n == 0) {
            uint8_t w[4];
            for (int i = 0; i < 4; i++) {
                int x = csw_byte(s);
                if (x < 0) return false;
                w[i] = x;
            }
            n = le32(w);
        }
        acc += (uint64_t)n * TAPE_HZ;
        uint32_t t = acc / rate;
        acc %= rate;
        bool ok;
        if (first && first_level >= 0) ok = tape_out_level(first_level, t);
        else ok = tape_out_pulse(t);
        first = false;
        if (!ok) return false;
    }
    return !c->error;
}

/* ------------------------------------------------------------------------ */
/* .tzx */

/* Length of the block body after the ID byte; the file is at the body. */
static bool tzx_body_len(ctx_t *c, uint8_t id, uint32_t *len)
{
    uint8_t h[0x14];
    long pos = ftell(c->f);
    uint32_t l;
#define PEEK(n) do { if (!get(c, h, (n))) return false; } while (0)
    switch (id) {
    case 0x10: PEEK(4);    l = 4 + le16(h + 2); break;
    case 0x11: PEEK(0x12); l = 0x12 + le24(h + 0x0F); break;
    case 0x12: l = 4; break;
    case 0x13: PEEK(1);    l = 1 + 2 * h[0]; break;
    case 0x14: PEEK(0x0A); l = 0x0A + le24(h + 7); break;
    case 0x15: PEEK(8);    l = 8 + le24(h + 5); break;
    case 0x20: case 0x23: case 0x24: l = 2; break;
    case 0x21: case 0x30: PEEK(1); l = 1 + h[0]; break;
    case 0x22: case 0x25: case 0x27: l = 0; break;
    case 0x26: PEEK(2);    l = 2 + 2 * le16(h); break;
    case 0x28: case 0x32: PEEK(2); l = 2 + le16(h); break;
    case 0x31: PEEK(2);    l = 2 + h[1]; break;
    case 0x33: PEEK(1);    l = 1 + 3 * h[0]; break;
    case 0x34: l = 8; break;
    case 0x35: PEEK(0x14); l = 0x14 + le32(h + 0x10); break;
    case 0x40: PEEK(4);    l = 4 + le24(h + 1); break;
    case 0x5A: l = 9; break;
    default:   PEEK(4);    l = 4 + le32(h); break;   /* 0x16..0x19, 0x2A, 0x2B, unknown: length first */
    }
#undef PEEK
    fseek(c->f, pos, SEEK_SET);
    *len = l;
    return true;
}

/* Generalized data block (0x19): symbols defined as pulse sequences. */
static bool tzx_symbol(const uint8_t *def, int np)
{
    for (int j = 0; j < np; j++) {
        uint16_t d = le16(def + 1 + 2 * j);
        if (d == 0) break;
        bool ok;
        if (j > 0) {
            ok = tape_out_pulse(d);
        } else {
            switch (def[0] & 3) {
            case 0:  ok = tape_out_pulse(d); break;                         /* edge */
            case 1:  ok = tape_out_level(tape_out_get_level(), d); break;   /* no edge */
            case 2:  ok = tape_out_level(0, d); break;
            default: ok = tape_out_level(1, d); break;
            }
        }
        if (!ok) return false;
    }
    return true;
}

static bool tzx_gdb(ctx_t *c, uint32_t blen)
{
    uint8_t h[0x0E];
    if (!get(c, h, sizeof(h))) return false;
    uint16_t pause = le16(h);
    uint32_t totp = le32(h + 2), totd = le32(h + 8);
    int npp = h[6], asp = h[7] ? h[7] : 256;
    int npd = h[12], asd = h[13] ? h[13] : 256;
    bool ok = begin(c);

    if (ok && totp > 0) {
        size_t dlen = (size_t)asp * (1 + 2 * npp);
        uint8_t *defs = malloc(dlen);
        if (!defs || !get(c, defs, dlen)) {
            free(defs);
            ESP_LOGE(TAG, "generalized data block: pilot table too big or truncated");
            return false;
        }
        for (uint32_t i = 0; ok && i < totp; i++) {
            uint8_t p[3];
            if (!get(c, p, 3)) { ok = false; break; }
            if (p[0] >= asp) continue;
            for (uint16_t k = le16(p + 1); ok && k > 0; k--) ok = tzx_symbol(defs + p[0] * (1 + 2 * npp), npp);
        }
        free(defs);
    }
    if (ok && totd > 0) {
        size_t dlen = (size_t)asd * (1 + 2 * npd);
        uint8_t *defs = malloc(dlen);
        if (!defs || !get(c, defs, dlen)) {
            free(defs);
            ESP_LOGE(TAG, "generalized data block: data table too big or truncated");
            return false;
        }
        int nb = 0;
        while ((1 << nb) < asd) nb++;
        rd_t r;
        rd_open(&r, c, (uint32_t)(((uint64_t)totd * nb + 7) / 8));
        int byte = 0, bits = 0;
        for (uint32_t i = 0; ok && i < totd; i++) {
            int sym = 0;
            for (int k = 0; k < nb; k++) {
                if (bits == 0) {
                    byte = rd_byte(&r);
                    if (byte < 0) { ok = false; break; }
                    bits = 8;
                }
                sym = (sym << 1) | ((byte >> 7) & 1);
                byte <<= 1;
                bits--;
            }
            if (ok && sym < asd) ok = tzx_symbol(defs + sym * (1 + 2 * npd), npd);
        }
        rd_close(&r);
        free(defs);
    }
    (void)blen;
    return ok && pause_after(c, pause, true, true);
}

static void log_text(const char *what, const uint8_t *s, int n)
{
    char t[81];
    if (n > 80) n = 80;
    for (int i = 0; i < n; i++) t[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : ' ';
    t[n] = 0;
    ESP_LOGI(TAG, "%s: %s", what, t);
}

static tf_result_t play_tzx(ctx_t *c)
{
    uint8_t sig[10];
    if (!get(c, sig, 10) || memcmp(sig, "ZXTape!\x1A", 8) != 0) {
        ESP_LOGE(TAG, "not a TZX file");
        return TF_ERROR;
    }
    if (c->o->verbose) ESP_LOGI(TAG, "TZX v%u.%02u", sig[8], sig[9]);

    /* Index the blocks: jumps, loops and calls refer to block numbers. */
    uint32_t *ofs = NULL;
    int nblk = 0, cap = 0;
    while (1) {
        long pos = ftell(c->f);
        uint8_t id;
        if (fread(&id, 1, 1, c->f) != 1) break;
        uint32_t len;
        if (!tzx_body_len(c, id, &len)) {
            ESP_LOGW(TAG, "block %d (0x%02X) truncated, ignored", nblk, id);
            c->error = false;
            break;
        }
        if (nblk == cap) {
            if (cap == TZX_MAX_BLOCKS) {
                ESP_LOGW(TAG, "more than %d blocks, the rest is ignored", TZX_MAX_BLOCKS);
                break;
            }
            cap = cap ? cap * 2 : 64;
            uint32_t *n = realloc(ofs, cap * sizeof(*ofs));
            if (!n) {
                free(ofs);
                ESP_LOGE(TAG, "out of memory");
                return TF_ERROR;
            }
            ofs = n;
        }
        ofs[nblk++] = pos;
        if (fseek(c->f, len, SEEK_CUR) != 0) break;
    }
    if (nblk == 0) {
        free(ofs);
        ESP_LOGE(TAG, "TZX without blocks");
        return TF_ERROR;
    }

    c->nblocks = nblk;
    int i = 0;
    if (c->o->start_offset) {
        while (i < nblk && ofs[i] < c->o->start_offset) i++;
    }
    int loop_start = -1, loop_left = 0;
    uint8_t *calls = NULL;
    int ncalls = 0, call_i = 0, call_from = -1;
    tf_result_t res = TF_DONE;

    while (i < nblk && !stopped(c)) {
        fseek(c->f, ofs[i], SEEK_SET);
        uint8_t id;
        if (!get(c, &id, 1)) break;
        int next = i + 1;
        uint8_t h[0x12];
        bool ok = true;

        switch (id) {
        case 0x10: {                                         /* standard speed data */
            if (!get(c, h, 4)) { ok = false; break; }
            uint16_t len = le16(h + 2);
            uint8_t head[HEADER_LEN];
            uint32_t n = len < HEADER_LEN ? len : HEADER_LEN;
            long pos = ftell(c->f);
            if (!get(c, head, n)) { ok = false; break; }
            fseek(c->f, pos, SEEK_SET);
            if (c->o->verbose) log_header(head, len, "", i);
            mark_std(c, i, head, len, "");
            ok = (len == 0 || gen_block(c, len, T_PILOT, head[0] < 128 ? PILOT_HEADER : PILOT_DATA,
                                      T_SYNC1, T_SYNC2, T_BIT0, T_BIT1, 8, NULL))
                 && pause_after(c, le16(h), true, true);
            break;
        }
        case 0x11: {                                         /* turbo speed data */
            if (!get(c, h, 0x12)) { ok = false; break; }
            uint32_t len = le24(h + 0x0F);
            if (c->o->verbose) ESP_LOGI(TAG, "block %d: turbo data, %u bytes", i, (unsigned)len);
            mark(c, i, "G", len);
            int last = h[0x0C] ? h[0x0C] : 8;
            ok = (len == 0 || gen_block(c, len, le16(h), le16(h + 0x0A), le16(h + 2), le16(h + 4),
                                        le16(h + 6), le16(h + 8), last, NULL))
                 && pause_after(c, le16(h + 0x0D), true, true);
            break;
        }
        case 0x12:                                           /* pure tone */
            if (!get(c, h, 4)) { ok = false; break; }
            ok = begin(c) && gen_tone(le16(h), le16(h + 2));
            break;
        case 0x13: {                                         /* pulse sequence */
            if (!get(c, h, 1)) { ok = false; break; }
            ok = begin(c);
            for (int k = h[0]; ok && k > 0; k--) {
                uint8_t p[2];
                ok = get(c, p, 2) && tape_out_pulse(le16(p));
            }
            break;
        }
        case 0x14: {                                         /* pure data */
            if (!get(c, h, 0x0A)) { ok = false; break; }
            uint32_t len = le24(h + 7);
            int last = h[4] ? h[4] : 8;
            mark(c, i, "U", len);
            rd_t r;
            rd_open(&r, c, len);
            ok = begin(c) && gen_data(&r, le16(h), le16(h + 2), last);
            rd_close(&r);
            ok = ok && pause_after(c, le16(h + 5), true, true);
            break;
        }
        case 0x15: {                                         /* direct recording */
            if (!get(c, h, 8)) { ok = false; break; }
            uint32_t tps = le16(h), len = le24(h + 5);
            int last = h[4] ? h[4] : 8;
            mark(c, i, "W", len);
            rd_t r;
            rd_open(&r, c, len);
            ok = begin(c);
            int level = -1;
            uint32_t run = 0;
            for (uint32_t k = 0; ok && k < len; k++) {
                int b = rd_byte(&r);
                if (b < 0) { ok = false; break; }
                int bits = (k == len - 1) ? last : 8;
                for (int q = 0; ok && q < bits; q++, b <<= 1) {
                    int lv = (b >> 7) & 1;
                    if (lv != level && run) {
                        ok = tape_out_level(level, run * tps);
                        run = 0;
                    }
                    level = lv;
                    run++;
                }
            }
            if (ok && run) ok = tape_out_level(level, run * tps);
            rd_close(&r);
            ok = ok && pause_after(c, le16(h + 2), false, true);
            break;
        }
        case 0x18: {                                         /* CSW recording */
            if (!get(c, h, 0x0E)) { ok = false; break; }
            uint32_t blen = le32(h), rate = le24(h + 6);
            uint8_t comp = h[9];
            csw_t s;
            if (rate == 0 || blen < 10 || (comp != 1 && comp != 2) || !csw_open(&s, c, blen - 10, comp == 2)) {
                if (comp != 1 && comp != 2) ESP_LOGW(TAG, "block %d: unknown CSW compression %u, skipped", i, comp);
                break;
            }
            mark(c, i, "C", 0);
            ok = begin(c) && gen_csw(c, &s, rate, -1);
            csw_close(&s);
            /* final edge: a recording cut right after its last pulse still
             * ends the last bit; harmless if it ends in silence */
            ok = ok && pause_after(c, le16(h + 4), true, true);
            break;
        }
        case 0x19:                                           /* generalized data */
            if (!get(c, h, 4)) { ok = false; break; }
            mark(c, i, "Q", 0);
            ok = tzx_gdb(c, le32(h));
            break;
        case 0x20:                                           /* pause / stop the tape */
            if (!get(c, h, 2)) { ok = false; break; }
            if (le16(h) > 0) {
                ok = pause_after(c, le16(h), true, true);
                break;
            }
            /* 0 = stop the tape */
            __attribute__((fallthrough));
        case 0x2A:                                           /* stop the tape if in 48K mode */
            if (!flush(c, true)) { ok = false; break; }
            if (c->o->ignore_stops) {
                next = nblk;
                break;
            }
            c->o->resume_offset = next < nblk ? ofs[next] : 0;
            if (next < nblk) {
                ESP_LOGI(TAG, "block %d: \"stop the tape\": play again to continue", i);
                res = TF_TAPE_STOP;
            }
            next = nblk;
            break;
        case 0x21:                                           /* group start */
            if (c->o->verbose && get(c, h, 1)) {
                uint8_t s[255];
                if (get(c, s, h[0])) log_text("group", s, h[0]);
            }
            break;
        case 0x23:                                           /* jump to block */
            if (!get(c, h, 2)) { ok = false; break; }
            next = i + (int16_t)le16(h);
            break;
        case 0x24:                                           /* loop start */
            if (!get(c, h, 2)) { ok = false; break; }
            loop_start = next;
            loop_left = le16(h);
            break;
        case 0x25:                                           /* loop end */
            if (loop_start >= 0 && --loop_left > 0) next = loop_start;
            else loop_start = -1;
            break;
        case 0x26: {                                         /* call sequence */
            if (!get(c, h, 2)) { ok = false; break; }
            int n = le16(h);
            free(calls);
            calls = malloc(2 * n + 1);
            if (!calls || !get(c, calls, 2 * n)) { ok = false; break; }
            ncalls = n;
            call_i = 0;
            call_from = i;
            if (n > 0) next = i + (int16_t)le16(calls);
            break;
        }
        case 0x27:                                           /* return from sequence */
            if (call_from >= 0 && ++call_i < ncalls) {
                next = call_from + (int16_t)le16(calls + 2 * call_i);
            } else if (call_from >= 0) {
                next = call_from + 1;
                call_from = -1;
            }
            break;
        case 0x2B:                                           /* set signal level */
            if (!get(c, h, 5)) { ok = false; break; }
            tape_out_level(h[4] ? 1 : 0, 0);
            if (!c->streaming) tape_out_set_idle_level(h[4] ? 1 : 0);
            break;
        case 0x30: {                                         /* text description */
            if (c->o->verbose && get(c, h, 1)) {
                uint8_t s[255];
                if (get(c, s, h[0])) log_text("text", s, h[0]);
            }
            break;
        }
        case 0x31: {                                         /* message */
            if (get(c, h, 2)) {
                uint8_t s[255];
                if (get(c, s, h[1])) log_text("message", s, h[1]);
            }
            break;
        }
        case 0x32: {                                         /* archive info: log the title */
            if (!c->o->verbose || !get(c, h, 3)) break;
            for (int k = h[2]; k > 0; k--) {
                uint8_t t[2], s[255];
                if (!get(c, t, 2) || !get(c, s, t[1])) break;
                if (t[0] == 0x00) log_text("title", s, t[1]);
                if (t[0] == 0x02) log_text("author", s, t[1]);
            }
            break;
        }
        case 0x16: case 0x17: case 0x34: case 0x40:
            ESP_LOGW(TAG, "block %d: obsolete block 0x%02X skipped", i, id);
            break;
        default:                                             /* info, glue, select, unknown: skip */
            break;
        }

        if (c->error) ok = false;
        if (!ok) {
            if (!stopped(c)) {
                ESP_LOGE(TAG, "block %d (0x%02X): %s", i, id, c->error ? "truncated" : "cannot play");
                res = TF_ERROR;
            }
            break;
        }
        if (next < 0 || next > nblk) {
            ESP_LOGE(TAG, "block %d: jump outside the file", i);
            res = TF_ERROR;
            break;
        }
        i = next;
    }
    if (!stopped(c)) flush(c, true);
    free(calls);
    free(ofs);
    if (stopped(c)) return TF_STOPPED;
    return res;
}

/* ------------------------------------------------------------------------ */
/* .pzx */

static uint32_t pzx_dur(ctx_t *c, uint32_t *left, bool *ok)
{
    uint8_t w[2];
    if (*left < 2 || !get(c, w, 2)) { *ok = false; return 0; }
    *left -= 2;
    return le16(w);
}

static tf_result_t play_pzx(ctx_t *c)
{
    uint8_t h[8];
    if (!get(c, h, 8) || memcmp(h, "PZXT", 4) != 0) {
        ESP_LOGE(TAG, "not a PZX file");
        return TF_ERROR;
    }
    fseek(c->f, le32(h + 4), SEEK_CUR);
    if (c->o->start_offset) fseek(c->f, c->o->start_offset, SEEK_SET);

    tf_result_t res = TF_DONE;
    int index = 0;
    while (!stopped(c) && fread(h, 1, 8, c->f) == 8) {
        uint32_t size = le32(h + 4);
        long next = ftell(c->f) + size;
        bool ok = true;

        if (memcmp(h, "PULS", 4) == 0) {
            uint32_t left = size;
            int level = 0;                              /* low at the start of the block */
            ok = begin(c);
            while (ok && left > 0) {
                uint32_t count = 1, d = pzx_dur(c, &left, &ok);
                if (ok && d > 0x8000) {
                    count = d & 0x7FFF;
                    d = pzx_dur(c, &left, &ok);
                }
                if (ok && d >= 0x8000) d = ((d & 0x7FFF) << 16) | pzx_dur(c, &left, &ok);
                for (; ok && count > 0; count--) {
                    ok = tape_out_level(level, d);
                    level ^= 1;
                }
            }
        } else if (memcmp(h, "DATA", 4) == 0) {
            uint8_t d[8];
            ok = size >= 8 && get(c, d, 8);
            uint32_t bits = ok ? le32(d) & 0x7FFFFFFF : 0;
            int level = ok ? d[3] >> 7 : 0;
            uint16_t tail = le16(d + 4);
            int p0 = d[6], p1 = d[7];
            uint16_t s[2][255];
            for (int k = 0; ok && k < p0; k++) { uint8_t w[2]; ok = get(c, w, 2); s[0][k] = le16(w); }
            for (int k = 0; ok && k < p1; k++) { uint8_t w[2]; ok = get(c, w, 2); s[1][k] = le16(w); }
            mark(c, index, "A", (bits + 7) / 8);
            rd_t r;
            rd_open(&r, c, (bits + 7) / 8);
            ok = ok && begin(c);
            int byte = 0;
            for (uint32_t k = 0; ok && k < bits; k++) {
                if ((k & 7) == 0 && (byte = rd_byte(&r)) < 0) { ok = false; break; }
                int bit = (byte >> (7 - (k & 7))) & 1;
                int np = bit ? p1 : p0;
                for (int q = 0; ok && q < np; q++) {
                    ok = tape_out_level(level, s[bit][q]);
                    level ^= 1;
                }
            }
            rd_close(&r);
            if (ok && tail) ok = tape_out_level(level, tail);
        } else if (memcmp(h, "PAUS", 4) == 0) {
            uint8_t d[4];
            ok = size >= 4 && get(c, d, 4);
            uint32_t t = le32(d) & 0x7FFFFFFF;
            int level = d[3] >> 7;
            if (ok && t >= 50 * T_PER_MS) {             /* a real gap: end the stream there */
                ok = flush(c, false);
                if (ok) tape_out_set_idle_level(level);
                ok = ok && pause_ms(c, t / T_PER_MS);
            } else if (ok) {
                ok = begin(c) && tape_out_level(level, t);
            }
        } else if (memcmp(h, "STOP", 4) == 0) {
            ok = flush(c, false);
            if (ok && !c->o->ignore_stops) {
                c->o->resume_offset = next;
                ESP_LOGI(TAG, "block %d: \"stop the tape\": play again to continue", index);
                res = TF_TAPE_STOP;
                break;
            }
        } else if (memcmp(h, "BRWS", 4) == 0 && c->o->verbose) {
            uint8_t s[80];
            uint32_t n = size < sizeof(s) ? size : sizeof(s);
            if (get(c, s, n)) log_text("browse", s, n);
        }
        if (c->error) ok = false;
        if (!ok) {
            if (!stopped(c)) {
                ESP_LOGE(TAG, "block %d (%.4s): %s", index, h, c->error ? "truncated" : "cannot play");
                res = TF_ERROR;
            }
            break;
        }
        fseek(c->f, next, SEEK_SET);
        index++;
    }
    if (!stopped(c)) flush(c, true);
    if (stopped(c)) return TF_STOPPED;
    return res;
}

/* ------------------------------------------------------------------------ */
/* .csw */

static tf_result_t play_csw(ctx_t *c)
{
    uint8_t h[0x34];
    if (!get(c, h, 0x20) || memcmp(h, "Compressed Square Wave\x1A", 23) != 0) {
        ESP_LOGE(TAG, "not a CSW file");
        return TF_ERROR;
    }
    uint32_t rate, data;
    uint8_t comp, flags;
    if (h[0x17] == 1) {
        rate = le16(h + 0x19);
        comp = h[0x1B];
        flags = h[0x1C];
        data = 0x20;
    } else {
        if (!get(c, h + 0x20, 0x14)) return TF_ERROR;
        rate = le32(h + 0x19);
        comp = h[0x21];
        flags = h[0x22];
        data = 0x34 + h[0x23];
    }
    fseek(c->f, 0, SEEK_END);
    long end = ftell(c->f);
    fseek(c->f, data, SEEK_SET);
    if (rate == 0 || (comp != 1 && comp != 2) || end <= (long)data) {
        ESP_LOGE(TAG, "CSW: unsupported (rate %u, compression %u)", (unsigned)rate, comp);
        return TF_ERROR;
    }
    if (c->o->verbose) {
        ESP_LOGI(TAG, "CSW v%u, %u Hz, %s", h[0x17], (unsigned)rate, comp == 2 ? "Z-RLE" : "RLE");
    }
    csw_t s;
    mark(c, 0, "C", 0);
    if (!csw_open(&s, c, end - data, comp == 2)) return TF_ERROR;
    bool ok = begin(c) && gen_csw(c, &s, rate, flags & 1);
    csw_close(&s);
    if (!stopped(c)) flush(c, true);
    if (stopped(c)) return TF_STOPPED;
    return ok ? TF_DONE : TF_ERROR;
}

/* ------------------------------------------------------------------------ */

typedef tf_result_t (*player_t)(ctx_t *c);

static const struct {
    const char *ext;
    player_t play;
} formats[] = {
    { ".tap", play_tap },
    { ".tzx", play_tzx },
    { ".pzx", play_pzx },
    { ".csw", play_csw },
};

static player_t player_for(const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        size_t e = strlen(formats[i].ext);
        if (n > e && strcasecmp(name + n - e, formats[i].ext) == 0) return formats[i].play;
    }
    return NULL;
}

/* Copies a Spectrum string for display: control codes dropped with their
 * parameters (INK..OVER take 1 byte, AT and TAB take 2), tokens and other
 * non-ASCII bytes dropped, spaces collapsed and trimmed. */
static void clean_text(char *out, size_t size, const uint8_t *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size; i++) {
        uint8_t ch = s[i];
        if (ch >= 0x10 && ch <= 0x15) { i += 1; continue; }
        if (ch == 0x16 || ch == 0x17) { i += 2; continue; }
        if (ch < 32 || ch > 126) continue;
        if (ch == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = ch;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static bool title_tap(FILE *f, char *out, size_t size)
{
    uint8_t b[21];
    if (fread(b, 1, 21, f) != 21 || le16(b) != HEADER_LEN || b[2] != 0x00) return false;
    clean_text(out, size, b + 4, 10);
    return out[0] != 0;
}

static bool title_tzx(FILE *f, char *out, size_t size)
{
    uint8_t sig[10];
    if (fread(sig, 1, 10, f) != 10 || memcmp(sig, "ZXTape!\x1A", 8) != 0) return false;
    tf_opts_t o = {0};
    ctx_t c = { .f = f, .o = &o };
    char header[16] = "";
    for (int n = 0; n < 32; n++) {                  /* the info is near the start */
        uint8_t id;
        uint32_t len;
        if (fread(&id, 1, 1, f) != 1 || !tzx_body_len(&c, id, &len)) break;
        long body = ftell(f);
        if (id == 0x32) {
            uint8_t h[3];
            if (!get(&c, h, 3)) break;
            for (int k = h[2]; k > 0; k--) {
                uint8_t t[2], txt[255];
                if (!get(&c, t, 2) || !get(&c, txt, t[1])) break;
                if (t[0] == 0x00) {
                    clean_text(out, size, txt, t[1]);
                    if (out[0]) return true;
                }
            }
        } else if (id == 0x10 && !header[0]) {
            uint8_t h[4 + HEADER_LEN];
            if (get(&c, h, sizeof(h)) && le16(h + 2) == HEADER_LEN && h[4] == 0x00) {
                clean_text(header, sizeof(header), h + 6, 10);
            }
        }
        if (fseek(f, body + len, SEEK_SET) != 0) break;
    }
    strlcpy(out, header, size);
    return out[0] != 0;
}

static bool title_pzx(FILE *f, char *out, size_t size)
{
    uint8_t h[10], t[64];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "PZXT", 4) != 0) return false;
    uint32_t n = le32(h + 4);
    if (n <= 2) return false;
    n -= 2;                                         /* after major, minor: the title */
    if (n > sizeof(t)) n = sizeof(t);
    if (fread(t, 1, n, f) != n) return false;
    size_t len = strnlen((const char *)t, n);
    clean_text(out, size, t, len);
    return out[0] != 0;
}

bool tape_file_title(const char *path, char *out, size_t size)
{
    if (size == 0) return false;
    out[0] = 0;
    player_t play = player_for(path);
    if (!play || play == play_csw) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = play == play_tap ? title_tap(f, out, size)
            : play == play_tzx ? title_tzx(f, out, size)
            : title_pzx(f, out, size);
    fclose(f);
    if (!ok) out[0] = 0;
    return ok;
}

bool tape_file_supported(const char *name)
{
    return player_for(name) != NULL;
}

bool tape_file_play_mem(const char *label, const uint8_t *b, size_t len, uint32_t pilot)
{
    tf_opts_t o = { 0 };
    ctx_t c = { .o = &o };
    char d[8];
    snprintf(d, sizeof(d), "F|%02X", len ? b[0] : 0);
    ts_begin(label, len);
    ts_block(0, 1, 0, d, len);
    bool ok = len > 0 && gen_mem(&c, b, len, pilot);
    return flush(&c, true) && ok;
}

tf_result_t tape_file_play(const char *path, tf_opts_t *o)
{
    player_t play = player_for(path);
    if (!play) {
        ESP_LOGE(TAG, "%s: unsupported format", path);
        return TF_ERROR;
    }
    ctx_t c = { .o = o };
    c.f = fopen(path, "rb");
    if (!c.f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return TF_ERROR;
    }
    fseek(c.f, 0, SEEK_END);
    ts_begin(path, (uint32_t)ftell(c.f));
    fseek(c.f, 0, SEEK_SET);
    if (o->start_offset && play == play_tap) fseek(c.f, o->start_offset, SEEK_SET);
    o->resume_offset = 0;
    tf_result_t r = play(&c);
    if (c.streaming) flush(&c, true);        /* also ends an aborted stream */
    fclose(c.f);
    return r;
}
