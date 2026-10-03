#include "dmod.h"
#include "libtodmv.h"
#include <errno.h>
#include <string.h>

/**
 * @brief todmv - assemble dmview assembly (.dmvs) into a binary view (.dmv),
 *        or disassemble a .dmv back into assembly. Both work file to file,
 *        in small pieces - see libtodmv.
 */

#define PATH_MAX_LEN    256

static void print_usage(const char *name)
{
    Dmod_Printf("Usage: %s [-o OUTPUT] VIEW.dmvs     assemble (default output: VIEW.dmv)\n", name);
    Dmod_Printf("       %s -d [-o OUTPUT] VIEW.dmv  disassemble (default output: the console)\n", name);
}

static void print_error(void *user, const libtodmv_error_t *e)
{
    const char *input = (const char *)user;
    Dmod_Printf("%s:%u:%u: error: %s\n", (e->file != NULL) ? e->file : input,
                (unsigned)e->line, (unsigned)e->column, e->message);
}

/* VIEW.dmvs -> VIEW.dmv */
static void default_output(const char *input, char *output, size_t size)
{
    size_t len = strlen(input);
    if (len > 5 && strcmp(input + len - 5, ".dmvs") == 0)
        len -= 5;
    if (len + sizeof(".dmv") > size)
        len = size - sizeof(".dmv");
    memcpy(output, input, len);
    memcpy(output + len, ".dmv", sizeof(".dmv"));
}

static int assemble(const char *input, const char *output)
{
    char default_path[PATH_MAX_LEN];
    if (output == NULL)
    {
        default_output(input, default_path, sizeof(default_path));
        output = default_path;
    }

    libtodmv_options_t options = { 0 };
    options.on_error = print_error;
    options.user = (void *)input;

    libtodmv_result_t result;
    int ret = libtodmv_assemble_file(input, output, &options, &result);
    switch (ret)
    {
        case 0:        return 0;
        case -EBADMSG: break;
        case -ENOENT:  Dmod_Printf("todmv: cannot read '%s'\n", input); break;
        case -ENOMEM:  Dmod_Printf("todmv: out of memory\n"); break;
        default:       Dmod_Printf("todmv: cannot write '%s'\n", output); break;
    }
    if (result.truncated)
        Dmod_Printf("todmv: too many errors, stopping\n");
    return 1;
}

static int disassemble(const char *input, const char *output)
{
    int ret = libtodmv_disassemble_file(input, output);
    switch (ret)
    {
        case 0:        return 0;
        case -ENOENT:  Dmod_Printf("todmv: cannot read '%s'\n", input); break;
        case -EBADMSG: Dmod_Printf("todmv: '%s' is not a valid .dmv file\n", input); break;
        case -ENOMEM:  Dmod_Printf("todmv: out of memory\n"); break;
        default:       Dmod_Printf("todmv: input/output error\n"); break;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    const char *input = NULL, *output = NULL;
    bool disassembly = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-d") == 0)
            disassembly = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (input == NULL && argv[i][0] != '-')
            input = argv[i];
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (input == NULL)
    {
        print_usage(argv[0]);
        return 1;
    }
    return disassembly ? disassemble(input, output) : assemble(input, output);
}
