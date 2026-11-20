// Tape image formats played to the Spectrum EAR: .tap, .tzx, .pzx, .csw

#ifndef _TAPE_FILE_H_
#define _TAPE_FILE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TF_DONE,        /* played to the end */
    TF_STOPPED,     /* stopped by the user (tape_out_abort / *stop) */
    TF_ERROR,       /* cannot open, unknown or corrupt format */
    TF_TAPE_STOP,   /* the file says "stop the tape" (multi-load games):
                       resume_offset tells where to continue */
} tf_result_t;

typedef struct {
    uint32_t pilot_pulses;      /* .tap only: pilot length of every block, 0 = ROM standard */
    bool fast_start;            /* .tap only: the ROM is already waiting (LOAD "" from the
                                   menu): short pilot for the first header and for every
                                   data block that follows its header, short pause between */
    uint32_t start_offset;      /* continue from here (a resume_offset), 0 = from the start */
    bool verbose;               /* log every block */
    bool ignore_stops;          /* "stop the tape" marks end the play as TF_DONE */
    volatile bool *stop;        /* set by another task to stop */
    uint32_t resume_offset;     /* out, with TF_TAPE_STOP */
} tf_opts_t;

/* True if the name has an extension this module plays. */
bool tape_file_supported(const char *name);

/* Title stored inside the file, cleaned up for display (Spectrum colour and
 * position codes removed, printable ASCII only): the TZX archive title or the
 * name in the first header (.tap/.tzx), the PZX title. False if none. */
bool tape_file_title(const char *path, char *out, size_t size);

/* Plays one ROM-timed block from memory: `b` = flag, data, checksum (`len`
 * bytes), with `pilot` pulses. `label` names it on the web page. Blocks
 * until done; call from the player task (tap_player_run). */
bool tape_file_play_mem(const char *label, const uint8_t *b, size_t len, uint32_t pilot);

/* Plays the file (full VFS path). Blocks until done; call from the player task. */
tf_result_t tape_file_play(const char *path, tf_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* _TAPE_FILE_H_ */
