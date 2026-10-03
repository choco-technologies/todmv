#ifndef LIBTODMV_TYPES_H
#define LIBTODMV_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Longest source line, including the line end and the terminator. */
#define LIBTODMV_LINE_MAX       256u

/** Longest error message, including the terminator. */
#define LIBTODMV_MESSAGE_MAX    96u

/** Errors after which assembling stops when libtodmv_options_t::max_errors is 0. */
#define LIBTODMV_DEFAULT_MAX_ERRORS 16u

/** Deepest .include nesting. */
#define LIBTODMV_MAX_INCLUDE_DEPTH  8u

/** One problem found in the source - handed to libtodmv_options_t::on_error. */
typedef struct
{
    const char *file;                       /**< Source name (NULL if none), valid during the call only */
    uint32_t    line;                       /**< 1-based, 0 = not tied to a line */
    uint32_t    column;                     /**< 1-based, 0 = not tied to a column */
    char        message[LIBTODMV_MESSAGE_MAX];
} libtodmv_error_t;

/** Called for every error, as soon as it is found. */
typedef void (*libtodmv_error_fn)(void *user, const libtodmv_error_t *error);

/**
 * Read the next line of a source into @p buffer - the contract of
 * Dmod_FileReadLine(): at most size - 1 bytes, the line end included if it
 * fits, zero-terminated; NULL at the end of the source.
 */
typedef char *(*libtodmv_read_line_fn)(void *ctx, char *buffer, int size);

/** Assembly source, read line by line. */
typedef struct
{
    libtodmv_read_line_fn   read_line;
    void                   *ctx;
    const char             *name;           /**< Used in errors (may be NULL) */
} libtodmv_source_t;

/** Read @p size bytes at @p offset; 0 on success. */
typedef int (*libtodmv_read_fn)(void *ctx, uint32_t offset, void *buffer, size_t size);

/** Binary view, read in pieces at random offsets. */
typedef struct
{
    libtodmv_read_fn        read;
    void                   *ctx;
    uint32_t                size;           /**< Size of the whole view */
} libtodmv_input_t;

/** Write at the current position; 0 on success. */
typedef int (*libtodmv_write_fn)(void *ctx, const void *data, size_t size);

/** Move the position to an absolute offset; 0 on success. */
typedef int (*libtodmv_seek_fn)(void *ctx, uint32_t offset);

/** Output. The assembler needs seek (it patches forward references and
 *  writes the header last); the disassembler only writes. */
typedef struct
{
    libtodmv_write_fn       write;
    libtodmv_seek_fn        seek;
    void                   *ctx;
} libtodmv_sink_t;

/** Options of the assembler - all fields may be zero. */
typedef struct
{
    const char             *include_dir;    /**< .include paths are relative to it (NULL: as written) */
    libtodmv_error_fn       on_error;       /**< Receives every error (NULL: errors are only counted) */
    void                   *user;           /**< Passed to on_error */
    uint32_t                max_errors;     /**< Stop after this many, 0 = LIBTODMV_DEFAULT_MAX_ERRORS */
} libtodmv_options_t;

/** Outcome of assembling. */
typedef struct
{
    uint32_t                error_count;    /**< Errors found */
    bool                    truncated;      /**< Stopped at max_errors - there may be more */
    uint32_t                size;           /**< Size of the written view (0 on errors) */
} libtodmv_result_t;

#endif /* LIBTODMV_TYPES_H */
