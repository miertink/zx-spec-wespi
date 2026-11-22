/*
 * File menu for the Spectrum.
 *
 * The menu is a BASIC program, rebuilt here whenever the file list changes
 * and written to ZX_MENU_PATH, which the tape player loops from power-up:
 *
 *    1 REM <Z80 menu code (asm/spmenu.asm)><file list>
 *   10 IF USR 23760=1 THEN GO TO 20
 *   15 LOAD ""
 *   20..60  WiFi password: INPUT LINE p$, put where the WiFi module wants it, sent
 *
 * The user picks a file; the menu sends a short SAVE block back through MIC
 * (flag ZX_MENU_REPLY_FLAG: 'L', list generation, index low, index high) and
 * returns to BASIC, whose LOAD "" then loads the file the ESP32 plays.
 *
 * The rest lives in modules (asm/spinfo.asm, asm/spwifi.asm), so the menu
 * that loads at every start stays small: the menu asks for one ('O', list
 * generation, module) and the ESP32 builds it with fresh data and plays it
 * as one block, which the menu loads at OVL and runs. In the WiFi module
 * the user picks a network; BASIC asks for the password with the Spectrum's
 * own editor, and the module sends 'W', list id, network, password length,
 * password; the ESP32 stores it and connects.
 *
 * Texts and the language's flag come with the data, in the language chosen
 * on the web page or on the menu's info screen (zx_text.c, web_i18n.c).
 *
 * The list generation byte changes when the list of files changes, so a
 * reply from a menu loaded before is rejected instead of loading the wrong
 * file (or a module built for another menu).
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include <sys/stat.h>

#include "tap_player.h"
#include "tape_file.h"
#include "mic_in.h"
#include "zx_menu.h"
#include "zx_save.h"
#include "wifi_sta.h"
#include "zx_turbo.h"
#include "zx_text.h"
#include "web_i18n.h"

static const char *TAG = "zx_menu";

#define MENU_MAX_FILES   200                /* count is one byte in the menu */
#define MENU_NAME_MAX    21                 /* columns 2..22; type and size follow */
#define MENU_INFO_MAX    26                 /* row 1, left of the stripes */
#define MENU_SYS_MAX     30                 /* lines of the info screen */
#define MENU_NETS        10                 /* WiFi networks, letters A..J */
#define MENU_NET_NAME    24
#define MENU_TEXT_MAX    32                 /* a text line of the menu or a module */
#define MENU_CODE_ADDR   23760              /* PROG (23755) + 5: first REM byte */
#define MENU_AUTOSTART   10
#define MENU_RELOOP_MS   3000               /* after the chosen file, back to the menu */
#define FILE_NAME_LEN    CONFIG_SPIFFS_OBJ_NAME_LEN

extern const uint8_t spmenu_start[] asm("_binary_spmenu_bin_start");
extern const uint8_t spmenu_end[]   asm("_binary_spmenu_bin_end");
extern const uint8_t spwifi_start[] asm("_binary_spwifi_bin_start");
extern const uint8_t spwifi_end[]   asm("_binary_spwifi_bin_end");
extern const uint8_t spinfo_start[] asm("_binary_spinfo_bin_start");
extern const uint8_t spinfo_end[]   asm("_binary_spinfo_bin_end");

/* Modules (see "Modules" in asm/spmenu.asm) */
#define OVL_FLAG         0xA6
#define OVL_MAX          0x0700             /* the menu loads at most this, checksum included */
#define OVL_PILOT        2600               /* the menu is already in the ROM loader */
enum { MOD_WIFI = 1, MOD_INFO = 2 };

#define MENU_IDLE_MS     120000             /* menu open with no choice: loop again */

#ifdef CONFIG_ZXS_MENU_ON_RESET
#define ON_RESET 1                          /* menu loops only from power-up / Spectrum reset */
#else
#define ON_RESET 0
#endif

