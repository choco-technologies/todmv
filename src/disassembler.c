#include "private.h"
#include "files.h"
#include <errno.h>
#include <string.h>

/*
 * .dmv -> .dmvs, reading the view in small pieces: instructions one by one,
 * strings in chunks. Only the symbol table (4 bytes per label) is loaded,
 * since every instruction looks up its labels. The text goes out through a
 * small buffered writer.
 *
 * The view is validated first, so every offset and index is in range.
 */

#define STRING_CHUNK        32u
#define MAX_INSTRUCTION     64u

typedef struct
{
    const libtodmv_input_t *in;
    writer_t                out;
    int                     status;         /* 0, or -EIO / -ENOMEM */
    dmv_section_t           code, strings, vars, fonts, boxes, items, symbols, gradients, stops;
    uint8_t                *symbol_table;   /* symbols.count records */
    uint16_t               *generated;      /* Label offsets without a symbol */
    uint32_t                generated_count;
    uint32_t                generated_capacity;
} disassembler_t;

static bool rd(disassembler_t *d, uint32_t offset, void *buffer, size_t size)
{
    if (d->status == 0 && d->in->read(d->in->ctx, offset, buffer, size) != 0)
        d->status = -EIO;
    if (d->status != 0)
        memset(buffer, 0, size);
    return d->status == 0;
}

static uint16_t rd16_at(disassembler_t *d, uint32_t offset)
{
    uint8_t b[2];
    rd(d, offset, b, sizeof(b));
    return get16(b);
}

static void read_section(const uint8_t *p, dmv_section_t *s)
{
    s->offset = get32(p);
    s->count = get32(p + 4);
}

/* Write string `index` - escaped for a "..." literal, or as is. */
static void print_string(disassembler_t *d, uint32_t index, bool escaped)
{
    uint8_t entry[4];
    char chunk[STRING_CHUNK];

    if (!rd(d, d->strings.offset + index * 4U, entry, sizeof(entry)))
        return;
    uint32_t at = d->strings.offset + get32(entry);
    for (;;)
    {
        uint32_t n = (d->in->size - at < STRING_CHUNK) ? d->in->size - at : STRING_CHUNK;
        if (n == 0 || !rd(d, at, chunk, n))
            return;
        size_t len = 0;
        while (len < n && chunk[len] != '\0')
            len++;
        if (escaped)
            writer_put_escaped(&d->out, chunk, len);
        else
            writer_put(&d->out, chunk, len);
        if (len < n)
            return;
        at += n;
    }
}

static void print_literal(disassembler_t *d, uint32_t index)
{
    writer_put8(&d->out, '"');
    print_string(d, index, true);
    writer_put8(&d->out, '"');
}

static int symbol_at(const disassembler_t *d, uint32_t word, uint32_t after)
{
    for (uint32_t i = after; i < d->symbols.count; i++)
    {
        if (get16(d->symbol_table + i * 4U + 2U) == word)
            return (int)i;
    }
    return -1;
}

static bool is_generated(const disassembler_t *d, uint32_t word)
{
    for (uint32_t i = 0; i < d->generated_count; i++)
    {
        if (d->generated[i] == word)
            return true;
    }
    return false;
}

/* Labels referenced without a symbol (a view not built by the assembler)
 * get generated names. */
static void note_label(disassembler_t *d, uint32_t word)
{
    if (symbol_at(d, word, 0) >= 0 || is_generated(d, word))
        return;
    if (d->generated_count == d->generated_capacity)
    {
        uint32_t capacity = d->generated_capacity ? d->generated_capacity * 2U : 8U;
        uint16_t *p = Dmod_Realloc(d->generated, capacity * sizeof(uint16_t));
        if (p == NULL)
        {
            d->status = -ENOMEM;
            return;
        }
        d->generated = p;
        d->generated_capacity = capacity;
    }
    d->generated[d->generated_count++] = (uint16_t)word;
}

static void print_label(disassembler_t *d, uint32_t word)
{
    int symbol = symbol_at(d, word, 0);
    if (symbol >= 0)
        print_string(d, get16(d->symbol_table + (uint32_t)symbol * 4U), false);
    else
        writer_printf(&d->out, "L_%04X", (unsigned)word);
}

static void print_labels_at(disassembler_t *d, uint32_t word)
{
    for (int i = symbol_at(d, word, 0); i >= 0; i = symbol_at(d, word, (uint32_t)i + 1U))
    {
        print_string(d, get16(d->symbol_table + (uint32_t)i * 4U), false);
        writer_printf(&d->out, ":\n");
    }
    if (is_generated(d, word))
        writer_printf(&d->out, "L_%04X:\n", (unsigned)word);
}

