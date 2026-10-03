#define DMOD_ENABLE_REGISTRATION ON
#include "private.h"
#include "files.h"
#include <errno.h>
#include <string.h>

/*
 * .dmvs -> .dmv in one pass, without holding the source or the code:
 *
 * - the source is read line by line into a buffer of LIBTODMV_LINE_MAX;
 * - every instruction has a fixed size, so it is written to the sink as soon
 *   as its line is read (behind a placeholder for the header);
 * - a reference to a label defined further down becomes a fixup, patched by
 *   seeking back at the end; the tables and the header follow.
 *
 * Kept in memory are only the view's tables: all strings in one block,
 * variables, fonts, boxes, labels, fixups of forward references and items.
 */

#define MAX_OPERAND_TOKENS  24u     /* .gradient: name, kind, 4 parameters, 16 stops */
#define MAX_BOX_DEPTH       32u
#define HEADER_SIZE         DMV_HEADER_SIZE
#define NO_INDEX            (-1)

typedef struct
{
    const char *text;
    size_t      len;
    uint32_t    column;     /* 1-based */
} token_t;

typedef struct
{
    const char *file;
    uint32_t    line;
    uint32_t    column;
} location_t;

#define LABEL_DEFINED       0x01u
#define LABEL_USED          0x02u

typedef struct
{
    uint16_t    name;       /* String index, as written (".local" for local labels) */
    int16_t     scope;      /* NO_INDEX for a global label, else its global label */
    uint16_t    word;       /* Code word offset once defined */
    uint8_t     flags;
    location_t  first_use;
} label_t;

typedef struct
{
    uint8_t     type;
    uint8_t     flags;
    uint16_t    capacity;
    uint16_t    name;
    uint16_t    env;
    int32_t     init;
} var_t;

typedef struct
{
    uint16_t    name;
    uint16_t    spec;
} font_t;

typedef struct
{
    uint16_t    name;
    uint8_t     kind;
    uint8_t     count;
    uint16_t    first;      /* First stop */
    int16_t     param[4];
} gradient_t;

typedef struct
{
    uint32_t    color;
    uint16_t    position;   /* 1/1000 */
} stop_t;

typedef struct
{
    uint16_t    name;
    uint16_t    parent;
    uint16_t    begin;
    uint16_t    end;
    bool        defined;
    location_t  where;      /* Definition, or first reference */
} box_t;

typedef struct
{
    uint32_t    name;       /* Offset in the constants' name block */
    int64_t     value;
} define_t;

typedef struct
{
    uint16_t    word;       /* Instruction */
    uint16_t    label;
    uint8_t     offset;     /* Operand offset in the instruction */
} fixup_t;

typedef struct
{
    uint8_t     kind;
    uint8_t     arg;
    int32_t     label;      /* Label index, or NO_INDEX */
    uint32_t    value;
} item_t;

#define ARRAY(type, name)   type *name; uint32_t name##_count; uint32_t name##_capacity

typedef struct
{
    const libtodmv_options_t *options;
    libtodmv_result_t        *result;
    uint32_t                  max_errors;
    bool                      stop;             /* max_errors reached or out of memory */
    bool                      out_of_memory;

    location_t                at;               /* Line being assembled */
    uint32_t                  include_depth;
    ARRAY(char *, files);                       /* Paths of included files */

    writer_t                  out;
    uint32_t                  code_size;        /* Bytes of code so far */

    buffer_t                  text;             /* All strings, zero-terminated, back to back */
    ARRAY(uint32_t, string_at);                 /* Offset of each string in text */
    ARRAY(uint8_t,  string_hash);
    buffer_t                  define_names;     /* Names of .define constants (not in the output) */

    ARRAY(label_t,  labels);
    ARRAY(uint16_t, definition_order);          /* Labels in the order they were defined */
    ARRAY(var_t,    vars);
    ARRAY(font_t,   fonts);
    ARRAY(gradient_t, gradients);
    ARRAY(stop_t,   stops);
    ARRAY(box_t,    boxes);
    ARRAY(define_t, defines);
    ARRAY(fixup_t,  fixups);
    ARRAY(item_t,   items);

    int32_t                   scope;            /* Last global label, or NO_INDEX */
    uint16_t                  stack[MAX_BOX_DEPTH];
    uint32_t                  depth;
    int                       last_opcode;      /* -1 at the start */

    bool                      have_view, have_size, have_longpress, have_scrollslop;
    bool                      uses_opacity;     /* OPACITY: a version 0.3 view */
    uint16_t                  view_name, width, height, longpress, scrollslop;
    int32_t                   entry;            /* Label index, or NO_INDEX */
} assembler_t;

/* ---- Memory ---- */

static bool grow(assembler_t *a, void **items, uint32_t *capacity, uint32_t count, size_t size)
{
    if (count < *capacity)
        return true;
    uint32_t new_capacity = (*capacity != 0) ? *capacity * 2U : 8U;
    void *p = Dmod_Realloc(*items, new_capacity * size);
    if (p == NULL)
    {
        a->out_of_memory = true;
        a->stop = true;
        return false;
    }
    *items = p;
    *capacity = new_capacity;
    return true;
}

#define PUSH(a, array)  grow((a), (void **)&(a)->array, &(a)->array##_capacity, (a)->array##_count, sizeof(*(a)->array))

/* ---- Errors ---- */

static void error_at(assembler_t *a, const location_t *where, const char *format, ...)
{
    if (a->stop)
        return;

    libtodmv_error_t e;
    e.file = where->file;
    e.line = where->line;
    e.column = where->column;
    va_list args;
    va_start(args, format);
    Dmod_VSnPrintf(e.message, sizeof(e.message), format, args);
    va_end(args);

    if (a->options->on_error != NULL)
        a->options->on_error(a->options->user, &e);
    if (++a->result->error_count >= a->max_errors)
    {
        a->result->truncated = true;
        a->stop = true;
    }
}

/* Error at a column of the current line. */
static void error(assembler_t *a, uint32_t column, const char *format, ...)
{
    char message[LIBTODMV_MESSAGE_MAX];
    location_t where = a->at;
    where.column = column;

    va_list args;
    va_start(args, format);
    Dmod_VSnPrintf(message, sizeof(message), format, args);
    va_end(args);
    error_at(a, &where, "%s", message);
}

static bool has_errors(const assembler_t *a)
{
    return a->result->error_count != 0 || a->out_of_memory;
}

/* A token as a zero-terminated string for messages (shortened if long). */
static const char *show(const token_t *t, char *buffer, size_t size)
{
    size_t len = (t->len < size) ? t->len : size - 1U;
    memcpy(buffer, t->text, len);
    buffer[len] = '\0';
    return buffer;
}

#define SHOW(t)     show((t), (char[32]){0}, 32)

/* ---- Strings ---- */

static uint8_t hash(const char *text, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)text[i]) * 16777619u;
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

static const char *string_text(const assembler_t *a, uint32_t index)
{
    return (const char *)a->text.data + a->string_at[index];
}

static size_t string_len(const assembler_t *a, uint32_t index)
{
    uint32_t end = (index + 1U < a->string_at_count) ? a->string_at[index + 1U] : (uint32_t)a->text.size;
    return end - a->string_at[index] - 1U;
}

static bool string_is(const assembler_t *a, uint32_t index, uint8_t h, const char *text, size_t len)
{
    return a->string_hash[index] == h && string_len(a, index) == len &&
           strncmp(string_text(a, index), text, len) == 0;
}

