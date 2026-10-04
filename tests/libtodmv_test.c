#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libtodmv.h"
#include "format.h"
#include "dmosi.h"
#include <errno.h>
#include <string.h>

/* ---- In-memory streams (the library itself never holds a whole file) ----
 *
 * Structures holding pointers are filled field by field: the compiler may
 * keep a constant initializer as data, and the dmod loader does not
 * relocate pointers stored in initialized data. */

#define MEMORY_SIZE     8192u
#define MAX_ERRORS      8u

typedef struct
{
    const char *text;
    size_t      size;
    size_t      pos;
} text_source_t;

typedef struct
{
    uint8_t     data[MEMORY_SIZE];
    uint32_t    size;
    uint32_t    pos;
} memory_t;

typedef struct
{
    char        file[256];
    uint32_t    line;
    uint32_t    column;
    char        message[LIBTODMV_MESSAGE_MAX];
} captured_error_t;

static memory_t          g_out;         /* Assembled view */
static memory_t          g_text;        /* Disassembled text */
static libtodmv_result_t g_result;
static captured_error_t  g_errors[MAX_ERRORS];
static uint32_t          g_error_count;

/* Dmod_FileReadLine contract */
static char *text_read_line(void *ctx, char *buffer, int size)
{
    text_source_t *s = (text_source_t *)ctx;
    int n = 0;
    if (s->pos >= s->size)
        return NULL;
    while (n < size - 1 && s->pos < s->size)
    {
        char c = s->text[s->pos++];
        buffer[n++] = c;
        if (c == '\n')
            break;
    }
    buffer[n] = '\0';
    return buffer;
}

static int memory_write(void *ctx, const void *data, size_t size)
{
    memory_t *m = (memory_t *)ctx;
    if (m->pos + size > MEMORY_SIZE)
        return -ENOSPC;
    memcpy(m->data + m->pos, data, size);
    m->pos += (uint32_t)size;
    if (m->pos > m->size)
        m->size = m->pos;
    return 0;
}

static int memory_seek(void *ctx, uint32_t offset)
{
    ((memory_t *)ctx)->pos = offset;
    return 0;
}

static int memory_read(void *ctx, uint32_t offset, void *buffer, size_t size)
{
    memory_t *m = (memory_t *)ctx;
    if (offset + size > m->size)
        return -EIO;
    memcpy(buffer, m->data + offset, size);
    return 0;
}

static void capture_error(void *user, const libtodmv_error_t *e)
{
    (void)user;
    if (g_error_count < MAX_ERRORS)
    {
        captured_error_t *c = &g_errors[g_error_count];
        memset(c, 0, sizeof(*c));
        if (e->file != NULL)
            strncpy(c->file, e->file, sizeof(c->file) - 1U);
        c->line = e->line;
        c->column = e->column;
        memcpy(c->message, e->message, sizeof(c->message));     /* Same size, zero-terminated */
    }
    g_error_count++;
}

static void reset(memory_t *m)
{
    m->size = 0;
    m->pos = 0;
}

static int assemble_with(const char *source, libtodmv_options_t *options)
{
    text_source_t text;
    text.text = source;
    text.size = strlen(source);
    text.pos = 0;
    libtodmv_source_t src;
    src.read_line = text_read_line;
    src.ctx = &text;
    src.name = "main.dmvs";
    libtodmv_sink_t sink;
    sink.write = memory_write;
    sink.seek = memory_seek;
    sink.ctx = &g_out;

    reset(&g_out);
    g_error_count = 0;
    options->on_error = capture_error;
    return libtodmv_assemble(&src, &sink, options, &g_result);
}

static int assemble(const char *source)
{
    libtodmv_options_t options = { 0 };
    return assemble_with(source, &options);
}

/* Disassemble `view` into g_text, zero-terminated. */
static int disassemble(memory_t *view)
{
    libtodmv_input_t in;
    in.read = memory_read;
    in.ctx = view;
    in.size = view->size;
    libtodmv_sink_t sink;
    sink.write = memory_write;
    sink.seek = NULL;
    sink.ctx = &g_text;
    reset(&g_text);
    int ret = libtodmv_disassemble(&in, &sink);
    g_text.data[(g_text.size < MEMORY_SIZE) ? g_text.size : MEMORY_SIZE - 1U] = 0;
    return ret;
}

void dmod_test_setup(void)
{
    memset(&g_result, 0, sizeof(g_result));
    g_error_count = 0;
}

void dmod_test_teardown(void)
{
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

/* dmod modules have no memcmp() */
static bool bytes_equal(const uint8_t *a, const uint8_t *b, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static const uint8_t *code(void)
{
    return g_out.data + rd32(g_out.data + 24);
}

static uint32_t code_words(void)
{
    return rd32(g_out.data + 28);
}

static const uint8_t *table(uint32_t header_offset, uint32_t index, uint32_t entry_size)
{
    return g_out.data + rd32(g_out.data + header_offset) + index * entry_size;
}

static const char *string(uint32_t index)
{
    uint32_t strings = rd32(g_out.data + 32);
    return (const char *)g_out.data + strings + rd32(g_out.data + strings + index * 4U);
}

/* The first error contains `text` and points at line/column. */
static bool first_error(uint32_t line, uint32_t column, const char *text)
{
    if (g_error_count == 0)
        return false;
    const captured_error_t *e = &g_errors[0];
    if (e->line != line || e->column != column)
    {
        Dmod_Printf("    got %u:%u: %s\n", (unsigned)e->line, (unsigned)e->column, e->message);
        return false;
    }
    size_t n = strlen(text);
    for (const char *p = e->message; *p != '\0'; p++)
    {
        if (strncmp(p, text, n) == 0)
            return true;
    }
    Dmod_Printf("    got message: %s\n", e->message);
    return false;
}

#define VIEW    ".view t\n.entry main\n"

/* ---- Output layout ---- */

DMOD_TEST_STEP(libtodmv_minimal_view)
{
    DMOD_TEST_EXPECT_EQ(assemble(".view   tiny\n.size 480, 272\n.entry  main\nmain:\n        RET\n"), 0);
    DMOD_TEST_EXPECT_EQ(g_result.error_count, 0u);
    DMOD_TEST_EXPECT_EQ(g_result.size, g_out.size);

    const uint8_t *h = g_out.data;
    DMOD_TEST_EXPECT_TRUE(h[0] == 'D' && h[1] == 'M' && h[2] == 'V' && h[3] == 0);
    DMOD_TEST_EXPECT_EQ(rd16(h + 4), DMV_VERSION_MAJOR);
    DMOD_TEST_EXPECT_EQ(rd16(h + 6), 1);                     /* No gradients: a version 0.1 view */
    DMOD_TEST_EXPECT_EQ(rd32(h + 8), g_out.size);
    DMOD_TEST_EXPECT_EQ(rd32(h + 24), DMV_HEADER_SIZE);
    DMOD_TEST_EXPECT_EQ(rd16(h + 12), 480);
    DMOD_TEST_EXPECT_EQ(rd16(h + 14), 272);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(h + 16)), "tiny"), 0);
    DMOD_TEST_EXPECT_EQ(rd16(h + 18), 0);
    DMOD_TEST_EXPECT_EQ(rd16(h + 20), DMV_DEFAULT_LONGPRESS_MS);
    DMOD_TEST_EXPECT_EQ(rd16(h + 22), DMV_DEFAULT_SCROLLSLOP);
    DMOD_TEST_EXPECT_EQ(code_words(), 1u);
    DMOD_TEST_EXPECT_EQ(code()[0], DMV_OP_RET);
    DMOD_TEST_EXPECT_EQ(code()[1], 4);

    /* One symbol: main at 0 */
    DMOD_TEST_EXPECT_EQ(rd32(h + 76), 1u);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(table(72, 0, 4))), "main"), 0);

    libtodmv_input_t in;

    in.read = memory_read;

    in.ctx = &g_out;

    in.size = g_out.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&in, NULL, NULL), 0);
}