typedef struct {
    uint8_t cmd;
    uint8_t gen;
    uint16_t index;
    char pw[64];                            /* 'W': the WiFi password */
} reply_t;

static char files[MENU_MAX_FILES][FILE_NAME_LEN];
static int nfiles;
static uint8_t file_kb[MENU_MAX_FILES];
static char menu_ip[16];      /* IP shown by the current menu */
static uint8_t gen;
static uint32_t list_hash;                  /* of the file names: gen changes with it */
static uint8_t wifi_id;                     /* WiFi module: id of its list, echoed in 'W' */
static char wifi_nets[MENU_NETS][33];       /* its networks, full names */
static int wifi_nnets;
static SemaphoreHandle_t lock;
static QueueHandle_t reply_q;

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* Lists the tape files in /spiffs, sorted, skipping '_' files (the menu). */
static void scan_files(void)
{
    nfiles = 0;
    DIR *dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE(TAG, "cannot open /spiffs");
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (n[0] == '_' || len < 5 || len >= FILE_NAME_LEN) continue;
        if (!tape_file_supported(n)) continue;
        if (nfiles == MENU_MAX_FILES) {
            ESP_LOGW(TAG, "more than %d files, the rest is not listed", MENU_MAX_FILES);
            break;
        }
        strlcpy(files[nfiles++], n, FILE_NAME_LEN);
    }
    closedir(dir);
    qsort(files, nfiles, FILE_NAME_LEN, cmp_name);
    for (int i = 0; i < nfiles; i++) {
        char path[8 + FILE_NAME_LEN];
        struct stat st;
        snprintf(path, sizeof(path), "/spiffs/%s", files[i]);
        long kb = stat(path, &st) == 0 ? (st.st_size + 1023) / 1024 : 0;
        file_kb[i] = kb > 255 ? 255 : kb;
    }
}

/* Puts a string for the menu: printable ASCII, at most `max` characters,
 * last character with bit 7 set (a single space if empty). Returns bytes. */
static size_t put_str(uint8_t *out, const char *s, size_t len, size_t max)
{
    if (len > max) len = max;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        out[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    if (n == 0) out[n++] = ' ';
    out[n - 1] |= 0x80;
    return n;
}

/* Type code for the menu: 0 TAP, 1 TZX, 2 PZX, 3 CSW */
static uint8_t file_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".tzx") == 0) return 1;
    if (strcasecmp(dot, ".pzx") == 0) return 2;
    if (strcasecmp(dot, ".csw") == 0) return 3;
    return 0;
}

/* A number in a BASIC line: its digits, then 0x0E and the 5-byte form
 * (small integer: 0, 0, low, high, 0). Returns bytes. */
static size_t put_num(uint8_t *out, unsigned v)
{
    size_t n = sprintf((char *)out, "%u", v);
    const uint8_t f[6] = { 0x0E, 0, 0, v & 0xFF, (v >> 8) & 0xFF, 0 };
    memcpy(out + n, f, 6);
    return n + 6;
}

static void put_block(FILE *f, uint8_t flag, const uint8_t *data, size_t len)
{
    uint8_t x = flag;
    for (size_t i = 0; i < len; i++) x ^= data[i];
    uint16_t blen = len + 2;
    uint8_t head[3] = { blen & 0xFF, blen >> 8, flag };
    fwrite(head, 1, 3, f);
    fwrite(data, 1, len, f);
    fwrite(&x, 1, 1, f);
}

/* FNV-1a of the file names, in menu order */
static uint32_t names_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < nfiles; i++) {
        for (const char *s = files[i]; ; s++) {
            h = (h ^ (uint8_t)*s) * 16777619u;
            if (!*s) break;
        }
    }
    return h;
}

