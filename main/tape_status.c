// What the tape is playing right now (see tape_status.h).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "tape_status.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    char file[48];
    uint32_t fsize;
    int block, nblocks;
    uint32_t fpos;
    char desc[40];
    uint32_t len, done;
    int64_t block_t0, play_t0;          /* esp_timer, us */
} st;

void ts_begin(const char *file, uint32_t fsize)
{
    const char *slash = strrchr(file, '/');
    portENTER_CRITICAL(&lock);
    strlcpy(st.file, slash ? slash + 1 : file, sizeof(st.file));
    st.fsize = fsize;
    st.block = -1;
    st.nblocks = 0;
    st.fpos = 0;
    st.desc[0] = 0;
    st.len = st.done = 0;
    st.play_t0 = st.block_t0 = esp_timer_get_time();
    portEXIT_CRITICAL(&lock);
}

void ts_block(int index, int nblocks, uint32_t fpos, const char *desc, uint32_t len)
{
    portENTER_CRITICAL(&lock);
    st.block = index;
    st.nblocks = nblocks;
    st.fpos = fpos;
    strlcpy(st.desc, desc ? desc : "", sizeof(st.desc));
    st.len = len;
    st.done = 0;
    st.block_t0 = esp_timer_get_time();
    portEXIT_CRITICAL(&lock);
}

/* ---- SAVE through MIC ----
 * pilot -> data (header) -> wait -> data -> saved / error; the states that
 * wait for the Spectrum fade out on their own if it stops (BREAK). */
typedef enum { REC_IDLE, REC_PILOT, REC_DATA, REC_WAIT, REC_SAVED, REC_ERROR } rec_state_t;
static const char *const rec_names[] = { "idle", "pilot", "data", "wait", "saved", "error" };
#define REC_LIVE_US   3000000           /* pilot / data / wait without news: gone */
#define REC_SHOW_US  12000000           /* saved / error stay on the page this long */
static struct {
    rec_state_t state;
    int64_t t;                          /* last change, esp_timer us */
    int64_t t0;                         /* start of the current block */
    uint8_t flag;
    char name[11];
    int type;
    uint32_t hlen;                      /* data length announced by the header */
    uint32_t len, done;                 /* current block: expected (0: unknown), received */
    char file[48];
    char err[16];
} rec;

static void rec_set(rec_state_t s)
{
    rec.state = s;
    rec.t = esp_timer_get_time();
}

void ts_rec_pilot(void)
{
    portENTER_CRITICAL(&lock);
    if (rec.state != REC_WAIT) {        /* between header and data: keep the name */
        rec.name[0] = 0;
        rec.hlen = 0;
    }
    rec.len = rec.done = 0;
    rec.t0 = esp_timer_get_time();
    rec_set(REC_PILOT);
    portEXIT_CRITICAL(&lock);
}

void ts_rec_block(uint8_t flag)
{
    portENTER_CRITICAL(&lock);
    bool after_hdr = rec.state == REC_WAIT || (rec.state == REC_PILOT && rec.name[0]);
    rec.flag = flag;
    rec.len = flag == 0x00 ? 19 : after_hdr && rec.hlen ? rec.hlen + 2 : 0;
    if (flag == 0x00) rec.name[0] = 0;
    rec.done = 1;
    rec.t0 = esp_timer_get_time();
    rec_set(REC_DATA);
    portEXIT_CRITICAL(&lock);
}

void ts_rec_bytes(uint32_t done)
{
    rec.done = done;
    rec.t = esp_timer_get_time();
}

void ts_rec_cancel(void)
{
    portENTER_CRITICAL(&lock);
    rec_set(REC_IDLE);
    portEXIT_CRITICAL(&lock);
}

void ts_rec_header(const char *name, int type, uint32_t len)
{
    portENTER_CRITICAL(&lock);
    strlcpy(rec.name, name, sizeof(rec.name));
    rec.type = type & 3;
    rec.hlen = len;
    rec_set(REC_WAIT);
    portEXIT_CRITICAL(&lock);
}