DMOD_TEST_STEP(libtodmv_encodes_operands)
{
    static const uint8_t rect[16] = {
        DMV_OP_RECT, 16, 0, 0,  1, 0,  2, 0,  0xFD, 0xFF,  4, 0,  0x44, 0x33, 0x22, 0x11,
    };
    static const uint8_t fill[4] = { DMV_OP_FILL, 8, 0, 0 };

    DMOD_TEST_EXPECT_EQ(assemble(VIEW "main:\n RECT 1, 2, -3, 4, #11223344\n FILL #3D85F5\n RET\n"), 0);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(code(), rect, sizeof(rect)));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(code() + 16, fill, sizeof(fill)));
    DMOD_TEST_EXPECT_EQ(rd32(code() + 20), 0xFF3D85F5u);      /* #RRGGBB is opaque */
}

DMOD_TEST_STEP(libtodmv_encodes_gradients)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        ".define TOP, #3D85F5\n"
        ".gradient sky, LINEAR, 90, TOP 0, #80000000 100%\n"
        ".gradient down, LINEAR, #FF0000, #00FF00, #0000FF, #FFFFFF\n"
        ".gradient glow, RADIAL, 25, 75, 50, 100, #FFFFFF, #000000 33.3\n"
        ".gradient dot, RADIAL, #FFFFFF, #000000\n"
        ".font f, \"sans-16\"\n"
        "main:\n RRECT 0, 0, 10, 10, 2, sky\n TEXT 0, 0, 8, 8, \"x\", f, glow, CENTER|MIDDLE\n FILL #102030\n RET\n"), 0);

    const uint8_t *h = g_out.data;
    DMOD_TEST_EXPECT_EQ(rd16(h + 6), 2);                     /* Gradients: version 0.2 */
    DMOD_TEST_EXPECT_EQ(rd32(h + 84), 4u);                   /* Gradients */
    DMOD_TEST_EXPECT_EQ(rd32(h + 92), 10u);                  /* Stops */

    /* sky: linear at 90 degrees, its first stop from a constant */
    const uint8_t *g = table(80, 0, sizeof(dmv_gradient_t));
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(g)), "sky"), 0);
    DMOD_TEST_EXPECT_TRUE(g[2] == DMV_GRADIENT_LINEAR && g[3] == 2 && rd16(g + 4) == 0 && rd16(g + 6) == 90);
    DMOD_TEST_EXPECT_EQ(rd32(table(88, 0, sizeof(dmv_stop_t))), 0xFF3D85F5u);
    DMOD_TEST_EXPECT_EQ(rd16(table(88, 1, sizeof(dmv_stop_t)) + 4), 1000u);

    /* down: the default angle (180, down), positions spread evenly */
    g = table(80, 1, sizeof(dmv_gradient_t));
    DMOD_TEST_EXPECT_TRUE(g[3] == 4 && rd16(g + 4) == 2 && rd16(g + 6) == 180);
    DMOD_TEST_EXPECT_EQ(rd16(table(88, 3, sizeof(dmv_stop_t)) + 4), 333u);
    DMOD_TEST_EXPECT_EQ(rd16(table(88, 4, sizeof(dmv_stop_t)) + 4), 666u);
    DMOD_TEST_EXPECT_EQ(rd16(table(88, 5, sizeof(dmv_stop_t)) + 4), 1000u);

    /* glow: radial with its parameters and a decimal position */
    g = table(80, 2, sizeof(dmv_gradient_t));
    DMOD_TEST_EXPECT_TRUE(g[2] == DMV_GRADIENT_RADIAL && rd16(g + 6) == 25 && rd16(g + 8) == 75 &&
                          rd16(g + 10) == 50 && rd16(g + 12) == 100);
    DMOD_TEST_EXPECT_EQ(rd16(table(88, 7, sizeof(dmv_stop_t)) + 4), 333u);

    /* dot: the inscribed ellipse */
    g = table(80, 3, sizeof(dmv_gradient_t));
    DMOD_TEST_EXPECT_TRUE(rd16(g + 6) == 50 && rd16(g + 8) == 50 && rd16(g + 10) == 50 && rd16(g + 12) == 50);

    /* In place of a color: the paint flag and the gradient's index */
    const uint8_t *insn = code();
    DMOD_TEST_EXPECT_TRUE(insn[0] == DMV_OP_RRECT && insn[2] == 0 && insn[3] == DMV_PAINT_GRADIENT);
    DMOD_TEST_EXPECT_EQ(rd32(insn + 16), 0u);
    insn += insn[1];
    DMOD_TEST_EXPECT_TRUE(insn[0] == DMV_OP_TEXT && insn[3] == (DMV_PAINT_GRADIENT | DMV_ALIGN_CENTER | DMV_ALIGN_MIDDLE));
    DMOD_TEST_EXPECT_EQ(rd32(insn + 16), 2u);
    insn += insn[1];
    DMOD_TEST_EXPECT_TRUE(insn[0] == DMV_OP_FILL && insn[3] == 0);

    libtodmv_input_t in;
    in.read = memory_read;
    in.ctx = &g_out;
    in.size = g_out.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&in, NULL, NULL), 0);

    /* Errors */
    assemble(VIEW ".gradient g, CONIC, #000000, #FFFFFF\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 14, "unknown gradient kind"));
    assemble(VIEW ".gradient g, LINEAR, #000000\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 1, "2 to 16 color stops"));
    assemble(VIEW ".gradient g, LINEAR, #000000 60, #FFFFFF 40\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 34, "must not decrease"));
    assemble(VIEW ".gradient g, LINEAR, #000000 101, #FFFFFF\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 30, "stop position"));
    assemble(VIEW ".gradient g, RADIAL, 50, 50, #000000, #FFFFFF\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 22, "RADIAL takes cx, cy, rx, ry"));
    assemble(VIEW ".define g, 1\n.gradient g, LINEAR, #000000, #FFFFFF\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 11, "declared twice"));
    assemble(VIEW ".gradient g, LINEAR, #000000, #FFFFFF\nmain:\n RECT 0, 0, g, 8, #000000\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(5, 13, "expected a number"));
}

DMOD_TEST_STEP(libtodmv_encodes_opacity)
{
    static const uint8_t opacity[8] = { DMV_OP_OPACITY, 8, 0, 0,  128, 0,  0, 0 };
    DMOD_TEST_EXPECT_EQ(assemble(VIEW ".var $a, int, 255\n"
                                 "main:\n BOX @b, 0, 0, 10, 10\n OPACITY 128\n END\n"
                                 " BOX @c, 0, 0, 10, 10\n SCROLL 10, 20\n FOCUS 1\n OPACITY $a\n END\n RET\n"), 0);
    DMOD_TEST_EXPECT_EQ(rd16(g_out.data + 6), 3);           /* OPACITY: version 0.3 */
    const uint8_t *insn = code();
    insn += insn[1];                                        /* BOX @b */
    DMOD_TEST_EXPECT_TRUE(bytes_equal(insn, opacity, sizeof(opacity)));
    do
        insn += insn[1];                                    /* To the OPACITY of @c, after SCROLL and FOCUS */
    while (insn[0] != DMV_OP_OPACITY && insn < code() + code_words() * 4U);
    DMOD_TEST_EXPECT_TRUE(insn[0] == DMV_OP_OPACITY && insn[2] == 0x01 && insn[-8] == DMV_OP_FOCUS);

    libtodmv_input_t in;
    in.read = memory_read;
    in.ctx = &g_out;
    in.size = g_out.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&in, NULL, NULL), 0);

    assemble(VIEW "main:\n BOX @b, 0, 0, 10, 10\n FILL #000000\n OPACITY 10\n END\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 2, "OPACITY must directly follow BOX, SCROLL or FOCUS"));
    assemble(VIEW "main:\n OPACITY 10\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 2, "OPACITY must directly follow"));
}

DMOD_TEST_STEP(libtodmv_encodes_icons)
{
    /* x, y, w, h, path, paint - like TEXT, a gradient in place of the color */
    static const uint8_t icon[20] = { DMV_OP_ICON, 20, 0, DMV_ALIGN_CENTER | DMV_ALIGN_MIDDLE,
                                      1, 0,  2, 0,  16, 0,  16, 0,  1, 0,  0, 0,  0x00, 0x00, 0xFF, 0xFF };
    DMOD_TEST_EXPECT_EQ(assemble(VIEW ".gradient g, LINEAR, 90, #000000, #FFFFFF\n"
                                 "main:\n ICON 1, 2, 16, 16, \"wifi.dmvi\", #FFFF0000, CENTER|MIDDLE\n"
                                 " ICON 0, 0, 8, 8, \"wifi.dmvi\", g, LEFT\n"
                                 " IMAGE 0, 0, 8, 8, \"photo.dmvi\", LEFT\n RET\n"), 0);
    DMOD_TEST_EXPECT_EQ(rd16(g_out.data + 6), 4);           /* ICON: version 0.4 */
    const uint8_t *insn = code();
    DMOD_TEST_EXPECT_TRUE(bytes_equal(insn, icon, 12));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(insn + 14, icon + 14, 6));
    insn += insn[1];
    DMOD_TEST_EXPECT_TRUE(insn[0] == DMV_OP_ICON && insn[3] == DMV_PAINT_GRADIENT && rd32(insn + 16) == 0);

    libtodmv_input_t in;
    in.read = memory_read;
    in.ctx = &g_out;
    in.size = g_out.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&in, NULL, NULL), 0);

    DMOD_TEST_EXPECT_TRUE(assemble(VIEW "main:\n ICON 0, 0, 8, 8, \"a.dmvi\", #FFFFFF\n RET\n") != 0);   /* no alignment */
}

DMOD_TEST_STEP(libtodmv_variables_set_the_varmask)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        ".var $w, int, 7\n"
        ".var $label, str[8], \"hi\", env:LABEL\n"
        "main:\n"
        " RECT 0, $box.h, $w, 8, #FF000000\n"
        " SET $label, \"ok\"\n"
        " SET $w, 0x10\n"
        " RET\n"), 0);

    const uint8_t *rect = code();
    DMOD_TEST_EXPECT_EQ(rect[2], 0x06);                         /* operands 1 and 2 */
    DMOD_TEST_EXPECT_EQ(rd16(rect + 6), DMV_VAR_BOX_H);
    DMOD_TEST_EXPECT_EQ(rd16(rect + 8), 0);                     /* $w */

    const uint8_t *set_str = rect + 16;
    DMOD_TEST_EXPECT_EQ(set_str[0], DMV_OP_SET);
    DMOD_TEST_EXPECT_EQ(set_str[2], 0);
    DMOD_TEST_EXPECT_EQ(rd16(set_str + 4), 1);                  /* $label */
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd32(set_str + 8)), "ok"), 0);
    DMOD_TEST_EXPECT_EQ(rd32(set_str + 12 + 8), 0x10u);         /* SET $w, 0x10 */

    const uint8_t *w = table(40, 0, 12), *label = table(40, 1, 12);
    DMOD_TEST_EXPECT_EQ(w[0], DMV_VAR_INT);
    DMOD_TEST_EXPECT_EQ(rd32(w + 8), 7u);
    DMOD_TEST_EXPECT_EQ(rd16(w + 6), DMV_NONE);
    DMOD_TEST_EXPECT_EQ(label[0], DMV_VAR_STR);
    DMOD_TEST_EXPECT_EQ(label[1], DMV_VARF_ENV);
    DMOD_TEST_EXPECT_EQ(rd16(label + 2), 8);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(label + 4)), "label"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(label + 6)), "LABEL"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd32(label + 8)), "hi"), 0);
}

