# libtodmv Documentation

`libtodmv` converts dmview assembly (`.dmvs`) into the binary view format
(`.dmv`) and back, reading and writing in small pieces - nothing is ever
held in memory as a whole. `todmv` is the command-line tool on top of it.

## Contents

- **[api-reference.md](api-reference.md)** - functions, streams, options, errors

The formats themselves are described in dmview:
[assembly.md](https://github.com/choco-technologies/dmview/blob/main/docs/assembly.md) and [binary-format.md](https://github.com/choco-technologies/dmview/blob/main/docs/binary-format.md).

## Quick Reference

```c
#include "libtodmv.h"

libtodmv_options_t options = { 0 };
libtodmv_result_t result;
int ret = libtodmv_assemble_file("/flash/main.dmvs", "/flash/main.dmv", &options, &result);
```

View documentation using `dmf-man`:

```bash
dmf-man libtodmv          # Main documentation
dmf-man libtodmv api      # API reference
```
