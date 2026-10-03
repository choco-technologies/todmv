#ifndef LIBTODMV_FILES_H
#define LIBTODMV_FILES_H

#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Adapters from the libtodmv streams to the dmod VFS.
 *
 * static inline on purpose: their addresses are handed out as stream
 * callbacks, and the address of a global function is taken through the GOT,
 * which the dmod loader does not relocate - a static function's address is
 * PC-relative and works.
 */


static inline char *file_read_line(void *file, char *buffer, int size)
{
    return Dmod_FileReadLine(buffer, size, file);
}

static inline int file_read(void *file, uint32_t offset, void *buffer, size_t size)
{
    if (Dmod_FileSeek(file, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) != 0)
        return -EIO;
    return (Dmod_FileRead(buffer, 1, size, file) == size) ? 0 : -EIO;
}

static inline int file_write(void *file, const void *data, size_t size)
{
    return (Dmod_FileWrite(data, 1, size, file) == size) ? 0 : -EIO;
}

static inline int file_seek(void *file, uint32_t offset)
{
    return (Dmod_FileSeek(file, (Dmod_FileOffset_t)offset, DMOD_SEEK_SET) == 0) ? 0 : -EIO;
}

static inline int console_write(void *ctx, const void *data, size_t size)
{
    char piece[WRITER_BUFFER + 1];
    (void)ctx;

    /* Dmod_Printf needs a zero-terminated string */
    while (size > 0)
    {
        size_t n = (size < WRITER_BUFFER) ? size : WRITER_BUFFER;
        memcpy(piece, data, n);
        piece[n] = '\0';
        Dmod_Printf("%s", piece);
        data = (const uint8_t *)data + n;
        size -= n;
    }
    return 0;
}

static inline int input_read(void *ctx, uint32_t offset, void *buffer, size_t size)
{
    return file_read(ctx, offset, buffer, size);
}

static inline void *file_open_input(const char *path, libtodmv_input_t *input)
{
    void *file = Dmod_FileOpen(path, "rb");
    if (file == NULL)
        return NULL;

    size_t size = 0;
    if (!Dmod_FileSizeToSizeT(Dmod_FileSize(file), &size) || size > 0xFFFFFFFFu)
    {
        Dmod_FileClose(file);
        return NULL;
    }
    input->read = input_read;
    input->ctx = file;
    input->size = (uint32_t)size;
    return file;
}

#endif /* LIBTODMV_FILES_H */
