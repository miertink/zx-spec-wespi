/*
 * Load on request, with a turbo loader, for .tap files.
 *
 * A real tape plays blocks one after the other; the ESP32 can do better: it
 * rewrites every BASIC program it sends so that each LOAD statement first
 * asks for its block (asm/turbo.asm, in a REM line appended to the program):
 *
 *   LOAD ""CODE, LOAD "name"CODE [literal address], LOAD ""SCREEN$
 *        -> RANDOMIZE USR <stub>              the routine loads it in turbo
 *   any other LOAD (programs, DATA, CODE with an expression)
 *        -> RANDOMIZE USR <stub>: LOAD ...    the ROM loads it, on request
 *
 * The request arrives through MIC (zx_menu.c routes it here); the ESP32 then
 * looks for the block as a tape would, from the current tape position: the
 * next header of the right type (and name, if the LOAD gives one), wrapping
 * around once. Programs that wait for the user between parts (multi-part
 * courses, "stop the tape" pauses) work without anybody stopping a tape.
 *
 * A program the rewrite cannot handle (machine code inside the BASIC area,
 * no free line number, too close to RAMTOP) is sent unchanged, and the rest
 * of the tape then plays in sequence, as a tape would.
 *
 * Program id = index of the program's header block in the .tap.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "tape_out.h"
#include "tap_player.h"
#include "tape_status.h"
#include "zx_turbo.h"
#include "turbo_bin.h"

static const char *TAG = "zx_turbo";

#define PROG_ADDR        23755          /* PROG on a 48K without Interface 1 */
#define MAX_BLOCKS       256
#define MAX_PROG         12000          /* larger programs are sent unchanged (heap) */
#define MAX_STMT         48             /* LOAD statements rewritten per program */
#define RAM_MARGIN       300            /* workspace + stack + new variables below RAMTOP */
#define USR_LEN          13             /* RANDOMIZE USR nnnnn, number included */
#define STUB_LEN         5              /* LD A,k / JP entry */

/* ROM standard timings, T-states */
#define T_PILOT          2168
#define T_SYNC1          667
#define T_SYNC2          735
#define T_BIT0           855
#define T_BIT1           1710
#define FAST_PILOT       2600           /* the ROM is already waiting (see tape_file.c) */
#define HDR_PAUSE_MS     300

/* Turbo signal, decoded by asm/turbo.asm with the ROM's LD-EDGE-1: two bits
 * per pulse. LD-EDGE-1 samples every 59 T-states, its first sample ~547
 * T-states after the previous edge (plus the caller's own path), so a pulse
 * of TB_SYM0 gives a count of ~2.5, and each symbol step adds 10 samples.
 * The routine's path between two edges depends on what it just did; every
 * pulse gets (that path - TB_REF) added, so all symbols look alike to it.
 * T-states of the paths: see the comments in asm/turbo.asm. */
#define TB_SYM0          CONFIG_ZXS_TURBO_SYM0
#define TB_STEP          (59 * 10)      /* SPACING in asm/turbo.asm: 10 samples */
#define TB_REF           95             /* path after a symbol 0 within a byte */
#define TB_OV_PILOT      160            /* path of a measured pilot pulse */
#define TB_OV_SYNC       263            /* sync edge to the first symbol */
#define TB_PILOT_N       300            /* the routine needs 32 + 32, plus a late start */
#define TB_SYNC_T        4300           /* count ~64: above SYNC_MIN (52), below a timeout */
static const uint16_t tb_class[4] = { 19, 37, 55, 54 };    /* symbol decision, per symbol */
enum { TB_IN_BYTE, TB_HDR_BYTE, TB_HDR_LAST, TB_DATA_BYTE, TB_DATA_LAST };
/* path after the last symbol of a byte, before the class, by what follows */
static const uint16_t tb_after_byte[] = {
    [TB_HDR_BYTE] = 159, [TB_HDR_LAST] = 238, [TB_DATA_BYTE] = 200, [TB_DATA_LAST] = 205,
};

/* BASIC tokens */
#define TK_CODE    0xAF
#define TK_SCREEN  0xAA
#define TK_VAL     0xB0
#define TK_USR     0xC0
#define TK_THEN    0xCB
#define TK_DATA    0xE4
#define TK_REM     0xEA
#define TK_LOAD    0xEF
#define TK_RANDOMIZE 0xF9
#define TK_CLEAR   0xFD

typedef struct {
    uint32_t ofs;                   /* file offset of the block (its flag byte) */
    uint16_t len;                   /* flag + data + checksum */
    bool hdr;                       /* a standard header */
    uint8_t h[17];                  /* header payload: type, name, length, param1, param2 */
} blk_t;

enum { K_PROG, K_CODE, K_SCREEN, K_DATA };

typedef struct {
    uint32_t pos, end;              /* LOAD token, end of the statement (':' or 0x0D) */
    uint8_t kind;
    bool name_lit;                  /* name is a string literal */
    char name[10];                  /* padded with spaces */
    uint8_t name_len;
    bool addr_lit;                  /* CODE address given as a literal */
    uint16_t addr;
    bool turbo;
} stmt_t;

