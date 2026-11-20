/* Web server: the page (tape status, upload, file list), the tape controls
 * (/play, /stop, /resume), /status for the live panel, /log, and the file
 * transfers (upload, download, delete, rename). Uploading zx_iot.bin writes
 * a firmware update to the other OTA partition.
 *
 * Started from the ESP-IDF "HTTP File Server" example (public domain / CC0).
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include "tap_player.h"
#include "tape_file.h"
#include "tape_status.h"
#include "zx_menu.h"
#include "zx_turbo.h"
#include "web_i18n.h"
#include "weblog.h"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    return ESP_OK;
}

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

/* Page writer: the page goes out in chunks of PW_SIZE bytes instead of many
 * small ones. If a send fails (the browser left: reload, another click),
 * the rest of the page is dropped at once; sending every remaining piece to
 * a closed socket kept the server busy for seconds. The HTTP server handles
 * one request at a time, so one writer is enough. */
#define PW_SIZE 1400
static struct {
    httpd_req_t *req;
    char buf[PW_SIZE];
    size_t n;
    bool dead;
} pw;

static void pw_begin(httpd_req_t *req)
{
    pw.req = req;
    pw.n = 0;
    pw.dead = false;
}

static void pw_flush(void)
{
    if (!pw.dead && pw.n && httpd_resp_send_chunk(pw.req, pw.buf, pw.n) != ESP_OK) {
        pw.dead = true;
        ESP_LOGW(TAG, "page: the browser left, rest not sent");
    }
    pw.n = 0;
}

static void pw_write(const char *s, size_t len)
{
    while (len && !pw.dead) {
        size_t k = sizeof(pw.buf) - pw.n;
        if (k > len) k = len;
        memcpy(pw.buf + pw.n, s, k);
        pw.n += k;
        s += k;
        len -= k;
        if (pw.n == sizeof(pw.buf)) pw_flush();
    }
}

static void pw_str(const char *s)
{
    pw_write(s, strlen(s));
}

static void pw_end(void)
{
    pw_flush();
    if (!pw.dead) httpd_resp_send_chunk(pw.req, NULL, 0);
}

/* Sends a string with the HTML special characters escaped */
static void send_esc(httpd_req_t *req, const char *s)
{
    char buf[96];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        size_t need = rep ? strlen(rep) : 1;
        if (n + need >= sizeof(buf)) {
            buf[n] = 0;
            pw_str(buf);
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, need);
            n += need;
        } else {
            buf[n++] = *s;
        }
    }
    buf[n] = 0;
    if (n) pw_str(buf);
}

/* Base name usable on the server and in URLs: letters, digits, '-' and '_'
 * (spaces and anything else become '_'), no leading '_' (hidden files). */
static void file_safe(char *out, size_t size, const char *in)
{
    size_t o = 0;
    for (; *in && o + 1 < size; in++) {
        char c = *in;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (keep) out[o++] = c;
        else if (o > 0 && out[o - 1] != '_') out[o++] = '_';
    }
    while (o > 0 && out[o - 1] == '_') o--;
    out[o] = 0;
}

#define RENAME_BASE_MAX 24      /* + extension fits SPIFFS names and the menu */

/* Built-in logo, used when no /spiffs/_logo.(png|svg|jpg) was uploaded: a
 * compact cassette at its real proportions (100.4 x 63.8 mm, 1 mm = 3.15
 * units), reels centred 42.5 mm apart, the name centred on its label and
 * the Spectrum colours as a band behind the window; the reels (class
 * "reel") spin while the tape plays (header class "spin", set by the page
 * script) */
