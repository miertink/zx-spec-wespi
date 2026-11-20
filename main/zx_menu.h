// File menu for the Spectrum: builds the menu .tap that loops on EAR, and
// handles the selection the menu sends back through MIC.

#ifndef _ZX_MENU_H_
#define _ZX_MENU_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZX_MENU_FILE        "_menu.tap"           /* in /spiffs; '_' files are not listed */
#define ZX_MENU_PATH        "/spiffs/" ZX_MENU_FILE
#define ZX_MENU_REPLY_FLAG  0xA5                  /* must match spmenu.asm */

/* Call once, after SPIFFS is mounted: builds the menu and starts its loop. */
void zx_menu_init(void);

/* Rebuilds the menu from the files in /spiffs (call after upload/delete).
 * A running menu loop is restarted with the new list. */
void zx_menu_rebuild(void);

/* Same, a little later (MENU_LATER_MS after the last call): for uploads,
 * which come several in a row. */
void zx_menu_rebuild_later(void);

/* A valid block decoded from MIC (flag first, checksum last), registered
 * with mic_in: menu replies are handled here, anything else is a SAVE for
 * the server (zx_save). Called from the MIC task: only queues the work. */
void zx_menu_on_block(const uint8_t *block, size_t len, int64_t start_us);

#ifdef __cplusplus
}
#endif

#endif /* _ZX_MENU_H_ */