/* The session is changed by the menu task (start, stop, requests); the web
 * server reads it (zx_turbo_next, zx_turbo_continue): the block table is
 * swapped and read under this lock */
static portMUX_TYPE ses_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool active;
    bool linear;                    /* an unchanged program was sent: play the rest */
    char path[64];
    blk_t *b;
    int n;
    int ptr;                        /* tape position: next block */
    int last_h;                     /* last turbo block (header), -1 none */
    uint16_t last_dest;
    int retries;                    /* error requests for it */
} ses;

static inline uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ------------------------------------------------------------------------ */
/* .tap index */

static int index_tap(const char *path, blk_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    blk_t *b = calloc(MAX_BLOCKS, sizeof(blk_t));
    int n = 0;
    uint8_t lb[2];
    while (b && n < MAX_BLOCKS && fread(lb, 1, 2, f) == 2) {
        uint16_t len = le16(lb);
        long ofs = ftell(f);
        if (len < 2) break;
        uint8_t head[19];
        size_t got = fread(head, 1, len < 19 ? len : 19, f);
        if (got < (len < 19 ? len : 19)) break;
        b[n].ofs = ofs;
        b[n].len = len;
        b[n].hdr = len == 19 && head[0] == 0x00;
        if (b[n].hdr) memcpy(b[n].h, head + 1, 17);
        n++;
        if (fseek(f, ofs + len, SEEK_SET) != 0) break;
    }
    fclose(f);
    *out = b;
    return b ? n : -1;
}

static uint8_t *read_block(const char *path, const blk_t *b)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *d = malloc(b->len);
    if (d && (fseek(f, b->ofs, SEEK_SET) != 0 || fread(d, 1, b->len, f) != b->len)) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

/* ------------------------------------------------------------------------ */
/* BASIC scanner */

/* A number literal at p[*j]: digits followed by the hidden 0x0E form, or
 * VAL "digits". Moves *j past it. */
static bool number_at(const uint8_t *p, uint32_t *j, uint32_t e, uint32_t *val)
{
    uint32_t k = *j;
    while (k < e && p[k] == ' ') k++;
    if (k < e && p[k] == TK_VAL && k + 1 < e && p[k + 1] == '"') {
        uint32_t v = 0, q = k + 2;
        if (q >= e || !isdigit(p[q])) return false;
        while (q < e && isdigit(p[q])) v = v * 10 + (p[q++] - '0');
        if (q >= e || p[q] != '"') return false;
        *val = v;
        *j = q + 1;
        return true;
    }
    if (k >= e || !(isdigit(p[k]) || p[k] == '.')) return false;
    while (k < e && p[k] != 0x0E) {
        if (!(isdigit(p[k]) || p[k] == '.' || p[k] == 'e' || p[k] == 'E' || p[k] == '+' || p[k] == '-'))
            return false;
        k++;
    }
    if (k + 6 > e) return false;
    const uint8_t *n5 = p + k + 1;
    if (n5[0] != 0) return false;                       /* not a small integer */
    *val = le16(n5 + 2);
    *j = k + 6;
    return true;
}

/* End of the statement starting at j: ':' or 0x0D outside strings */
static uint32_t stmt_end(const uint8_t *p, uint32_t j, uint32_t e)
{
    while (j < e) {
        if (p[j] == '"') {
            j++;
            while (j < e && p[j] != '"') j++;
            j++;
        } else if (p[j] == 0x0E) {
            j += 6;
        } else if (p[j] == ':' || p[j] == 0x0D) {
            return j;
        } else {
            j++;
        }
    }
    return e;
}

static void parse_load(const uint8_t *p, uint32_t j, uint32_t e, stmt_t *s)
{
    memset(s, 0, sizeof(*s));
    memset(s->name, ' ', sizeof(s->name));
    s->pos = j;
    s->end = stmt_end(p, j, e);
    uint32_t k = j + 1;
    while (k < s->end && p[k] == ' ') k++;
    if (k < s->end && p[k] == '"') {
        s->name_lit = true;
        k++;
        while (k < s->end && p[k] != '"') {
            if (s->name_len < 10) s->name[s->name_len++] = p[k];
            k++;
        }
        k++;
    } else {
        /* name expression: find the type token, if any */
        while (k < s->end && p[k] != TK_CODE && p[k] != TK_SCREEN && p[k] != TK_DATA) k++;
    }
    s->kind = K_PROG;
    if (k < s->end && p[k] == TK_CODE) {
        s->kind = K_CODE;
        k++;
        uint32_t v;
        bool args_ok = true;
        while (k < s->end && p[k] == ' ') k++;
        if (k < s->end) {
            args_ok = number_at(p, &k, s->end, &v);
            if (args_ok) {
                s->addr_lit = true;
                s->addr = v;
                if (k < s->end && p[k] == ',') {
                    k++;
                    args_ok = number_at(p, &k, s->end, &v);
                }
                while (k < s->end && p[k] == ' ') k++;
                args_ok = args_ok && k == s->end;
            }
        }
        s->turbo = s->name_lit && args_ok;
    } else if (k < s->end && p[k] == TK_SCREEN) {
        s->kind = K_SCREEN;
        s->addr_lit = true;
        s->addr = 16384;
        s->turbo = s->name_lit && k + 1 == s->end;
    } else if (k < s->end && p[k] == TK_DATA) {
        s->kind = K_DATA;
    }
}

