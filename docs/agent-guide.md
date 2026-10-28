# Writing rin: a guide for agents

rin is C with a few decisions changed. It compiles to C, links with C, and
has no runtime, no garbage collector, no exceptions and no hidden allocation.
If you know C, you know most of rin; this page is the part you don't.

Every ```` ```rin ```` block below is a complete file, and the test suite
checks each one with `rin check` (`docs_agent_guide` in `tests/run_tests.py`),
so what you read here compiles today.

## Work with the compiler

Run `rin check file.rin` after every edit. It parses, imports and type-checks
without writing anything, and its messages say what to write instead:
`'[*]T' is now spelled '^T'`, `prefix '&' was removed; write the address after
its operand, as 'x&'`, `implicit conversion from 'i64' to 'i32' can lose bits;
write cast(x, i32)`. Read the message before guessing. `--diagnostics=json`
gives the same errors as JSON.

| command | does |
|---|---|
| `rin check file.rin` | parse, import, type-check; no output files |
| `rin compile file.rin -o file.c` | write the C (`--single-file` inlines rin's headers) |
| `rin build` | read `build.rin`, compile every entry, drive CMake and the C compiler |
| `python tests/run_tests.py` | the compiler's own test suite |

std lives in `src/std/` (copied beside `rin.exe` when it builds), and is
imported as `import "std/vec.rin"`.

## A whole program

```rin
import "std/print.rin"
import "std/memops.rin"
import "std/vec.rin"

enemy: struct = {
    name: ^const char;
    hp: i32;
}

hit: enum[must_use] = { missed, wounded, killed, }

// Defaults are trailing and constant.
attack: proc(e: *enemy, damage: i32 = 10) -> hit = {
    if (e*.hp <= 0) { return hit.missed; }
    e*.hp -= damage;
    return e*.hp <= 0 ? hit.killed : hit.wounded;
}

main: proc() -> i32 = {
    arena: memops_arena = {};
    memops_arena_initialize(arena&);

    wave: vec<enemy> = {};
    vec<enemy>append(arena&, wave&, { .name = "imp", .hp = 15 });
    vec<enemy>append(arena&, wave&, { .name = "ogre", .hp = 40 });

    for (i: u64 = 0; i < wave.length; i += 1) {
        e: *enemy = wave.data[i]&;
        result: hit = attack(e, 20);
        switch (result) {
            case hit.missed:  { printfmt("{} missed\n", e*.name); }
            case hit.wounded: { printfmt("{} has {} hp\n", e*.name, e*.hp); }
            case hit.killed:  { printfmt("{} is down\n", e*.name); }
        }
    }
    memops_arena_clear(arena&);
    return 0;
}
```

## C to rin

| C | rin | note |
|---|---|---|
| `int x = 0;` | `x: i32 = 0;` | name first: `name: type = value` |
| `int x;` | `x: i32 = ?;` | every variable has an initializer; `?` leaves it unset |
| `struct S s = {0};` | `s: S = {};` | `{}` zeroes |
| `int f(int a) { ... }` | `f: proc(a: i32) -> i32 = { ... }` | |
| `static int f(void)` | `f: static proc() -> i32 = { ... }` | internal linkage |
| `typedef struct {...} V2;` | `V2: struct = { x: f32; y: f32; }` | |
| `enum C { RED };` and `RED` | `C: enum = { red, }` and `C.red` | members are always qualified |
| `T *p` (one thing) | `p: *T` | only `p*` and `p[0]`; `p[1]` is an error |
| `T *p` (many, or from C) | `p: ^T` | C's pointer: indexing and arithmetic, unchecked |
| `*p`, `p->f` | `p*`, `p*.f` | `p[0]` and `p[0].f` also work |
| `&x` | `x&` | postfix, like everything else |
| `int a[4];` | `a: [4]i32 = {};` | |
| `int a[] = {1, 2};` | `a: [_]i32 = { 1, 2 };` | `_` is counted from the initializer |
| pointer + length | `s: []i32` | a slice: `.data`, `.length`, bounds-checked |
| `(float)x` | `cast(x, f32)` | |
| `bool`, `char` | `b32`, `char` (or `c8`) | `true`, `false`, `null` |
| `a >> 3`, `a << 3` | `a shr 3`, `a shl 3` | `shr=` and `shl=` too |
| `i++` | `i += 1` | no `++` or `--` |
| `(void)f();` | `_ = f();` | discards a result on purpose |
| `#include <x.h>` | `cinclude "x.h"` | `import "file.rin"` for rin files |
| `#define N 4` | `#define N 4` | `#` lines pass through to C |

## Declarations

