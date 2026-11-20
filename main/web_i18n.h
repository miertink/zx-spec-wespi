// Languages: Portuguese (Brazil), Spanish (Spain), English (US), for the web
// page and the Spectrum menu. One choice for both, stored in NVS: made with
// the flags in the footer of the page or keys 1-3 of the menu's info
// screen. Until a choice is made, the page follows the browser's
// Accept-Language and the menu is in English.

#ifndef _WEB_I18N_H_
#define _WEB_I18N_H_

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LANG_PT, LANG_ES, LANG_EN, LANG_COUNT } lang_t;

/* Language of this request: the chosen one, else the browser's */
lang_t i18n_lang(httpd_req_t *req);

/* The chosen language (NVS); false if none was chosen yet */
bool i18n_chosen(lang_t *out);

/* Language of the Spectrum menu: the chosen one, else English */
lang_t i18n_menu_lang(void);

/* Stores the choice (page and menu) */
void i18n_choose(lang_t l);

/* "pt-BR", "es-ES", "en-US"; "pt", "es", "en" */
const char *i18n_tag(lang_t l);
const char *i18n_code(lang_t l);

/* "pt" / "es" / "en" -> language; false if unknown */
bool i18n_parse(const char *code, lang_t *out);

/* Text for `key` (see web_i18n.c); the key itself if unknown */
const char *tr(lang_t l, const char *key);

/* Writes a page part through `out`, replacing every {{KEY}} with tr(l, KEY) */
typedef void (*i18n_out_t)(const char *s, size_t n);
void i18n_send_tpl(i18n_out_t out, const char *s, size_t n, lang_t l);

#ifdef __cplusplus
}
#endif

#endif /* _WEB_I18N_H_ */