typedef struct {
    int n;
    stmt_t st[MAX_STMT];
    uint16_t max_line;
    uint32_t clear_max;             /* highest CLEAR literal (the RAMTOP in use), 0 = none */
    bool usr_in_prog;               /* USR into the BASIC area: code moves if lines grow */
    bool more;                      /* more LOAD statements than MAX_STMT */
    bool bad;
} scan_t;

/* p: program (plen bytes) followed by its variables (total bytes in all) */
static void scan(const uint8_t *p, uint32_t plen, uint32_t total, scan_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    uint32_t i = 0;
    while (i + 4 <= plen) {
        uint16_t num = (p[i] << 8) | p[i + 1];
        uint16_t ll = le16(p + i + 2);
        uint32_t b = i + 4, e = b + ll;
        if (num > 9999 || e > plen) {
            sc->bad = true;
            return;
        }
        if (num > sc->max_line) sc->max_line = num;
        bool start = true;
        for (uint32_t j = b; j < e;) {
            uint8_t c = p[j];
            if (c == 0x0D) break;
            if (c == '"') {
                j++;
                while (j < e && p[j] != '"') j++;
                j++;
                start = false;
                continue;
            }
            if (c == 0x0E) {
                j += 6;
                continue;
            }
            if (c == ':' || c == TK_THEN) {
                start = true;
                j++;
                continue;
            }
            if (c == ' ') {
                j++;
                continue;
            }
            if (c == TK_REM && start) break;
            if (c == TK_CLEAR || c == TK_USR) {
                uint32_t k = j + 1, v;
                if (number_at(p, &k, e, &v)) {
                    if (c == TK_CLEAR && v > sc->clear_max) sc->clear_max = v;
                    if (c == TK_USR && v >= PROG_ADDR && v < PROG_ADDR + total) sc->usr_in_prog = true;
                }
            }
            if (c == TK_LOAD && start) {
                if (sc->n < MAX_STMT) parse_load(p, j, e, &sc->st[sc->n++]);
                else sc->more = true;
            }
            start = false;
            j++;
        }
        i = e;
    }
}

/* ------------------------------------------------------------------------ */
/* Rewrite */

static size_t put_usr(uint8_t *o, uint16_t addr)
{
    size_t n = 0;
    o[n++] = TK_RANDOMIZE;
    o[n++] = TK_USR;
    n += sprintf((char *)o + n, "%05u", addr);
    o[n++] = 0x0E;
    o[n++] = 0;
    o[n++] = 0;
    o[n++] = addr & 0xFF;
    o[n++] = addr >> 8;
    o[n++] = 0;
    return n;
}

/* Request-only routine, for programs too close to their CLEAR address for
 * the turbo routine: sends the request (as asm/turbo.asm does) and returns,
 * every LOAD then loads through the ROM.
 *   ld (req+2),a / ld ix,req / ld hl,SA_LD_RET / push hl / ld hl,REQ_PILOT /
 *   ld de,4 / ld a,REQ_FLAG / jp SA_FLAG / req: 'T', id, k, 0 */
static const uint8_t req_code[] = {
    0x32, 0x18, 0x80, 0xDD, 0x21, 0x16, 0x80, 0x21, 0x3F, 0x05, 0xE5, 0x21, 0x90, 0x01,
    0x11, 0x04, 0x00, 0x3E, 0xA5, 0xC3, 0xD0, 0x04, 'T', 0, 0, 0,
};
static const uint16_t req_reloc[] = { 1, 5 };
#define REQ_OFS_REQ      22

/* Rewrites the program block data `p` (program + variables, `plen` bytes of
 * program). Returns a new buffer (program + variables) or NULL. lite: the
 * request-only routine, all LOADs through the ROM. */