static void print_var(disassembler_t *d, uint32_t index)
{
    writer_put8(&d->out, '$');
    if (index >= DMV_BUILTIN_BASE)
        writer_printf(&d->out, "%s", builtin_name(index));
    else
        print_string(d, rd16_at(d, d->vars.offset + index * (uint32_t)sizeof(dmv_var_t) + 4U), false);
}

static uint8_t var_type(disassembler_t *d, uint32_t index)
{
    uint8_t type = DMV_VAR_INT;
    rd(d, d->vars.offset + index * (uint32_t)sizeof(dmv_var_t), &type, 1);
    return type;
}

static void print_operand(disassembler_t *d, const uint8_t *insn, const dmv_opcode_info_t *info,
                          const dmv_layout_t *layout, uint8_t i)
{
    uint8_t kind = info->operands[i];
    const uint8_t *slot = insn + layout->offsets[i];
    uint32_t value = (dmv_operand_size(kind) == 4) ? get32(slot) : get16(slot);

    if (insn[2] & (1U << i))
    {
        print_var(d, value);
        return;
    }
    switch (kind)
    {
        case DMV_OPERAND_V16:
            writer_printf(&d->out, "%d", (int)(int16_t)value);
            break;
        case DMV_OPERAND_V32:
            /* SET into a string variable: the immediate is a string index */
            if (insn[0] == DMV_OP_SET && var_type(d, get16(insn + layout->offsets[0])) == DMV_VAR_STR)
                print_literal(d, value);
            else
                writer_printf(&d->out, "%d", (int)(int32_t)value);
            break;
        case DMV_OPERAND_COLOR:
            if (insn[3] & DMV_PAINT_GRADIENT)
                print_string(d, rd16_at(d, d->gradients.offset + value * (uint32_t)sizeof(dmv_gradient_t)), false);
            else
                writer_printf(&d->out, "#%08X", (unsigned)value);
            break;
        case DMV_OPERAND_STR:
            print_literal(d, value);
            break;
        case DMV_OPERAND_VAR:
            print_var(d, value);
            break;
        case DMV_OPERAND_LABEL:
            print_label(d, value);
            break;
        case DMV_OPERAND_BOX:
            writer_put8(&d->out, '@');
            print_string(d, rd16_at(d, d->boxes.offset + value * (uint32_t)sizeof(dmv_box_t)), false);
            break;
        case DMV_OPERAND_FONT:
            print_string(d, rd16_at(d, d->fonts.offset + value * (uint32_t)sizeof(dmv_font_t)), false);
            break;
        case DMV_OPERAND_EVENT:
            writer_printf(&d->out, "%s", event_name(value));
            break;
        default:
            break;
    }
}

static void print_instruction(disassembler_t *d, const uint8_t *insn)
{
    const dmv_opcode_info_t *info = dmv_get_opcode_info(insn[0]);
    dmv_layout_t layout;
    bool first = true;
    (void)dmv_get_layout(insn[0], &layout);

    writer_printf(&d->out, "        %s", info->mnemonic);
    for (uint8_t i = 0; i < info->operand_count; i++)
    {
        /* A left-out optional operand (REDRAW without a box) */
        if (i >= info->operand_count - info->optional && info->operands[i] == DMV_OPERAND_BOX &&
            get16(insn + layout.offsets[i]) == DMV_NONE)
            break;
        writer_printf(&d->out, first ? " " : ", ");
        first = false;
        print_operand(d, insn, info, &layout, i);
    }
    uint8_t flags = insn[3] & (uint8_t)~DMV_PAINT_GRADIENT;     /* The paint flag is the operand's */
    if (info->flags_kind == DMV_FLAGS_ALIGN || (info->flags_kind != DMV_FLAGS_NONE && flags != 0))
    {
        writer_printf(&d->out, first ? " " : ", ");
        flags_print(&d->out, info->flags_kind, flags);
    }
    writer_put8(&d->out, '\n');
}

/* Next instruction at byte `pos` of the code; false at the end or on error. */
static bool read_instruction(disassembler_t *d, uint32_t pos, uint8_t insn[MAX_INSTRUCTION])
{
    if (pos >= d->code.count * DMV_CODE_WORD || !rd(d, d->code.offset + pos, insn, 2))
        return false;
    return rd(d, d->code.offset + pos, insn, insn[1]);
}

