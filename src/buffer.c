#include "private.h"
#include <errno.h>
#include <string.h>

/* ---- Growable memory ---- */

bool buffer_reserve(buffer_t *b, size_t extra)
{
    if (b->failed)
        return false;
    if (b->size + extra <= b->capacity)
        return true;

    size_t capacity = (b->capacity != 0) ? b->capacity : 64;
    while (capacity < b->size + extra)
        capacity *= 2;
    uint8_t *data = Dmod_Realloc(b->data, capacity);
    if (data == NULL)
    {
        b->failed = true;
        return false;
    }
    b->data = data;
    b->capacity = capacity;
    return true;
}

void buffer_put(buffer_t *b, const void *data, size_t size)
{
    if (size == 0 || !buffer_reserve(b, size))
        return;
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

void buffer_free(buffer_t *b)
{
    if (b->data != NULL)
        Dmod_Free(b->data);
    memset(b, 0, sizeof(*b));
}

/* ---- Buffered output ---- */

void writer_init(writer_t *w, const libtodmv_sink_t *sink)
{
    memset(w, 0, sizeof(*w));
    w->sink = sink;
}

void writer_flush(writer_t *w)
{
    if (w->used != 0 && w->status == 0 && w->sink->write(w->sink->ctx, w->buf, w->used) != 0)
        w->status = -EIO;
    w->position += w->used;
    w->used = 0;
}

void writer_put(writer_t *w, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    while (size > 0)
    {
        if (w->used == WRITER_BUFFER)
            writer_flush(w);
        size_t n = WRITER_BUFFER - w->used;
        if (n > size)
            n = size;
        memcpy(w->buf + w->used, p, n);
        w->used += (uint32_t)n;
        p += n;
        size -= n;
    }
}

void writer_put8(writer_t *w, uint8_t value)
{
    writer_put(w, &value, 1);
}

void writer_put16(writer_t *w, uint16_t value)
{
    uint8_t bytes[2];
    put16(bytes, value);
    writer_put(w, bytes, sizeof(bytes));
}

void writer_put32(writer_t *w, uint32_t value)
{
    uint8_t bytes[4];
    put32(bytes, value);
    writer_put(w, bytes, sizeof(bytes));
}

void writer_printf(writer_t *w, const char *format, ...)
{
    char text[64];
    va_list args;

    va_start(args, format);
    int length = Dmod_VSnPrintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0)
        return;
    if ((size_t)length >= sizeof(text))
        length = (int)sizeof(text) - 1;
    writer_put(w, text, (size_t)length);
}

void writer_put_escaped(writer_t *w, const char *text, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)text[i];
        char escaped[4] = { '\\', 0, 0, 0 };
        size_t n = 2;

        if (c == '"' || c == '\\')
            escaped[1] = (char)c;
        else if (c == '\n')
            escaped[1] = 'n';
        else if (c == '\t')
            escaped[1] = 't';
        else if (c < 0x20 || c == 0x7F)
        {
            escaped[1] = 'x';
            escaped[2] = hex[c >> 4];
            escaped[3] = hex[c & 0x0F];
            n = 4;
        }
        else
        {
            writer_put8(w, c);
            continue;
        }
        writer_put(w, escaped, n);
    }
}

void writer_seek(writer_t *w, uint32_t offset)
{
    writer_flush(w);
    if (w->status == 0 && (w->sink->seek == NULL || w->sink->seek(w->sink->ctx, offset) != 0))
        w->status = -EIO;
    w->position = offset;
}

uint32_t writer_tell(const writer_t *w)
{
    return w->position + w->used;
}

/* ---- Little-endian helpers ---- */

void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

void put32(uint8_t *p, uint32_t value)
{
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}
