# Arrays: values, or the C model

> **Measured, not implemented.** rin's type checker treats `[N]T` as a value,
> but the C it writes uses plain C arrays, which are not values. This records
> the five places that clash, the two ways out, where each change goes in
> `src/main.c`, and the order to do it in. Line numbers are at `e1fe382`;
> results are from clang 18 and GCC 13.
>
> Companions: [clib-removal.md](clib-removal.md) for where `memset` and
> `memmove` come from once libc is gone; [slices.md](slices.md) for `[]T`,
> which is what an array parameter should usually be.

## The short version

Start with the C model: make `rin check` reject the five ways an array is used
as a whole value. Open some of them as values later if real code wants them.
Both paths are good, but only one direction is safe. Lifting an error breaks
no program; adding one breaks every program that relied on the feature.

Zeroing and copying stay one call each, `memset` and `memmove`.

## The problem

The type checker already treats `[4]u32` as a value. The length is part of
the type, so `a = b` between a `[4]u32` and a `[3]u32` is rejected. But C has
no array values. An array's name turns into a pointer to its first element in
almost every expression, so it can't be assigned, passed or compared whole.

With `a` and `b` both `[4]u32`:

| rin code | what happens | found when |
|---|---|---|
| `a = b;`, `a = {};`, `p*.slots = {};` | C error: array type is not assignable | C compile, after `rin check` passed |
| `b: [4]u32 = a;` | C error: array initializer must be an initializer list | C compile |
| `v: [4]u32` as a parameter | C reads `u32 v[4]` as `u32 *v`: the callee's writes change the caller's array | never: it runs |
| `-> [4]u32` as a return type | C error: the return type is emitted as `u32`, the length dropped | C compile |
| `a == b` | compares addresses, so two equal arrays are unequal | never: clang only warns |

Two of the five are silent, and those are the ones to fix first.

Two things already work and need nothing: copying a struct that holds an
array (C copies structs whole), and turning an array into a slice,
`s: []u32 = a;`.

The first case turned up writing a rin-dod dojo block, where both
`t*.slots = {}` and `kind_keys = { ... }` failed in C.

## Path 1: the C model

Reject in the type checker what C can't do, so the error comes from rin, at
the right line, with the fix in the message. All five checks share one test:
the type, with aliases resolved, has `kind == Type_Array` (defined at line
578).

| case | where in `src/main.c` | suggested error |
|---|---|---|
| `a = b;`, `a = {};` | `Stmt_Assign` check, about line 14502, when `expected` is an array | arrays can't be assigned; use memmove to copy one, or memset to zero one |
| `b: [4]u32 = a;` | `Stmt_Var` check, about line 14491, when the type is an array and the initializer isn't a `{...}` literal | an array can only be initialized from a {...} literal; declare it, then memmove |
| `v: [4]u32` parameter | parameter loop in `type_check_proc_body`, about line 14711 | arrays can't be passed by value; take a slice ([]u32) or a pointer (*[4]u32) |
| `-> [4]u32` return | about line 14717, beside the parameters | procs can't return arrays; return a struct holding one, or write into the caller's array |
| `a == b`, `a != b` | the `==` branch at line 14418, when either side is an array | == on arrays would compare addresses; compare the elements |

What stays allowed, all of it valid C:

  * literal initializers in a declaration, `a: [4]u32 = { 1, 2 };` and
    `= {}`, which zero-fill the rest;
  * reading and writing elements, `a[i]`;
  * copying a struct that holds an array;
  * turning an array into a slice, which is why the parameter error can point
    at `[]u32`.

The return-type check makes the emitter bug that drops the length unreachable.
It's still worth fixing on its own.

**Open question:** reject array parameters outright, or keep C's meaning (a
pointer) and only warn? Rejecting is the honest choice, since the parameter's
type says `[4]u32` and it isn't one.

## Path 2: arrays as values