/* Index of the string (added if new), NO_INDEX on error. */
static int intern(assembler_t *a, const char *text, size_t len)
{
    uint8_t h = hash(text, len);
    for (uint32_t i = 0; i < a->string_at_count; i++)
    {
        if (string_is(a, i, h, text, len))
            return (int)i;
    }
    if (a->string_at_count >= 0xFFFFu)
    {
        error(a, 0, "too many strings");
        return NO_INDEX;
    }
    if (!PUSH(a, string_at) || !PUSH(a, string_hash) || !buffer_reserve(&a->text, len + 1U))
    {
        a->out_of_memory = a->stop = true;
        return NO_INDEX;
    }
    a->string_at[a->string_at_count++] = (uint32_t)a->text.size;
    a->string_hash[a->string_hash_count++] = h;
    buffer_put(&a->text, text, len);
    buffer_put(&a->text, "", 1);
    return (int)(a->string_at_count - 1U);
}

/* ---- Tables ---- */

static int find_var(const assembler_t *a, const char *name, size_t len)
{
    uint8_t h = hash(name, len);
    for (uint32_t i = 0; i < a->vars_count; i++)
    {
        if (string_is(a, a->vars[i].name, h, name, len))
            return (int)i;
    }
    return NO_INDEX;
}

static int find_font(const assembler_t *a, const char *name, size_t len)
{
    uint8_t h = hash(name, len);
    for (uint32_t i = 0; i < a->fonts_count; i++)
    {
        if (string_is(a, a->fonts[i].name, h, name, len))
            return (int)i;
    }
    return NO_INDEX;
}

static int find_gradient(const assembler_t *a, const char *name, size_t len)
{
    uint8_t h = hash(name, len);
    for (uint32_t i = 0; i < a->gradients_count; i++)
    {
        if (string_is(a, a->gradients[i].name, h, name, len))
            return (int)i;
    }
    return NO_INDEX;
}

static const define_t *find_define(const assembler_t *a, const char *name, size_t len)
{
    for (uint32_t i = 0; i < a->defines_count; i++)
    {
        if (token_eq(name, len, (const char *)a->define_names.data + a->defines[i].name))
            return &a->defines[i];
    }
    return NULL;
}

static uint16_t code_words(const assembler_t *a)
{
    return (uint16_t)(a->code_size / DMV_CODE_WORD);
}

/* Index of label `t` (created if new), NO_INDEX on error. */
static int32_t label_index(assembler_t *a, const token_t *t)
{
    bool local = t->len > 1 && t->text[0] == '.';
    if (!token_is_ident(t->text + (local ? 1 : 0), t->len - (local ? 1U : 0U)))
    {
        error(a, t->column, "invalid label '%s'", SHOW(t));
        return NO_INDEX;
    }
    if (local && a->scope == NO_INDEX)
    {
        error(a, t->column, "local label '%s' before any global label", SHOW(t));
        return NO_INDEX;
    }

    int16_t scope = local ? (int16_t)a->scope : (int16_t)NO_INDEX;
    uint8_t h = hash(t->text, t->len);
    for (uint32_t i = 0; i < a->labels_count; i++)
    {
        if (a->labels[i].scope == scope && string_is(a, a->labels[i].name, h, t->text, t->len))
            return (int32_t)i;
    }
    if (a->labels_count >= 0x7FFFu)
    {
        error(a, t->column, "too many labels");
        return NO_INDEX;
    }
    int name = intern(a, t->text, t->len);
    if (name == NO_INDEX || !PUSH(a, labels))
        return NO_INDEX;
    label_t *l = &a->labels[a->labels_count];
    memset(l, 0, sizeof(*l));
    l->name = (uint16_t)name;
    l->scope = scope;
    return (int32_t)a->labels_count++;
}

static int32_t label_use(assembler_t *a, const token_t *t)
{
    int32_t index = label_index(a, t);
    if (index != NO_INDEX && !(a->labels[index].flags & LABEL_USED))
    {
        a->labels[index].flags |= LABEL_USED;
        a->labels[index].first_use = a->at;
        a->labels[index].first_use.column = t->column;
    }
    return index;
}

static void define_label(assembler_t *a, const token_t *t)
{
    int32_t index = label_index(a, t);
    if (index == NO_INDEX)
        return;
    label_t *l = &a->labels[index];
    if (l->flags & LABEL_DEFINED)
    {
        error(a, t->column, "label '%s' defined twice", SHOW(t));
        return;
    }
    if (!PUSH(a, definition_order))
        return;
    a->definition_order[a->definition_order_count++] = (uint16_t)index;
    l->flags |= LABEL_DEFINED;
    l->word = code_words(a);
    if (t->text[0] != '.')
        a->scope = index;
}

/* Index of box `@name` (created if new), NO_INDEX on error. */
static int box_index(assembler_t *a, const token_t *t)
{
    if (t->len < 2 || t->text[0] != '@' || !token_is_ident(t->text + 1, t->len - 1))
    {
        error(a, t->column, "expected a box (@name), got '%s'", SHOW(t));
        return NO_INDEX;
    }
    uint8_t h = hash(t->text + 1, t->len - 1);
    for (uint32_t i = 0; i < a->boxes_count; i++)
    {
        if (string_is(a, a->boxes[i].name, h, t->text + 1, t->len - 1))
            return (int)i;
    }
    if (a->boxes_count >= DMV_NONE - 1U)
    {
        error(a, t->column, "too many boxes");
        return NO_INDEX;
    }
    int name = intern(a, t->text + 1, t->len - 1);
    if (name == NO_INDEX || !PUSH(a, boxes))
        return NO_INDEX;
    box_t *b = &a->boxes[a->boxes_count];
    memset(b, 0, sizeof(*b));
    b->name = (uint16_t)name;
    b->parent = DMV_NONE;
    b->where = a->at;
    b->where.column = t->column;
    return (int)a->boxes_count++;
}

/* ---- Operands ---- */

typedef struct
{
    int64_t value;
    bool    variable;       /* value is a variable index */
    int     type;           /* DMV_VAR_INT / _STR of the variable */
} value_t;

/* A variable reference "$name" (user or built-in). */
static bool parse_variable(assembler_t *a, const token_t *t, value_t *v, bool writable)
{
    int builtin = builtin_find(t->text + 1, t->len - 1);
    if (builtin >= 0)
    {
        if (writable)
        {
            error(a, t->column, "'%s' is read-only", SHOW(t));
            return false;
        }
        v->value = builtin;
        v->variable = true;
        v->type = DMV_VAR_INT;
        return true;
    }
    int index = find_var(a, t->text + 1, t->len - 1);
    if (index == NO_INDEX)
    {
        error(a, t->column, "undeclared variable '%s'", SHOW(t));
        return false;
    }
    v->value = index;
    v->variable = true;
    v->type = a->vars[index].type;
    return true;
}

/* An integer, a color or a constant; with allow_variable also a variable. */
static bool parse_number(assembler_t *a, const token_t *t, int64_t min, int64_t max, bool allow_variable, value_t *v)
{
    memset(v, 0, sizeof(*v));
    if (t->text[0] == '$')
    {
        if (!allow_variable)
        {
            error(a, t->column, "a variable is not allowed here");
            return false;
        }
        return parse_variable(a, t, v, false);
    }
    if (t->text[0] == '#')
    {
        uint32_t color;
        if (!parse_color(t->text, t->len, &color))
        {
            error(a, t->column, "invalid color '%s' (#RRGGBB or #AARRGGBB)", SHOW(t));
            return false;
        }
        v->value = color;
    }
    else if (is_digit(t->text[0]) || t->text[0] == '-')
    {
        if (!parse_integer(t->text, t->len, &v->value))
        {
            error(a, t->column, "invalid number '%s'", SHOW(t));
            return false;
        }
    }
    else if (token_eq(t->text, t->len, "POINTER_CONTACT"))
        v->value = DMV_POINTER_CONTACT;
    else if (token_eq(t->text, t->len, "FOCUS_CONTACT"))
        v->value = DMV_FOCUS_CONTACT;
    else
    {
        const define_t *d = token_is_ident(t->text, t->len) ? find_define(a, t->text, t->len) : NULL;
        if (d == NULL)
        {
            error(a, t->column, "expected a number, color or variable, got '%s'", SHOW(t));
            return false;
        }
        v->value = d->value;
    }

    if (v->value < min || v->value > max)
    {
        error(a, t->column, "value '%s' out of range", SHOW(t));
        return false;
    }
    return true;
}