/* Builds the menu program and writes it as a .tap. Caller holds the lock. */
static bool write_menu(void)
{
    size_t code_len = spmenu_end - spmenu_start;
    /* 512: line headers and the BASIC lines 10..60 (~220 bytes), with room */
    size_t max = 512 + code_len + 2 + ZX_FLAG_LEN + ZT_MENU_N * (MENU_TEXT_MAX + 1) + (MENU_INFO_MAX + 1)
                 + MENU_MAX_FILES * (MENU_NAME_MAX + 2);
    uint8_t *prog = malloc(max);
    if (!prog) {
        ESP_LOGE(TAG, "out of memory");
        return false;
    }

    /* line 1: REM <code><gen><count><flag><texts><info><files> */
    const zx_text_t *Tx = &zx_text[i18n_menu_lang()];
    size_t p = 4;                                    /* line number + length filled below */
    prog[p++] = 0xEA;                                /* REM */
    memcpy(prog + p, spmenu_start, code_len);
    p += code_len;
    prog[p++] = gen;
    prog[p++] = nfiles;
    memcpy(prog + p, zx_flag[i18n_menu_lang()], ZX_FLAG_LEN);
    p += ZX_FLAG_LEN;
    for (int i = 0; i < ZT_MENU_N; i++) p += put_str(prog + p, Tx->menu[i], strlen(Tx->menu[i]), MENU_TEXT_MAX);

    /* row 1: address and number of tapes */
    char line[48];
    const char *ip = wifi_get_ip();
    strlcpy(menu_ip, ip, sizeof(menu_ip));
    snprintf(line, sizeof(line), "%s %d %s", ip[0] ? ip : Tx->no_wifi, nfiles, nfiles == 1 ? Tx->tape : Tx->tapes);
    p += put_str(prog + p, line, strlen(line), MENU_INFO_MAX);

    /* files: type, size in KB, name without the extension */
    for (int i = 0; i < nfiles; i++) {
        uint8_t type = file_type(files[i]);
#if CONFIG_ZXS_TURBO
        char fpath[8 + FILE_NAME_LEN];
        snprintf(fpath, sizeof(fpath), "/spiffs/%s", files[i]);
        if (zx_turbo_capable(fpath)) type |= 0x80;
#endif
        prog[p++] = type;
        prog[p++] = file_kb[i];
        const char *dot = strrchr(files[i], '.');
        size_t len = dot ? (size_t)(dot - files[i]) : strlen(files[i]);
        p += put_str(prog + p, files[i], len, MENU_NAME_MAX);
    }

    prog[p++] = 0x0D;
    prog[0] = 0;
    prog[1] = 1;
    prog[2] = (p - 4) & 0xFF;
    prog[3] = (p - 4) >> 8;

    /* lines 10..60, see the top of this file */
    size_t lstart = 0;
#define LINE(n)   do { lstart = p; prog[p++] = 0; prog[p++] = (n); p += 2; } while (0)
#define END()     do { prog[p++] = 0x0D; prog[lstart + 2] = (p - lstart - 4) & 0xFF; \
                       prog[lstart + 3] = (p - lstart - 4) >> 8; } while (0)
#define T(...)    do { static const uint8_t t_[] = { __VA_ARGS__ }; memcpy(prog + p, t_, sizeof(t_)); \
                       p += sizeof(t_); } while (0)