void ts_rec_end(bool ok)
{
    portENTER_CRITICAL(&lock);
    if (ok) {
        rec.file[0] = 0;
        rec_set(REC_SAVED);             /* the file name follows (ts_rec_saved) */
    } else {
        strlcpy(rec.err, "E_CHECKSUM", sizeof(rec.err));
        rec_set(REC_ERROR);
    }
    portEXIT_CRITICAL(&lock);
}

void ts_rec_saved(const char *path)
{
    const char *slash = strrchr(path, '/');
    portENTER_CRITICAL(&lock);
    strlcpy(rec.file, slash ? slash + 1 : path, sizeof(rec.file));
    rec_set(REC_SAVED);
    portEXIT_CRITICAL(&lock);
}

void ts_rec_error(const char *code)
{
    portENTER_CRITICAL(&lock);
    strlcpy(rec.err, code, sizeof(rec.err));
    rec_set(REC_ERROR);
    portEXIT_CRITICAL(&lock);
}

/* current state, faded out when its time is up */
static rec_state_t rec_now(int64_t now)
{
    rec_state_t s = rec.state;
    int64_t age = now - rec.t;
    if ((s == REC_PILOT || s == REC_DATA || s == REC_WAIT) && age > REC_LIVE_US) return REC_IDLE;
    if ((s == REC_SAVED || s == REC_ERROR) && age > REC_SHOW_US) return REC_IDLE;
    return s;
}

bool ts_rec_active(void)
{
    return rec_now(esp_timer_get_time()) != REC_IDLE;
}

void ts_bytes(uint32_t done)
{
    st.done = done;                     /* one aligned word: no lock needed */
}

/* Copies s into a JSON string body (quotes and control characters escaped) */
static size_t esc(char *o, size_t size, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < size; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = c; }
        else if (c < 0x20) n += snprintf(o + n, size - n, "\\u%04x", c);
        else o[n++] = c;
    }
    o[n] = 0;
    return n;
}

size_t ts_json(char *out, size_t size, const char *state, const char *current,
               const char *next_file, int next_block, int next_nblocks, int next_type,
               const char *next_name)
{
    portENTER_CRITICAL(&lock);
    typeof(st) s = st;
    portEXIT_CRITICAL(&lock);
    int64_t now = esp_timer_get_time();
    char f[100], d[90], c[100], nf[100], nn[30], rn[30], rf[100];
    esc(f, sizeof(f), s.file);
    esc(d, sizeof(d), s.desc);
    esc(c, sizeof(c), current ? current : "");
    esc(nf, sizeof(nf), next_file ? next_file : "");
    esc(nn, sizeof(nn), next_name ? next_name : "");
    portENTER_CRITICAL(&lock);
    typeof(rec) r = rec;
    portEXIT_CRITICAL(&lock);
    rec_state_t rs = rec_now(now);
    esc(rn, sizeof(rn), r.name);
    esc(rf, sizeof(rf), r.file);
    int n = snprintf(out, size,
                     "{\"state\":\"%s\",\"current\":\"%s\",\"file\":\"%s\",\"fsize\":%u,"
                     "\"block\":%d,\"nblocks\":%d,\"fpos\":%u,\"desc\":\"%s\",\"len\":%u,\"done\":%u,"
                     "\"block_ms\":%u,\"play_ms\":%u,"
                     "\"next\":{\"file\":\"%s\",\"block\":%d,\"nblocks\":%d,\"type\":%d,\"name\":\"%s\"},"
                     "\"rec\":{\"state\":\"%s\",\"flag\":%u,\"name\":\"%s\",\"type\":%d,\"hlen\":%u,"
                     "\"len\":%u,\"done\":%u,\"ms\":%u,\"file\":\"%s\",\"err\":\"%s\"}}",
                     state, c, f, (unsigned)s.fsize, s.block, s.nblocks, (unsigned)s.fpos, d,
                     (unsigned)s.len, (unsigned)s.done,
                     (unsigned)((now - s.block_t0) / 1000), (unsigned)((now - s.play_t0) / 1000),
                     nf, next_block, next_nblocks, next_type, nn,
                     rec_names[rs], r.flag, rn, r.type, (unsigned)r.hlen, (unsigned)r.len,
                     (unsigned)r.done, (unsigned)((now - r.t0) / 1000), rf, r.err);
    return n < 0 ? 0 : (size_t)n;
}