static bool is_string_literal(const token_t *t)
{
    return t->len >= 2 && t->text[0] == '"';
}

/* A string literal, interned; NO_INDEX on error. */
static int parse_string_literal(assembler_t *a, const token_t *t, size_t *length)
{
    char text[LIBTODMV_LINE_MAX];
    size_t len = 0, error_offset = 0;

    if (!parse_string(t->text, t->len, text, sizeof(text), &len, &error_offset))
    {
        error(a, t->column + (uint32_t)error_offset, "invalid string");
        return NO_INDEX;
    }
    if (length != NULL)
        *length = len;
    return intern(a, text, len);
}

/* A string operand: literal (string index) or string variable. */
static bool parse_str_operand(assembler_t *a, const token_t *t, value_t *v)
{
    memset(v, 0, sizeof(*v));
    if (t->text[0] == '$')
    {
        if (!parse_variable(a, t, v, false))
            return false;
        if (v->type != DMV_VAR_STR)
        {
            error(a, t->column, "'%s' is not a string variable", SHOW(t));
            return false;
        }
        return true;
    }
    if (!is_string_literal(t))
    {
        error(a, t->column, "expected a string, got '%s'", SHOW(t));
        return false;
    }
    v->value = parse_string_literal(a, t, NULL);
    return v->value != NO_INDEX;
}

/* FORMAT strings take exactly one %d or %x; "%%" is a literal '%'. */
static bool format_valid(const assembler_t *a, uint32_t index)
{
    const char *s = string_text(a, index);
    int conversions = 0;
    for (; *s != '\0'; s++)
    {
        if (*s != '%')
            continue;
        s++;
        if (*s == 'd' || *s == 'x')
            conversions++;
        else if (*s != '%')
            return false;
    }
    return conversions == 1;
}

/* Flags operand: names of the instruction's flag set joined with '|'. */
static bool parse_flags(assembler_t *a, const token_t *t, uint8_t flags_kind, uint8_t *flags)
{
    uint32_t horizontal = 0, vertical = 0;
    size_t start = 0;

    *flags = 0;
    for (size_t i = 0; i <= t->len; i++)
    {
        if (i < t->len && t->text[i] != '|')
            continue;
        token_t part = { t->text + start, i - start, t->column + (uint32_t)start };
        while (part.len > 0 && is_space(part.text[0]))
        {
            part.text++;
            part.len--;
            part.column++;
        }
        while (part.len > 0 && is_space(part.text[part.len - 1]))
            part.len--;

        int value = flag_find(flags_kind, part.text, part.len);
        if (value < 0)
        {
            error(a, part.column, "unknown flag '%s'", SHOW(&part));
            return false;
        }
        if (flags_kind == DMV_FLAGS_ALIGN && value != DMV_ALIGN_WRAP)
        {
            bool is_vertical = token_ieq(part.text, part.len, "TOP") || (value & DMV_ALIGN_VMASK);
            if (++(*(is_vertical ? &vertical : &horizontal)) > 1)
            {
                error(a, part.column, "conflicting alignment '%s'", SHOW(&part));
                return false;
            }
        }
        *flags |= (uint8_t)value;
        start = i + 1;
    }
    return true;
}

/* ---- Instructions ---- */

static int find_opcode(const token_t *t)
{
    for (int op = 0; op <= DMV_OP_SETFOCUS; op++)
    {
        const dmv_opcode_info_t *info = dmv_get_opcode_info((uint8_t)op);
        if (info != NULL && token_ieq(t->text, t->len, info->mnemonic))
            return op;
    }
    return -1;
}

/* Destination type of a variable instruction (-1 = either). */
static int dest_type(uint8_t opcode)
{
    switch (opcode)
    {
        case DMV_OP_SET:    return -1;
        case DMV_OP_FORMAT: return DMV_VAR_STR;
        default:            return DMV_VAR_INT;
    }
}

/* Parse operand i into its slot. A label operand is returned in *label for
 * the caller to resolve. */
static bool parse_operand(assembler_t *a, uint8_t opcode, uint8_t i, uint8_t kind, const token_t *t,
                          int dest, uint8_t *slot, uint8_t *varmask, uint8_t *paint, int32_t *label)
{
    value_t v;
    bool ok = true;
    bool wide = dmv_operand_size(kind) == 4;

    memset(&v, 0, sizeof(v));
    switch (kind)
    {
        case DMV_OPERAND_V16:
        case DMV_OPERAND_V32:
        case DMV_OPERAND_COLOR:
            if (opcode == DMV_OP_SET && i == 1 && dest == DMV_VAR_STR)
            {
                /* SET into a string variable: a string or a string variable */
                ok = parse_str_operand(a, t, &v);
                break;
            }
            if (kind == DMV_OPERAND_COLOR && token_is_ident(t->text, t->len) &&
                (v.value = find_gradient(a, t->text, t->len)) != NO_INDEX)
            {
                *paint = DMV_PAINT_GRADIENT;     /* A gradient instead of the color */
                break;
            }
            ok = wide ? parse_number(a, t, INT32_MIN, UINT32_MAX, true, &v)
                      : parse_number(a, t, INT16_MIN, INT16_MAX, true, &v);
            if (ok && v.variable && v.type != DMV_VAR_INT)
            {
                error(a, t->column, "'%s' is a string variable", SHOW(t));
                ok = false;
            }
            break;

        case DMV_OPERAND_STR:
            ok = parse_str_operand(a, t, &v);
            if (ok && opcode == DMV_OP_FORMAT && !v.variable && !format_valid(a, (uint32_t)v.value))
            {
                error(a, t->column, "format needs exactly one %%d or %%x");
                ok = false;
            }
            break;

        case DMV_OPERAND_VAR:
        {
            if (t->len < 2 || t->text[0] != '$')
            {
                error(a, t->column, "expected a variable, got '%s'", SHOW(t));
                return false;
            }
            ok = parse_variable(a, t, &v, true);
            int expected = dest_type(opcode);
            if (ok && expected >= 0 && v.type != expected)
            {
                error(a, t->column, "'%s' must be %s variable", SHOW(t),
                      (expected == DMV_VAR_STR) ? "a string" : "an integer");
                ok = false;
            }
            v.variable = false;     /* Always a variable - no varmask bit */
            break;
        }

        case DMV_OPERAND_LABEL:
            *label = label_use(a, t);
            ok = *label != NO_INDEX;
            break;

        case DMV_OPERAND_BOX:
            v.value = box_index(a, t);
            ok = v.value != NO_INDEX;
            break;

        case DMV_OPERAND_FONT:
            v.value = find_font(a, t->text, t->len);
            if (v.value == NO_INDEX)
            {
                error(a, t->column, "unknown font '%s'", SHOW(t));
                ok = false;
            }
            break;

        case DMV_OPERAND_EVENT:
            v.value = event_find(t->text, t->len);
            if (v.value < 0)
            {
                error(a, t->column, "unknown event '%s'", SHOW(t));
                ok = false;
            }
            break;

        default:
            return false;
    }
    if (!ok)
        return false;

    if (v.variable)
        *varmask |= (uint8_t)(1U << i);
    if (wide)
        put32(slot, (uint32_t)v.value);
    else
        put16(slot, (uint16_t)v.value);
    return true;
}