Everything is `name: kind = value`. Locals, globals and fields read the same,
and the type is a prefix chain read left to right: `x: [4]*const T` is an
array of four pointers to const `T`.

```rin
#define LIMIT 8

counter: i32 = 0;                  // a global
scores: [LIMIT]f32 = {};           // a #define as a count
names: [_]^const char = { "a", "b", "c" };

v2: struct = { x: f32; y: f32; }
shape: union = { as_i32: i32; as_f32: f32; }
level: enum[u8] = { low = 1, high = 4, }

bump: proc() -> i32 = {
    total: const i32 = 3;          // const local
    scratch: [64]u8 = ?;           // deliberately left unset
    scratch[0] = 1;
    counter += total;
    return counter + cast(scratch[0], i32);
}
```

## Types and conversions

- Integers: `i8 i16 i32 i64 u8 u16 u32 u64 usize`. Floats: `f32 f64`.
  Booleans: `b32` (`b8`, `b16` exist). Characters: `char`, which is `c8`.
- A number literal takes its type from context. Suffix with `'T` where there
  is none, as in a `printf` vararg: `1024'u64`.
- **Conversions that cannot lose information are implicit** (widening, int to
  float). **Narrowing and float to int need `cast`**: `small: i32 = cast(big, i32);`.
- String literals are `^const char`.

## Pointers, slices and arrays

```rin
sum: proc(xs: []const i32) -> i32 = {
    total: i32 = 0;
    for (i: u64 = 0; i < xs.length; i += 1) { total += xs[i]; }   // checked
    return total;
}

main: proc() -> i32 = {
    a: [5]i32 = { 3, 1, 4, 1, 5 };
    all: i32 = sum(a);              // an array converts to a slice
    mid: i32 = sum(a[1..4]);        // a range is a sub-slice, no copy

    one: *i32 = a[2]&;              // exactly one i32
    one* = 9;                       // read and write through it with p*
    raw: ^i32 = one;                // *T converts to ^T implicitly
    raw[1] = 2;                     // ^T indexes anywhere, unchecked
    back: []i32 = raw[0..2];        // a raw pointer becomes a slice by giving its length

    return all + mid + sum(back);
}
```

- `p*` is the dereference. Write it glued to its operand: `p* - 1` reads
  through `p`; `p * -1` multiplies, which is an error for a pointer. Field
  access is `p*.field`.
- A fixed array has no `.length`. Convert it to a slice (`s: []T = a;`) or use
  `sizeof(a) / sizeof(a[0])`.
