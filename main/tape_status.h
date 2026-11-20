// What the tape is playing right now, for the web page (/status): file,
// block, bytes sent, timing. Written by the players (tape_file, zx_turbo),
// read by the web server; cheap enough to call per byte.

#ifndef _TAPE_STATUS_H_
#define _TAPE_STATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A play of `file` starts; fsize = its size in bytes (0: unknown) */
void ts_begin(const char *file, uint32_t fsize);

/* Block `index` of `nblocks` (0: unknown) starts at file offset `fpos`;
 * `desc` = a code for what it is (see tape_file.c mark()), `len` = its
 * data bytes */
void ts_block(int index, int nblocks, uint32_t fpos, const char *desc, uint32_t len);

/* `done` data bytes of the current block have been queued */
void ts_bytes(uint32_t done);

/* The other direction: a SAVE coming in through MIC (mic_in.c, zx_save.c) */
void ts_rec_pilot(void);                    /* a pilot is being heard */
void ts_rec_block(uint8_t flag);            /* a block started: its flag byte */
void ts_rec_bytes(uint32_t done);           /* bytes of it received so far */
void ts_rec_cancel(void);                   /* not a SAVE (a menu reply, a request) */
void ts_rec_header(const char *name, int type, uint32_t len);   /* header received */
void ts_rec_end(bool ok);                   /* data block over: checksum OK or not */
void ts_rec_saved(const char *path);        /* stored on the server */
void ts_rec_error(const char *code);        /* not stored: "E_NOHDR", "E_UPLOAD"... */

/* True while a SAVE is shown (in progress, or just finished) */
bool ts_rec_active(void);

/* JSON for /status: the play state comes from tap_player, the rest from
 * the calls above; where a stopped session stands: next_file ("" if none),
 * next_block of next_nblocks, header type and name */
size_t ts_json(char *out, size_t size, const char *state, const char *current,
               const char *next_file, int next_block, int next_nblocks, int next_type,
               const char *next_name);

#ifdef __cplusplus
}
#endif

#endif /* _TAPE_STATUS_H_ */
