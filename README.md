# The Rin Systems Programming Language

## Synopsis
Rin is C-shaped code with a smaller syntax surface and compile-time metaprogramming. It transpiles to C, then the normal C toolchain takes over.

## How to Run

### Modules
- run `git submodule update --init --recursive`

### Bunyan
- run `python bunyan.py build debug` to build the `rin.exe` compiler
- the compiler build packages `rin.exe`, `rinbind.exe`, and `std\` into `..\rin-windows-x64`
- recommended user environment:
  - `RIN_HOME=C:\devel\rin-windows-x64`
  - put `%RIN_HOME%` on `PATH`
- Bunyan-based rin projects resolve tools through `RIN_HOME` first, then `PATH`
- `rin.exe` automatically resolves imports through the `std` folder beside it
- rin projects should use Bunyan `mode="i"` and keep generated C under their build directory

### Compiler CLI
- `rin.exe compile [input.i] -o [output.c] --header [output.h] --importdir [dir]`
- `rin.exe check [input.i]`
- `rin.exe check [input.i] --diagnostics=json`
- `rin.exe symbols [input.i]`
- `rin.exe lsp [input.i]`
- `rin.exe --help`
- `rin.exe [input.i] [output.c] [output.h]` still works as the legacy compile form
- compile defaults: `src\main.rin` -> `build\rin_gen\main.c`
- the compiler also writes a companion header beside the output C file, for example `build\rin_gen\main.h`
- `check` parses, imports, validates, and type-checks without writing generated C
- `symbols` emits compiler symbol metadata as JSON
- `lsp` emits diagnostics plus compiler symbol metadata as JSON
- `import "std/Print.rin"` resolves through the `std` folder beside `rin.exe`
- `--importdir` adds an extra import search root for project-local libraries
- `import "foo.rin"` emits `#include "foo.h"` and lets the current file type-check against `foo.i` declarations

### Tests
- run `python tests\run_tests.py`
- `tests\run_i_execute.py` is a differential suite: it transpiles each fixture in
  `tests\rin-torture\execute`, links it, runs it, and compares stdout against the
  companion `.expected` file. Re-record expectations with `--bless` after an
  intentional behaviour change.
- `tests\run_i_debuginfo.py` builds a fixture with DWARF and reads the resulting
  line table, checking that generated code maps back to `.i` source and that
  reduced `#line` output is byte-identical to `--emit-all-line-directives` output.
- `tests\run_i_fuzz.py` mutates the corpus and asserts the front end always
  degrades into diagnostics rather than a crash, a hang, or malformed JSON. The
  suite runs a bounded pass; soak it with
  `python tests\run_i_fuzz.py --iterations 6000 --seed 99 --keep-going`.
- `tests\run_tests.py` also runs the `gcc.c-torture/compile` hook. By default it compiles the tiny local smoke fixture in `tests\gcc.c-torture\compile`; set `RIN_GCC_TORTURE` or pass `--suite` to `tests\run_c_torture.py` to point at a full GCC checkout.

### Editor Support
- `python nvim/install.py` installs filetype detection, syntax highlighting, and
  LSP attachment into your Neovim config. See `nvim/README.md` for options.

## Uniform Declaration And Block Rules

Every declaration is `name : kind = value`, and every body is a block:

- variables must be initialized: `= {}` zeroes, `= ?` leaves storage untouched
- switch cases and defaults take a block, giving each case its own scope
- labels take a block: `done: label = { ... }`

Struct fields and proc parameters have nothing to initialize, so the rule does not
reach them.

## Comments And The Preprocessor

Rin keeps these strictly separate:

- `//` line comments and `/* */` block comments are Rin comments. They do not reach the generated C.
- `#` starts a C preprocessor line. It is passed through to the generated C verbatim.

A `#` line that is not a recognized C directive is an error, so a stray `# note to self` is
caught in Rin instead of silently becoming a preprocessor directive in the generated C.

## Language Sketch
```rust
// this is a work in progress syntax for rin
import "stdio.h"
define("SAHA_IMPLEMENTATION")

// variable
num: i32 = 7;

// struct type
vec2: struct = { x: f32; y: f32; }

// enum type
Color: enum = { Red, Green, Blue }

// enum with explicit and negative values
Status: enum = { Failed = -1, Idle = 0, Busy = 1 }

// generic struct type
array: struct<T> = {
    length: u64;
    border: u64;
    data: *T;
}

// proc
make: proc(x: i32) -> i32 = { return x; }

// proc with internal linkage, not exported in the generated header
helper: static proc(x: i32) -> i32 = { return x + 1; }

// semantic-only C declaration, used when an imported C header already declares it
puts: proc(text: *const char) -> i32 = { external; }

// emitted C prototype, used when Rin owns the type surface for a linked C module
fx_step: proc(dt :f32) -> void = { external_emit; }

// generic proc
makeg: proc<T>(x: T) -> T = { return x; }

// requirement implementation for i32
hash: proc<i32>(x: *i32) -> u64 = { return hash_fnv1a(x, sizeof(x[0])); }

// constrained generic proc
makehash: proc<T: hash>(x: T) -> u64 = { return hash<T>(&x); }

// usage
a:array<i32> = {};
y:i32 = makeg<i32>(3);
h:u64 = makehash<i32>(num);
printf("num = {}\n", num);

// bitfields and anonymous struct/union members, for binding real C headers
Header:struct = {
    version: u32:4;
    flags: u32:28;
    union = {
        as_i32:i32;
        as_f32:f32;
    }
}

// volatile, for memory-mapped registers and signal-visible state
Regs: struct = {
    status: volatile u32;
    data: *volatile u32;
}

// type arguments are not limited to plain names
pointers: array<*i32> = {};
strings: array<*const char> = {};

// boring C control flow/operators
Packet: struct = {
    values: [4]i32;
    flags: u32;
}

WinProc: proc[WINCALL](value:i32) -> i32 = {
    letter: char = 'x';
    scratch: [64]u8 = ?;      // deliberately not zeroed
    while (value > 0) {
        value -= 1;
        value %= 8;
        if (value == 2) { continue; }
        if (value == 7) { goto done; }
        switch (value) {
            case 1: {
                step: i32 = 1;    // each case is its own scope
                value -= step;
                break;
            }
            default: { break; }
        }
    }
    value shl= 1;
    value = ~value;
    done: label = {
        letter = 'y';
    }
    return (value shl 1) | 1;
}
```

## License
MIT - see LICENSE file.
