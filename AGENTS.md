# Agents working on rin

**To read or write rin code, read [docs/agent-guide.md](docs/agent-guide.md)
first.** It is rin as a list of differences from C, with examples the test
suite compiles, and the mistakes C habits make. Trust it and the compiler's
error messages over the older design docs in `docs/`.

## The repository

| path | what |
|---|---|
| `src/main.c` | the compiler: lexer, parser, checks and C emitter, one file |
| `src/std/` | the standard library, rin sources plus `core.h` |
| `src/rinbind.c` | `rinbind`: rin bindings from a C header, through libclang |
| `src/asm.c` | `rin asm`, the rasm assembler; its material is in `rasm/` |
| `tests/run_tests.py` | the test suite: every check, the torture, fuzz, MSVC and debugger tests |
| `docs/` | design records; `agent-guide.md` is the one to start from |
| `editors/`, `nvim/` | the TextMate and Vim grammars |
| `scripts/` | a git submodule: the LSP (`rin_lsp.py`) and helper scripts |

## Build and test

    cmake --build build
    python tests/run_tests.py

The build writes `build/rin.exe` and copies it, `rinbind.exe` and `src/std/`
into `../rin-windows-x64/`, the package other projects build with. The std
copy is a post-build step of the `rin` target, so it only runs when the
compiler relinks: after changing only `src/std/`, run
`cmake -E copy_directory src/std build/std` (and the same into
`../rin-windows-x64/std`).

`run_tests.py` stops at the first failure and prints why. All of it should
pass before a change is done.

## Changing the language

- A new rule needs a test in `tests/run_tests.py`: what compiles, and what is
  refused with which message.
- Error messages say what to write instead, as the existing ones do.
- A spelling change also touches `docs/agent-guide.md`, the grammars in
  `editors/` and `nvim/`, and `scripts/rin_lsp.py`; the test suite checks the
  guide's examples and the LSP's keyword tables.