DMOD_TEST_STEP(libtodmv_resolves_labels)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        ".var $v, int, 0\n"
        "main:\n"
        " JEQ $v, 0, .skip\n"       /* word 0, 16 bytes: forward local label */
        " CALL helper\n"            /* word 4, 8 bytes */
        ".skip:\n"
        " RET\n"                    /* word 6 */
        "helper:\n"
        " JMP .skip\n"              /* word 7, 8 bytes: helper's own .skip */
        ".skip:\n"
        " RET\n"                    /* word 9 */
        " JMP main\n"), 0);         /* word 10: backward, resolved at once */
    DMOD_TEST_EXPECT_EQ(rd16(code() + 12), 6);
    DMOD_TEST_EXPECT_EQ(rd16(code() + 16 + 4), 7);
    DMOD_TEST_EXPECT_EQ(rd16(code() + 28 + 4), 9);
    DMOD_TEST_EXPECT_EQ(rd16(code() + 40 + 4), 0);
}

DMOD_TEST_STEP(libtodmv_builds_the_box_table)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        "main:\n"
        " BOX @outer, 0, 0, 100, 100, OPAQUE\n"     /* word 0, 16 bytes */
        " SCROLL 100, 400, VERTICAL|BAR\n"          /* word 4 */
        " BOX @inner, 10, 10, 20, 20, MULTI\n"      /* word 6 */
        " ON CLICK, tap\n"                          /* word 10 */
        " END\n"                                    /* word 12 */
        " END\n"                                    /* word 13 */
        " RET\n"
        "tap:\n"
        " REDRAW @outer\n"
        " REDRAW\n"
        " RET\n"), 0);

    DMOD_TEST_EXPECT_EQ(rd32(g_out.data + 60), 2u);
    const uint8_t *outer = table(56, 0, 8), *inner = table(56, 1, 8);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(outer)), "outer"), 0);
    DMOD_TEST_EXPECT_EQ(rd16(outer + 2), DMV_NONE);
    DMOD_TEST_EXPECT_EQ(rd16(outer + 4), 0);
    DMOD_TEST_EXPECT_EQ(rd16(outer + 6), 13);
    DMOD_TEST_EXPECT_EQ(rd16(inner + 2), 0);
    DMOD_TEST_EXPECT_EQ(rd16(inner + 4), 6);
    DMOD_TEST_EXPECT_EQ(rd16(inner + 6), 12);

    DMOD_TEST_EXPECT_EQ(code()[3], DMV_BOX_OPAQUE);
    DMOD_TEST_EXPECT_EQ(code()[16 + 3], DMV_SCROLL_VERTICAL | DMV_SCROLL_BAR);
    DMOD_TEST_EXPECT_EQ(code()[24 + 3], DMV_BOX_MULTI);
    DMOD_TEST_EXPECT_EQ(rd16(code() + 40 + 4), DMV_EVENT_CLICK);

    /* REDRAW @outer, REDRAW (current box) */
    const uint8_t *redraw = code() + 14 * 4 + 4;
    DMOD_TEST_EXPECT_EQ(redraw[0], DMV_OP_REDRAW);
    DMOD_TEST_EXPECT_EQ(rd16(redraw + 4), 0);
    DMOD_TEST_EXPECT_EQ(rd16(redraw + 8 + 4), DMV_NONE);
}