static void print_directives(disassembler_t *d, const uint8_t *h)
{
    writer_printf(&d->out, "; Disassembled by libtodmv\n.view   ");
    print_string(d, get16(h + 16), false);
    writer_put8(&d->out, '\n');
    if (get16(h + 12) != 0 || get16(h + 14) != 0)
        writer_printf(&d->out, ".size   %u, %u\n", (unsigned)get16(h + 12), (unsigned)get16(h + 14));
    writer_printf(&d->out, ".entry  ");
    print_label(d, get16(h + 18));
    writer_put8(&d->out, '\n');
    if (get16(h + 20) != DMV_DEFAULT_LONGPRESS_MS)
        writer_printf(&d->out, ".longpress %u\n", (unsigned)get16(h + 20));
    if (get16(h + 22) != DMV_DEFAULT_SCROLLSLOP)
        writer_printf(&d->out, ".scrollslop %u\n", (unsigned)get16(h + 22));

    for (uint32_t i = 0; i < d->fonts.count; i++)
    {
        uint8_t f[sizeof(dmv_font_t)];
        rd(d, d->fonts.offset + i * (uint32_t)sizeof(dmv_font_t), f, sizeof(f));
        writer_printf(&d->out, ".font   ");
        print_string(d, get16(f), false);
        writer_printf(&d->out, ", ");
        print_literal(d, get16(f + 2));
        writer_put8(&d->out, '\n');
    }

    for (uint32_t i = 0; i < d->gradients.count; i++)
    {
        uint8_t g[sizeof(dmv_gradient_t)];
        rd(d, d->gradients.offset + i * (uint32_t)sizeof(dmv_gradient_t), g, sizeof(g));
        writer_printf(&d->out, ".gradient ");
        print_string(d, get16(g), false);
        if (g[2] == DMV_GRADIENT_LINEAR)
            writer_printf(&d->out, ", LINEAR, %d", (int)(int16_t)get16(g + 6));
        else
            writer_printf(&d->out, ", RADIAL, %d, %d, %d, %d", (int)(int16_t)get16(g + 6), (int)(int16_t)get16(g + 8),
                          (int)(int16_t)get16(g + 10), (int)(int16_t)get16(g + 12));
        for (uint32_t k = 0; k < g[3]; k++)
        {
            uint8_t st[sizeof(dmv_stop_t)];
            rd(d, d->stops.offset + (get16(g + 4) + k) * (uint32_t)sizeof(dmv_stop_t), st, sizeof(st));
            unsigned position = get16(st + 4);
            writer_printf(&d->out, ", #%08X %u", (unsigned)get32(st), position / 10U);
            if (position % 10U != 0)
                writer_printf(&d->out, ".%u", position % 10U);
        }
        writer_put8(&d->out, '\n');
    }

    for (uint32_t i = 0; i < d->vars.count; i++)
    {
        uint8_t v[sizeof(dmv_var_t)];
        rd(d, d->vars.offset + i * (uint32_t)sizeof(dmv_var_t), v, sizeof(v));
        writer_printf(&d->out, ".var    $");
        print_string(d, get16(v + 4), false);
        if (v[0] == DMV_VAR_STR)
        {
            writer_printf(&d->out, ", str[%u], ", (unsigned)get16(v + 2));
            print_literal(d, get32(v + 8));
        }
        else
            writer_printf(&d->out, ", int, %d", (int)(int32_t)get32(v + 8));
        if (v[1] & DMV_VARF_ENV)
        {
            writer_printf(&d->out, ", env:");
            print_string(d, get16(v + 6), false);
        }
        writer_put8(&d->out, '\n');
    }

    for (uint32_t i = 0; i < d->items.count; i++)
    {
        uint8_t it[sizeof(dmv_item_t)];
        rd(d, d->items.offset + i * (uint32_t)sizeof(dmv_item_t), it, sizeof(it));
        unsigned value = (unsigned)get32(it + 4);
        switch (it[0])
        {
            case DMV_ITEM_INIT:   writer_printf(&d->out, ".init   "); break;
            case DMV_ITEM_TIMER:  writer_printf(&d->out, ".timer  %u, ", value); break;
            case DMV_ITEM_KEY:    writer_printf(&d->out, ".key    %u, ", value); break;
            default:              writer_printf(&d->out, ".navkeys %s, %u\n", nav_role_name(it[1]), value); continue;
        }
        print_label(d, get16(it + 2));
        writer_put8(&d->out, '\n');
    }
}