#define NUM(v)    (p += put_num(prog + p, (v)))
#define STR(s)    do { prog[p++] = '"'; memcpy(prog + p, s, strlen(s)); p += strlen(s); prog[p++] = '"'; } while (0)
    enum { IF = 0xFA, USR = 0xC0, THEN = 0xCB, GOTO = 0xEC, LOAD = 0xEF, INPUT = 0xEE, LINE_ = 0xCA,
           LEN = 0xB1, LET = 0xF1, PEEK = 0xBE, POKE = 0xF4, FOR = 0xEB, TO = 0xCC, CODE = 0xAF,
           NEXT = 0xF3, RANDOMIZE = 0xF9 };
    LINE(MENU_AUTOSTART); T(IF, USR); NUM(MENU_CODE_ADDR); T('='); NUM(1); T(THEN, GOTO); NUM(20); END();
    LINE(15); T(LOAD); STR(""); END();
    LINE(20); T(INPUT); STR(Tx->pw_prompt); T(';', LINE_, 'p', '$'); END();
    LINE(30); T(IF, LEN, 'p', '$', '>'); NUM(63); T(THEN, GOTO); NUM(20); END();
    LINE(40); T(LET, 'b', '=', PEEK); NUM(MENU_CODE_ADDR + 6); T('+'); NUM(256); T('*', PEEK);
              NUM(MENU_CODE_ADDR + 7); T(':', POKE, 'b', ',', LEN, 'p', '$'); END();
    LINE(50); T(FOR, 'i', '='); NUM(1); T(TO, LEN, 'p', '$', ':', POKE, 'b', '+', 'i', ',', CODE, 'p', '$',
              '(', 'i', ')', ':', NEXT, 'i'); END();
    LINE(60); T(RANDOMIZE, USR); NUM(MENU_CODE_ADDR + 3); T(':', GOTO); NUM(MENU_AUTOSTART); END();