DMOD_TEST_STEP(libtodmv_view_items)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        ".init start\n"
        ".timer 1000, tick\n"
        ".key 2, key\n"
        ".navkeys NEXT, 0, OK, 1\n"
        ".longpress 800\n"
        "main:\n start:\n tick:\n key:\n RET\n"), 0);
    DMOD_TEST_EXPECT_EQ(rd32(g_out.data + 68), 5u);
    DMOD_TEST_EXPECT_EQ(table(64, 1, 8)[0], DMV_ITEM_TIMER);
    DMOD_TEST_EXPECT_EQ(rd32(table(64, 1, 8) + 4), 1000u);
    DMOD_TEST_EXPECT_EQ(table(64, 4, 8)[0], DMV_ITEM_NAVKEY);
    DMOD_TEST_EXPECT_EQ(table(64, 4, 8)[1], DMV_NAV_OK);
    DMOD_TEST_EXPECT_EQ(rd16(table(64, 4, 8) + 2), DMV_NONE);
    DMOD_TEST_EXPECT_EQ(rd16(g_out.data + 20), 800);
}

DMOD_TEST_STEP(libtodmv_text_and_strings)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW
        ".font body, \"sans-16\"\n"
        ".define WHITE, #FFFFFF\n"
        "main:\n"
        " TEXT 0, 0, 100, 20, \"a; b, \\\"c\\\"\\n\", body, WHITE, CENTER|MIDDLE|WRAP ; comment\r\n"
        " RET"), 0);                                /* last line without a line end */
    const uint8_t *text = code();
    DMOD_TEST_EXPECT_EQ(text[0], DMV_OP_TEXT);
    DMOD_TEST_EXPECT_EQ(text[3], DMV_ALIGN_CENTER | DMV_ALIGN_MIDDLE | DMV_ALIGN_WRAP);
    DMOD_TEST_EXPECT_EQ(strcmp(string(rd16(text + 12)), "a; b, \"c\"\n"), 0);
    DMOD_TEST_EXPECT_EQ(rd16(text + 14), 0);                    /* font body */
    DMOD_TEST_EXPECT_EQ(rd32(text + 16), 0xFFFFFFFFu);
    DMOD_TEST_EXPECT_EQ(code_words(), 6u);
}