static const char LOGO_SVG[] =
    "<svg viewBox=\"0 0 316 201\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"ZX-Spec Wespi\">"
    "<defs><clipPath id=\"lbl\"><rect x=\"18\" y=\"12\" width=\"280\" height=\"124\" rx=\"8\"/></clipPath>"
    "<clipPath id=\"win\"><rect x=\"62\" y=\"66\" width=\"192\" height=\"50\" rx=\"25\"/></clipPath></defs>"
    "<rect x=\"2\" y=\"2\" width=\"312\" height=\"197\" rx=\"12\" fill=\"#1f222b\" stroke=\"#ececf1\" stroke-width=\"3\"/>"
    "<circle cx=\"11\" cy=\"11\" r=\"3.5\" fill=\"#2b2f3b\"/>"
    "<circle cx=\"305\" cy=\"11\" r=\"3.5\" fill=\"#2b2f3b\"/>"
    "<circle cx=\"11\" cy=\"190\" r=\"3.5\" fill=\"#2b2f3b\"/>"
    "<circle cx=\"305\" cy=\"190\" r=\"3.5\" fill=\"#2b2f3b\"/>"
    "<rect x=\"18\" y=\"12\" width=\"280\" height=\"124\" rx=\"8\" fill=\"#0d0e12\"/><g clip-path=\"url(#lbl)\">"
    "<polygon points=\"48,79 292,79 286,85 42,85\" fill=\"#e4002b\"/>"
    "<polygon points=\"42,85 286,85 280,91 36,91\" fill=\"#ffcd00\"/>"
    "<polygon points=\"36,91 280,91 274,97 30,97\" fill=\"#00a651\"/>"
    "<polygon points=\"30,97 274,97 268,103 24,103\" fill=\"#00a9e0\"/></g>"
    "<g font-family=\"'Arial Black','Helvetica Neue',Arial,sans-serif\" font-weight=\"900\" font-style=\"italic\" font-size=\"30\">"
    "<text x=\"158\" y=\"50\" text-anchor=\"middle\" textLength=\"200\" lengthAdjust=\"spacingAndGlyphs\">"
    "<tspan fill=\"#ececf1\">ZX-Spec </tspan><tspan fill=\"#ffcd00\">Wespi</tspan></text></g>"
    "<rect x=\"62\" y=\"66\" width=\"192\" height=\"50\" rx=\"25\" fill=\"#171920\"/><g clip-path=\"url(#win)\">"
    "<circle cx=\"91\" cy=\"91\" r=\"22\" fill=\"#2a2e3a\"/>"
    "<circle cx=\"225\" cy=\"91\" r=\"17\" fill=\"#2a2e3a\"/></g><g class=\"reel\">"
    "<circle cx=\"91\" cy=\"91\" r=\"13\" fill=\"none\" stroke=\"#ececf1\" stroke-width=\"3\"/>"
    "<circle cx=\"91\" cy=\"91\" r=\"4.5\" fill=\"#ececf1\"/>"
    "<rect x=\"89.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(0 91 91)\"/>"
    "<rect x=\"89.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(120 91 91)\"/>"
    "<rect x=\"89.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(240 91 91)\"/>"
    "</g><g class=\"reel\">"
    "<circle cx=\"225\" cy=\"91\" r=\"13\" fill=\"none\" stroke=\"#ececf1\" stroke-width=\"3\"/>"
    "<circle cx=\"225\" cy=\"91\" r=\"4.5\" fill=\"#ececf1\"/>"
    "<rect x=\"223.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(0 225 91)\"/>"
    "<rect x=\"223.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(120 225 91)\"/>"
    "<rect x=\"223.5\" y=\"79\" width=\"3\" height=\"7\" rx=\"1\" fill=\"#ececf1\" transform=\"rotate(240 225 91)\"/>"
    "</g>"
    "<rect x=\"62\" y=\"66\" width=\"192\" height=\"50\" rx=\"25\" fill=\"none\" stroke=\"#9497a6\" stroke-width=\"2\"/>"
    "<path d=\"M48 199 L68 163 L248 163 L268 199\" fill=\"#171920\" stroke=\"#2b2f3b\" stroke-width=\"2\"/>"
    "<circle cx=\"98\" cy=\"185\" r=\"5\" fill=\"#0d0e12\"/>"
    "<circle cx=\"218\" cy=\"185\" r=\"5\" fill=\"#0d0e12\"/>"
    "<g fill=\"none\" stroke=\"#00a9e0\" stroke-width=\"3.5\" stroke-linecap=\"round\">"
    "<path d=\"M150 183 a11 11 0 0 1 16 0\"/><path d=\"M143 176 a21 21 0 0 1 30 0\"/></g>"
    "<circle cx=\"158\" cy=\"189\" r=\"3.2\" fill=\"#00a9e0\"/></svg>";

/* Language flags (page and Spectrum menu), in lang_t order */
static const char *const FLAG_SVG[LANG_COUNT] = {
    "<svg viewBox=\"0 0 20 14\"><rect width=\"20\" height=\"14\" fill=\"#009c3b\"/>"
    "<polygon points=\"10,1.6 18.4,7 10,12.4 1.6,7\" fill=\"#ffdf00\"/>"
    "<circle cx=\"10\" cy=\"7\" r=\"3.3\" fill=\"#002776\"/></svg>",
    "<svg viewBox=\"0 0 20 14\"><rect width=\"20\" height=\"14\" fill=\"#c60b1e\"/>"
    "<rect y=\"3.5\" width=\"20\" height=\"7\" fill=\"#ffc400\"/></svg>",
    "<svg viewBox=\"0 0 20 14\"><rect width=\"20\" height=\"14\" fill=\"#fff\"/>"
    "<path d=\"M0 0h20v1.08H0zM0 2.15h20v1.08H0zM0 4.31h20v1.08H0zM0 6.46h20v1.08H0z"
    "M0 8.62h20v1.08H0zM0 10.77h20v1.08H0zM0 12.92h20V14H0z\" fill=\"#b22234\"/>"
    "<rect width=\"9\" height=\"7.54\" fill=\"#3c3b6e\"/></svg>",
};
static const char *const LANG_NAME[LANG_COUNT] = { "Português", "Español", "English" };