static uint8_t *rewrite_as(const uint8_t *p, uint32_t total, uint32_t plen, uint8_t pid, bool lite,
                           uint32_t *out_total, uint32_t *out_plen, int *out_n, bool *tight)
{
    const uint8_t *rcode = lite ? req_code : turbo_code;
    size_t rsize = lite ? sizeof(req_code) : sizeof(turbo_code);
    const uint16_t *rreloc = lite ? req_reloc : turbo_reloc;
    size_t nreloc = lite ? sizeof(req_reloc) / sizeof(req_reloc[0])
                         : sizeof(turbo_reloc) / sizeof(turbo_reloc[0]);
    uint16_t ofs_entry = lite ? 0 : TURBO_OFS_ENTRY, ofs_req = lite ? REQ_OFS_REQ : TURBO_OFS_REQ;
#define TURBO(s) ((s)->turbo && !lite)
    *tight = false;
    scan_t *sc = malloc(sizeof(scan_t));
    if (!sc) return NULL;
    scan(p, plen, total, sc);
    uint8_t *o = NULL;
    const char *why = NULL;
    if (sc->bad) why = "not a valid program";
    else if (sc->n == 0) why = "no LOAD";
    else if (sc->usr_in_prog) why = "machine code inside the BASIC area";
    else if (sc->more) why = "too many LOAD statements";
    if (why) goto done;

    /* the REM line goes after the last line, or into the first gap in the
     * line numbers (it is skipped like any REM if the program runs into it) */
    uint16_t line = 0, after = 0;
    if (sc->max_line < 9999) {
        after = sc->max_line;
        line = after + 1;
    } else {
        for (uint32_t i = 0; i + 4 <= plen; i += 4 + le16(p + i + 2)) {
            uint16_t num = (p[i] << 8) | p[i + 1];
            uint32_t nx = i + 4 + le16(p + i + 2);
            uint16_t next = nx + 4 <= plen ? (p[nx] << 8) | p[nx + 1] : 10000;
            if (next > num + 1) {
                after = num;
                line = num + 1;
                break;
            }
        }
        if (!line) {
            why = "no free line number";
            goto done;
        }
    }

    int n = sc->n;
    int32_t grow = 0;
    for (int k = 0; k < n; k++) {
        stmt_t *s = &sc->st[k];
        grow += TURBO(s) ? USR_LEN - (int32_t)(s->end - s->pos) : USR_LEN + 1;
    }
    uint32_t rem_len = 4 + 1 + STUB_LEN * n + rsize + 1;
    uint32_t nplen = plen + grow + rem_len;
    uint32_t ntotal = nplen + (total - plen);

    /* offset of the REM line in the new program: lines up to `after`, grown */
    uint32_t rem_at = 0;
    for (uint32_t i = 0, k = 0; i + 4 <= plen;) {
        uint16_t num = (p[i] << 8) | p[i + 1];
        uint32_t e = i + 4 + le16(p + i + 2);
        rem_at += e - i;
        for (; k < (uint32_t)n && sc->st[k].pos < e; k++) {
            stmt_t *s = &sc->st[k];
            rem_at += TURBO(s) ? USR_LEN - (int32_t)(s->end - s->pos) : USR_LEN + 1;
        }
        i = e;
        if (num == after) break;
    }
    uint16_t stubs = PROG_ADDR + rem_at + 5;
    uint16_t code = stubs + STUB_LEN * n;
    if (sc->clear_max && PROG_ADDR + ntotal + RAM_MARGIN >= sc->clear_max) {
        ESP_LOGI(TAG, "program %u: ends at %u, CLEAR %u", pid, (unsigned)(PROG_ADDR + ntotal),
                 (unsigned)sc->clear_max);
        why = "too close to its CLEAR address";
        *tight = true;
        goto done;
    }
    o = malloc(ntotal);
    if (!o) {
        why = "out of memory";
        goto done;
    }

    /* lines, with the statements replaced, and the REM line */
    uint32_t i = 0, w = 0;
    int k = 0;
    while (i + 4 <= plen) {
        uint16_t num = (p[i] << 8) | p[i + 1];
        uint16_t ll = le16(p + i + 2);
        uint32_t b = i + 4, e = b + ll;
        uint32_t lw = w;
        memcpy(o + w, p + i, 4);
        w += 4;
        uint32_t j = b;
        while (k < n && sc->st[k].pos < e) {
            stmt_t *s = &sc->st[k];
            memcpy(o + w, p + j, s->pos - j);
            w += s->pos - j;
            w += put_usr(o + w, stubs + STUB_LEN * k);
            if (TURBO(s)) {
                j = s->end;
            } else {
                o[w++] = ':';
                j = s->pos;
            }
            k++;
        }
        memcpy(o + w, p + j, e - j);
        w += e - j;
        uint16_t nl = w - lw - 4;
        o[lw + 2] = nl & 0xFF;
        o[lw + 3] = nl >> 8;
        i = e;
        if (num != after) continue;

        if (w != rem_at) {
            ESP_LOGE(TAG, "rewrite: REM line at %u, expected %u", (unsigned)w, (unsigned)rem_at);
            why = "internal error";
            goto done;
        }
        o[w++] = line >> 8;
        o[w++] = line & 0xFF;
        o[w++] = (rem_len - 4) & 0xFF;
        o[w++] = (rem_len - 4) >> 8;
        o[w++] = TK_REM;
        for (int q = 0; q < n; q++) {
            o[w++] = 0x3E;
            o[w++] = q | (TURBO(&sc->st[q]) ? 0 : 0x80);
            o[w++] = 0xC3;
            o[w++] = (code + ofs_entry) & 0xFF;
            o[w++] = (code + ofs_entry) >> 8;
        }
        uint32_t cw = w;
        memcpy(o + w, rcode, rsize);
        w += rsize;
        for (size_t r = 0; r < nreloc; r++) {
            uint8_t *a = o + cw + rreloc[r];
            uint16_t v = le16(a) - TURBO_ORG + code;
            a[0] = v & 0xFF;
            a[1] = v >> 8;
        }
        o[cw + ofs_req + 1] = pid;
        o[w++] = 0x0D;
    }
    if (w != nplen) {
        ESP_LOGE(TAG, "rewrite: length %u, expected %u", (unsigned)w, (unsigned)nplen);
        why = "internal error";
        goto done;
    }
    memcpy(o + w, p + plen, total - plen);          /* variables */
    *out_total = ntotal;
    *out_plen = nplen;
    *out_n = n;
done:
    if (why) {
        if (!*tight || lite) ESP_LOGI(TAG, "program %u sent unchanged: %s", pid, why);
        free(o);
        o = NULL;
    }
    free(sc);
    return o;
#undef TURBO
}