static bool check_structure(assembler_t *a, uint8_t opcode, const token_t *word, const uint8_t *insn,
                            const dmv_layout_t *layout)
{
    switch (opcode)
    {
        case DMV_OP_BOX:
        {
            uint16_t index = get16(insn + layout->offsets[0]);
            box_t *b = &a->boxes[index];
            if (b->defined)
            {
                error(a, word->column, "box @%s defined twice", string_text(a, b->name));
                return false;
            }
            if (a->depth == MAX_BOX_DEPTH)
            {
                error(a, word->column, "boxes nested too deep");
                return false;
            }
            b->defined = true;
            b->begin = code_words(a);
            b->parent = a->depth ? a->stack[a->depth - 1] : (uint16_t)DMV_NONE;
            b->where = a->at;
            b->where.column = word->column;
            a->stack[a->depth++] = index;
            return true;
        }
        case DMV_OP_END:
            if (a->depth == 0)
            {
                error(a, word->column, "END without BOX");
                return false;
            }
            a->boxes[a->stack[--a->depth]].end = code_words(a);
            return true;
        case DMV_OP_ON:
            if (a->depth == 0)
            {
                error(a, word->column, "ON outside a box");
                return false;
            }
            return true;
        case DMV_OP_SCROLL:
            if (a->last_opcode != DMV_OP_BOX)
            {
                error(a, word->column, "SCROLL must directly follow BOX");
                return false;
            }
            return true;
        case DMV_OP_FOCUS:
            if (a->last_opcode != DMV_OP_BOX && a->last_opcode != DMV_OP_SCROLL)
            {
                error(a, word->column, "FOCUS must directly follow BOX or SCROLL");
                return false;
            }
            return true;
        case DMV_OP_OPACITY:
            if (a->last_opcode != DMV_OP_BOX && a->last_opcode != DMV_OP_SCROLL && a->last_opcode != DMV_OP_FOCUS)
            {
                error(a, word->column, "OPACITY must directly follow BOX, SCROLL or FOCUS");
                return false;
            }
            a->uses_opacity = true;
            return true;
        default:
            return true;
    }
}

static void assemble_instruction(assembler_t *a, const token_t *word, const token_t *ops, uint32_t n)
{
    int opcode = find_opcode(word);
    if (opcode < 0)
    {
        error(a, word->column, "unknown instruction '%s'", SHOW(word));
        return;
    }
    const dmv_opcode_info_t *info = dmv_get_opcode_info((uint8_t)opcode);
    dmv_layout_t layout;
    (void)dmv_get_layout((uint8_t)opcode, &layout);

    uint32_t most = info->operand_count + ((info->flags_kind != DMV_FLAGS_NONE) ? 1U : 0U);
    uint32_t least = info->operand_count - info->optional + (info->flags_required ? 1U : 0U);
    if (n < least || n > most)
    {
        if (least == most)
            error(a, word->column, "%s takes %u operand%s", info->mnemonic, (unsigned)least, (least == 1) ? "" : "s");
        else
            error(a, word->column, "%s takes %u to %u operands", info->mnemonic, (unsigned)least, (unsigned)most);
        return;
    }

    uint8_t insn[64];
    int32_t labels[DMV_MAX_OPERANDS];
    bool ok = true;
    int dest = -1;
    uint8_t paint = 0;

    memset(insn, 0, sizeof(insn));
    insn[0] = (uint8_t)opcode;
    insn[1] = layout.size;
    for (uint8_t i = 0; i < info->operand_count; i++)
    {
        uint8_t *slot = insn + layout.offsets[i];
        labels[i] = NO_INDEX;
        if (i >= n)
        {
            put16(slot, DMV_NONE);      /* Left-out optional operand (REDRAW's box) */
            continue;
        }
        if (!parse_operand(a, (uint8_t)opcode, i, info->operands[i], &ops[i], dest, slot, &insn[2], &paint,
                           &labels[i]))
        {
            ok = false;
            continue;
        }
        if (info->operands[i] == DMV_OPERAND_VAR)
            dest = a->vars[get16(slot)].type;
    }
    if (info->flags_kind != DMV_FLAGS_NONE && n > info->operand_count)
        ok = parse_flags(a, &ops[info->operand_count], info->flags_kind, &insn[3]) && ok;
    insn[3] |= paint;
    if (!ok || !check_structure(a, (uint8_t)opcode, word, insn, &layout))
        return;

    if ((a->code_size + layout.size) / DMV_CODE_WORD > 0xFFFFu)
    {
        error(a, word->column, "the view's code is larger than 256 KiB");
        a->stop = true;
        return;
    }

    /* Labels: defined ones are known now, the others are patched at the end */
    for (uint8_t i = 0; i < info->operand_count; i++)
    {
        if (labels[i] == NO_INDEX)
            continue;
        const label_t *l = &a->labels[labels[i]];
        if (l->flags & LABEL_DEFINED)
            put16(insn + layout.offsets[i], l->word);
        else if (PUSH(a, fixups))
        {
            fixup_t *f = &a->fixups[a->fixups_count++];
            f->word = code_words(a);
            f->label = (uint16_t)labels[i];
            f->offset = layout.offsets[i];
        }
    }

    /* Once there is an error the output is discarded - stop writing it */
    if (!has_errors(a))
        writer_put(&a->out, insn, layout.size);
    a->code_size += layout.size;
    a->last_opcode = opcode;
}

/* ---- Directives ---- */

static bool operand_count(assembler_t *a, const token_t *word, uint32_t n, uint32_t least, uint32_t most)
{
    if (n >= least && n <= most)
        return true;
    if (least == most)
        error(a, word->column, "%s takes %u operand%s", SHOW(word), (unsigned)least, (least == 1) ? "" : "s");
    else
        error(a, word->column, "%s takes %u to %u operands", SHOW(word), (unsigned)least, (unsigned)most);
    return false;
}

static bool immediate(assembler_t *a, const token_t *t, int64_t min, int64_t max, int64_t *out)
{
    value_t v;
    if (!parse_number(a, t, min, max, false, &v))
        return false;
    *out = v.value;
    return true;
}

static bool valid_name(assembler_t *a, const token_t *t, const char *what)
{
    if (token_is_ident(t->text, t->len))
        return true;
    error(a, t->column, "invalid %s name '%s'", what, SHOW(t));
    return false;
}