static void send_flags(lang_t cur)
{
    char s[80];
    pw_str("<nav class=\"flags\">");
    for (int i = 0; i < LANG_COUNT; i++) {
        snprintf(s, sizeof(s), "<a href=\"/lang/%s\" title=\"%s\" lang=\"%s\"%s>", i18n_code(i),
                 LANG_NAME[i], i18n_code(i), i == (int)cur ? " class=\"on\"" : "");
        pw_str(s);
        pw_str(FLAG_SVG[i]);
        pw_str("</a>");
    }
    pw_str("</nav>");
}

static void send_logo(httpd_req_t *req)
{
    static const char *logos[] = { "_logo.png", "_logo.svg", "_logo.jpg" };
    struct stat st;
    char path[32];
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "/spiffs/%s", logos[i]);
        if (stat(path, &st) == 0) {
            pw_str("<img src=\"/");
            pw_str(logos[i]);
            pw_str("\" alt=\"logo\">");
            return;
        }
    }
    pw_str(LOGO_SVG);
}

static void send_size(httpd_req_t *req, long bytes)
{
    char s[24];
    if (bytes < 1024) snprintf(s, sizeof(s), "%ld B", bytes);
    else snprintf(s, sizeof(s), "%ld,%ld KB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    pw_str(s);
}

/* An error for the user, as plain text in their language (a fetch shows it
 * in an alert, a form post as a page) */
static void send_err_l(httpd_req_t *req, const char *status, const char *key)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, tr(i18n_lang(req), key));
}

/* The menu tape is made by the ESP32 itself (zx_menu.c) and rebuilt at every
 * change: it can be played, not renamed, deleted or replaced. `name` with or
 * without a leading "/". */
static bool is_protected(const char *name)
{
    if (name[0] == '/') name++;
    return strcmp(name, ZX_MENU_FILE) == 0;
}

/* A form posting to /<action>/<name> */
static void send_form_open(httpd_req_t *req, const char *action, const char *name)
{
    pw_str("<form method=\"post\" action=\"/");
    pw_str(action);
    pw_str("/");
    send_esc(req, name);
    pw_str("\"");
}

/* One row of the file table */
static void send_file_row(httpd_req_t *req, const char *name, long size, lang_t L)
{
    char path[FILE_PATH_MAX], title[40], base[CONFIG_SPIFFS_OBJ_NAME_LEN], sugg[RENAME_BASE_MAX + 1];
    bool tape = tape_file_supported(name);
    const char *dot = strrchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    if (blen >= sizeof(base)) blen = sizeof(base) - 1;
    memcpy(base, name, blen);
    base[blen] = 0;

    snprintf(path, sizeof(path), "/spiffs/%s", name);
    bool has_title = tape && tape_file_title(path, title, sizeof(title));

    pw_str("<tr><td><a class=\"fname\" href=\"/");
    send_esc(req, name);
    pw_str("\">");
    send_esc(req, name);
    pw_str("</a>");
    if (has_title && strcasecmp(title, base) != 0) {
        pw_str("<span class=\"ititle\">");
        pw_str(tr(L, "IN_FILE"));
        pw_str(" ");
        send_esc(req, title);
        pw_str("</span>");
    }

    /* type badge: the extension */
    char ext[8] = "";
    if (dot) {
        size_t i = 0;
        for (const char *p = dot + 1; *p && i < sizeof(ext) - 1; p++) ext[i++] = tolower((unsigned char)*p);
        ext[i] = 0;
    }
    pw_str("</td><td><span class=\"badge ");
    pw_str(tape ? "b-" : "b-other");
    if (tape) pw_str(ext);
    pw_str("\">");
    for (char *p = ext; *p; p++) *p = toupper((unsigned char)*p);
    send_esc(req, ext[0] ? ext : "?");
    pw_str("</span></td><td class=\"num hide-s\">");
    send_size(req, size);
    pw_str("</td><td class=\"acts\">");

    bool locked = is_protected(name);
    if (tape) {
        send_form_open(req, "play", name);
        pw_str("><button class=\"btn go\" type=\"submit\">&#9654; Play</button>"
                                      "<button class=\"btn\" type=\"submit\" name=\"loop\" value=\"1\">Loop</button></form>");

    }
    if (locked) {
        pw_str("</td></tr>\n");
        return;
    }
    if (tape) {
        file_safe(sugg, sizeof(sugg), has_title ? title : base);
        if (!sugg[0]) file_safe(sugg, sizeof(sugg), base);
        pw_str("<details class=\"ren\"><summary class=\"btn\">");
        pw_str(tr(L, "RENAME"));
        pw_str("</summary><div class=\"pop\">");
        send_form_open(req, "rename", name);
        pw_str("><input type=\"text\" name=\"to\" maxlength=\"24\" size=\"18\" value=\"");
        send_esc(req, sugg);
        pw_str("\"> <button class=\"btn go\" type=\"submit\">OK</button></form></div></details>");
    }
    send_form_open(req, "delete", name);
    pw_str(" onsubmit=\"return confirm(this.dataset.q)\" data-q=\"");
    pw_str(tr(L, "DELETE_Q"));
    pw_str(" ");
    send_esc(req, name);
    pw_str("?\"><button class=\"btn warn\" type=\"submit\">");
    pw_str(tr(L, "DELETE"));
    pw_str("</button></form></td></tr>\n");
}

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

