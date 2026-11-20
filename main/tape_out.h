// ZX Spectrum tape signal encoder (ESP32 -> Spectrum EAR): a stream of pulses

#ifndef _TAPE_OUT_H_
#define _TAPE_OUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Durations are in "tape T-states": Z80 T-states at CONFIG_ZXS_CPU_CLOCK_HZ
 * (3.5 MHz by default), the unit used by the TZX and PZX formats. */

/* Call once at startup. Sets up the RMT channel on CONFIG_ZXS_TAPE_OUT_GPIO. */
void tape_out_init(void);

/*
 * Pulse stream. A single task (the player) queues pulses; they are played
 * back to back, with no gap, until tape_out_flush(). The first pulses are
 * buffered, playback starts when the buffer is full or at the flush, and
 * the queueing calls block while the buffer is full.
 *
 * Every call returns false once tape_out_abort() was called: the caller
 * should stop generating and call tape_out_flush().
 */

/* An edge, then the new level held for `t` T-states. */
bool tape_out_pulse(uint32_t t);

/* The line at `level` (0/1) for `t` T-states: an edge only if the line was
 * at the other level. t = 0 just sets the level for the next call. */
bool tape_out_level(int level, uint32_t t);

/* Current line level, as of the last queued pulse. */
int tape_out_get_level(void);

/* Ends the stream and waits until everything queued has been played.
 * final_edge: one more edge after the last pulse, then hold (the ROM loader
 * needs it to see the end of the last bit). Returns false if aborted. */
bool tape_out_flush(bool final_edge);

/* Sets the line level while the output is idle (between flushes). */
void tape_out_set_idle_level(int level);

/* Requests the stream to end early; the queueing calls then return false.
 * Safe to call from any task. Cleared by tape_out_reset_abort(). */
void tape_out_abort(void);
void tape_out_reset_abort(void);
bool tape_out_aborted(void);

/* Time (esp_timer, us) the output was last active: now while a stream is
 * playing, else the end of the last one (-1 = never). EAR leaks into MIC,
 * so a block decoded from MIC while this was active is our own echo. */
int64_t tape_out_last_activity_us(void);

#ifdef __cplusplus
}
#endif

#endif /* _TAPE_OUT_H_ */