static void directive_var(assembler_t *a, const token_t *ops, uint32_t n)
{
    const token_t *name = &ops[0], *type = &ops[1], *init = &ops[2];
    var_t v;
    memset(&v, 0, sizeof(v));

    if (name->len < 2 || name->text[0] != '$' || !token_is_ident(name->text + 1, name->len - 1))
    {
        error(a, name->column, "invalid variable name '%s'", SHOW(name));
        return;
    }
    if (find_var(a, name->text + 1, name->len - 1) != NO_INDEX)
    {
        error(a, name->column, "variable '%s' declared twice", SHOW(name));
        return;
    }
    if (a->vars_count >= DMV_BUILTIN_BASE)
    {
        error(a, name->column, "too many variables");
        return;
    }

    if (token_ieq(type->text, type->len, "int"))
    {
        int64_t value;
        if (!immediate(a, init, INT32_MIN, UINT32_MAX, &value))
            return;
        v.type = DMV_VAR_INT;
        v.init = (int32_t)value;
    }
    else if (type->len > 5 && token_ieq(type->text, 4, "str[") && type->text[type->len - 1] == ']')
    {
        int64_t capacity;
        size_t length = 0;
        token_t size_token = { type->text + 4, type->len - 5, type->column + 4 };
        if (!immediate(a, &size_token, 1, 0xFFFF, &capacity))
            return;
        if (!is_string_literal(init))
        {
            error(a, init->column, "a string variable needs a string initial value");
            return;
        }
        int index = parse_string_literal(a, init, &length);
        if (index == NO_INDEX)
            return;
        if (length > (size_t)capacity)
        {
            error(a, init->column, "initial value longer than %u bytes", (unsigned)capacity);
            return;
        }
        v.type = DMV_VAR_STR;
        v.capacity = (uint16_t)capacity;
        v.init = index;
    }
    else
    {
        error(a, type->column, "unknown variable type '%s' (int or str[N])", SHOW(type));
        return;
    }

    v.env = DMV_NONE;
    if (n == 4)
    {
        const token_t *env = &ops[3];
        if (env->len < 5 || !token_ieq(env->text, 4, "env:"))
        {
            error(a, env->column, "expected env:NAME, got '%s'", SHOW(env));
            return;
        }
        int index = intern(a, env->text + 4, env->len - 4);
        if (index == NO_INDEX)
            return;
        v.env = (uint16_t)index;
        v.flags = DMV_VARF_ENV;
    }

    int name_index = intern(a, name->text + 1, name->len - 1);
    if (name_index == NO_INDEX || !PUSH(a, vars))
        return;
    v.name = (uint16_t)name_index;
    a->vars[a->vars_count++] = v;
}

static void directive_font(assembler_t *a, const token_t *ops)
{
    if (!valid_name(a, &ops[0], "font"))
        return;
    if (find_font(a, ops[0].text, ops[0].len) != NO_INDEX)
    {
        error(a, ops[0].column, "font '%s' declared twice", SHOW(&ops[0]));
        return;
    }
    if (!is_string_literal(&ops[1]))
    {
        error(a, ops[1].column, "expected the font spec as a string");
        return;
    }
    int spec = parse_string_literal(a, &ops[1], NULL);
    int name = (spec != NO_INDEX) ? intern(a, ops[0].text, ops[0].len) : NO_INDEX;
    if (name == NO_INDEX || !PUSH(a, fonts))
        return;
    a->fonts[a->fonts_count].name = (uint16_t)name;
    a->fonts[a->fonts_count].spec = (uint16_t)spec;
    a->fonts_count++;
}

static bool is_number_token(const token_t *t)
{
    return is_digit(t->text[0]) || t->text[0] == '-';
}

/* "COLOR [POSITION]" - the position in 1/1000, -1 when left out */
static bool parse_stop(assembler_t *a, const token_t *t, uint32_t *color, int32_t *permille)
{
    size_t split = 0;
    while (split < t->len && !is_space(t->text[split]))
        split++;
    token_t color_token = { t->text, split, t->column };
    value_t v;
    if (!parse_number(a, &color_token, 0, UINT32_MAX, false, &v))
        return false;
    *color = (uint32_t)v.value;
    *permille = -1;

    size_t at = split;
    while (at < t->len && is_space(t->text[at]))
        at++;
    if (at == t->len)
        return true;
    token_t position = { t->text + at, t->len - at, t->column + (uint32_t)at };
    if (position.len > 1 && position.text[position.len - 1] == '%')
        position.len--;
    /* Percent with at most one decimal: "50", "33.3" */
    int64_t value;
    size_t point = 0;
    while (point < position.len && position.text[point] != '.')
        point++;
    bool ok = is_number_token(&position) && parse_integer(position.text, point, &value) && value >= 0;
    int64_t tenths = 0;
    if (ok && point < position.len)
    {
        ok = position.len == point + 2U && is_digit(position.text[point + 1]);
        tenths = ok ? position.text[point + 1] - '0' : 0;
    }
    if (!ok || value * 10 + tenths > (int64_t)DMV_STOP_SCALE)
    {
        error(a, position.column, "expected a stop position 0 ... 100, got '%s'", SHOW(&position));
        return false;
    }
    *permille = (int32_t)(value * 10 + tenths);
    return true;
}

static void directive_gradient(assembler_t *a, const token_t *word, const token_t *ops, uint32_t n)
{
    gradient_t g;
    uint32_t colors[DMV_MAX_STOPS];
    int32_t positions[DMV_MAX_STOPS];      /* 1/1000, -1 = left out */
    uint32_t next = 2;
    int64_t value;

    if (n < 2)
    {
        error(a, word->column, ".gradient takes a name, LINEAR or RADIAL, and its stops");
        return;
    }
    if (!valid_name(a, &ops[0], "gradient"))
        return;
    if (find_gradient(a, ops[0].text, ops[0].len) != NO_INDEX || find_define(a, ops[0].text, ops[0].len) != NULL)
    {
        error(a, ops[0].column, "'%s' declared twice", SHOW(&ops[0]));
        return;
    }
    memset(&g, 0, sizeof(g));
    if (token_ieq(ops[1].text, ops[1].len, "LINEAR"))
    {
        g.kind = DMV_GRADIENT_LINEAR;
        g.param[0] = 180;                   /* Down, as in CSS */
        if (n > next && is_number_token(&ops[next]))
        {
            if (!immediate(a, &ops[next], INT16_MIN, INT16_MAX, &value))
                return;
            g.param[0] = (int16_t)(((value % 360) + 360) % 360);
            next++;
        }
    }
    else if (token_ieq(ops[1].text, ops[1].len, "RADIAL"))
    {
        g.kind = DMV_GRADIENT_RADIAL;
        for (uint32_t k = 0; k < 4U; k++)
            g.param[k] = 50;                /* The ellipse inscribed in the shape */
        if (n > next && is_number_token(&ops[next]))
        {
            uint32_t numbers = 0;
            while (next + numbers < n && is_number_token(&ops[next + numbers]))
                numbers++;
            if (numbers != 4U)
            {
                error(a, ops[next].column, "RADIAL takes cx, cy, rx, ry");
                return;
            }
            for (uint32_t k = 0; k < 4U; k++, next++)
            {
                if (!immediate(a, &ops[next], (k < 2U) ? INT16_MIN : 1, INT16_MAX, &value))
                    return;
                g.param[k] = (int16_t)value;
            }
        }
    }
    else
    {
        error(a, ops[1].column, "unknown gradient kind '%s' (LINEAR or RADIAL)", SHOW(&ops[1]));
        return;
    }

    uint32_t count = n - next;
    if (count < DMV_MIN_STOPS || count > DMV_MAX_STOPS)
    {
        error(a, word->column, "a gradient takes %u to %u color stops", (unsigned)DMV_MIN_STOPS, (unsigned)DMV_MAX_STOPS);
        return;
    }
    for (uint32_t k = 0; k < count; k++)
    {
        if (!parse_stop(a, &ops[next + k], &colors[k], &positions[k]))
            return;
    }

    /* Left-out positions, as in CSS: the ends at 0 and 100 %, the others
     * evenly between their neighbors */
    if (positions[0] < 0)
        positions[0] = 0;
    if (positions[count - 1U] < 0)
        positions[count - 1U] = (int32_t)DMV_STOP_SCALE;
    for (uint32_t k = 1; k < count; k++)
    {
        if (positions[k] >= 0)
            continue;
        uint32_t end = k;
        while (positions[end] < 0)
            end++;
        for (uint32_t m = k; m < end; m++)
            positions[m] = positions[k - 1U] + (positions[end] - positions[k - 1U]) * (int32_t)(m - k + 1U) /
                                               (int32_t)(end - k + 1U);
    }
    for (uint32_t k = 1; k < count; k++)
    {
        if (positions[k] < positions[k - 1U])
        {
            error(a, ops[next + k].column, "stop positions must not decrease");
            return;
        }
    }

    int name = intern(a, ops[0].text, ops[0].len);
    if (name == NO_INDEX || a->stops_count + count > 0xFFFFu || !PUSH(a, gradients))
        return;
    g.name = (uint16_t)name;
    g.count = (uint8_t)count;
    g.first = (uint16_t)a->stops_count;
    for (uint32_t k = 0; k < count; k++)
    {
        if (!PUSH(a, stops))
            return;
        a->stops[a->stops_count].color = colors[k];
        a->stops[a->stops_count].position = (uint16_t)positions[k];
        a->stops_count++;
    }
    a->gradients[a->gradients_count++] = g;
}