/* Main page: header with the tape status, upload, file list, footer.
 * SPIFFS has no directories, so only "/" is listed. */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
    extern const unsigned char web_head_start[] asm("_binary_web_head_html_start");
    extern const unsigned char web_head_end[]   asm("_binary_web_head_html_end");
    extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
    extern const unsigned char upload_script_end[]   asm("_binary_upload_script_html_end");

    DIR *dir = opendir(dirpath);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir : %s", dirpath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
        return ESP_FAIL;
    }

    /* file names, sorted */
    enum { MAX_LIST = 200 };
    char **names = calloc(MAX_LIST, sizeof(char *));
    int n = 0;
    struct dirent *entry;
    while (names && n < MAX_LIST && (entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) continue;
        names[n] = strdup(entry->d_name);
        if (names[n]) n++;
    }
    closedir(dir);
    if (n) qsort(names, n, sizeof(char *), cmp_names);

    lang_t L = i18n_lang(req);
    pw_begin(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Language", i18n_tag(L));
    i18n_send_tpl(pw_write, (const char *)web_head_start, web_head_end - web_head_start, L);

    /* header: logo, name, tape status */
    zx_turbo_pos_t pos;
    char s[160];
    pw_str(tap_player_is_busy() ? "<header class=\"spin\">" : "<header>");
    pw_str("<div class=\"stripes\"></div><div class=\"brandbox\"><div class=\"logo\">");
    send_logo(req);
    pw_str("</div></div>");

    /* the tape deck: always the same rows (state, block, bar, details,
     * buttons), so it keeps its size; the page script fills it from /status */
    bool next = zx_turbo_next(&pos);
    bool busy = tap_player_is_busy(), looping = busy && tap_player_is_looping();
    pw_str("<div class=\"deck\" id=\"st\"><div class=\"l1\"><span class=\"dot");
    pw_str(looping || (!busy && (next || tap_player_has_resume())) ? " loop" : busy ? " play" : "");
    pw_str("\" id=\"std\"></span><span class=\"t\" id=\"st1\">");
    if (busy) {
        pw_str(tr(L, looping ? "LOOP" : "PLAYING"));
        pw_str(": <b>");
        send_esc(req, tap_player_current());
        pw_str("</b>");
    } else if (next || tap_player_has_resume()) {
        pw_str(tr(L, "STOPPED_AT"));
        pw_str(": <b>");
        if (next) {
            snprintf(s, sizeof(s), "%s, %s %d %s %d: %s \"%s\"", pos.file, tr(L, "BLOCK"), pos.block,
                     tr(L, "OF"), pos.nblocks, tr(L, pos.type == 0 ? "TYPE0" : pos.type == 3 ? "TYPE3"
                                                    : pos.type == 1 ? "TYPE1" : "TYPE2"), pos.name);
            send_esc(req, s);
        } else {
            send_esc(req, tr(L, "STOP_MARK"));
        }
        pw_str("</b>");
    } else {
        pw_str(tr(L, "IDLE"));
    }
    pw_str("</span></div><div class=\"l2\" id=\"st2\">&mdash;</div>"
                                  "<div class=\"bar\"><div id=\"stb\" style=\"width:0\"></div></div>"
                                  "<div class=\"l3\" id=\"st3\">&mdash;</div><div class=\"acts\" id=\"sta\">");
    if (busy) {
        pw_str("<form method=\"post\" action=\"/stop\">"
                                      "<button class=\"btn warn\" type=\"submit\">&#9632; ");
        pw_str(tr(L, "STOP"));
        pw_str("</button></form>");
    } else {
        if (next || tap_player_has_resume()) {
            pw_str("<form method=\"post\" action=\"/resume\">"
                                          "<button class=\"btn go\" type=\"submit\">&#9654; ");
            pw_str(tr(L, "CONTINUE"));
            pw_str("</button></form>");
        }
        pw_str("<form method=\"post\" action=\"/play/" ZX_MENU_FILE "\">"
                                      "<button class=\"btn\" type=\"submit\">&#9654; ");
        pw_str(tr(L, "PLAY_MENU"));
        pw_str("</button></form>");
    }
    pw_str("</div></div></header>");

    i18n_send_tpl(pw_write, (const char *)upload_script_start, upload_script_end - upload_script_start, L);

    /* tapes and other files; '_' files (menu, logo) apart */
    snprintf(s, sizeof(s), "<section class=\"card\"><h2>%s</h2><table><thead><tr><th>%s</th><th>%s</th>",
             tr(L, "TAPES"), tr(L, "COL_FILE"), tr(L, "COL_TYPE"));
    pw_str(s);
    snprintf(s, sizeof(s), "<th class=\"num hide-s\">%s</th><th></th></tr></thead><tbody>", tr(L, "COL_SIZE"));
    pw_str(s);
    char path[FILE_PATH_MAX];
    struct stat st;
    int shown = 0, hidden = 0;
    for (int i = 0; i < n; i++) {
        if (pw.dead) break;
        if (names[i][0] == '_') {
            hidden++;
            continue;
        }
        snprintf(path, sizeof(path), "%s%s", dirpath, names[i]);
        if (stat(path, &st) != 0) continue;
        send_file_row(req, names[i], st.st_size, L);
        shown++;
        vTaskDelay(1);
    }
    if (!shown) {
        pw_str("<tr><td colspan=\"4\" class=\"empty\">");
        pw_str(tr(L, "EMPTY"));
        pw_str("</td></tr>");
    }
    pw_str("</tbody></table>");
    if (hidden) {
        pw_str("<details class=\"sys\"><summary>");
        pw_str(tr(L, "SYSFILES"));
        pw_str("</summary><table><tbody>");
        for (int i = 0; i < n; i++) {
            if (names[i][0] != '_') continue;
            snprintf(path, sizeof(path), "%s%s", dirpath, names[i]);
            if (stat(path, &st) != 0) continue;
            send_file_row(req, names[i], st.st_size, L);
        }
        pw_str("</tbody></table></details>");
    }
    pw_str("</section>");
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);

    /* footer: storage, version, language, diagnostics */
    size_t total = 0, used = 0;
    pw_str("<footer><div style=\"flex:1 1 260px\">");
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK && total > 0) {
        snprintf(s, sizeof(s), tr(L, "STORAGE"), (unsigned)((total - used) / 1024), (unsigned)(total / 1024));
        pw_str(s);
        snprintf(s, sizeof(s), "<div class=\"bar\"><div style=\"width:%u%%\"></div></div>",
                 (unsigned)(used * 100 / total));
        pw_str(s);
    }
    const esp_app_desc_t *app = esp_ota_get_app_description();
    pw_str("ZX-Wespi 2020 by members of ZX-TEAM &middot; ");
    pw_str(tr(L, "VERSION"));
    pw_str(" ");
    send_esc(req, app->version);
    pw_str(" (");
    send_esc(req, app->date);
    pw_str(", ESP-IDF ");
    send_esc(req, app->idf_ver);
    pw_str(")</div>");
    send_flags(L);
    snprintf(s, sizeof(s), "<details class=\"diag\"><summary>%s</summary><p>"
             "<a class=\"btn\" href=\"/log\">Log</a> ", tr(L, "DIAG"));
    pw_str(s);
    snprintf(s, sizeof(s), "<form method=\"post\" action=\"/mark\"><button class=\"btn\" type=\"submit\">%s</button></form> ",
             tr(L, "MARK_LOG"));
    pw_str(s);
    pw_str("</p></details></footer>");

    pw_str("</div></body></html>");
    pw_end();
    return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    } else if (IS_FILE_EXT(filename, ".png")) {
        return httpd_resp_set_type(req, "image/png");
    } else if (IS_FILE_EXT(filename, ".jpg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".svg")) {
        return httpd_resp_set_type(req, "image/svg+xml");
    } else if (tape_file_supported(filename)) {
        return httpd_resp_set_type(req, "application/octet-stream");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri, sizeof(filepath));
    if (!filename) {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        send_err_l(req, "500 Internal Server Error", "E_NAMELEN");
        return ESP_FAIL;
    }

    /* If name has trailing '/', respond with directory contents */
    if (filename[strlen(filename) - 1] == '/') {
        return http_resp_dir_html(req, filepath);
    }

    if (stat(filepath, &file_stat) == -1) {
        /* If file not present on SPIFFS check if URI
         * corresponds to one of the hardcoded paths */
        if (strcmp(filename, "/index.html") == 0) {
            return index_html_get_handler(req);
        } else if (strcmp(filename, "/favicon.ico") == 0) {
            return favicon_get_handler(req);
        }
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
    set_content_type_from_file(req, filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

        /* Send the buffer contents as HTTP response chunk */
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            ESP_LOGE(TAG, "File sending failed!");
            /* Abort sending file */
            httpd_resp_sendstr_chunk(req, NULL);
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            return ESP_FAIL;
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);

    /* Close file after sending complete */
    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* A name the page, the URLs and the menu handle: letters, digits, '.',
 * '-', '_' (the page makes names like this before sending) */
static bool name_ok(const char *name)
{
    if (name[0] == '/') name++;
    if (!name[0]) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

#define UPLOAD_WAITS 3              /* receive timeouts (5 s each) before giving up */

/* Handler to upload a file onto the server. A name ending in zx_iot.bin is a
 * firmware update: written to the other OTA partition, run after a reset. */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;

    /* Skip leading "/upload" from URI to get filename */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename) {
        send_err_l(req, "500 Internal Server Error", "E_NAMELEN");
        return ESP_FAIL;
    }
    if (!name_ok(filename)) {
        ESP_LOGE(TAG, "invalid file name: %s", filename);
        send_err_l(req, "400 Bad Request", "E_BADNAME");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == 0) {
        ESP_LOGW(TAG, "%s already exists: refused", filename);
        /* take in the body first: closing the connection while the browser
         * still sends makes it think the network failed, and it sends again */
        for (int left = req->content_len, r, waits = 0; left > 0; left -= r) {
            r = httpd_req_recv(req, buf, MIN(left, SCRATCH_BUFSIZE));
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++waits < UPLOAD_WAITS) { r = 0; continue; }
            if (r <= 0) return ESP_FAIL;
        }
        send_err_l(req, "400 Bad Request", "E_EXISTS");
        return ESP_OK;
    }

    static const char ota_name[] = "zx_iot.bin";
    size_t fl = strlen(filename), ol = sizeof(ota_name) - 1;
    bool is_ota = fl >= ol && strcmp(filename + fl - ol, ota_name) == 0;
    FILE *fd = NULL;
    const esp_partition_t *update_partition = NULL;
    esp_ota_handle_t update_handle = 0;
    if (is_ota) {
        update_partition = esp_ota_get_next_update_partition(NULL);
        esp_err_t err = update_partition ? esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle)
                                         : ESP_ERR_NOT_FOUND;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "firmware update: cannot start (%s)", esp_err_to_name(err));
            send_err_l(req, "500 Internal Server Error", "E_UPLOAD");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "firmware update: writing to the partition at 0x%x", update_partition->address);
    } else {
        if (req->content_len > MAX_FILE_SIZE) {
            ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "File size must be less than " MAX_FILE_SIZE_STR "!");
            /* closes the connection: the content would keep the socket busy */
            return ESP_FAIL;
        }
        fd = fopen(filepath, "w");
        if (!fd) {
            ESP_LOGE(TAG, "Failed to create file : %s", filepath);
            send_err_l(req, "500 Internal Server Error", "E_UPLOAD");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Receiving file : %s...", filename);

    /* a file that does not arrive whole is not kept: a cut tape fails to
     * load much later, far from the cause */
    int remaining = req->content_len, waits = 0;
    bool ok = true;
    while (ok && remaining > 0) {
        int received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE));
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++waits < UPLOAD_WAITS) continue;
        if (received <= 0) {
            ESP_LOGE(TAG, "File reception failed: %d bytes missing", remaining);
            ok = false;
            break;
        }
        waits = 0;
        if (is_ota) {
            esp_err_t err = esp_ota_write(update_handle, buf, received);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "firmware update: write failed (%s)", esp_err_to_name(err));
                ok = false;
            }
            vTaskDelay(1);
        } else if (fwrite(buf, 1, received, fd) != (size_t)received) {
            ESP_LOGE(TAG, "File write failed (storage full?)");
            ok = false;
        }
        remaining -= received;
    }

    if (is_ota) {
        esp_err_t err = ESP_FAIL;
        if (ok) {
            err = esp_ota_end(update_handle);               /* also checks the image */
            if (err == ESP_OK) err = esp_ota_set_boot_partition(update_partition);
            if (err != ESP_OK) ESP_LOGE(TAG, "firmware update: not accepted (%s)", esp_err_to_name(err));
        } else {
            esp_ota_abort(update_handle);
        }
        ok = err == ESP_OK;
        /* no automatic restart: the new firmware runs after the next reset */
        if (ok) ESP_LOGI(TAG, "firmware update written: reset the ESP32 to run it");
    } else {
        if (fclose(fd) != 0) ok = false;
        if (!ok) unlink(filepath);
    }
    if (!ok) {
        send_err_l(req, "500 Internal Server Error", "E_UPLOAD");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "File reception complete");
    /* several files come in a row: the Spectrum menu is rebuilt once, a
     * moment after the last */
    if (!is_ota) zx_menu_rebuild_later();
    /* the page script uploads and then reloads the list itself */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* Handler to delete a file from the server */