static uint8_t *rewrite(const uint8_t *p, uint32_t total, uint32_t plen, uint8_t pid,
                        uint32_t *out_total, uint32_t *out_plen, int *out_n)
{
    bool tight;
    uint8_t *o = rewrite_as(p, total, plen, pid, false, out_total, out_plen, out_n, &tight);
    if (!o && tight) {
        o = rewrite_as(p, total, plen, pid, true, out_total, out_plen, out_n, &tight);
        if (o) ESP_LOGI(TAG, "program %u: little RAM below its CLEAR, its LOADs go through the ROM", pid);
    }
    return o;
}

/* ------------------------------------------------------------------------ */
/* Signal */

static bool tone(uint32_t t, uint32_t n)
{
    while (n--) {
        if (!tape_out_pulse(t)) return false;
    }
    return true;
}

/* Web status: block `index` of the session starts; std_bytes / turbo_file
 * count its bytes */
static uint32_t ts_sent;

static void tz_mark(int index, const char *desc, uint32_t len)
{
    ts_sent = 0;
    ts_block(index, ses.n, index >= 0 && index < ses.n ? ses.b[index].ofs : 0, desc, len);
}

/* Describes block `index` (a header: its type and name) */
static void tz_mark_blk(int index, const char *what)
{
    const blk_t *b = &ses.b[index];
    char d[40];
    if (b->hdr) {
        char name[11];
        memcpy(name, b->h + 1, 10);
        int l = 10;
        while (l > 0 && name[l - 1] == ' ') l--;
        name[l] = 0;
        for (int k = 0; k < l; k++) if (name[k] < 32 || name[k] > 126 || name[k] == '|') name[k] = '?';
        snprintf(d, sizeof(d), "H|%u|%s", b->h[0] & 3, name);
    } else {
        snprintf(d, sizeof(d), "%s", what);
    }
    tz_mark(index, d, b->len - 2);
}

static bool std_bytes(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((++ts_sent & 31) == 0) ts_bytes(ts_sent);
        for (int k = 7; k >= 0; k--) {
            uint32_t t = (d[i] >> k) & 1 ? T_BIT1 : T_BIT0;
            if (!tape_out_pulse(t) || !tape_out_pulse(t)) return false;
        }
    }
    return true;
}

/* A ROM block from memory given as flag + data: the checksum is added */
static bool std_parts(uint8_t flag, const uint8_t *d, size_t len, uint32_t pilot)
{
    uint8_t x = flag;
    for (size_t i = 0; i < len; i++) x ^= d[i];
    bool ok = tone(T_PILOT, pilot) && tape_out_pulse(T_SYNC1) && tape_out_pulse(T_SYNC2)
              && std_bytes(&flag, 1) && std_bytes(d, len) && std_bytes(&x, 1);
    return tape_out_flush(true) && ok;
}

/* A ROM block from the file, streamed */
static bool std_file(FILE *f, const blk_t *b, uint32_t pilot)
{
    uint8_t buf[256];
    tz_mark_blk(b - ses.b, "R");
    bool ok = fseek(f, b->ofs, SEEK_SET) == 0 && tone(T_PILOT, pilot)
              && tape_out_pulse(T_SYNC1) && tape_out_pulse(T_SYNC2);
    for (uint32_t done = 0; ok && done < b->len;) {
        size_t n = b->len - done > sizeof(buf) ? sizeof(buf) : b->len - done;
        ok = fread(buf, 1, n, f) == n && std_bytes(buf, n);
        done += n;
    }
    return tape_out_flush(true) && ok;
}

static uint32_t tb_ov;              /* path of the routine before the next edge */

/* One byte, 4 symbols, high bits first; `next` says what the routine does
 * after it (TB_HDR_BYTE...), for the timing of the following pulse. */
static bool tb_byte(uint8_t v, int next)
{
    for (int k = 3; k >= 0; k--) {
        int s = (v >> (2 * k)) & 3;
        if (!tape_out_pulse(TB_SYM0 + TB_STEP * s + tb_ov - TB_REF)) return false;
        tb_ov = (k ? 76 : tb_after_byte[next]) + tb_class[s];
    }
    return true;
}

