#include "private.h"
#include <string.h>

/* Plain data only - names are char arrays, not pointers: the dmod loader
 * does not relocate pointers stored in initialized data. */

typedef struct
{
    char     name[NAME_MAX_LEN];
    uint16_t value;
} named_value_t;

typedef struct
{
    char    name[NAME_MAX_LEN];
    uint8_t kind;       /* dmv_flags_kind_t */
    uint8_t value;
} named_flag_t;

/* Indexed by dmv_event_t */
static const char g_events[DMV_EVENT_COUNT][NAME_MAX_LEN] = {
    "PRESS", "DRAG", "LONG", "RELEASE", "CLICK", "PINCH", "ROTATE",
    "ENTER", "LEAVE", "WHEEL", "SCROLLED", "FOCUS", "BLUR", "KEY",
};

static const named_value_t g_builtins[] = {
    { "box.w",        DMV_VAR_BOX_W },
    { "box.h",        DMV_VAR_BOX_H },
    { "box.pressed",  DMV_VAR_BOX_PRESSED },
    { "box.sx",       DMV_VAR_BOX_SX },
    { "box.sy",       DMV_VAR_BOX_SY },
    { "box.contacts", DMV_VAR_BOX_CONTACTS },
    { "box.focused",  DMV_VAR_BOX_FOCUSED },
    { "ev.contact",   DMV_VAR_EV_CONTACT },
    { "ev.x",         DMV_VAR_EV_X },
    { "ev.y",         DMV_VAR_EV_Y },
    { "ev.dx",        DMV_VAR_EV_DX },
    { "ev.dy",        DMV_VAR_EV_DY },
    { "ev.wheel",     DMV_VAR_EV_WHEEL },
    { "ev.button",    DMV_VAR_EV_BUTTON },
    { "ev.scale",     DMV_VAR_EV_SCALE },
    { "ev.angle",     DMV_VAR_EV_ANGLE },
    { "ev.cx",        DMV_VAR_EV_CX },
    { "ev.cy",        DMV_VAR_EV_CY },
    { "ev.key",       DMV_VAR_EV_KEY },
    { "view.w",       DMV_VAR_VIEW_W },
    { "view.h",       DMV_VAR_VIEW_H },
    { "time",         DMV_VAR_TIME },
};

static const named_flag_t g_flags[] = {
    { "OPAQUE",     DMV_FLAGS_BOX,    DMV_BOX_OPAQUE },
    { "MULTI",      DMV_FLAGS_BOX,    DMV_BOX_MULTI },
    { "HORIZONTAL", DMV_FLAGS_SCROLL, DMV_SCROLL_HORIZONTAL },
    { "VERTICAL",   DMV_FLAGS_SCROLL, DMV_SCROLL_VERTICAL },
    { "BAR",        DMV_FLAGS_SCROLL, DMV_SCROLL_BAR },
    { "LEFT",       DMV_FLAGS_ALIGN,  DMV_ALIGN_LEFT },
    { "CENTER",     DMV_FLAGS_ALIGN,  DMV_ALIGN_CENTER },
    { "RIGHT",      DMV_FLAGS_ALIGN,  DMV_ALIGN_RIGHT },
    { "TOP",        DMV_FLAGS_ALIGN,  DMV_ALIGN_TOP },
    { "MIDDLE",     DMV_FLAGS_ALIGN,  DMV_ALIGN_MIDDLE },
    { "BOTTOM",     DMV_FLAGS_ALIGN,  DMV_ALIGN_BOTTOM },
    { "WRAP",       DMV_FLAGS_ALIGN,  DMV_ALIGN_WRAP },
};

/* Indexed by dmv_nav_role_t */
static const char g_nav_roles[DMV_NAV_COUNT][NAME_MAX_LEN] = {
    "NEXT", "PREV", "UP", "DOWN", "LEFT", "RIGHT", "OK",
};

#define COUNT_OF(a)     (sizeof(a) / sizeof((a)[0]))

const char *event_name(uint32_t event)
{
    return (event < DMV_EVENT_COUNT) ? g_events[event] : NULL;
}

int event_find(const char *name, size_t len)
{
    for (int i = 0; i < (int)DMV_EVENT_COUNT; i++)
    {
        if (token_ieq(name, len, g_events[i]))
            return i;
    }
    return -1;
}

const char *builtin_name(uint32_t index)
{
    for (size_t i = 0; i < COUNT_OF(g_builtins); i++)
    {
        if (g_builtins[i].value == index)
            return g_builtins[i].name;
    }
    return NULL;
}

int builtin_find(const char *name, size_t len)
{
    for (size_t i = 0; i < COUNT_OF(g_builtins); i++)
    {
        if (token_eq(name, len, g_builtins[i].name))
            return g_builtins[i].value;
    }
    return -1;
}

int flag_find(uint8_t flags_kind, const char *name, size_t len)
{
    for (size_t i = 0; i < COUNT_OF(g_flags); i++)
    {
        if (g_flags[i].kind == flags_kind && token_ieq(name, len, g_flags[i].name))
            return g_flags[i].value;
    }
    return -1;
}

static const char *flag_name(uint8_t flags_kind, uint8_t value)
{
    for (size_t i = 0; i < COUNT_OF(g_flags); i++)
    {
        if (g_flags[i].kind == flags_kind && g_flags[i].value == value)
            return g_flags[i].name;
    }
    return NULL;
}

void flags_print(writer_t *w, uint8_t flags_kind, uint8_t flags)
{
    if (flags_kind == DMV_FLAGS_ALIGN)
    {
        writer_printf(w, "%s", flag_name(flags_kind, flags & DMV_ALIGN_HMASK));
        if (flags & DMV_ALIGN_VMASK)
            writer_printf(w, "|%s", flag_name(flags_kind, flags & DMV_ALIGN_VMASK));
        if (flags & DMV_ALIGN_WRAP)
            writer_printf(w, "|%s", flag_name(flags_kind, DMV_ALIGN_WRAP));
        return;
    }

    bool first = true;
    for (uint8_t bit = 1; bit != 0; bit = (uint8_t)(bit << 1))
    {
        const char *name = (flags & bit) ? flag_name(flags_kind, bit) : NULL;
        if (name != NULL)
        {
            writer_printf(w, first ? "%s" : "|%s", name);
            first = false;
        }
    }
}

const char *nav_role_name(uint32_t role)
{
    return (role < DMV_NAV_COUNT) ? g_nav_roles[role] : NULL;
}

int nav_role_find(const char *name, size_t len)
{
    for (int i = 0; i < (int)DMV_NAV_COUNT; i++)
    {
        if (token_ieq(name, len, g_nav_roles[i]))
            return i;
    }
    return -1;
}
