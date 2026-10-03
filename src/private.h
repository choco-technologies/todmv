#ifndef LIBTODMV_PRIVATE_H
#define LIBTODMV_PRIVATE_H

#include "dmod.h"
#include "format.h"
#include "libtodmv.h"
#include <stdarg.h>

/* ---- Growable memory for the assembler's tables (buffer.c) ---- */

typedef struct
{
    uint8_t *data;
    size_t   size;
    size_t   capacity;
    bool     failed;        /* An allocation failed - the content is incomplete */
} buffer_t;

bool buffer_reserve(buffer_t *b, size_t extra);
void buffer_put(buffer_t *b, const void *data, size_t size);
void buffer_free(buffer_t *b);

/* ---- Buffered output to a sink (buffer.c) ---- */

#define WRITER_BUFFER   128u

typedef struct
{
    const libtodmv_sink_t *sink;
    uint32_t  position;         /* Sink position of buf[0] */
    uint32_t  used;
    int       status;           /* 0, or -EIO once the sink failed */
    uint8_t   buf[WRITER_BUFFER];
} writer_t;

void writer_init(writer_t *w, const libtodmv_sink_t *sink);
void writer_put(writer_t *w, const void *data, size_t size);
void writer_put8(writer_t *w, uint8_t value);
void writer_put16(writer_t *w, uint16_t value);
void writer_put32(writer_t *w, uint32_t value);
void writer_printf(writer_t *w, const char *format, ...);
/* Characters of a string as they appear inside a "..." literal. */
void writer_put_escaped(writer_t *w, const char *text, size_t len);
void writer_flush(writer_t *w);
void writer_seek(writer_t *w, uint32_t offset);
uint32_t writer_tell(const writer_t *w);

void put16(uint8_t *p, uint16_t value);
void put32(uint8_t *p, uint32_t value);
uint16_t get16(const uint8_t *p);
uint32_t get32(const uint8_t *p);

/* ---- Names shared by the assembler and the disassembler (names.c) ---- */

#define NAME_MAX_LEN    16u

const char *event_name(uint32_t event);
int event_find(const char *name, size_t len);

/* Built-in variables: "box.w" -> DMV_VAR_BOX_W (name without '$'). */
const char *builtin_name(uint32_t index);
int builtin_find(const char *name, size_t len);

/* Flag names of a dmv_flags_kind_t; -1 if unknown. */
int flag_find(uint8_t flags_kind, const char *name, size_t len);
/* The flags as names joined with '|' (nothing for 0, except ALIGN). */
void flags_print(writer_t *w, uint8_t flags_kind, uint8_t flags);

const char *nav_role_name(uint32_t role);
int nav_role_find(const char *name, size_t len);

/* ---- Lexical helpers (parse.c) ---- */

bool is_space(char c);
bool is_digit(char c);
bool is_ident_start(char c);
bool is_ident_char(char c);

/* Case-insensitive comparison of a token with a zero-terminated word. */
bool token_ieq(const char *token, size_t len, const char *word);
/* Case-sensitive comparison of a token with a zero-terminated string. */
bool token_eq(const char *token, size_t len, const char *str);
/* The token is an identifier ([A-Za-z_][A-Za-z0-9_]*). */
bool token_is_ident(const char *token, size_t len);

/* Decimal, 0x hexadecimal or 0b binary integer with an optional '-'. */
bool parse_integer(const char *token, size_t len, int64_t *value);
/* #RRGGBB (opaque) or #AARRGGBB. */
bool parse_color(const char *token, size_t len, uint32_t *value);
/* "..." with escapes, unescaped into out (zero-terminated, *out_len without
 * the terminator). On error returns false with *error_at at the offending
 * position in the token. */
bool parse_string(const char *token, size_t len, char *out, size_t out_size, size_t *out_len, size_t *error_at);

#endif /* LIBTODMV_PRIVATE_H */