/* A turbo block: address, length, the data of tape block b, checksum */
static bool turbo_file(FILE *f, const blk_t *b, uint16_t dest)
{
    uint16_t len = b->len - 2;                      /* without flag and checksum */
    uint8_t hdr[4] = { dest & 0xFF, dest >> 8, len & 0xFF, len >> 8 };
    uint8_t x = hdr[0] ^ hdr[1] ^ hdr[2] ^ hdr[3];
    uint8_t buf[256];
    char d[40];
    snprintf(d, sizeof(d), "T|%u", dest);
    tz_mark(b - ses.b, d, len);
    bool ok = len > 0 && fseek(f, b->ofs + 1, SEEK_SET) == 0
              && tone(TB_SYM0 + 3 * TB_STEP + TB_OV_PILOT - TB_REF, TB_PILOT_N)
              && tape_out_pulse(TB_SYNC_T);
    tb_ov = TB_OV_SYNC;
    for (int i = 0; ok && i < 4; i++) ok = tb_byte(hdr[i], i < 3 ? TB_HDR_BYTE : TB_HDR_LAST);
    for (uint32_t done = 0; ok && done < len;) {
        size_t n = len - done > sizeof(buf) ? sizeof(buf) : len - done;
        ok = fread(buf, 1, n, f) == n;
        for (size_t i = 0; ok && i < n; i++) {
            x ^= buf[i];
            ok = tb_byte(buf[i], done + i + 1 < len ? TB_DATA_BYTE : TB_DATA_LAST);
            if (((done + i + 1) & 31) == 0) ts_bytes(done + i + 1);
        }
        done += n;
        ts_bytes(done);
    }
    ok = ok && tb_byte(x, TB_DATA_BYTE);            /* checksum: nothing follows */
    return tape_out_flush(true) && ok;
}

static bool pause_ms(uint32_t ms)
{
    for (; ms > 0 && !tape_out_aborted(); ms -= ms > 20 ? 20 : ms) vTaskDelay(pdMS_TO_TICKS(20));
    return !tape_out_aborted();
}

/* ------------------------------------------------------------------------ */
/* Jobs, run in the tape player task */

/* Sends program header h + data h+1, rewritten if possible */
/* Last line number of a program (data without its flag), -1 if none */
static int last_line(const uint8_t *p, uint32_t plen)
{
    int last = -1;
    for (uint32_t i = 0; i + 4 <= plen; i += 4 + le16(p + i + 2)) {
        int num = (p[i] << 8) | p[i + 1];
        if (num > 9999) break;
        last = num;
    }
    return last;
}

static bool send_program(FILE *f, int h)
{
    blk_t *hb = &ses.b[h], *db = &ses.b[h + 1];
    uint8_t *d = db->len <= MAX_PROG + 2 ? read_block(ses.path, db) : NULL;
    uint32_t nt = 0, np = 0;
    int n = 0;
    uint16_t plen = le16(hb->h + 15);
    if (d && plen > db->len - 2) plen = db->len - 2;
    /* the first program comes from the menu's LOAD "": without an autostart
     * line it would return into the menu's line (see tape_file.c,
     * autostart_fix); it gets the line after its last one */
    int autostart = -1;
    if (h == 0 && d && le16(hb->h + 13) >= 32768) {
        int last = last_line(d + 1, plen);
        if (last >= 0 && last < 9999) autostart = last + 1;
    }
    uint8_t *o = d ? rewrite(d + 1, db->len - 2, plen, h, &nt, &np, &n) : NULL;
    free(d);                                        /* only one copy in the heap at a time */
    bool ok;
    if (autostart >= 0) ESP_LOGI(TAG, "program without autostart: LINE %d added for the menu", autostart);
    if (o) {
        uint8_t hdr[18];
        memcpy(hdr, hb->h, 17);
        if (autostart >= 0) {
            hdr[13] = autostart & 0xFF;
            hdr[14] = autostart >> 8;
        }
        hdr[11] = nt & 0xFF;
        hdr[12] = nt >> 8;
        hdr[15] = np & 0xFF;
        hdr[16] = np >> 8;
        char name[11];
        memcpy(name, hb->h + 1, 10);
        name[10] = 0;
        ESP_LOGI(TAG, "program \"%s\": %d LOAD(s) on request, %u -> %u bytes",
                 name, n, (unsigned)(db->len - 2), (unsigned)nt);
        tz_mark_blk(h, "");
        ok = std_parts(0x00, hdr, 17, FAST_PILOT) && pause_ms(HDR_PAUSE_MS);
        if (ok) tz_mark(h + 1, "P", nt);
        ok = ok && std_parts(0xFF, o, nt, FAST_PILOT);
        free(o);
    } else {
        ses.linear = true;              /* its LOADs will not ask: play the rest after it */
        if (autostart >= 0) {
            uint8_t hdr[17];
            memcpy(hdr, hb->h, 17);
            hdr[13] = autostart & 0xFF;
            hdr[14] = autostart >> 8;
            tz_mark_blk(h, "");
            ok = std_parts(0x00, hdr, 17, FAST_PILOT);
        } else {
            ok = std_file(f, hb, FAST_PILOT);
        }
        ok = ok && pause_ms(HDR_PAUSE_MS) && std_file(f, db, FAST_PILOT);
    }
    return ok;
}