#undef LINE
#undef END
#undef T
#undef NUM
#undef STR

    /* header: Program "ZX-Wespi", autostart line 10, no variables */
    uint8_t hdr[17];
    hdr[0] = 0;
    memcpy(hdr + 1, "ZX-Wespi  ", 10);
    hdr[11] = p & 0xFF;      hdr[12] = p >> 8;
    hdr[13] = MENU_AUTOSTART; hdr[14] = 0;
    hdr[15] = p & 0xFF;      hdr[16] = p >> 8;

    FILE *f = fopen(ZX_MENU_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", ZX_MENU_PATH);
        free(prog);
        return false;
    }
    put_block(f, 0x00, hdr, sizeof(hdr));
    put_block(f, 0xFF, prog, p);
    bool ok = ferror(f) == 0;
    ok &= fclose(f) == 0;
    free(prog);
    if (ok) {
        ESP_LOGI(TAG, "menu built: %d file(s), list generation %u, program %u bytes",
                 nfiles, gen, (unsigned)p);
    } else {
        ESP_LOGE(TAG, "error writing %s", ZX_MENU_PATH);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Modules */

typedef struct {
    char label[12];                         /* web status */
    size_t len;                             /* b: flag, payload, checksum */
    uint8_t b[];
} module_t;

/* A module calls the menu's routines by address: it must have been
 * assembled with this menu (asm/build.cmd builds both). OVL+8 holds the
 * menu's `list`, which moves with any change of the menu code. */
static bool module_matches(const uint8_t *code, size_t len)
{
    uint16_t list = MENU_CODE_ADDR + (spmenu_end - spmenu_start);
    return len > 10 && (code[8] | (code[9] << 8)) == list;
}

/* Module `id` as a tape block, built now with fresh data; id 0: the empty
 * module (the menu is out of date). NULL if out of memory or too big. */
static module_t *build_module(int id)
{
    const uint8_t *code = NULL;
    size_t code_len = 0;
    const char *label = "_old_menu";
    if (id == MOD_WIFI) {
        code = spwifi_start;
        code_len = spwifi_end - spwifi_start;
        label = "_wifi";
    } else if (id == MOD_INFO) {
        code = spinfo_start;
        code_len = spinfo_end - spinfo_start;
        label = "_info";
    }
    if (code && !module_matches(code, code_len)) return NULL;
    module_t *m = malloc(sizeof(module_t) + 1 + OVL_MAX);
    if (!m) return NULL;
    strlcpy(m->label, label, sizeof(m->label));
    uint8_t *pl = m->b + 1;                 /* payload: length word, code, data */
    size_t p = 2;
    if (code_len >= 2) {
        memcpy(pl, code, code_len);
        p = code_len;
    }
    char line[48];
    lang_t lang = i18n_menu_lang();
    const zx_text_t *T = &zx_text[lang];
#define PUT_TEXT(s) (p += put_str(pl + p, (s), strlen(s), MENU_TEXT_MAX))
    if (id == MOD_WIFI) {
        /* list id, count, texts, the network in use, the networks in reach
         * (signal 1..4, bit 7 = password needed); the full names stay here */
        wifi_net_t nets[MENU_NETS];
        int n = wifi_sta_scan(nets, MENU_NETS);
        xSemaphoreTake(lock, portMAX_DELAY);
        pl[p++] = ++wifi_id;
        pl[p++] = n;
        for (int i = 0; i < ZT_WIFI_N; i++) PUT_TEXT(T->wifi[i]);
        snprintf(line, sizeof(line), "%s%s", wifi_sta_ssid()[0] ? wifi_sta_ssid() : T->net_none,
                 wifi_sta_is_connected() ? "" : T->offline);
        p += put_str(pl + p, line, strlen(line), MENU_SYS_MAX);
        for (int i = 0; i < n; i++) {
            int r = nets[i].rssi;
            pl[p++] = (r > -55 ? 4 : r > -67 ? 3 : r > -75 ? 2 : 1) | (nets[i].secure ? 0x80 : 0);
            p += put_str(pl + p, nets[i].ssid, strlen(nets[i].ssid), MENU_NET_NAME);
            strlcpy(wifi_nets[i], nets[i].ssid, sizeof(wifi_nets[i]));
        }
        wifi_nnets = n;
        xSemaphoreGive(lock);
    } else if (id == MOD_INFO) {
        /* language, the flags, texts, then the lines: web address,
         * network, MAC, disk, tapes, version */
        pl[p++] = lang;
        for (int i = 0; i < LANG_COUNT; i++) {
            memcpy(pl + p, zx_flag[i], ZX_FLAG_LEN);
            p += ZX_FLAG_LEN;
        }
        for (int i = 0; i < ZT_INFO_N; i++) PUT_TEXT(T->info[i]);
        size_t nsys_at = p++;
        int nsys = 0;
        size_t total = 0, used = 0;
        const char *ip = wifi_get_ip();
#define SYS_LINE(...) do { snprintf(line, sizeof(line), __VA_ARGS__); \
                           p += put_str(pl + p, line, strlen(line), MENU_SYS_MAX); nsys++; } while (0)
        if (ip[0]) SYS_LINE(T->f_web, ip);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            SYS_LINE(T->f_wifi, (const char *)ap.ssid, ap.rssi);
        } else {
            SYS_LINE(T->f_wifi_off, wifi_sta_ssid());
        }
        SYS_LINE(T->f_mac, wifi_get_MAC_addr());
        if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
            SYS_LINE(T->f_disk, (unsigned)((total - used) / 1024), (unsigned)(total / 1024));
        }
        SYS_LINE(T->f_tapes, nfiles);
        SYS_LINE(T->f_ver, esp_ota_get_app_description()->version);
#undef SYS_LINE
        pl[nsys_at] = nsys;
    }
#undef PUT_TEXT
    if (p + 1 >= OVL_MAX) {                 /* the menu stores payload + checksum */
        ESP_LOGE(TAG, "module %d too big: %u bytes", id, (unsigned)p);
        free(m);
        return NULL;
    }
    pl[0] = p & 0xFF;
    pl[1] = p >> 8;
    m->b[0] = OVL_FLAG;
    uint8_t x = 0;
    for (size_t i = 0; i <= p; i++) x ^= m->b[i];
    m->b[p + 1] = x;
    m->len = p + 2;
    return m;
}

static void play_module(void *arg)
{
    module_t *m = arg;
    tape_file_play_mem(m->label, m->b, m->len, OVL_PILOT);
    free(m);
}

