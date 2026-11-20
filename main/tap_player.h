// Plays tape images (.tap, .tzx, .pzx, .csw) to the Spectrum EAR input

#ifndef _TAP_PLAYER_H_
#define _TAP_PLAYER_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup; also initialises the tape encoder and the button. */
void tap_player_init(void);

/* Starts playing a tape image once (full VFS path, e.g. "/spiffs/hello.tap").
 * If the last play of the same file ended at a "stop the tape" mark, it
 * continues from there. Anything already playing is stopped first. */
esp_err_t tap_player_play(const char *path);

/* Same, for a file chosen in the menu: the ROM is already waiting, so .tap
 * files get short pilots where that is safe (tf_opts_t.fast_start). */
esp_err_t tap_player_play_fast(const char *path);

/* Runs fn(arg) in the player task, as a play of its own (busy while it
 * runs, stopped by tap_player_stop through tape_out_abort). `label` is shown
 * as the file being played. Anything already playing is stopped first. */
esp_err_t tap_player_run(void (*fn)(void *arg), void *arg, const char *label);

/* Continues the file that stopped at a "stop the tape" mark, if any. */
esp_err_t tap_player_resume(void);

/* Plays a tape image over and over, with the pilot of every .tap block
 * shortened to CONFIG_ZXS_LOOP_PILOT_PULSES, until stopped. The Spectrum
 * needs no trigger: whenever LOAD "" is typed, the ROM picks up the next
 * header. Anything already playing is stopped first. */
esp_err_t tap_player_loop(const char *path);

/* Stops the file that is playing, if any. */
void tap_player_stop(void);

bool tap_player_is_busy(void);

/* True while a loop (tap_player_loop) is running. */
bool tap_player_is_looping(void);

/* Name of the file being played, or "" when idle. */
const char *tap_player_current(void);

/* True if the last file played stopped at a "stop the tape" mark (multi-load
 * game): playing it again continues from there. */
bool tap_player_has_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* _TAP_PLAYER_H_ */
