// Texts of the Spectrum menu and its modules in the languages of the web
// page, and the flags that show and pick them (zx_text.c). The menu and the
// modules get the texts of one language from the ESP32 with their data, so
// the Z80 code holds none.

#ifndef _ZX_TEXT_H_
#define _ZX_TEXT_H_

#include <stdint.h>
#include "web_i18n.h"

/* Menu texts, in the order of the M_ constants in asm/spmenu.asm.
 * Widths: KEYS1 26 (the page number follows), WAIT 11 (the file name
 * follows), the others 31. */
enum { ZT_HELLO, ZT_EMPTY, ZT_KEYS1, ZT_KEYS2, ZT_WAIT, ZT_MLOAD, ZT_MERR, ZT_MOLD, ZT_MENU_N };

/* WiFi module, in the order of the W_ constants in asm/spwifi.asm (32) */
enum { ZT_W_NONE, ZT_W_KEYS1, ZT_W_KEYS2, ZT_W_PASS, ZT_W_HINT1, ZT_W_HINT2, ZT_W_SENT1,
       ZT_W_SENT2, ZT_W_ANYKEY, ZT_WIFI_N };

/* Info module, in the order of the T_ constants in asm/spinfo.asm (32) */
#define ZT_HELP_N 6
enum { ZT_I_LABEL, ZT_I_KEYS, ZT_I_HELP, ZT_INFO_N = ZT_I_HELP + ZT_HELP_N };

typedef struct {
    const char *menu[ZT_MENU_N];
    const char *wifi[ZT_WIFI_N];
    const char *info[ZT_INFO_N];
    const char *no_wifi;            /* menu row 1 without an address */
    const char *tape, *tapes;       /* menu row 1: "1 tape", "12 tapes" */
    const char *pw_prompt;          /* BASIC: INPUT "<this>"; LINE p$ */
    const char *net_none;           /* WiFi module: no network stored */
    const char *offline;            /* WiFi module: after the network in use */
    /* info module lines, printf formats (30 characters printed) */
    const char *f_web, *f_wifi, *f_wifi_off, *f_mac, *f_disk, *f_tapes, *f_ver;
} zx_text_t;

extern const zx_text_t zx_text[LANG_COUNT];

#define ZX_FLAG_LEN 36              /* 4 characters: attribute, 8 pixel rows (FLAG_LEN) */
extern const uint8_t zx_flag[LANG_COUNT][ZX_FLAG_LEN];

#endif /* _ZX_TEXT_H_ */