DMOD_TEST_STEP(libtodmv_line_length)
{
    char source[600];
    const char *head = VIEW "main:\n";
    size_t n = strlen(head);
    memcpy(source, head, n);

    /* A line of exactly LIBTODMV_LINE_MAX - 2 characters is fine ... */
    source[n++] = ' ';
    memcpy(source + n, "RET", 3);
    n += 3;
    while (n - strlen(head) < LIBTODMV_LINE_MAX - 2U)
        source[n++] = ' ';
    source[n++] = '\n';
    source[n] = '\0';
    DMOD_TEST_EXPECT_EQ(assemble(source), 0);

    /* ... one more is an error, and the rest of the line is skipped */
    n--;
    source[n++] = 'x';
    memcpy(source + n, "y\n RET\n", 8);
    DMOD_TEST_EXPECT_EQ(assemble(source), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(g_error_count, 1u);
    DMOD_TEST_EXPECT_TRUE(first_error(4, LIBTODMV_LINE_MAX - 1U, "line longer than"));
}

/* ---- Errors ---- */

DMOD_TEST_STEP(libtodmv_reports_errors_with_position)
{
    DMOD_TEST_EXPECT_EQ(assemble(VIEW "main:\n  BLAH 1\n RET\n"), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(g_result.size, 0u);
    DMOD_TEST_EXPECT_TRUE(first_error(4, 3, "unknown instruction 'BLAH'"));
    DMOD_TEST_EXPECT_EQ(strcmp(g_errors[0].file, "main.dmvs"), 0);

    assemble(VIEW "main:\n RECT 0, 0, $nope, 8, #000000\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 13, "undeclared variable '$nope'"));

    assemble(VIEW "main:\n JMP nowhere\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 6, "undefined label 'nowhere'"));

    assemble(VIEW "main:\n RECT 0, 0, 8\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 2, "RECT takes 5 operands"));

    assemble(VIEW "main:\n RECT 0, 40000, 8, 8, #000000\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 10, "out of range"));

    assemble(VIEW "main:\n RECT 0, 0, 8, 8, #12345\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 19, "invalid color"));

    assemble(VIEW "main:\n TEXT 0, 0, 8, 8, \"x\", nofont, #000000, LEFT\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 24, "unknown font"));

    assemble(VIEW "main:\n FILL #000000\n RET\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 1, "defined twice"));

    assemble(VIEW "main:\n SET $box.w, 1\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 6, "read-only"));

    assemble(".entry main\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(0, 0, "missing .view"));

    assemble(".view v\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(0, 0, "missing .entry"));

    assemble(VIEW "main:\n TEXT 0, 0, 8, 8, \"x, 0, #0, LEFT\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 19, "unterminated string"));
}

DMOD_TEST_STEP(libtodmv_checks_structure)
{
    assemble(VIEW "main:\n BOX @a, 0, 0, 8, 8\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 2, "BOX @a is not closed"));

    assemble(VIEW "main:\n END\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 2, "END without BOX"));

    assemble(VIEW "main:\n ON CLICK, main\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 2, "ON outside a box"));

    assemble(VIEW "main:\n BOX @a, 0, 0, 8, 8\n FILL #000000\n SCROLL 8, 80\n END\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 2, "SCROLL must directly follow BOX"));

    assemble(VIEW "main:\n BOX @a, 0, 0, 8, 8\n END\n BOX @a, 0, 0, 8, 8\n END\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 2, "box @a defined twice"));

    assemble(VIEW "main:\n REDRAW @ghost\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(4, 9, "undefined box @ghost"));
}

DMOD_TEST_STEP(libtodmv_checks_types)
{
    assemble(VIEW ".var $s, str[4], \"\"\nmain:\n ADD $s, 1\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(5, 6, "must be an integer variable"));

    assemble(VIEW ".var $n, int, 0\n.var $s, str[4], \"\"\nmain:\n FORMAT $n, \"%d\", 1\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 9, "must be a string variable"));

    assemble(VIEW ".var $s, str[8], \"\"\nmain:\n FORMAT $s, \"%d %d\", 1\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(5, 13, "exactly one"));

    /* A width, padded with spaces or zeros */
    DMOD_TEST_EXPECT_EQ(assemble(VIEW ".var $s, str[8], \"\"\nmain:\n FORMAT $s, \"%02d:%%02d\", 1\n FORMAT $s, \"%4x\", 1\n RET\n"), 0);
    DMOD_TEST_EXPECT_EQ(g_error_count, 0u);
    assemble(VIEW ".var $s, str[8], \"\"\nmain:\n FORMAT $s, \"%123d\", 1\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(5, 13, "exactly one"));

    assemble(VIEW ".var $s, str[2], \"long\"\nmain:\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(3, 18, "longer than 2 bytes"));

    assemble(VIEW ".font f, \"x\"\n.var $n, int, 0\nmain:\n TEXT 0, 0, 8, 8, $n, f, #0, LEFT\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(6, 19, "not a string variable"));

    assemble(VIEW ".font f, \"x\"\nmain:\n TEXT 0, 0, 8, 8, \"x\", f, #000000, LEFT|RIGHT\n RET\n");
    DMOD_TEST_EXPECT_TRUE(first_error(5, 41, "conflicting alignment"));
}

DMOD_TEST_STEP(libtodmv_stops_after_max_errors)
{
    libtodmv_options_t options = { 0 };

    DMOD_TEST_EXPECT_EQ(assemble(VIEW "main:\n BAD1\n BAD2\n BAD3\n RET\n"), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(g_result.error_count, 3u);
    DMOD_TEST_EXPECT_EQ(g_error_count, 3u);
    DMOD_TEST_EXPECT_FALSE(g_result.truncated);

    options.max_errors = 2;
    DMOD_TEST_EXPECT_EQ(assemble_with(VIEW "main:\n BAD1\n BAD2\n BAD3\n RET\n", &options), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(g_result.error_count, 2u);
    DMOD_TEST_EXPECT_TRUE(g_result.truncated);

    libtodmv_sink_t no_seek;

    no_seek.write = memory_write;

    no_seek.seek = NULL;

    no_seek.ctx = &g_out;
    text_source_t text;
    text.text = "x";
    text.size = 1;
    text.pos = 0;
    libtodmv_source_t src;
    src.read_line = text_read_line;
    src.ctx = &text;
    src.name = NULL;
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble(&src, &no_seek, NULL, &g_result), -EINVAL);
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble(NULL, &no_seek, NULL, &g_result), -EINVAL);
}

/* ---- Files: .include, *_file ---- */

static bool write_text_file(const char *path, const char *text)
{
    void *f = Dmod_FileOpen(path, "w");
    if (f == NULL)
        return false;
    bool ok = Dmod_FileWrite(text, 1, strlen(text), f) == strlen(text);
    Dmod_FileClose(f);
    return ok;
}

#define TEST_FILE(name)     LIBTODMV_TEST_DIR "/" name

DMOD_TEST_STEP(libtodmv_includes_files)
{
    libtodmv_options_t options = { 0 };
    options.include_dir = LIBTODMV_TEST_DIR;

    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("widgets.dmvs"),
                                          "button_bg:\n RRECT 0, 0, $box.w, $box.h, 8, #3D85F5\n RET\n"));
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("broken.dmvs"), "\n BROKEN\n"));

    const char *ok = VIEW "main:\n BOX @b, 0, 0, 8, 8\n CALL button_bg\n END\n RET\n.include \"widgets.dmvs\"\n";
    DMOD_TEST_EXPECT_EQ(assemble_with(ok, &options), 0);

    DMOD_TEST_EXPECT_EQ(assemble_with(VIEW "main:\n RET\n.include \"broken.dmvs\"\n RECT\n", &options), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(g_error_count, 2u);
    DMOD_TEST_EXPECT_EQ(strcmp(g_errors[0].file, TEST_FILE("broken.dmvs")), 0);
    DMOD_TEST_EXPECT_EQ(g_errors[0].line, 2u);
    DMOD_TEST_EXPECT_EQ(strcmp(g_errors[1].file, "main.dmvs"), 0);
    DMOD_TEST_EXPECT_EQ(g_errors[1].line, 6u);

    assemble_with(VIEW "main:\n RET\n.include \"nope.dmvs\"\n", &options);
    DMOD_TEST_EXPECT_TRUE(first_error(5, 10, "cannot include 'nope.dmvs'"));
}

DMOD_TEST_STEP(libtodmv_works_file_to_file)
{
    libtodmv_options_t options = { 0 };
    const char *reason = NULL;

    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("view.dmvs"), VIEW "main:\n FILL #102030\n RET\n"));
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("view.dmvs"), TEST_FILE("view.dmv"), &options, &g_result), 0);
    DMOD_TEST_EXPECT_EQ(libtodmv_validate_file(TEST_FILE("view.dmv"), NULL, &reason), 0);
    DMOD_TEST_EXPECT_EQ(libtodmv_disassemble_file(TEST_FILE("view.dmv"), TEST_FILE("view-again.dmvs")), 0);
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("view-again.dmvs"), TEST_FILE("view-again.dmv"), &options, &g_result), 0);

    /* A failed assembly leaves no output behind */
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("bad.dmvs"), VIEW "main:\n NOPE\n"));
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("bad.dmvs"), TEST_FILE("bad.dmv"), &options, &g_result), -EBADMSG);
    DMOD_TEST_EXPECT_FALSE(Dmod_FileAvailable(TEST_FILE("bad.dmv")));

    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("missing.dmvs"), TEST_FILE("x.dmv"), &options, &g_result), -ENOENT);
    DMOD_TEST_EXPECT_EQ(libtodmv_validate_file(TEST_FILE("bad.dmvs"), NULL, &reason), -EBADMSG);
    DMOD_TEST_EXPECT_NOT_NULL(reason);
}

/* ---- The example of dmview's docs/assembly.md, and round trips ---- */

static const char g_demo[] =
    ".view   demo\n"
    ".size   480, 272\n"
    ".entry  draw\n"
    "\n"
    ".font   title, \"sans-24\"\n"
    ".font   body,  \"sans-16\"\n"
    "\n"
    ".define BLUE,      #3D85F5\n"
    ".define BLUE_DOWN, #2A6FDB\n"
    "\n"
    ".var    $count, int, 0\n"
    ".var    $level, int, 50\n"
    ".var    $ip,    str[16], \"-\", env:ETH0_IP\n"
    ".var    $label, str[24], \"\"\n"
    ".var    $fill,  int, 0\n"
    "\n"
    "draw:\n"
    "        FILL    #101820\n"
    "        TEXT    16, 12, 448, 32, \"dmview demo\", title, #FFFFFF, LEFT|MIDDLE\n"
    "\n"
    "        BOX     @counter, 16, 60, 160, 48\n"
    "        ON      CLICK, increment\n"
    "        CALL    button_bg\n"
    "        FORMAT  $label, \"Count: %d\", $count\n"
    "        TEXT    0, 0, $box.w, $box.h, $label, body, #FFFFFF, CENTER|MIDDLE\n"
    "        END\n"
    "\n"
    "        BOX     @slider, 16, 130, 300, 32\n"
    "        ON      PRESS, slide\n"
    "        ON      DRAG,  slide\n"
    "        RRECT   0, 12, $box.w, 8, 4, #303A48\n"
    "        SET     $fill, $level           ; temporary: fill = level * width / 100\n"
    "        MUL     $fill, $box.w\n"
    "        DIV     $fill, 100\n"
    "        RRECT   0, 12, $fill, 8, 4, BLUE\n"
    "        END\n"
    "\n"
    "        BOX     @ip, 16, 190, 448, 32, OPAQUE\n"
    "        FILL    #101820\n"
    "        TEXT    0, 0, $box.w, $box.h, $ip, body, #A0A8B0, LEFT|MIDDLE\n"
    "        END\n"
    "        RET\n"
    "\n"
    "; Background of a button-like box, darker while pressed\n"
    "button_bg:\n"
    "        JNE     $box.pressed, 0, .down\n"
    "        RRECT   0, 0, $box.w, $box.h, 8, BLUE\n"
    "        RET\n"
    ".down:\n"
    "        RRECT   0, 0, $box.w, $box.h, 8, BLUE_DOWN\n"
    "        RET\n"
    "\n"
    "increment:\n"
    "        ADD     $count, 1\n"
    "        RET\n"
    "\n"
    "slide:\n"
    "        SET     $level, $ev.x\n"
    "        MUL     $level, 100\n"
    "        DIV     $level, $box.w\n"
    "        CLAMP   $level, 0, 100\n"
    "        RET\n";

DMOD_TEST_STEP(libtodmv_assembles_the_documented_example)
{
    DMOD_TEST_EXPECT_EQ(assemble(g_demo), 0);
    DMOD_TEST_EXPECT_EQ(g_error_count, 0u);
    if (g_error_count != 0)
        Dmod_Printf("    %u:%u: %s\n", (unsigned)g_errors[0].line, (unsigned)g_errors[0].column, g_errors[0].message);
    DMOD_TEST_EXPECT_EQ(rd32(g_out.data + 60), 3u);      /* @counter, @slider, @ip */
}

static memory_t g_first, g_text_first;

/* assemble -> disassemble -> assemble gives an equivalent view: its
 * disassembly is the same text. The disassembly is canonical - assembling
 * it gives the same bytes every time. (The first binary may differ from
 * the second one only in the order of its strings, which follows the order
 * of the source.) */
static void check_round_trip(const char *source)
{
    DMOD_TEST_EXPECT_EQ(assemble(source), 0);
    DMOD_TEST_EXPECT_EQ(disassemble(&g_out), 0);
    g_text_first = g_text;

    DMOD_TEST_EXPECT_EQ(assemble((const char *)g_text_first.data), 0);
    if (g_error_count != 0)
        Dmod_Printf("    reassembly: %u:%u: %s\n", (unsigned)g_errors[0].line, (unsigned)g_errors[0].column, g_errors[0].message);
    g_first = g_out;
    DMOD_TEST_EXPECT_EQ(disassemble(&g_first), 0);
    DMOD_TEST_EXPECT_EQ(strcmp((const char *)g_text.data, (const char *)g_text_first.data), 0);

    DMOD_TEST_EXPECT_EQ(assemble((const char *)g_text.data), 0);
    DMOD_TEST_EXPECT_TRUE(g_out.size == g_first.size && bytes_equal(g_out.data, g_first.data, g_out.size));
}

DMOD_TEST_STEP(libtodmv_round_trips)
{
    check_round_trip(g_demo);
    check_round_trip(VIEW
        ".var $s, str[8], \"tab\\there\"\n"
        ".var $n, int, -5, env:N\n"
        ".font f, \"mono-8\"\n"
        ".gradient shade, LINEAR, 45, #FF000000, #40FFFFFF 12.5, #FF102030\n"
        ".gradient spot, RADIAL, -10, 50, 200, 30, #FFFFFFFF, #00000000\n"
        ".timer 250, main\n"
        ".navkeys UP, 3\n"
        ".scrollslop 12\n"
        "main:\n"
        " BOX @list, 0, 0, 100, 200, OPAQUE|MULTI\n"
        " SCROLL 100, 900, VERTICAL|BAR\n"
        " FOCUS 2\n"
        " OPACITY $n\n"
        " ON SCROLLED, main\n"
        " IMAGE 0, 0, 64, 64, \"/flash/a.dmvi\", RIGHT|BOTTOM\n"
        " TEXT 0, 0, 10, 10, $s, f, #80FF0000, LEFT\n"
        " TEXT 0, 0, 10, 10, $s, f, shade, RIGHT|WRAP\n"
        " CIRCLE 5, 5, 4, spot\n"
        " END\n"
        " SET $s, \"x\"\n"
        " JGE $n, POINTER_CONTACT, main\n"
        " SCROLLTO @list, 0, $n\n"
        " SETFOCUS @list\n"
        " EXEC \"ifconfig eth0 up\"\n"
        " SIGNAL \"ui\"\n"
        " GOTO \"/flash/next.dmv\"\n"
        " RELOAD \"/flash/a.dmvi\"\n"
        " TOGGLE $n\n"
        " REDRAW\n"
        " RET\n");
}

DMOD_TEST_STEP(libtodmv_rejects_invalid_views)
{
    static memory_t junk;
    const char *reason = NULL;
    uint32_t offset = 0;

    reset(&junk);
    junk.size = 96;
    junk.data[0] = 'D'; junk.data[1] = 'M'; junk.data[2] = 'V'; junk.data[3] = 0;
    DMOD_TEST_EXPECT_EQ(disassemble(&junk), -EBADMSG);

    libtodmv_input_t in;

    in.read = memory_read;

    in.ctx = &junk;

    in.size = junk.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&in, &offset, &reason), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(strcmp(reason, "invalid header"), 0);

    /* A corrupted assembled view */
    DMOD_TEST_EXPECT_EQ(assemble(VIEW "main:\n RET\n"), 0);
    g_out.data[rd32(g_out.data + 24)] = 0x30;           /* unknown opcode */
    libtodmv_input_t view;
    view.read = memory_read;
    view.ctx = &g_out;
    view.size = g_out.size;
    DMOD_TEST_EXPECT_EQ(libtodmv_validate(&view, &offset, &reason), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(offset, DMV_HEADER_SIZE);
    DMOD_TEST_EXPECT_EQ(strcmp(reason, "unknown opcode"), 0);
}

/* ---- Outputs are replaced atomically ---- */

/* No temporary file (*.tmp) is left in the test directory. */
static bool no_temp_files(void)
{
    void *dir = Dmod_OpenDir(LIBTODMV_TEST_DIR);
    bool clean = true;
    if (dir == NULL)
        return false;
    for (const char *name = Dmod_ReadDir(dir); name != NULL; name = Dmod_ReadDir(dir))
    {
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".tmp") == 0)
        {
            Dmod_Printf("    left behind: %s\n", name);
            clean = false;
        }
    }
    Dmod_CloseDir(dir);
    return clean;
}

static uint32_t file_size(const char *path)
{
    size_t size = 0;
    void *f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return 0;
    if (!Dmod_FileSizeToSizeT(Dmod_FileSize(f), &size))
        size = 0;
    Dmod_FileClose(f);
    return (uint32_t)size;
}

DMOD_TEST_STEP(libtodmv_failure_keeps_the_previous_output)
{
    libtodmv_options_t options = { 0 };

    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("keep.dmvs"), VIEW "main:\n FILL #102030\n RET\n"));
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("keep-bad.dmvs"), VIEW "main:\n FILL #102030\n NOPE\n RET\n"));
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("keep.dmvs"), TEST_FILE("keep.dmv"), &options, &g_result), 0);
    uint32_t size = file_size(TEST_FILE("keep.dmv"));
    DMOD_TEST_EXPECT_NE(size, 0u);

    /* A failed conversion into the same output leaves it as it was */
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("keep-bad.dmvs"), TEST_FILE("keep.dmv"), &options, &g_result), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(file_size(TEST_FILE("keep.dmv")), size);
    DMOD_TEST_EXPECT_EQ(libtodmv_validate_file(TEST_FILE("keep.dmv"), NULL, NULL), 0);

    /* Disassembly too: an invalid view does not destroy an existing text */
    DMOD_TEST_EXPECT_EQ(libtodmv_disassemble_file(TEST_FILE("keep.dmv"), TEST_FILE("keep-text.dmvs")), 0);
    uint32_t text_size = file_size(TEST_FILE("keep-text.dmvs"));
    DMOD_TEST_EXPECT_EQ(libtodmv_disassemble_file(TEST_FILE("keep-bad.dmvs"), TEST_FILE("keep-text.dmvs")), -EBADMSG);
    DMOD_TEST_EXPECT_EQ(file_size(TEST_FILE("keep-text.dmvs")), text_size);

    /* Replacing works as well */
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("keep.dmvs"), VIEW "main:\n FILL #102030\n FILL #000000\n RET\n"));
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("keep.dmvs"), TEST_FILE("keep.dmv"), &options, &g_result), 0);
    DMOD_TEST_EXPECT_EQ(file_size(TEST_FILE("keep.dmv")), size + 8u);
    DMOD_TEST_EXPECT_TRUE(no_temp_files());
}

