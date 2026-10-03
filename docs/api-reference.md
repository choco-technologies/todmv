# libtodmv API Reference

`#include "libtodmv.h"` - types in `libtodmv_types.h`. Every function
returns 0 on success or a negative errno value.

## Assembling

| Function | Description |
|----------|-------------|
| `int libtodmv_assemble(const libtodmv_source_t* source, const libtodmv_sink_t* sink, const libtodmv_options_t* options, libtodmv_result_t* result)` | Assemble lines from `source` into a binary view written to `sink` |
| `int libtodmv_assemble_file(const char* input, const char* output, const libtodmv_options_t* options, libtodmv_result_t* result)` | The same, file to file through the dmod VFS; `output` is removed on failure |

Results: `-EBADMSG` the source has errors (reported through
`options->on_error`), `-EIO` the sink failed, `-ENOENT` the input file
cannot be opened, `-ENOMEM`, `-EINVAL`. When there are errors, what was
written to the sink is incomplete and must be discarded.

The source is read line by line into one buffer of `LIBTODMV_LINE_MAX` (256)
bytes; each instruction is written as soon as its line is read. Forward
label references are patched at the end with `sink->seek`, then the tables
and the header are written - so the sink needs both `write` and `seek`.

```c
typedef struct {
    const char *include_dir;    /* .include paths are relative to it (NULL: as written;
                                   _file: the directory of the input) */
    libtodmv_error_fn on_error; /* every error, as soon as it is found (NULL: only counted) */
    void *user;                 /* passed to on_error */
    uint32_t max_errors;        /* stop after this many, 0 = 16 */
} libtodmv_options_t;

typedef struct {
    uint32_t error_count;       /* errors found */
    bool     truncated;         /* stopped at max_errors */
    uint32_t size;              /* size of the written view, 0 on errors */
} libtodmv_result_t;

typedef struct {
    const char *file;           /* source name, valid during the callback only */
    uint32_t line, column;      /* 1-based; 0 = not tied to a line/column */
    char message[96];
} libtodmv_error_t;
```

## Disassembling and validating

| Function | Description |
|----------|-------------|
| `int libtodmv_disassemble(const libtodmv_input_t* input, const libtodmv_sink_t* sink)` | Write equivalent assembly to `sink` (only `write` is used) |
| `int libtodmv_disassemble_file(const char* input, const char* output)` | The same from a file, to a file or to the console (`output` NULL) |
| `int libtodmv_validate(const libtodmv_input_t* input, uint32_t* error_offset, const char** reason)` | Check a binary view |
| `int libtodmv_validate_file(const char* path, uint32_t* error_offset, const char** reason)` | The same for a file |

The view is read in small pieces through `input->read` (an instruction, a
string in 32-byte chunks). Disassembly validates the view first and returns
`-EBADMSG` for an invalid one. The disassembled text is canonical:
assembling it always gives the same bytes; assembling the original source
gives an equivalent view whose strings may be stored in another order.

## Streams

```c
/* Source: the Dmod_FileReadLine contract - at most size - 1 bytes, the line
 * end included if it fits, zero-terminated; NULL at the end. */
typedef char *(*libtodmv_read_line_fn)(void *ctx, char *buffer, int size);
typedef struct { libtodmv_read_line_fn read_line; void *ctx; const char *name; } libtodmv_source_t;

/* Binary input: read `size` bytes at `offset`. */
typedef int (*libtodmv_read_fn)(void *ctx, uint32_t offset, void *buffer, size_t size);
typedef struct { libtodmv_read_fn read; void *ctx; uint32_t size; } libtodmv_input_t;

/* Output: write at the current position / move to an absolute offset. */
typedef int (*libtodmv_write_fn)(void *ctx, const void *data, size_t size);
typedef int (*libtodmv_seek_fn)(void *ctx, uint32_t offset);
typedef struct { libtodmv_write_fn write; libtodmv_seek_fn seek; void *ctx; } libtodmv_sink_t;
```

Two things to keep in mind when filling these structures in a dmod module:

- assign the fields one by one rather than with an initializer of function
  pointers - the compiler may keep a constant initializer as data, and the
  dmod loader does not relocate pointers stored in initialized data;
- pass `static` functions as callbacks - the address of a global function
  is taken through the GOT, which the loader does not relocate either.