static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;

    /* Skip leading "/delete" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri  + sizeof("/delete") - 1, sizeof(filepath));
    if (!filename) {
        /* Respond with 500 Internal Server Error */
        send_err_l(req, "500 Internal Server Error", "E_NAMELEN");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "File does not exist : %s", filename);
        /* Respond with 400 Bad Request */
        send_err_l(req, "400 Bad Request", "E_NOFILE");
        return ESP_FAIL;
    }
    if (is_protected(filename)) {
        send_err_l(req, "403 Forbidden", "E_PROTECTED");
        return ESP_OK;
    }
    if (tap_player_is_busy() && strcmp(tap_player_current(), filename + 1) == 0) {
        send_err_l(req, "400 Bad Request", "E_PLAYING");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleting file : %s", filename);
    /* Delete file */
    unlink(filepath);
    zx_menu_rebuild();

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

/* Handler to play a tape file to the Spectrum EAR input */
static esp_err_t play_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;

    /* Skip leading "/play" from URI to get filename */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/play") - 1, sizeof(filepath));
    if (!filename || !tape_file_supported(filename)) {
        send_err_l(req, "400 Bad Request", "E_NOTAPE");
        return ESP_FAIL;
    }
    if (stat(filepath, &file_stat) == -1) {
        send_err_l(req, "400 Bad Request", "E_NOFILE");
        return ESP_FAIL;
    }

    /* The Loop button posts "loop=1" in the form body */
    char body[16] = {0};
    bool loop = false;
    if (req->content_len > 0 && req->content_len < sizeof(body)) {
        if (httpd_req_recv(req, body, req->content_len) > 0) loop = strstr(body, "loop=1") != NULL;
    }

    esp_err_t err = loop ? tap_player_loop(filepath) : tap_player_play(filepath);
    if (err == ESP_ERR_INVALID_STATE) {
        send_err_l(req, "500 Internal Server Error", "E_BUSY");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        send_err_l(req, "500 Internal Server Error", "E_PLAY");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s requested: %s", loop ? "Loop" : "Play", filename);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Playing");
    return ESP_OK;
}