dmod_libtodmv_api_declaration(1.0, int, _disassemble, ( const libtodmv_input_t* input, const libtodmv_sink_t* sink ))
{
    disassembler_t d;
    uint8_t header[sizeof(dmv_header_t)], insn[MAX_INSTRUCTION];

    if (input == NULL || input->read == NULL || sink == NULL || sink->write == NULL)
        return -EINVAL;
    switch (dmv_validate(input, NULL))
    {
        case DMV_VALID:      break;
        case DMV_ERR_IO:     return -EIO;
        case DMV_ERR_MEMORY: return -ENOMEM;
        default:             return -EBADMSG;
    }

    memset(&d, 0, sizeof(d));
    d.in = input;
    writer_init(&d.out, sink);
    memset(header, 0, sizeof(header));
    rd(&d, 0, header, DMV_HEADER_SIZE_0_1);
    if (get16(header + 6) >= 2)
    {
        rd(&d, DMV_HEADER_SIZE_0_1, header + DMV_HEADER_SIZE_0_1, DMV_HEADER_SIZE - DMV_HEADER_SIZE_0_1);
        read_section(header + 80, &d.gradients);
        read_section(header + 88, &d.stops);
    }
    read_section(header + 24, &d.code);
    read_section(header + 32, &d.strings);
    read_section(header + 40, &d.vars);
    read_section(header + 48, &d.fonts);
    read_section(header + 56, &d.boxes);
    read_section(header + 64, &d.items);
    read_section(header + 72, &d.symbols);

    d.symbol_table = Dmod_Malloc(d.symbols.count * 4U + 1U);
    if (d.symbol_table == NULL)
        d.status = -ENOMEM;
    else
        rd(&d, d.symbols.offset, d.symbol_table, d.symbols.count * 4U);

    /* Labels that need a generated name */
    note_label(&d, get16(header + 18));
    for (uint32_t i = 0; i < d.items.count && d.status == 0; i++)
    {
        uint16_t label = rd16_at(&d, d.items.offset + i * (uint32_t)sizeof(dmv_item_t) + 2U);
        if (label != DMV_NONE)
            note_label(&d, label);
    }
    for (uint32_t pos = 0; d.status == 0 && read_instruction(&d, pos, insn); pos += insn[1])
    {
        const dmv_opcode_info_t *info = dmv_get_opcode_info(insn[0]);
        dmv_layout_t layout;
        (void)dmv_get_layout(insn[0], &layout);
        for (uint8_t i = 0; i < info->operand_count; i++)
        {
            if (info->operands[i] == DMV_OPERAND_LABEL)
                note_label(&d, get16(insn + layout.offsets[i]));
        }
    }

    if (d.status == 0)
    {
        print_directives(&d, header);
        writer_put8(&d.out, '\n');
        for (uint32_t pos = 0; d.status == 0 && read_instruction(&d, pos, insn); pos += insn[1])
        {
            print_labels_at(&d, pos / DMV_CODE_WORD);
            print_instruction(&d, insn);
        }
        print_labels_at(&d, d.code.count);
        writer_flush(&d.out);
    }

    if (d.symbol_table != NULL)
        Dmod_Free(d.symbol_table);
    if (d.generated != NULL)
        Dmod_Free(d.generated);
    return (d.status != 0) ? d.status : d.out.status;
}

dmod_libtodmv_api_declaration(1.0, int, _disassemble_file, ( const char* input, const char* output ))
{
    libtodmv_input_t in;

    if (input == NULL)
        return -EINVAL;
    void *file = file_open_input(input, &in);
    if (file == NULL)
        return -ENOENT;

    void *out = NULL;
    if (output != NULL && (out = Dmod_FileOpen(output, "wb")) == NULL)
    {
        Dmod_FileClose(file);
        return -EIO;
    }
    libtodmv_sink_t sink;
    sink.write = (out != NULL) ? file_write : console_write;
    sink.seek = NULL;
    sink.ctx = out;
    int ret = libtodmv_disassemble(&in, &sink);
    if (out != NULL)
    {
        Dmod_FileClose(out);
        if (ret != 0)
            Dmod_FileRemove(output);
    }
    Dmod_FileClose(file);
    return ret;
}

dmod_libtodmv_api_declaration(1.0, int, _validate, ( const libtodmv_input_t* input, uint32_t* error_offset, const char** reason ))
{
    if (input == NULL || input->read == NULL)
        return -EINVAL;
    dmv_status_t status = dmv_validate(input, error_offset);
    if (reason != NULL)
        *reason = dmv_status_name(status);
    switch (status)
    {
        case DMV_VALID:      return 0;
        case DMV_ERR_IO:     return -EIO;
        case DMV_ERR_MEMORY: return -ENOMEM;
        default:             return -EBADMSG;
    }
}

dmod_libtodmv_api_declaration(1.0, int, _validate_file, ( const char* path, uint32_t* error_offset, const char** reason ))
{
    libtodmv_input_t in;

    if (path == NULL)
        return -EINVAL;
    void *file = file_open_input(path, &in);
    if (file == NULL)
        return -ENOENT;
    int ret = libtodmv_validate(&in, error_offset, reason);
    Dmod_FileClose(file);
    return ret;
}