static void directive_define(assembler_t *a, const token_t *ops)
{
    int64_t value;
    if (!valid_name(a, &ops[0], "constant"))
        return;
    if (find_define(a, ops[0].text, ops[0].len) != NULL || find_gradient(a, ops[0].text, ops[0].len) != NO_INDEX ||
        token_eq(ops[0].text, ops[0].len, "POINTER_CONTACT") || token_eq(ops[0].text, ops[0].len, "FOCUS_CONTACT"))
    {
        error(a, ops[0].column, "constant '%s' defined twice", SHOW(&ops[0]));
        return;
    }
    if (!immediate(a, &ops[1], INT32_MIN, UINT32_MAX, &value) || !PUSH(a, defines))
        return;
    uint32_t at = (uint32_t)a->define_names.size;
    buffer_put(&a->define_names, ops[0].text, ops[0].len);
    buffer_put(&a->define_names, "", 1);
    if (a->define_names.failed)
    {
        a->out_of_memory = a->stop = true;
        return;
    }
    a->defines[a->defines_count].name = at;
    a->defines[a->defines_count].value = value;
    a->defines_count++;
}

static void add_item(assembler_t *a, uint8_t kind, uint8_t arg, int32_t label, uint32_t value)
{
    if (!PUSH(a, items))
        return;
    item_t *item = &a->items[a->items_count++];
    item->kind = kind;
    item->arg = arg;
    item->label = label;
    item->value = value;
}

static void directive_navkeys(assembler_t *a, const token_t *word, const token_t *ops, uint32_t n)
{
    if (n < 2 || (n % 2) != 0)
    {
        error(a, word->column, ".navkeys takes pairs of role, button");
        return;
    }
    for (uint32_t i = 0; i < n; i += 2)
    {
        int64_t button;
        int role = nav_role_find(ops[i].text, ops[i].len);
        if (role < 0)
            error(a, ops[i].column, "unknown navigation role '%s'", SHOW(&ops[i]));
        else if (immediate(a, &ops[i + 1], 0, 31, &button))
            add_item(a, DMV_ITEM_NAVKEY, (uint8_t)role, NO_INDEX, (uint32_t)button);
    }
}

static void process_source(assembler_t *a, const libtodmv_source_t *source);

static void directive_include(assembler_t *a, const token_t *ops)
{
    char path[LIBTODMV_LINE_MAX];
    size_t len = 0, error_offset = 0;
    const char *dir = a->options->include_dir;

    if (!is_string_literal(&ops[0]) || !parse_string(ops[0].text, ops[0].len, path, sizeof(path), &len, &error_offset))
    {
        error(a, ops[0].column, "expected the file to include as a string");
        return;
    }
    if (a->include_depth >= LIBTODMV_MAX_INCLUDE_DEPTH)
    {
        error(a, ops[0].column, "includes nested too deep");
        return;
    }

    /* The full path stays allocated: errors found later point at it */
    size_t dir_len = (dir != NULL && path[0] != '/') ? strlen(dir) : 0;
    char *full = Dmod_Malloc(dir_len + 1U + len + 1U);
    if (full == NULL || !PUSH(a, files))
    {
        if (full != NULL)
            Dmod_Free(full);
        a->out_of_memory = a->stop = true;
        return;
    }
    memcpy(full, dir, dir_len);
    if (dir_len > 0 && dir[dir_len - 1] != '/')
        full[dir_len++] = '/';
    memcpy(full + dir_len, path, len + 1U);
    a->files[a->files_count++] = full;

    void *file = Dmod_FileOpen(full, "r");
    if (file == NULL)
    {
        error(a, ops[0].column, "cannot include '%s'", path);
        return;
    }
    /* Field by field: an initializer of pointers may be kept as data, which
     * the dmod loader does not relocate */
    libtodmv_source_t source;
    source.read_line = file_read_line;
    source.ctx = file;
    source.name = full;
    location_t saved = a->at;
    a->include_depth++;
    process_source(a, &source);
    a->include_depth--;
    a->at = saved;
    Dmod_FileClose(file);
}

static void assemble_directive(assembler_t *a, const token_t *word, const token_t *ops, uint32_t n)
{
    int64_t v1, v2;

    if (token_ieq(word->text, word->len, ".view"))
    {
        if (!operand_count(a, word, n, 1, 1) || !valid_name(a, &ops[0], "view"))
            return;
        if (a->have_view)
        {
            error(a, word->column, ".view given twice");
            return;
        }
        int name = intern(a, ops[0].text, ops[0].len);
        a->have_view = name != NO_INDEX;
        a->view_name = (uint16_t)name;
    }
    else if (token_ieq(word->text, word->len, ".size"))
    {
        if (!operand_count(a, word, n, 2, 2) || !immediate(a, &ops[0], 1, 0xFFFF, &v1) ||
            !immediate(a, &ops[1], 1, 0xFFFF, &v2))
            return;
        if (a->have_size)
        {
            error(a, word->column, ".size given twice");
            return;
        }
        a->have_size = true;
        a->width = (uint16_t)v1;
        a->height = (uint16_t)v2;
    }
    else if (token_ieq(word->text, word->len, ".entry"))
    {
        if (!operand_count(a, word, n, 1, 1))
            return;
        if (a->entry != NO_INDEX)
        {
            error(a, word->column, ".entry given twice");
            return;
        }
        a->entry = label_use(a, &ops[0]);
    }
    else if (token_ieq(word->text, word->len, ".var"))
    {
        if (operand_count(a, word, n, 3, 4))
            directive_var(a, ops, n);
    }
    else if (token_ieq(word->text, word->len, ".font"))
    {
        if (operand_count(a, word, n, 2, 2))
            directive_font(a, ops);
    }
    else if (token_ieq(word->text, word->len, ".gradient"))
    {
        directive_gradient(a, word, ops, n);
    }
    else if (token_ieq(word->text, word->len, ".define"))
    {
        if (operand_count(a, word, n, 2, 2))
            directive_define(a, ops);
    }
    else if (token_ieq(word->text, word->len, ".include"))
    {
        if (operand_count(a, word, n, 1, 1))
            directive_include(a, ops);
    }
    else if (token_ieq(word->text, word->len, ".init"))
    {
        int32_t label;
        if (operand_count(a, word, n, 1, 1) && (label = label_use(a, &ops[0])) != NO_INDEX)
            add_item(a, DMV_ITEM_INIT, 0, label, 0);
    }
    else if (token_ieq(word->text, word->len, ".timer") || token_ieq(word->text, word->len, ".key"))
    {
        bool timer = token_ieq(word->text, word->len, ".timer");
        int32_t label;
        if (!operand_count(a, word, n, 2, 2) ||
            !immediate(a, &ops[0], timer ? 1 : 0, timer ? INT32_MAX : 31, &v1) ||
            (label = label_use(a, &ops[1])) == NO_INDEX)
            return;
        add_item(a, timer ? DMV_ITEM_TIMER : DMV_ITEM_KEY, 0, label, (uint32_t)v1);
    }
    else if (token_ieq(word->text, word->len, ".navkeys"))
    {
        directive_navkeys(a, word, ops, n);
    }
    else if (token_ieq(word->text, word->len, ".longpress") || token_ieq(word->text, word->len, ".scrollslop"))
    {
        bool longpress = token_ieq(word->text, word->len, ".longpress");
        bool *given = longpress ? &a->have_longpress : &a->have_scrollslop;
        if (!operand_count(a, word, n, 1, 1) || !immediate(a, &ops[0], longpress ? 1 : 0, 0xFFFF, &v1))
            return;
        if (*given)
        {
            error(a, word->column, "%s given twice", SHOW(word));
            return;
        }
        *given = true;
        *(longpress ? &a->longpress : &a->scrollslop) = (uint16_t)v1;
    }
    else
    {
        error(a, word->column, "unknown directive '%s'", SHOW(word));
    }
}