/* Decodes an application/x-www-form-urlencoded value in place */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* Handler to rename a tape file: form field "to" = new base name; the
 * extension is kept. The Spectrum menu is rebuilt. */
static esp_err_t rename_post_handler(httpd_req_t *req)
{
    const char *base_path = ((struct file_server_data *)req->user_ctx)->base_path;
    char from[FILE_PATH_MAX], to[FILE_PATH_MAX];
    struct stat st;
    const char *fname = get_path_from_uri(from, base_path, req->uri + sizeof("/rename") - 1, sizeof(from));
    if (fname && is_protected(fname)) {
        send_err_l(req, "403 Forbidden", "E_PROTECTED");
        return ESP_OK;
    }
    if (!fname || !tape_file_supported(fname) || stat(from, &st) != 0) {
        send_err_l(req, "400 Bad Request", "E_NOFILE");
        return ESP_FAIL;
    }

    char body[128] = "", val[96] = "", base[RENAME_BASE_MAX + 1];
    int len = req->content_len < (int)sizeof(body) - 1 ? (int)req->content_len : (int)sizeof(body) - 1;
    if (len > 0 && httpd_req_recv(req, body, len) > 0) body[len] = 0;
    if (httpd_query_key_value(body, "to", val, sizeof(val)) == ESP_OK) url_decode(val);
    file_safe(base, sizeof(base), val);
    if (!base[0]) {
        send_err_l(req, "400 Bad Request", "E_BADNAME");
        return ESP_FAIL;
    }

    snprintf(to, sizeof(to), "%s/%s%s", base_path, base, strrchr(fname, '.'));
    if (strcmp(to, from) != 0) {
        if (strcasecmp(to, from) != 0 && stat(to, &st) == 0) {
            send_err_l(req, "400 Bad Request", "E_EXISTS");
            return ESP_FAIL;
        }
        if (tap_player_is_busy() && strcmp(tap_player_current(), fname + 1) == 0) {
            send_err_l(req, "400 Bad Request", "E_PLAYING");
            return ESP_FAIL;
        }
        if (rename(from, to) != 0) {
            ESP_LOGE(TAG, "rename %s -> %s failed", from, to);
            send_err_l(req, "500 Internal Server Error", "E_RENAME");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "renamed %s -> %s", from, to);
        zx_menu_rebuild();
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Renamed");
    return ESP_OK;
}

/* Continues the tape from where it stopped: the next pair of a file served
 * on request, or a TZX/PZX after a "stop the tape" mark */
static esp_err_t resume_post_handler(httpd_req_t *req)
{
    bool ok = !tap_player_is_busy()
              && (zx_turbo_continue() || (tap_player_has_resume() && tap_player_resume() == ESP_OK));
    if (!ok) ESP_LOGW(TAG, "continue: nothing to continue (or the tape is playing)");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, ok ? "Continuing" : "Nothing to continue");
    return ESP_OK;
}