Keep C arrays in the output, and lower each case to an explicit copy or zero
through two macros in `core.h` (see [The mem calls](#the-mem-calls)). Wrapping
every array in a C struct would give value semantics for free, but it turns
every index into `a.v[i]` and breaks C interop, external struct layouts and the
reflect records.

| case | lowers to | cost |
|---|---|---|
| `a = b` | `rin_copy(&(a), b, sizeof(a));` | one copy, visible where it's written |
| `a = {}` | `rin_zero(&(a), sizeof(a));` | one zero |
| `a = { 1, 2 }` | `rin_copy(&(a), (u32[4]){1, 2}, sizeof(a));` | one copy; C zero-fills the literal's rest |
| `b: [4]u32 = a` | `u32 b[4]; rin_copy(&(b), a, sizeof(b));` | one copy; globals rejected, C needs a constant initializer |
| `v: [4]u32` parameter | `const u32 *v__in`, and the body starts `u32 v[4]; rin_copy(v, v__in, sizeof(v));` | a copy on every call, invisible at the call site |
| `-> [4]u32` | return `struct { u32 v[4]; }`; the call site reads `make().v` | a struct wrapper, only on returns |
| `a == b` | an element loop | `memcmp` is wrong for floats (NaN, -0.0) and padded elements |

The sizes always match. The checker already rejects `[3]u32` where `[4]u32` is
wanted, and too many values in a literal, and rin writes a literal with the
target's own type. So `sizeof(lhs)` is always the right count. Copying between
different lengths is a slice operation and a proc call (see
[Slice procs](#slice-procs)), never `=`.

The emitter sees expressions, not types. The checker sets a flag on the
statement where it already knows the target type, in the `Stmt_Assign` and
`Stmt_Var` checks. Parameters and returns the emitter decides itself, from the
proc's signature.

Parameters are the risky case for data-oriented code: passing a 16 KB array
copies 16 KB, and nothing at the call site says so. Odin, as I understand it,
makes parameters read-only, so the compiler can pass big ones by pointer with
no copy and keep value semantics. rin's parameters can be written to today
(`v[0] = 100` passes `rin check`), so that would be a decision about parameters
in general, not just arrays.

## Why the C path first

Going from the C path to values breaks nothing: every program that compiled
under the rejections still compiles. Going the other way breaks every `a = b`
and every array parameter already written.

There is evidence for opening some cases later. Writing the dojo block, the
first thing reached for was `t*.slots = {}` and `kind_keys = { ... }`. rin puts
the length in the type, so people read `[4]u32` as a value and expect `=` to
work.

The order:

1. **Now:** the C path, all five rejected. A few checks; it fixes the silent
   parameter and `==` cases.
2. **When wanted:** open assignment and initialization from another array as
   values. They are the cases people reach for, each is one `rin_copy` or
   `rin_zero`, the copy is visible where it's written, and opening them breaks
   nothing.
3. **Only if real code asks:** parameters, returns and `==`. By then the
   question of read-only parameters will have an answer.

## The mem calls

On either path, zeroing and copying are `memset` and `memmove`, and they are
always declared: every C file rin writes starts with `#include <core.h>`
(line 17521), and `core.h` includes `<string.h>` (line 14). In rin today, with
`std/cstd.rin` imported:

    memset(t*.slots[0]&, 0, sizeof(t*.slots));

For the compiler's own lowerings (path 2), emit two macros from `core.h`
rather than the libc names, so there is one place to change later:

    #if defined(__GNUC__) || defined(__clang__)
    #  define rin_zero(dst, n)      __builtin_memset((dst), 0, (n))
    #  define rin_copy(dst, src, n) __builtin_memmove((dst), (src), (n))
    #else
    #  define rin_zero(dst, n)      memset((dst), 0, (n))
    #  define rin_copy(dst, src, n) memmove((dst), (src), (n))
    #endif

The builtins need no header. A small fixed size becomes a few stores; a large
one becomes a call to `memset` or `memmove`. `memmove` rather than `memcpy`,
so `a = a` and overlapping copies are defined.

Those calls can't be avoided, even without libc, because C compilers emit them
on their own (clang 18):

| program | C library symbols it needs |
|---|---|
| empty `main` | none |
| loop zeroing a 1 KB table, `-O2` | `memset`, from a loop with no `memset` in it |
| same, `-ffreestanding` | none |
| 16 KB table zeroed with `= {}`, `-ffreestanding` | `memset` |

Today libc provides them; without libc, `std/mem.rin` does, as
[clib-removal.md](clib-removal.md) plans.

One trap that plan doesn't cover yet. GCC 13 at `-O2` turns the loop inside a
hand-written `memset` into `call memset`, which recurses forever; clang 18
doesn't. rin writes one C file per program, so a flag like
`-fno-tree-loop-distribute-patterns` would apply to the whole program. A
per-function attribute works on both; with it, neither `memset` nor `memmove`
calls anything at `-O2`:

    #if defined(__clang__)
    #  define RIN_NO_BUILTIN __attribute__((no_builtin))
    #elif defined(__GNUC__)
    #  define RIN_NO_BUILTIN __attribute__((optimize("no-tree-loop-distribute-patterns")))
    #else
    #  define RIN_NO_BUILTIN
    #endif

So `std/mem.rin` needs a proc attribute that lowers to `RIN_NO_BUILTIN`.
MSVC's equivalent is `#pragma function(memset)`.

This still doesn't make rin free of the C runtime. Linked with `-nostdlib`, a
program has no `_start` to call `main`, and std's abort and print path still
needs a home, which clib-removal.md puts in `memops_os`.

## Slice procs

Optional. Two procs over slices would make the explicit form shorter than the
`memset` line above, since an array already turns into a slice:

    slice_zero: proc<T>(s: []T) -> void              // memset(s.data, 0, s.length * sizeof(T))
    slice_copy: proc<T>(dst: []T, src: []T) -> u64   // memmove of the shorter length; returns the count

A call reads `slice_zero(t*.slots);`. `slice_copy` is also the answer to
copying between different lengths, the way Go's `copy` works. std has no slice
copy or fill today (`slice.rin`, `slicecore.rin`, `array.rin`), so these could
live in `std/mem.rin`.

## Repros

All of these pass `rin check` at `e1fe382`. Once the checks land, each should
fail `rin check` with its new error, which makes them tests.

    table: struct = { slots: [4]u32; }
    keys: [3]u64 = {};

    main: proc() -> i32 = {
        t: table = {};
        p: *table = t&;
        p*.slots = {};          // C: p[0].slots = ((u32[4]){0});   not assignable
        keys = { 1, 2, 3 };     // C: keys = ((u64[3]){1, 2, 3});  not assignable
        a: [4]u32 = { 1, 2, 3, 4 };
        b: [4]u32 = {};
        b = a;                  // C: b = a;                       not assignable
        c: [4]u32 = a;          // C: array initializer must be an initializer list
        return 0;
    }

The silent parameter, exits with 204 where by-value gives 105:

    sum: proc(v: [4]u32) -> u32 = { v[0] = 100; return v[0] + v[3]; }
    main: proc() -> i32 = { a: [4]u32 = { 1, 2, 3, 4 }; s: u32 = sum(a); return cast(s + a[0], i32); }

The silent `==`, exits with 0 though the arrays are equal:

    main: proc() -> i32 = { a: [4]u32 = { 1, 2, 3, 4 }; b: [4]u32 = { 1, 2, 3, 4 }; return cast(a == b, i32); }

The return type, emitted as `u32`:

    make: proc() -> [4]u32 = { r: [4]u32 = { 1, 2, 3, 4 }; return r; }

The size checks that already work, with `a: [4]u32` and `b: [3]u32`:

| code | `rin check` today |
|---|---|
| `a = b;` | expected 'array_4_u32', got 'array_3_u32' |
| `x: [3]u32 = a;` | initializer expected 'array_3_u32', got 'array_4_u32' |
| `a = { 1, 2, 3, 4, 5 };` | too many positional initializer values for type 'array_4_u32' |
| `a = { 1, 2 };` | passes, then fails in C |
| `s: []u32 = a;` | passes, and works |