/* ---- Several conversions at once ----
 *
 * Every worker assembles the documented example in memory (the result must
 * equal the reference built before the threads start) and one of three
 * sources - two valid views of different sizes and a broken one - into one
 * shared file. libtodmv keeps no global state, so the in-memory results are
 * always exact. Meanwhile a reader validates the shared file all the time:
 * it may be missing for a moment (the old one is removed right before the
 * new one gets its name), but whenever it exists it is complete. */

#define WORKERS         4
#define ROUNDS          16
#define SHARED_OUTPUT   TEST_FILE("shared.dmv")

typedef struct
{
    int         index;
    int         failures;
    memory_t    out;
} worker_t;

static worker_t g_workers[WORKERS];
static memory_t g_reference;
static volatile bool g_writing;
static int g_reads, g_bad_reads;

static void reader_entry(void *arg)
{
    (void)arg;
    while (g_writing)
    {
        int ret = libtodmv_validate_file(SHARED_OUTPUT, NULL, NULL);
        g_reads++;
        if (ret != 0 && ret != -ENOENT)
            g_bad_reads++;
    }
}

static const char *worker_source(int index)
{
    switch (index % 3)
    {
        case 0:  return TEST_FILE("shared-a.dmvs");
        case 1:  return TEST_FILE("shared-b.dmvs");
        default: return TEST_FILE("shared-bad.dmvs");
    }
}