/* ---- Lines ---- */

static void process_line(assembler_t *a, const char *line, size_t len)
{
    size_t end = len, pos = 0, quote = 0;
    bool in_string = false;

    /* Comment: ';' outside a string */
    for (size_t i = 0; i < len; i++)
    {
        char c = line[i];
        if (in_string)
        {
            if (c == '\\')
                i++;
            else if (c == '"')
                in_string = false;
        }
        else if (c == '"')
        {
            in_string = true;
            quote = i;
        }
        else if (c == ';')
        {
            end = i;
            break;
        }
    }
    if (in_string)
    {
        error(a, (uint32_t)quote + 1, "unterminated string");
        return;
    }
    while (end > 0 && is_space(line[end - 1]))
        end--;
    while (pos < end && is_space(line[pos]))
        pos++;

    /* Label: name: or .name: */
    size_t q = pos + ((pos < end && line[pos] == '.') ? 1U : 0U);
    if (q < end && is_ident_start(line[q]))
    {
        while (q < end && is_ident_char(line[q]))
            q++;
        if (q < end && line[q] == ':')
        {
            token_t label = { line + pos, q - pos, (uint32_t)pos + 1 };
            define_label(a, &label);
            pos = q + 1;
            while (pos < end && is_space(line[pos]))
                pos++;
        }
    }
    if (pos >= end)
        return;

    /* Mnemonic or directive */
    token_t word = { line + pos, 0, (uint32_t)pos + 1 };
    while (pos < end && !is_space(line[pos]))
        pos++;
    word.len = (size_t)(line + pos - word.text);

    /* Operands, separated by commas outside strings */
    token_t ops[MAX_OPERAND_TOKENS];
    uint32_t n = 0;
    while (pos < end && is_space(line[pos]))
        pos++;
    while (pos < end)
    {
        size_t start = pos;
        in_string = false;
        while (pos < end && (in_string || line[pos] != ','))
        {
            if (in_string && line[pos] == '\\')
                pos++;
            else if (line[pos] == '"')
                in_string = !in_string;
            pos++;
        }
        size_t s = start, e = (pos < end) ? pos : end;
        while (s < e && is_space(line[s]))
            s++;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (s == e)
        {
            error(a, (uint32_t)s + 1, "empty operand");
            return;
        }
        if (n == MAX_OPERAND_TOKENS)
        {
            error(a, (uint32_t)s + 1, "too many operands");
            return;
        }
        ops[n].text = line + s;
        ops[n].len = e - s;
        ops[n].column = (uint32_t)s + 1;
        n++;
        if (pos < end && ++pos == end)
        {
            error(a, (uint32_t)pos, "empty operand");
            return;
        }
    }

    if (word.text[0] == '.')
        assemble_directive(a, &word, ops, n);
    else
        assemble_instruction(a, &word, ops, n);
}

static void process_source(assembler_t *a, const libtodmv_source_t *source)
{
    char *line = Dmod_Malloc(LIBTODMV_LINE_MAX);
    if (line == NULL)
    {
        a->out_of_memory = a->stop = true;
        return;
    }

    a->at.file = source->name;
    a->at.line = 0;
    while (!a->stop && source->read_line(source->ctx, line, (int)LIBTODMV_LINE_MAX) != NULL)
    {
        size_t len = strlen(line);
        a->at.line++;
        a->at.column = 0;

        if (len > 0 && line[len - 1] == '\n')
            len--;
        else if (len == LIBTODMV_LINE_MAX - 1U)
        {
            /* Filled the buffer: the line is complete only if the source
             * ends here or the line end comes next */
            char probe[2];
            if (source->read_line(source->ctx, probe, (int)sizeof(probe)) != NULL && probe[0] != '\n')
            {
                error(a, (uint32_t)len, "line longer than %u characters", (unsigned)(LIBTODMV_LINE_MAX - 2U));
                while (source->read_line(source->ctx, probe, (int)sizeof(probe)) != NULL && probe[0] != '\n')
                    ;
                continue;
            }
        }
        if (len > 0 && line[len - 1] == '\r')
            len--;
        process_line(a, line, len);
    }
    Dmod_Free(line);
}

/* ---- Output ---- */

static void check_references(assembler_t *a)
{
    location_t nowhere = { NULL, 0, 0 };

    for (uint32_t i = 0; i < a->depth; i++)
        error_at(a, &a->boxes[a->stack[i]].where, "BOX @%s is not closed", string_text(a, a->boxes[a->stack[i]].name));
    for (uint32_t i = 0; i < a->boxes_count; i++)
    {
        if (!a->boxes[i].defined)
            error_at(a, &a->boxes[i].where, "undefined box @%s", string_text(a, a->boxes[i].name));
    }
    for (uint32_t i = 0; i < a->labels_count; i++)
    {
        const label_t *l = &a->labels[i];
        if ((l->flags & LABEL_USED) && !(l->flags & LABEL_DEFINED))
            error_at(a, &l->first_use, "undefined label '%s'", string_text(a, l->name));
        else if ((l->flags & LABEL_USED) && l->word == code_words(a))
            error_at(a, &l->first_use, "label '%s' has no instruction after it", string_text(a, l->name));
    }
    if (!a->have_view)
        error_at(a, &nowhere, "missing .view");
    if (a->entry == NO_INDEX)
        error_at(a, &nowhere, "missing .entry");
    else if (a->code_size == 0)
        error_at(a, &nowhere, "the view has no instructions");
}

static void put_section(uint8_t *header, uint32_t at, uint32_t offset, uint32_t count)
{
    put32(header + at, offset);
    put32(header + at + 4, count);
}

