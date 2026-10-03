#include "private.h"
#include <string.h>

/* dmod modules have no libc ctype or strtol - these stand in for them. */

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_ident_char(char c)
{
    return is_ident_start(c) || is_digit(c);
}

static char to_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool token_ieq(const char *token, size_t len, const char *word)
{
    size_t i = 0;
    for (; i < len; i++)
    {
        if (word[i] == '\0' || to_upper(token[i]) != to_upper(word[i]))
            return false;
    }
    return word[i] == '\0';
}

bool token_eq(const char *token, size_t len, const char *str)
{
    return strlen(str) == len && strncmp(token, str, len) == 0;
}

bool token_is_ident(const char *token, size_t len)
{
    if (len == 0 || !is_ident_start(token[0]))
        return false;
    for (size_t i = 1; i < len; i++)
    {
        if (!is_ident_char(token[i]))
            return false;
    }
    return true;
}

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

/* Unsigned number in `base`, at most 2^32 * 2 so overflow is detectable. */
static bool parse_digits(const char *s, size_t len, int base, uint64_t *value)
{
    uint64_t v = 0;
    if (len == 0)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        int d = digit_value(s[i]);
        if (d >= base)
            return false;
        v = v * (uint64_t)base + (uint64_t)d;
        if (v > 0x1FFFFFFFFull)
            return false;
    }
    *value = v;
    return true;
}

bool parse_integer(const char *token, size_t len, int64_t *value)
{
    bool negative = false;
    uint64_t v;

    if (len > 0 && token[0] == '-')
    {
        negative = true;
        token++;
        len--;
    }
    bool ok;
    if (len > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        ok = parse_digits(token + 2, len - 2, 16, &v);
    else if (len > 2 && token[0] == '0' && (token[1] == 'b' || token[1] == 'B'))
        ok = parse_digits(token + 2, len - 2, 2, &v);
    else
        ok = parse_digits(token, len, 10, &v);
    if (!ok)
        return false;
    *value = negative ? -(int64_t)v : (int64_t)v;
    return true;
}

bool parse_color(const char *token, size_t len, uint32_t *value)
{
    uint64_t v;
    if (len < 1 || token[0] != '#' || (len != 7 && len != 9) || !parse_digits(token + 1, len - 1, 16, &v))
        return false;
    *value = (len == 7) ? (uint32_t)(0xFF000000u | (uint32_t)v) : (uint32_t)v;
    return true;
}

bool parse_string(const char *token, size_t len, char *out, size_t out_size, size_t *out_len, size_t *error_at)
{
    size_t n = 0;

    *error_at = 0;
    if (len < 2 || token[0] != '"' || token[len - 1] != '"')
        return false;

    for (size_t i = 1; i < len - 1; i++)
    {
        char c = token[i];
        size_t at = i;

        if (c == '"')
        {
            *error_at = i;
            return false;
        }
        if (c == '\\')
        {
            if (++i >= len - 1)
            {
                *error_at = at;
                return false;
            }
            switch (token[i])
            {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case 'x':
                {
                    int hi = (i + 2 < len) ? digit_value(token[i + 1]) : 99;
                    int lo = (i + 2 < len) ? digit_value(token[i + 2]) : 99;
                    if (hi > 15 || lo > 15 || (hi | lo) == 0)
                    {
                        *error_at = at;
                        return false;
                    }
                    c = (char)((hi << 4) | lo);
                    i += 2;
                    break;
                }
                default:
                    *error_at = at;
                    return false;
            }
        }
        if (n + 1 >= out_size)
        {
            *error_at = at;
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    *out_len = n;
    return true;
}