/* Plays the rest of the tape in sequence, from ses.ptr */
static void play_linear(FILE *f)
{
    ESP_LOGI(TAG, "playing the rest of the tape in sequence (block %d on)", ses.ptr);
    while (ses.ptr < ses.n && !tape_out_aborted()) {
        if (!pause_ms(CONFIG_ZXS_TAPE_BLOCK_PAUSE_MS)) break;
        blk_t *b = &ses.b[ses.ptr];
        if (!std_file(f, b, b->hdr ? 8063 : 3223)) break;
        ses.ptr++;
    }
}

typedef struct {
    int kind;                       /* 0 first program, 1 ROM blocks, 2 turbo */
    int h;                          /* header block */
    uint16_t dest;
} job_t;

/* Runs in the tape player task; arg = a job_t from new_job(), freed here */
static void run_job(void *arg)
{
    job_t job = *(job_t *)arg;
    free(arg);
    if (!ses.active) return;
    FILE *f = fopen(ses.path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", ses.path);
        return;
    }
    bool ok = true;
    int h = job.h;
    ts_begin(ses.path, ses.n ? ses.b[ses.n - 1].ofs + ses.b[ses.n - 1].len : 0);
    if (job.kind == 2) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "turbo: block %d, %u bytes to %u", h + 1, (unsigned)(ses.b[h + 1].len - 2), job.dest);
        ok = turbo_file(f, &ses.b[h + 1], job.dest);
    } else {
        if (job.kind == 1) vTaskDelay(pdMS_TO_TICKS(200));    /* the ROM LOAD starts */
        if (ses.b[h].h[0] == 0) {
            ok = send_program(f, h);
        } else {
            ok = std_file(f, &ses.b[h], FAST_PILOT) && pause_ms(HDR_PAUSE_MS)
                 && std_file(f, &ses.b[h + 1], FAST_PILOT);
        }
    }
    if (ok && ses.linear) play_linear(f);
    fclose(f);
    if (!ok && !tape_out_aborted()) ESP_LOGE(TAG, "block %d: play failed", h);
}