/* Patch forward references, append the tables and write the header. */
static void finish_output(assembler_t *a)
{
    writer_t *w = &a->out;
    uint8_t header[HEADER_SIZE];

    for (uint32_t i = 0; i < a->fixups_count; i++)
    {
        const fixup_t *f = &a->fixups[i];
        writer_seek(w, HEADER_SIZE + f->word * DMV_CODE_WORD + f->offset);
        writer_put16(w, a->labels[f->label].word);
    }
    if (a->fixups_count != 0)
        writer_seek(w, HEADER_SIZE + a->code_size);

    memset(header, 0, sizeof(header));
    header[0] = DMV_MAGIC_0; header[1] = DMV_MAGIC_1; header[2] = DMV_MAGIC_2; header[3] = DMV_MAGIC_3;
    put16(header + 4, DMV_VERSION_MAJOR);
    /* The oldest version that has what the view uses: runtimes that know
     * only that run it (they find every table through the header) */
    put16(header + 6, a->uses_opacity ? 3U : (a->gradients_count != 0) ? 2U : 1U);
    put16(header + 12, a->width);
    put16(header + 14, a->height);
    put16(header + 16, a->view_name);
    put16(header + 18, a->labels[a->entry].word);
    put16(header + 20, a->have_longpress ? a->longpress : (uint16_t)DMV_DEFAULT_LONGPRESS_MS);
    put16(header + 22, a->have_scrollslop ? a->scrollslop : (uint16_t)DMV_DEFAULT_SCROLLSLOP);
    put_section(header, 24, HEADER_SIZE, code_words(a));

    /* Strings: offsets relative to the table, then the block of texts as is */
    put_section(header, 32, writer_tell(w), a->string_at_count);
    for (uint32_t i = 0; i < a->string_at_count; i++)
        writer_put32(w, a->string_at_count * 4U + a->string_at[i]);
    writer_put(w, a->text.data, a->text.size);
    while (writer_tell(w) % 4U)
        writer_put8(w, 0);

    put_section(header, 40, writer_tell(w), a->vars_count);
    for (uint32_t i = 0; i < a->vars_count; i++)
    {
        const var_t *v = &a->vars[i];
        writer_put8(w, v->type);
        writer_put8(w, v->flags);
        writer_put16(w, v->capacity);
        writer_put16(w, v->name);
        writer_put16(w, v->env);
        writer_put32(w, (uint32_t)v->init);
    }

    put_section(header, 48, writer_tell(w), a->fonts_count);
    for (uint32_t i = 0; i < a->fonts_count; i++)
    {
        writer_put16(w, a->fonts[i].name);
        writer_put16(w, a->fonts[i].spec);
    }

    put_section(header, 56, writer_tell(w), a->boxes_count);
    for (uint32_t i = 0; i < a->boxes_count; i++)
    {
        writer_put16(w, a->boxes[i].name);
        writer_put16(w, a->boxes[i].parent);
        writer_put16(w, a->boxes[i].begin);
        writer_put16(w, a->boxes[i].end);
    }

    put_section(header, 64, writer_tell(w), a->items_count);
    for (uint32_t i = 0; i < a->items_count; i++)
    {
        const item_t *item = &a->items[i];
        writer_put8(w, item->kind);
        writer_put8(w, item->arg);
        writer_put16(w, (item->label != NO_INDEX) ? a->labels[item->label].word : (uint16_t)DMV_NONE);
        writer_put32(w, item->value);
    }

    put_section(header, 72, writer_tell(w), a->definition_order_count);
    for (uint32_t i = 0; i < a->definition_order_count; i++)
    {
        const label_t *l = &a->labels[a->definition_order[i]];
        writer_put16(w, l->name);
        writer_put16(w, l->word);
    }

    put_section(header, 80, writer_tell(w), a->gradients_count);
    for (uint32_t i = 0; i < a->gradients_count; i++)
    {
        const gradient_t *g = &a->gradients[i];
        writer_put16(w, g->name);
        writer_put8(w, g->kind);
        writer_put8(w, g->count);
        writer_put16(w, g->first);
        for (uint32_t k = 0; k < 4U; k++)
            writer_put16(w, (uint16_t)g->param[k]);
        writer_put16(w, 0);
    }

    put_section(header, 88, writer_tell(w), a->stops_count);
    for (uint32_t i = 0; i < a->stops_count; i++)
    {
        writer_put32(w, a->stops[i].color);
        writer_put16(w, a->stops[i].position);
        writer_put16(w, 0);
    }

    a->result->size = writer_tell(w);
    put32(header + 8, a->result->size);
    writer_seek(w, 0);
    writer_put(w, header, sizeof(header));
    writer_flush(w);
}

static void release(assembler_t *a)
{
    for (uint32_t i = 0; i < a->files_count; i++)
        Dmod_Free(a->files[i]);

    void *arrays[] = { a->files, a->string_at, a->string_hash, a->labels, a->definition_order, a->vars,
                       a->fonts, a->gradients, a->stops, a->boxes, a->defines, a->fixups, a->items };
    for (size_t i = 0; i < sizeof(arrays) / sizeof(arrays[0]); i++)
    {
        if (arrays[i] != NULL)
            Dmod_Free(arrays[i]);
    }
    buffer_free(&a->text);
    buffer_free(&a->define_names);
}

dmod_libtodmv_api_declaration(1.0, int, _assemble, ( const libtodmv_source_t* source, const libtodmv_sink_t* sink, const libtodmv_options_t* options, libtodmv_result_t* result ))
{
    static const libtodmv_options_t defaults = { 0 };
    uint8_t placeholder[HEADER_SIZE];
    assembler_t a;

    if (result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (source == NULL || source->read_line == NULL || sink == NULL || sink->write == NULL || sink->seek == NULL)
        return -EINVAL;

    memset(&a, 0, sizeof(a));
    a.options = (options != NULL) ? options : &defaults;
    a.result = result;
    a.max_errors = (a.options->max_errors != 0) ? a.options->max_errors : LIBTODMV_DEFAULT_MAX_ERRORS;
    a.scope = NO_INDEX;
    a.entry = NO_INDEX;
    a.last_opcode = -1;
    writer_init(&a.out, sink);

    /* The header is written last */
    memset(placeholder, 0, sizeof(placeholder));
    writer_put(&a.out, placeholder, sizeof(placeholder));

    process_source(&a, source);
    if (!a.out_of_memory)
        check_references(&a);
    if (!has_errors(&a))
        finish_output(&a);

    int ret = 0;
    if (a.out_of_memory)
        ret = -ENOMEM;
    else if (has_errors(&a))
        ret = -EBADMSG;
    else if (a.out.status != 0)
        ret = -EIO;
    if (ret != 0)
        result->size = 0;
    release(&a);
    return ret;
}

static void directory_of(const char *path, char *directory, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t len = (slash != NULL) ? (size_t)(slash - path) : 0;
    if (len >= size)
        len = size - 1U;
    memcpy(directory, path, len);
    directory[len] = '\0';
}

dmod_libtodmv_api_declaration(1.0, int, _assemble_file, ( const char* input, const char* output, const libtodmv_options_t* options, libtodmv_result_t* result ))
{
    char directory[LIBTODMV_LINE_MAX];
    libtodmv_options_t opts;

    if (input == NULL || output == NULL || result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (options != NULL)
        opts = *options;
    else
        memset(&opts, 0, sizeof(opts));
    if (opts.include_dir == NULL)
    {
        directory_of(input, directory, sizeof(directory));
        opts.include_dir = directory;
    }

    void *in = Dmod_FileOpen(input, "r");
    if (in == NULL)
        return -ENOENT;
    void *out = Dmod_FileOpen(output, "wb");
    if (out == NULL)
    {
        Dmod_FileClose(in);
        return -EIO;
    }

    libtodmv_source_t source;

    source.read_line = file_read_line;

    source.ctx = in;

    source.name = input;
    libtodmv_sink_t sink;
    sink.write = file_write;
    sink.seek = file_seek;
    sink.ctx = out;
    int ret = libtodmv_assemble(&source, &sink, &opts, result);
    Dmod_FileClose(out);
    Dmod_FileClose(in);
    if (ret != 0)
        Dmod_FileRemove(output);
    return ret;
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
