# todmv

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/todmv/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/todmv/actions/workflows/ci.yml)

Converts dmview assembly (`.dmvs`) into the binary view format (`.dmv`) that
dmgui executes, and back.

## Description

The formats are defined in [dmview](https://github.com/choco-technologies/dmview):
[assembly.md](https://github.com/choco-technologies/dmview/blob/main/docs/assembly.md) (the `.dmvs` language and instruction set) and
[binary-format.md](https://github.com/choco-technologies/dmview/blob/main/docs/binary-format.md) (the `.dmv` file). This repository
holds only the conversion:

| Module | Kind | What it does |
|--------|------|--------------|
| `libtodmv` | Library | Assembler, disassembler and validator - usable on a PC and on the device (e.g. by `dmhtml`) |
| `todmv` | Application ([apps/todmv](apps/todmv)) | Command-line tool on top of `libtodmv` |

## Building

```bash
cmake -S . -B build -DDMOD_TOOLS_NAME=arch/x86_64
cmake --build build
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub, and e.g.
`-DDMOD_TOOLS_NAME=arch/armv7/cortex-m7` for a target.

## Memory

Nothing is ever loaded as a whole:

- the source is read **line by line** into one buffer of `LIBTODMV_LINE_MAX`
  (256) bytes - a longer line is an error;
- every instruction is **written as soon as its line is read**; references to
  labels further down are patched at the end by seeking back, then the
  tables and the header are appended;
- only the view's tables stay in memory while assembling: the strings (in
  one block), variables, fonts, boxes, labels, the forward references and
  the view-level items - their size depends on the number of names, not on
  the length of the code;
- errors are reported one by one through a callback, not collected;
- validation and disassembly read the view **in small pieces** (an
  instruction, a string in 32-byte chunks); the disassembler keeps only the
  symbol table (4 bytes per label), the validator one bit per code word and
  one byte per variable.

Assembling the documented example takes about 1.6 KB of stack on x86-64.

## todmv

```
todmv [-o OUTPUT] VIEW.dmvs       assemble (default output: VIEW.dmv)
todmv -d [-o OUTPUT] VIEW.dmv     disassemble (default output: the console)
```

Errors are printed as `file:line:column: error: message` as they are found
(assembling stops after 16); the exit code is 1 when there are any, and no
output file is left behind. `.include` paths are relative to the directory of
the assembled file.

On a PC through `dmod_loader`:

```bash
dmod_loader todmv.dmf --args view.dmvs -o view.dmv
dmod_loader todmv.dmf --args -d view.dmv
```

## libtodmv

Files through the dmod VFS:

```c
#include "libtodmv.h"

static void on_error(void *user, const libtodmv_error_t *e)
{
    Dmod_Printf("%s:%u:%u: %s\n", e->file, e->line, e->column, e->message);
}

libtodmv_options_t options = { 0 };
libtodmv_result_t result;
options.on_error = on_error;
int ret = libtodmv_assemble_file("/flash/main.dmvs", "/flash/main.dmv", &options, &result);
```

Or streams, e.g. for `dmhtml` generating assembly on the fly:

- `libtodmv_assemble(source, sink, options, result)` - `source` gives lines
  (the `Dmod_FileReadLine` contract), `sink` takes the binary (write and
  seek);
- `libtodmv_disassemble(input, sink)` - `input` reads at an offset, `sink`
  takes the text (write only);
- `libtodmv_validate(input, ...)` / `libtodmv_validate_file(path, ...)`.

Disassembling and assembling again gives an equivalent view (its strings
may be stored in another order); the disassembled text is canonical -
assembling it always gives the same bytes. `.define` constants and comments
are not part of the binary and do not come back.

## Tests

`tests/` (host, `dmod_loader`) check the exact binary encoding, the
tables, every kind of error with its position, the line length limit,
`.include` and the `*_file` functions on real files, the example of
dmview's `docs/assembly.md` (also in [examples/demo.dmvs](examples/demo.dmvs)) and
round trips. CI additionally runs `todmv` on `examples/demo.dmvs`:
assemble, disassemble, assemble, disassemble - the two texts and the two
binaries assembled from them must be identical.

## Project Structure

```
todmv/
├── include/            # libtodmv API (libtodmv.h, libtodmv_types.h)
├── src/                # libtodmv: assembler, disassembler, validator, format tables
├── tests/              # libtodmv host tests (dmod_loader)
├── apps/todmv/         # todmv command-line tool
├── examples/           # demo.dmvs - the example of dmview's docs
├── docs/               # Documentation (dmf-man)
├── CMakeLists.txt
├── libtodmv.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