/* Builds module `id` (0: the empty one) and plays it; true if started */
static bool send_module(int id)
{
    int64_t t0 = esp_timer_get_time();
    module_t *m = build_module(id);
    if (!m) return false;
    ESP_LOGI(TAG, "module %s: %u bytes, built in %d ms", m->label, (unsigned)m->len,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (tap_player_run(play_module, m, m->label) != ESP_OK) {
        free(m);
        return false;
    }
    return true;
}

#define MENU_LATER_MS 2000
static volatile TickType_t rebuild_at;      /* 0 = no rebuild pending */

void zx_menu_rebuild_later(void)
{
    rebuild_at = xTaskGetTickCount() + pdMS_TO_TICKS(MENU_LATER_MS);
    if (!rebuild_at) rebuild_at = 1;
}

/* Starts the menu loop. Under the lock: the loop never starts while the menu
 * file is being rewritten (the rewrite stops it first, see below). */
static void menu_loop(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    tap_player_loop(ZX_MENU_PATH);
    xSemaphoreGive(lock);
}

void zx_menu_rebuild(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    /* The loop reopens the file every round: stop it while rewriting. */
    bool restart = tap_player_is_busy() && strcmp(tap_player_current(), ZX_MENU_FILE) == 0;
    if (restart) {
        tap_player_stop();
        for (int i = 0; i < 100 && tap_player_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    }
    scan_files();
    uint32_t h = names_hash();
    if (h != list_hash) {                   /* not for a new IP: the menu loaded stays valid */
        list_hash = h;
        gen++;
    }
    bool ok = write_menu();
    if (restart && ok) tap_player_loop(ZX_MENU_PATH);
    xSemaphoreGive(lock);
}

void zx_menu_on_block(const uint8_t *block, size_t len, int64_t start_us)
{
    /* anything that is not a menu reply is a SAVE for the server */
    if (block[0] != ZX_MENU_REPLY_FLAG) {
        zx_save_on_block(block, len, start_us);
        return;
    }
    /* 'W': flag, 'W', gen, network, password length, password, checksum */
    if (len >= 6 && block[1] == 'W') {
        size_t plen = block[4];
        if (plen > 63 || len != 6 + plen) {
            ESP_LOGW(TAG, "WiFi reply: bad length");
            return;
        }
        reply_t r = { .cmd = 'W', .gen = block[2], .index = block[3] };
        memcpy(r.pw, block + 5, plen);
        r.pw[plen] = 0;
        xQueueSend(reply_q, &r, 0);
        return;
    }
    /* flag, command, gen, index low, index high, checksum */
    if (len != 6) return;
    if (!strchr("LMTEOG", block[1]) || !block[1]) {
        ESP_LOGW(TAG, "reply with unknown command 0x%02x", block[1]);
        return;
    }
    reply_t r = { .cmd = block[1], .gen = block[2], .index = block[3] | (block[4] << 8) };
    xQueueSend(reply_q, &r, 0);
}

/* Spectrum reset seen on MIC (MIC task): handled by the menu task */
static void __attribute__((unused)) on_reset(void)
{
    reply_t r = { .cmd = 'R' };
    xQueueSend(reply_q, &r, 0);
}

static bool menu_looping(void)
{
    return tap_player_is_looping() && strcmp(tap_player_current(), ZX_MENU_FILE) == 0;
}

#ifndef CONFIG_ZXS_MENU_RESET_DELAY_MS
#define CONFIG_ZXS_MENU_RESET_DELAY_MS 5000
#endif

static void zx_menu_task(void *arg)
{
    reply_t r;
    char path[8 + FILE_NAME_LEN];
    bool menu_open = false;                 /* 'M' received, loop stopped, no choice yet */
    TickType_t opened = 0;
    bool loop_due = false;                  /* Spectrum reset: loop once it has started */
    TickType_t reset_at = 0;
    while (1) {
        if (xQueueReceive(reply_q, &r, pdMS_TO_TICKS(loop_due ? 100 : 1000)) != pdTRUE) {
            if (loop_due && xTaskGetTickCount() - reset_at >= pdMS_TO_TICKS(CONFIG_ZXS_MENU_RESET_DELAY_MS)) {
                loop_due = false;
                if (!tap_player_is_busy()) {
                    ESP_LOGI(TAG, "Spectrum reset %d ms ago: menu loop", CONFIG_ZXS_MENU_RESET_DELAY_MS);
                    menu_loop();  /* also drops a multi-load position */
                }
            }
            if (rebuild_at && (int32_t)(xTaskGetTickCount() - rebuild_at) >= 0) {
                rebuild_at = 0;
                zx_menu_rebuild();          /* after uploads */
            }
            if (strcmp(wifi_get_ip(), menu_ip) != 0) {
                ESP_LOGI(TAG, "IP is now %s: menu rebuilt", wifi_get_ip()[0] ? wifi_get_ip() : "none");
                zx_menu_rebuild();
            }
            /* menu left open (or the Spectrum was reset): offer the loop again */
            if (menu_open && xTaskGetTickCount() - opened >= pdMS_TO_TICKS(MENU_IDLE_MS)) {
                menu_open = false;
                if (!ON_RESET && !tap_player_is_busy()) {
                    ESP_LOGI(TAG, "no choice in %d s: menu loop again", MENU_IDLE_MS / 1000);
                    menu_loop();
                }
            }
            continue;
        }

        if (r.cmd == 'T') {                 /* a LOAD of a program served on request */
            zx_turbo_request(r.gen, r.index & 0xFF);
            continue;
        }

        if (r.cmd == 'E') {                 /* a turbo block failed: Bp, bytes left */
            zx_turbo_error(r.gen, r.index);
            continue;
        }

        if (r.cmd == 'W') {                 /* a WiFi network chosen in the WiFi module */
            char ssid[33] = "";
            xSemaphoreTake(lock, portMAX_DELAY);
            if (r.gen == wifi_id && r.index < wifi_nnets) strlcpy(ssid, wifi_nets[r.index], sizeof(ssid));
            xSemaphoreGive(lock);
            if (!ssid[0]) {
                ESP_LOGW(TAG, "WiFi reply from an old list or for an unknown network: ignored");
            } else {
                ESP_LOGI(TAG, "WiFi chosen on the Spectrum: \"%s\"", ssid);
                wifi_sta_set_network(ssid, r.pw);
            }
            memset(r.pw, 0, sizeof(r.pw));
            continue;
        }

        if (r.cmd == 'R') {                 /* the Spectrum was reset / powered up */
            zx_turbo_stop();
            menu_open = false;
            if (tap_player_is_busy()) continue;
            /* give it time to start up and the user to type LOAD "" */
            ESP_LOGI(TAG, "Spectrum reset: menu loop in %d ms", CONFIG_ZXS_MENU_RESET_DELAY_MS);
            loop_due = true;
            reset_at = xTaskGetTickCount();
            continue;
        }
        loop_due = false;                   /* the Spectrum is talking to us */

        if (r.cmd == 'M') {
            xSemaphoreTake(lock, portMAX_DELAY);
            bool current = r.gen == gen;
            xSemaphoreGive(lock);
            if (!current) {
                ESP_LOGW(TAG, "old menu started (list generation %u, current %u): loop kept",
                         r.gen, gen);
                continue;
            }
            ESP_LOGI(TAG, "menu running on the Spectrum: loop stopped");
            zx_turbo_stop();
            if (menu_looping()) tap_player_stop();
            menu_open = true;
            opened = xTaskGetTickCount();
            continue;
        }

        if (r.cmd == 'G') {                 /* language chosen in the info module */
            menu_open = false;
            if (r.index < LANG_COUNT) {
                i18n_choose((lang_t)r.index);
                zx_menu_rebuild();
            }
            /* the menu said "press a key": its LOAD "" takes the menu, in the
             * new language */
            if (!tap_player_is_busy()) menu_loop();
            continue;
        }

        if (r.cmd == 'O') {                 /* the menu wants a module */
            xSemaphoreTake(lock, portMAX_DELAY);
            bool current = r.gen == gen;
            xSemaphoreGive(lock);
            menu_open = true;
            opened = xTaskGetTickCount();
            if (current) {
                ESP_LOGI(TAG, "module %u requested", r.index);
                if (!send_module(r.index == MOD_WIFI || r.index == MOD_INFO ? r.index : MOD_INFO))
                    ESP_LOGE(TAG, "module %u: not sent", r.index);
                continue;
            }
            /* a menu from before the files changed: its indexes are wrong.
             * The empty module tells it so; then the new menu, for its LOAD "" */
            ESP_LOGW(TAG, "module from an old menu (list generation %u, current %u): menu again",
                     r.gen, gen);
            menu_open = false;
            if (send_module(0)) {
                vTaskDelay(pdMS_TO_TICKS(500));
                for (int i = 0; i < 100 && tap_player_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!tap_player_is_busy()) menu_loop();
            continue;
        }

        menu_open = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        bool valid = false;
        if (r.gen != gen) {
            ESP_LOGW(TAG, "reply from an old menu (list generation %u, current %u): ignored, "
                          "load the menu again", r.gen, gen);
        } else if (r.index >= nfiles) {
            ESP_LOGW(TAG, "reply with index %u, only %d file(s)", r.index, nfiles);
        } else {
            snprintf(path, sizeof(path), "/spiffs/%s", files[r.index]);
            valid = true;
        }
        xSemaphoreGive(lock);
        if (!valid) {
            /* reload the menu */
            if (!tap_player_is_busy()) menu_loop();
            continue;
        }

        ESP_LOGI(TAG, "selected %u: %s", r.index, path);
#if CONFIG_ZXS_TURBO
        if (zx_turbo_start(path)) continue;     /* served on request from now on */
#endif
        if (tap_player_play_fast(path) != ESP_OK) {
            ESP_LOGE(TAG, "could not start %s", path);
            continue;
        }
#if CONFIG_ZXS_MENU_RELOOP
        /* when the chosen file is done, offer the menu again */
        vTaskDelay(pdMS_TO_TICKS(500));
        while (tap_player_is_busy() && !tap_player_is_looping()) vTaskDelay(pdMS_TO_TICKS(200));
        if (tap_player_has_resume()) {
            /* multi-load game waiting for its next part: the menu would be
             * loaded by the game instead; Play (web) continues the file */
            ESP_LOGI(TAG, "%s stopped at a \"stop the tape\" mark: menu not looped, "
                          "play the file again (web) to continue", path);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(MENU_RELOOP_MS));
        if (!tap_player_is_busy()) menu_loop();
#endif
    }
}

void zx_menu_init(void)
{
    lock = xSemaphoreCreateMutex();
    reply_q = xQueueCreate(2, sizeof(reply_t));
    gen = esp_random() & 0xFF;
    if (!module_matches(spwifi_start, spwifi_end - spwifi_start)
        || !module_matches(spinfo_start, spinfo_end - spinfo_start)) {
        ESP_LOGE(TAG, "the menu modules were assembled for another menu: run main/asm/build.cmd "
                      "and build again (the WiFi and info screens are off)");
    }
    xTaskCreate(zx_menu_task, "zx_menu", 4096, NULL, 6, NULL);
    zx_save_init();
    mic_in_set_block_cb(zx_menu_on_block);

    xSemaphoreTake(lock, portMAX_DELAY);
    scan_files();
    list_hash = names_hash();
    bool ok = write_menu();
    if (ok) tap_player_loop(ZX_MENU_PATH);
    xSemaphoreGive(lock);
#ifdef CONFIG_ZXS_MENU_RESET_DETECT
    mic_in_set_reset_cb(on_reset);
#endif
}
