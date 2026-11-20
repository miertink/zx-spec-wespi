// SAVE to the server: blocks saved on the Spectrum become .tap files.

#ifndef _ZX_SAVE_H_
#define _ZX_SAVE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, after SPIFFS is mounted. */
void zx_save_init(void);

/* A block decoded from MIC with a good checksum (flag first), not an echo
 * and not a menu reply. Copies it and queues it; returns at once. */
void zx_save_on_block(const uint8_t *block, size_t len, int64_t start_us);

#ifdef __cplusplus
}
#endif

#endif /* _ZX_SAVE_H_ */