static bool queue_job(int kind, int h, uint16_t dest)
{
    job_t *j = malloc(sizeof(job_t));
    if (!j) return false;
    j->kind = kind;
    j->h = h;
    j->dest = dest;
    if (tap_player_run(run_job, j, ses.path) != ESP_OK) {
        free(j);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */

static bool pairs_only(const blk_t *b, int n)
{
    if (n < 2 || !b[0].hdr || b[0].h[0] != 0) return false;
    for (int i = 0; i < n; i += 2) {
        if (!b[i].hdr || i + 1 >= n || b[i + 1].hdr) return false;
    }
    return true;
}

bool zx_turbo_capable(const char *path)
{
    size_t l = strlen(path);
    if (l < 4 || strcasecmp(path + l - 4, ".tap") != 0) return false;
    blk_t *b = NULL;
    int n = index_tap(path, &b);
    bool ok = n > 0 && pairs_only(b, n) && b[1].len <= MAX_PROG + 2;
    if (ok) {
        uint8_t *d = read_block(path, &b[1]);
        scan_t *sc = d ? malloc(sizeof(scan_t)) : NULL;
        if (sc) {
            scan(d + 1, le16(b[0].h + 15) <= b[1].len - 2 ? le16(b[0].h + 15) : b[1].len - 2,
                 b[1].len - 2, sc);
            ok = !sc->bad && sc->n > 0 && !sc->usr_in_prog && !sc->more;
        } else {
            ok = false;
        }
        free(sc);
        free(d);
    }
    free(b);
    return ok;
}

void zx_turbo_stop(void)
{
    if (ses.active) {
        ESP_LOGI(TAG, "session ended");
        ses.active = false;
        /* a block of the session may still be playing: it uses ses.b */
        const char *slash = strrchr(ses.path, '/');
        if (tap_player_is_busy() && strcmp(tap_player_current(), slash ? slash + 1 : ses.path) == 0) {
            tap_player_stop();
            for (int i = 0; i < 100 && tap_player_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    portENTER_CRITICAL(&ses_mux);
    blk_t *b = ses.b;
    ses.b = NULL;
    ses.n = 0;
    portEXIT_CRITICAL(&ses_mux);
    free(b);
}

bool zx_turbo_start(const char *path)
{
    zx_turbo_stop();
    if (!zx_turbo_capable(path)) return false;
    blk_t *b = NULL;
    int n = index_tap(path, &b);
    if (n <= 0) {
        free(b);
        return false;
    }
    portENTER_CRITICAL(&ses_mux);
    ses.b = b;
    ses.n = n;
    strlcpy(ses.path, path, sizeof(ses.path));
    ses.ptr = 2;
    ses.linear = false;
    ses.last_h = -1;
    ses.active = true;
    portEXIT_CRITICAL(&ses_mux);
    ESP_LOGI(TAG, "session: %s, %d blocks, loads on request", path, n);
    if (!queue_job(0, 0, 0)) {
        zx_turbo_stop();
        return false;
    }
    return true;
}

/* Next header from the tape position that a LOAD of this kind and name
 * would accept, wrapping around once; -1 if none. */
static int find_header(const stmt_t *s)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int h = pass ? 0 : ses.ptr; h + 1 < ses.n; h++) {
            const blk_t *b = &ses.b[h];
            if (!b->hdr || ses.b[h + 1].hdr) continue;
            uint8_t t = b->h[0];
            bool kind_ok = s->kind == K_PROG ? t == 0
                         : s->kind == K_DATA ? (t == 1 || t == 2)
                         : t == 3;
            if (!kind_ok) continue;
            if (s->name_len && memcmp(b->h + 1, s->name, 10) != 0) continue;
            return h;
        }
    }
    return -1;
}

void zx_turbo_request(uint8_t pid, uint8_t k)
{
    if (!ses.active) {
        ESP_LOGW(TAG, "request %u/%u without a session (Spectrum not reset since?)", pid, k & 0x7F);
        return;
    }
    bool rom = k & 0x80;                    /* the stub asks for a ROM load */
    k &= 0x7F;
    if (pid + 1 >= ses.n || !ses.b[pid].hdr || ses.b[pid].h[0] != 0) {
        ESP_LOGW(TAG, "request from unknown program %u", pid);
        return;
    }
    uint8_t *d = read_block(ses.path, &ses.b[pid + 1]);
    scan_t *sc = d ? malloc(sizeof(scan_t)) : NULL;
    if (!sc) {
        free(d);
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    uint16_t plen = le16(ses.b[pid].h + 15);
    if (plen > ses.b[pid + 1].len - 2) plen = ses.b[pid + 1].len - 2;
    scan(d + 1, plen, ses.b[pid + 1].len - 2, sc);
    if (k >= sc->n) {
        ESP_LOGW(TAG, "program %u has no LOAD %u", pid, k);
    } else {
        stmt_t *s = &sc->st[k];
        int h = find_header(s);
        char name[11];
        memcpy(name, s->name, 10);
        name[10] = 0;
        if (h < 0) {
            ESP_LOGW(TAG, "LOAD \"%s\" (program %u, LOAD %u): no such block on the tape", name, pid, k);
        } else {
            ses.ptr = h + 2;
            ESP_LOGI(TAG, "LOAD %u of program %u (\"%s\"): block %d, %s", k, pid, name, h,
                     s->turbo && !rom ? "turbo" : "ROM");
            uint16_t dest = s->addr_lit ? s->addr : le16(ses.b[h].h + 13);
            bool turbo = s->turbo && !rom;
            if (turbo) {
                ses.last_h = h;
                ses.last_dest = dest;
                ses.retries = 0;
            }
            queue_job(turbo ? 2 : 1, h, dest);
        }
    }
    free(sc);
    free(d);
}

void zx_turbo_error(uint8_t bp, uint16_t left)
{
    if (!ses.active || ses.last_h < 0) {
        ESP_LOGW(TAG, "error request without a turbo block (Bp %u)", bp);
        return;
    }
    int h = ses.last_h;
    uint16_t len = ses.b[h + 1].len - 2;
    ses.retries++;
    if (left == 0xFFFF)
        ESP_LOGW(TAG, "turbo block %d failed in its header (pilot count %u): again, try %d",
                 h + 1, bp, ses.retries);
    else if (left == 0)
        ESP_LOGW(TAG, "turbo block %d: checksum error (pilot count %u): again, try %d",
                 h + 1, bp, ses.retries);
    else
        ESP_LOGW(TAG, "turbo block %d: signal lost at byte %u of %u (pilot count %u): again, try %d",
                 h + 1, (unsigned)(len - left), (unsigned)len, bp, ses.retries);
    /* the Spectrum waited for the end of the signal: the player is done or
     * about to be */
    for (int i = 0; i < 50 && tap_player_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    queue_job(2, h, ses.last_dest);
}


bool zx_turbo_next(zx_turbo_pos_t *pos)
{
    portENTER_CRITICAL(&ses_mux);
    int h = ses.ptr;
    bool ok = ses.active && ses.b && h >= 0 && h + 1 < ses.n && ses.b[h].hdr && !ses.b[h + 1].hdr;
    if (ok && pos) {
        const char *slash = strrchr(ses.path, '/');
        strlcpy(pos->file, slash ? slash + 1 : ses.path, sizeof(pos->file));
        pos->block = h;
        pos->nblocks = ses.n;
        pos->type = ses.b[h].h[0] & 3;
        memcpy(pos->name, ses.b[h].h + 1, 10);
    }
    portEXIT_CRITICAL(&ses_mux);
    if (ok && pos) {
        int l = 10;
        while (l > 0 && pos->name[l - 1] == ' ') l--;
        pos->name[l] = 0;
        for (int k = 0; k < l; k++) if (pos->name[k] < 32 || pos->name[k] > 126) pos->name[k] = '?';
    }
    return ok;
}

bool zx_turbo_continue(void)
{
    if (!zx_turbo_next(NULL)) return false;
    int h = ses.ptr;
    ESP_LOGI(TAG, "continue (web): block %d", h);
    ses.ptr = h + 2;
    if (!queue_job(1, h, 0)) {
        ses.ptr = h;
        return false;
    }
    return true;
}
