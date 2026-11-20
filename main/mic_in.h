// Spectrum MIC input (Spectrum -> ESP32) on GPIO36 / ADC1 channel 0, AC
// coupled and biased (see mic_in.c): SAVE decoder and reset detector.

#ifndef _MIC_IN_H_
#define _MIC_IN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup: starts sampling the input and decoding it. */
void mic_in_init(void);

/* Called from the MIC task for every block decoded with a good checksum
 * (block = flag, data..., checksum; start_us = esp_timer time of the sync,
 * where the data starts). Keep it short: copy and queue the work. */
typedef void (*mic_in_block_cb_t)(const uint8_t *block, size_t len, int64_t start_us);
void mic_in_set_block_cb(mic_in_block_cb_t cb);

/* Called from the MIC task when the Spectrum was reset or powered up: the
 * MIC line, normally never quiet while the machine is on, went silent (RAM
 * test / power off) and came back. Only seen while our tape output is idle.
 * Keep it short: queue the work. */
typedef void (*mic_in_reset_cb_t)(void);
void mic_in_set_reset_cb(mic_in_reset_cb_t cb);

/* True while the Spectrum is saving (pilot under way, block in progress, or
 * a SAVE block received less than 2.5 s ago: its data block may follow):
 * the tape player waits, so EAR does not talk over MIC. */
bool mic_in_is_receiving(void);

#ifdef __cplusplus
}
#endif

#endif /* _MIC_IN_H_ */