/* Tape status as JSON, polled by the page (upload_script.html) */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char out[900];
    zx_turbo_pos_t pos;
    bool next = zx_turbo_next(&pos);
    const char *state;
    if (ts_rec_active()) state = "rec";
    else if (tap_player_is_busy()) state = tap_player_is_looping() ? "loop" : "play";
    else if (next) state = "next";
    else if (tap_player_has_resume()) state = "paused";
    else state = "idle";
    size_t n = ts_json(out, sizeof(out), state, tap_player_is_busy() ? tap_player_current() : "",
                       next ? pos.file : "", next ? pos.block : 0, next ? pos.nblocks : 0,
                       next ? pos.type : 0, next ? pos.name : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

/* GET /lang/pt|es|en: the language of the page and of the Spectrum menu
 * (rebuilt in it), back to the page */
static esp_err_t lang_get_handler(httpd_req_t *req)
{
    lang_t l, before;
    httpd_resp_set_status(req, "303 See Other");
    if (i18n_parse(req->uri + sizeof("/lang/") - 1, &l)) {
        bool same = i18n_chosen(&before) && before == l;
        i18n_choose(l);
        if (!same) zx_menu_rebuild_later();
    }
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "");
}

/* Handler to stop the tape player */
static esp_err_t stop_post_handler(httpd_req_t *req)
{
    tap_player_stop();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Stopped");
    return ESP_OK;
}