static void worker_entry(void *arg)
{
    worker_t *w = (worker_t *)arg;

    for (int round = 0; round < ROUNDS; round++)
    {
        text_source_t text;
        text.text = g_demo;
        text.size = sizeof(g_demo) - 1U;
        text.pos = 0;
        libtodmv_source_t src;
        src.read_line = text_read_line;
        src.ctx = &text;
        src.name = NULL;
        libtodmv_sink_t sink;
        sink.write = memory_write;
        sink.seek = memory_seek;
        sink.ctx = &w->out;
        libtodmv_result_t result;

        reset(&w->out);
        if (libtodmv_assemble(&src, &sink, NULL, &result) != 0 || w->out.size != g_reference.size ||
            !bytes_equal(w->out.data, g_reference.data, g_reference.size))
            w->failures++;

        libtodmv_options_t options = { 0 };
        int expected = ((w->index % 3) == 2) ? -EBADMSG : 0;
        if (libtodmv_assemble_file(worker_source(w->index), SHARED_OUTPUT, &options, &result) != expected)
            w->failures++;
    }
}

DMOD_TEST_STEP(libtodmv_runs_several_conversions_at_once)
{
    libtodmv_options_t options = { 0 };
    dmosi_thread_t threads[WORKERS];

    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("shared-a.dmvs"), g_demo));
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("shared-b.dmvs"), VIEW "main:\n FILL #102030\n FILL #405060\n RET\n"));
    DMOD_TEST_EXPECT_TRUE(write_text_file(TEST_FILE("shared-bad.dmvs"), VIEW "main:\n BROKEN\n"));
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("shared-a.dmvs"), TEST_FILE("shared-a.dmv"), &options, &g_result), 0);
    DMOD_TEST_EXPECT_EQ(libtodmv_assemble_file(TEST_FILE("shared-b.dmvs"), TEST_FILE("shared-b.dmv"), &options, &g_result), 0);
    DMOD_TEST_EXPECT_EQ(assemble(g_demo), 0);
    g_reference = g_out;

    g_writing = true;
    g_reads = g_bad_reads = 0;
    dmosi_thread_t reader = dmosi_thread_create(reader_entry, NULL, 0, 16384 + DMOSI_THREAD_STACK_OVERHEAD,
                                                "todmv-reader", NULL);
    DMOD_TEST_EXPECT_NOT_NULL(reader);
    for (int i = 0; i < WORKERS; i++)
    {
        memset(&g_workers[i], 0, sizeof(g_workers[i]));
        g_workers[i].index = i;
        threads[i] = dmosi_thread_create(worker_entry, &g_workers[i], 0, 16384 + DMOSI_THREAD_STACK_OVERHEAD,
                                         "todmv-worker", NULL);
        DMOD_TEST_EXPECT_NOT_NULL(threads[i]);
    }
    for (int i = 0; i < WORKERS; i++)
    {
        if (threads[i] != NULL)
        {
            dmosi_thread_join(threads[i]);
            dmosi_thread_destroy(threads[i]);
        }
        DMOD_TEST_EXPECT_EQ(g_workers[i].failures, 0);
    }
    g_writing = false;
    if (reader != NULL)
    {
        dmosi_thread_join(reader);
        dmosi_thread_destroy(reader);
    }
    DMOD_TEST_EXPECT_NE(g_reads, 0);
    DMOD_TEST_EXPECT_EQ(g_bad_reads, 0);
    if (g_bad_reads != 0)
        Dmod_Printf("    %d of %d reads saw an incomplete view\n", g_bad_reads, g_reads);

    /* The shared output is one of the two valid views, complete */
    uint32_t size = file_size(SHARED_OUTPUT);
    DMOD_TEST_EXPECT_EQ(libtodmv_validate_file(SHARED_OUTPUT, NULL, NULL), 0);
    DMOD_TEST_EXPECT_TRUE(size == file_size(TEST_FILE("shared-a.dmv")) || size == file_size(TEST_FILE("shared-b.dmv")));
    DMOD_TEST_EXPECT_TRUE(no_temp_files());
}
