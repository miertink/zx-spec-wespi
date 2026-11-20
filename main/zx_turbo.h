// Load on request with a turbo loader, for .tap files: the BASIC programs
// are rewritten so that every LOAD asks the ESP32 for its block (zx_turbo.c).

#ifndef _ZX_TURBO_H_
#define _ZX_TURBO_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True if the .tap can be served on request: a BASIC program first, only
 * header + data pairs, and at least one LOAD the rewrite can handle. */
bool zx_turbo_capable(const char *path);

/* Starts a session with the file chosen in the menu: plays its (rewritten)
 * first program; later blocks are played when the program asks for them.
 * False if the file cannot be served this way (play it as usual). */
bool zx_turbo_start(const char *path);

/* A request from the Spectrum (MIC): program id, statement. Plays the block
 * that LOAD statement needs. Call from a task, not from the MIC callback. */
void zx_turbo_request(uint8_t prog_id, uint8_t stmt);

/* An error request from the turbo routine: it measured a pilot count of bp
 * and had `left` bytes to go (0: checksum, 0xFFFF: header). Plays the last
 * turbo block again; the routine is waiting for it. */
void zx_turbo_error(uint8_t bp, uint16_t left);

/* Where the tape of the session stands (for the web page) */
typedef struct {
    char file[48];          /* file name, without the directory */
    int block, nblocks;     /* next block (a header), blocks in the file */
    int type;               /* its header type: 0 program, 1, 2 arrays, 3 bytes */
    char name[11];          /* its name, trailing spaces removed */
} zx_turbo_pos_t;

/* True if a header + data pair is next; `pos` (optional) says which */
bool zx_turbo_next(zx_turbo_pos_t *pos);

/* Plays that pair (a program goes rewritten, its LOADs keep asking), as a
 * tape would if the recorder were started again. */
bool zx_turbo_continue(void);

/* Ends the session (Spectrum reset, another file chosen). */
void zx_turbo_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* _ZX_TURBO_H_ */