/* Handler to show the buffered device log */
static esp_err_t log_get_handler(httpd_req_t *req)
{
    return weblog_send(req);
}

/* Handler to write a numbered marker into the log */
static esp_err_t mark_post_handler(httpd_req_t *req)
{
    weblog_mark();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Marked");
    return ESP_OK;
}

/* Function to start the file server */
esp_err_t start_file_server(const char *base_path)
{
    static struct file_server_data *server_data = NULL;

    /* Validate file storage base path */
    if (!base_path || strcmp(base_path, "/spiffs") != 0) {
        ESP_LOGE(TAG, "File server presently supports only '/spiffs' as base path");
        return ESP_ERR_INVALID_ARG;
    }

    if (server_data) {
        ESP_LOGE(TAG, "File server already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate memory for server data */
    server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(server_data->base_path, base_path,
            sizeof(server_data->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;   /* default 8 is already used up */
    config.lru_purge_enable = true; /* pages poll /status: drop the oldest idle connection */

    ESP_LOGI(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }

    /* Must be registered before the catch-all GET wildcard below, which
     * would otherwise take GET /log as a file download */
    httpd_uri_t log_view = {
        .uri       = "/log",
        .method    = HTTP_GET,
        .handler   = log_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &log_view);

    httpd_uri_t log_mark = {
        .uri       = "/mark",
        .method    = HTTP_POST,
        .handler   = mark_post_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &log_mark);

    httpd_uri_t tape_status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &tape_status);

    httpd_uri_t lang_set = {
        .uri       = "/lang/*",
        .method    = HTTP_GET,
        .handler   = lang_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &lang_set);

    /* URI handler for getting uploaded files */
    httpd_uri_t file_download = {
        .uri       = "/*",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = download_get_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_download);

    /* URI handler for uploading files to server */
    httpd_uri_t file_upload = {
        .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
        .method    = HTTP_POST,
        .handler   = upload_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_upload);

    /* URI handler for deleting files from server */
    httpd_uri_t file_delete = {
        .uri       = "/delete/*",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = delete_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_delete);

    /* URI handlers for the Spectrum tape player */
    httpd_uri_t file_play = {
        .uri       = "/play/*",
        .method    = HTTP_POST,
        .handler   = play_post_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &file_play);

    httpd_uri_t file_rename = {
        .uri       = "/rename/*",
        .method    = HTTP_POST,
        .handler   = rename_post_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &file_rename);

    httpd_uri_t tape_resume = {
        .uri       = "/resume",
        .method    = HTTP_POST,
        .handler   = resume_post_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &tape_resume);

    httpd_uri_t tape_stop = {
        .uri       = "/stop",
        .method    = HTTP_POST,
        .handler   = stop_post_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &tape_stop);

    return ESP_OK;
}