- `noalias` on a pointer or slice parameter promises it overlaps nothing else
  for that call (C's `restrict`); Debug builds check it.

## Control flow

```rin
main: proc() -> i32 = {
    hits: i32 = 0;
    for (i: i32 = 0; i < 10; i += 1) {
        if (i == 2) { continue; }
        switch (i % 3) {
            case 0: { hits += 1; }        // a case ends by itself: no fallthrough
            case 1: { break; }            // break leaves the switch, as in C
            default: { hits += 10; }
        }
    }
    n: i32 = 0;
    while (n < 3) { n += 1; }
    do { n -= 1; } while (n > 0);
    if (hits > 100) { goto done; }
    hits += 1;
    done: label = { hits *= 2; }
    return hits;
}
```

- Bodies always take braces: `if (x) { y = 1; }`, never `if (x) y = 1;`.
- `for` is C's three-part loop. There is no `for (v in xs)`, no `defer`, no
  `++`.
- A `switch` over an enum with no `default` must list every member, or it does
  not compile.

## Procs and generics

```rin
pair: struct<T> = { a: T; b: T; }

// Type arguments are always written at the call: larger<f32>(...).
larger: proc<T>(p: pair<T>) -> T = {
    return p.a > p.b ? p.a : p.b;
}

// A proc "on" a generic type is a qualified name, not a method.
pair<T>swap: proc<T>(p: *pair<T>) -> void = {
    t: T = p*.a;
    p*.a = p*.b;
    p*.b = t;
}

point: struct = { x: f32; y: f32; z: f32; }

// T<> is the reflection record of whatever T is in this instance.
field_count: proc<T>() -> u64 = { return T<>.count; }

main: proc() -> i32 = {
    p: pair<f32> = { .a = 1.0, .b = 2.0 };
    pair<f32>swap(p&);
    m: f32 = larger<f32>(p);
    n: u64 = field_count<point>();
    return cast(m, i32) + cast(n, i32);
}
```

- No methods, no overloading, no multiple return values: return a struct or
  take an out pointer (`out: *T`).
- `Type<>.name`, `.size`, `.count` and `.variant.fields[i].name` describe a
  struct or enum at compile time.
- `enum[must_use]` marks a status type: ignoring a result of that type is an
  error unless written `_ = f();`.

## C interop

```rin
cinclude "stdlib.h"
cinclude "string.h"

// A C function: declared, not defined, with the attribute.
abs: proc[external](x: i32) -> i32 = {}
memcpy: proc[external](dst: ^void, src: ^const void, n: usize) -> ^void = {}

// A C struct: the fields rin may touch, or `= {}` for an opaque one.
div_t: struct[external] = { quot: i32; rem: i32; }
FILE: struct[external] = {}

main: proc() -> i32 = {
    a: [4]u8 = { 1, 2, 3, 4 };
    b: [4]u8 = {};
    memcpy(b, a, 4);
    return abs(-3) + cast(b[3], i32);
}
```

- Every C pointer is `^T`, including `void *` (`^void`). `rinbind` generates
  bindings from a header in this form.
- A calling convention goes in the attribute slot: `proc[callconv(CALLBACK)]`.
- A global that C owns has no initializer: `errno_value: const i32;`.
- `struct[external, no_layout_check]` skips the static layout checks rin emits
  against the C definition.

## std at a glance

std allocates only from arenas: there is no `malloc` API, and containers take
the arena they allocate from.

| import | gives |
|---|---|
| `std/print.rin` | `printfmt("{} {}\n", a, b)`: `{}` per argument, enums print their names |
| `std/memops.rin` | `memops_arena`, `memops_arena_initialize`, `memops_arena_push(arena, size, align)`, `memops_arena_clear`, `memops_arena_temp_begin/end` |
| `std/vec.rin` | `vec<T>`: `append`, `at`, `get`, `last`, `pop`, `items`, `clear`; `reserve` makes room only, the length stays 0 |
| `std/map.rin` | `map<T>` keyed by C strings: `create`, `set`, `get`, `length` |
| `std/option.rin`, `std/result.rin` | `option<T>some/none/unwrap_or`, `result<T>ok/err/unwrap_or` |
| `std/string8.rin` | `string8` (pointer, length, capacity): `string8_from_cstr`, `string8_append_cstr`, ... |
| `std/array.rin`, `heap`, `deque`, `list`, `hashmap`, `sort` | the other containers, same shape |

## Building

A project is described by `build.rin`, which is read, not run:

```rin
build_name:      ^const char = "game";
build_entry:     ^const char = "src/main.rin";
build_dir:       ^const char = "build";
build_generator: ^const char = "Ninja";
build_compiler:  ^const char = "clang-cl";
build_type:      ^const char = "Debug";
build_libraries: [_]^const char = { "user32", "d3d11" };
```

`build_entries` lists several programs instead of one `build_entry`; others
are `build_include_dirs`, `build_defines`, `build_c_flags`, `build_c_sources`
and `build_pch`. `rin build` writes the C to `build/rin_gen/` and the
executables to `build/cmake/`, with debugger visualizers linked into each PDB.

## Mistakes C habits make

| you write | write instead |
|---|---|
| `&x`, `*p`, `p->f` | `x&`, `p*`, `p*.f` |
| `int x;` | `x: i32 = ?;` or `x: i32 = {};` |
| `[*]T`, `T*` for a C pointer | `^T` |
| `p[1]` on a `*T` | make it a slice `[]T`, or a `^T` if it comes from C |
| `p * -1` meaning a dereference | `p* - 1` |
| `small: i32 = big;` from an `i64` | `small: i32 = cast(big, i32);` |
| `red` for an enum member | `color.red` |
| `bool`, `i++`, `a >> 1`, `and`, `or` | `b32`, `i += 1`, `a shr 1`, `&&`, `||` |
| `if (x) y = 1;` | `if (x) { y = 1; }` |
| `case 1: x = 2; break;` | `case 1: { x = 2; }` (no fallthrough) |
| `for (v in xs)` | `for (i: u64 = 0; i < xs.length; i += 1)` |
| `id(3)` for a generic | `id<i32>(3)` |
| `malloc`, `free` | an arena from `std/memops.rin` |
| `xs.length` on a `[4]T` | convert to `[]T` first |
| `puts: proc(...) = { external; }` | `puts: proc[external](...) -> i32 = {}` |
| ignoring a `must_use` result | use it, or `_ = f();` |

## Reading further

The `docs/` folder records how rin was designed. Some of it is history; trust
this page and the compiler over a design doc when they disagree.

- current reference: `pointers-and-slices.md`, `pointer-syntax.md`,
  `attributes.md`, `build-command.md`, `debugger-integration.md`,
  `noalias-variadic.md`
- design history, describing older spellings: `c-pointer-spelling.md`,
  `slices-talk.md`, `conversion-report-00.md`, and the README's "Language
  Sketch", which predates several changes
