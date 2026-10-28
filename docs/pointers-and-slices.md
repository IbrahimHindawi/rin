# Pointers And Slices

> C's pointer was spelled `[*]T` until 2026-10-28; it is `^T` now, and a single
> pointer reads through with `p*` as well as `p[0]`. Both are described in
> [pointer-syntax.md](pointer-syntax.md); this doc uses the new spellings.

> **Implemented** (2026-10-21). Decided in discussion, 2026-10-20 to
> 2026-10-21. `*T` points at exactly one element, `[]T` is a built-in slice
> lowered to the existing `slice<T>`, `^T` is the raw pointer for C and
> low-level code, and indexing through arrays and slices is bounds checked.
> What the build found is in "Suggested order" at the end.
>
> Builds on [slices.md](slices.md), which answers its own "should slices become
> a language feature?" here, and reverses the bounds-check stance in
> [safety.md](safety.md).

## 1. Four types, one index syntax

    *T    one element       only [0]                 pointer
    []T   many elements     checked, stored length   pointer + length
    [N]T  array             checked, length N        inline storage
    ^T  raw pointer       unchecked                pointer

All four are read with `x[i]` and addressed with `x[i]&`. **None of them gets
heavier than today**: `*T` and `^T` are both a single C pointer, and the
difference between them is which operations the compiler accepts, not what is
stored.

This is Zig's split (`*T`, `^T`, `[]T`) without its fourth type. It keeps
what [operators-and-conversions.md](operators-and-conversions.md) section 6
wanted from "every pointer a slice" -- a reference that cannot be indexed past
what it points at -- without the cost that section rejected it for: `next: *Node`
stays eight bytes.

### `[_]T`: an array counted from its initializer

    names: [_]^const char = { "imp", "ogre", "wyrm" };   // [3]
    sparse: [_]f32 = { [4] = 1.0, 2.0 };                    // [6]

`_` is C's `T x[] = {...}`: the count is one past the highest index the
initializer writes, so `[i] =` designators count too. The result is an ordinary
`[N]T` -- `sizeof`, bounds checks and conversion to `[]T` all see the number
-- and the emitted C spells it out. Only a declaration's own outermost
dimension can be counted: `[_]` in a field, a parameter, a return type, an
inner dimension, after `= {}` or with no initializer is an error, since there
is nothing to count.

## 2. `*T` points at one element

    p: *T = thing&;
    p*;              // ok: the dereference, the same as p[0]
    p*.field;        // ok
    p[0];            // ok
    p[1];            // error: '*T' points at one element; use a slice or '^T'
    p[i];            // error, same
    p + 1;           // error: pointer arithmetic needs '^T'

**Only a literal `[0]` is accepted.** A variable index on a `*T` is not checked
at runtime against a length of one -- it is rejected, because `p[i]` on a
pointer to one thing almost always means the programmer wanted a slice or a raw
pointer. Every rule in this section is a compile-time check with no runtime
cost.

**Taking the address of an element gives a `*T`**, whatever it was taken from:

    a: [16]T;
    a[i]&;           // *T
    s[i]&;           // *T, from a []T
    q[i]&;           // *T, from a ^T

### Conversions

    q: ^T = p;     // implicit: one element is a valid "unknown extent"
    r: *T = q;       // error: say which element
    r: *T = q[0]&;   // ok -- no cast needed
    s: []T = q[0..n];       // a raw pointer becomes a slice when someone
                            // vouches for its length

`^T` to `*T` needs no new operator. `q[0]&` already says "the address of one
element", which is exactly what a `*T` is. Building a slice from a raw pointer
is a subrange of it, `q[0..n]` -- the same `..` that slices a slice, so there is
no constructor to learn.

No auto-deref, still. Every hop through a `*T` is a visible `[0]`, as section 1
of operators-and-conversions.md decided.

## 3. The C boundary

**Every C pointer is a `^T`.** `rinbind` emits it for every pointer it
imports, and hand-written `[external]` declarations use it too, external struct
fields included. A C header cannot say whether `T*` means one or many, so Rin
does not guess: C code stays unchecked, which is the truth, and the program
converts once it knows what the pointer actually points at.

    // memcpy from the binding: proc(dst: ^void, src: ^const void, n: u64)
    memcpy(constants.mvp, mvp_t, sizeof(constants.mvp));  // arrays convert to ^
    memcpy(mapped.pData, data, byte_width);               // pData is ^void

    // cgltf: data[0].meshes is ^cgltf_mesh
    mesh: *cgltf_mesh = data[0].meshes[0]&;               // unchanged from today
    meshes: []cgltf_mesh = data[0].meshes[0..data[0].meshes_count];
    meshes[i];                                            // checked from here on

**No `[*c]T`.** Zig has a fourth pointer type for translated C headers -- it
allows single-item deref, indexing and null, with no checks -- because its
header translation wants C pointers to feel like C. Rin's answer is simpler:
they are raw, and raw is already a type.

## 4. `[]T` lowers to `slice<T>`

**`[]T` is spelling for the existing `slice<T>`, not a new type.** In
`parse_type` (`src/main.c`, the `Token_LBracket` branch), `[` followed directly
by `]` builds the same `Type_Generic` node that `slice<T>` does. So:

- one type and one C struct -- `[]u32` and `slice<u32>` are the same type and
  mix freely
- every proc in `std/slice.rin` keeps working on `[]T` values
- the generated C is what it is today

What the compiler adds on top of the struct:

- **`s[i]` on a slice** lowers to a checked `s.data[i]` (section 5).
- **An array converts to `[]T`**, with the length taken from its type.
  `[256]u32` passed to a `[]u32` parameter needs no `sizeof(x) / sizeof(x[0])`.
- **`[]T` widens to `[]const T`** for free, as it does in every other language
  with slices.
- **`s[a..b]`** takes a sub-slice, `a` included and `b` excluded, and traps
  when the range is out of bounds. On a `^T` it is unchecked and is how a raw
  pointer becomes a slice. `slice<T>sub` stays as the library form that clamps.

**The struct must be available without `std/slice.rin`'s imports.** That file
pulls in `stdio.h`, `stdlib.h`, `string.h`, `memops`, `option`, `array` and
`vec`. Writing `[]T` should not. The likely shape is a minimal core declaration
the compiler knows about, with `std/slice.rin` building its procs on it.

## 5. Bounds checks

**A constant index out of range is always a compile error.** On an array
(`xs: [4]i32; xs[9]`), and on a `*T` (anything but `[0]`). This is stage 1 of
safety.md: no runtime cost, no new syntax, cases the compiler already decides.

**A variable index into an array or slice gets a runtime check.** It is emitted
as a macro rather than spelled out at every site, so the generated C stays
readable and the switch is one `#define`:

    xs[RIN_INDEX(i, 4)]
    s.data[RIN_INDEX(i, s.length)]

**Checks are on in Debug builds and compiled out in Release**, keyed to C's own
`NDEBUG`, which CMake's Release configuration already defines -- no new flag. A
failed check prints `file:line: index i out of bounds (length n)` and calls
`abort()`.
Signed indexes are checked as unsigned, so a negative index fails the same
comparison as one past the end.

### This reverses safety.md

safety.md parked bounds checks partly on this argument: a flag that turns them
off "is a second language, not a safety feature". It does not hold up. A
correct program behaves identically with checks on or off -- out of bounds is a
bug in both modes, and the check only detects it. Zig (`ReleaseFast`) and Odin
(`-no-bounds-check`) ship exactly this split.

### What this does not cover

**Lifetimes.** A slice or a `*T` can still point at memory that was freed, or
an arena that was reset. Bounds safety says which element may be accessed, not
whether the storage is still alive. Non-null pointers are also separate. Both
are to be discussed later.

## 6. Holes in today's slices

Counted in njinn, which converted to `slice<T>` (slices.md).

- **The three sharp edges from slices.md**, all still present: no array to
  slice coercion, no `slice<T>` to `slice<const T>` widening, and
  `*const slice<T>` looking read-only when it is not. Section 4 fixes the first
  two; the third goes away when `[]const T` is the obvious spelling.
- **Nobody calls the checked accessor.** `slice<T>at` has **0** callers. Reads
  go through raw `.data[i]` (329 uses across all containers). A check only gets
  used if the safe spelling is the short one -- which is the argument for `s[i]`.
- **39 `from_parts` calls**, several with a hand-written length
  (`slice<u32>from_parts(filtered[0]&, 256)`) and two that build a slice of one
  element out of `x&` with a const cast:

      slice<const fxops_swipe_stimulus>from_parts(cast(swipe&, *const fxops_swipe_stimulus), 1)

  njinn already writes "a slice of one" in practice. Under this design it is a
  `*const fxops_swipe_stimulus`, or the API takes a `[]const T` and gets a
  converted array.
- **`sub` clamps silently.** Right for a scanning helper, wrong for indexing
  syntax; section 4 keeps both.
- **`slice.rin` is heavy to import** (section 4).
- **`string8slice` vs `[]u8`** stays open, as [strings.md](strings.md) decision 1.

## 7. What changes in njinn

These are API changes made by hand, not a script -- and that is the point: each
one replaces a pointer that trusted its caller with a slice that knows its
length.

    // before: trusts that eight bytes follow the pointer
    cryptops_read_u64_le: static proc(data: *const u8) -> u64 = { ... data[7] ... }
    out_header[0].table_offset = cryptops_read_u64_le(raw[24]&);

    // now: data[7] is checked against the slice
    cryptops_read_u64_le: static proc(data: []const u8) -> u64 = { ... data[7] ... }
    out_header[0].table_offset = cryptops_read_u64_le(raw[24..32]);

    // before: a pointer and a capacity, passed separately, free to disagree
    fmt_to: proc(data: *c8, capacity: u64) -> fmt
    w: fmt = fmt_to(normalized[0]&, sizeof(normalized));

    // now: the array converts, and its length comes from its type
    fmt_to: proc(buf: []c8) -> fmt
    w: fmt = fmt_to(normalized);

The size of the job:

| what | njinn sites |
|---|---|
| `x[k]&` with `k` not `0` -- usually a "many" pointer in a `*T` | 406 |
| `x[0]&` -- often one element, sometimes the start of a buffer | 151 |
| `slice<T>from_parts` calls | 39 |
| pointer arithmetic | not counted yet |

Not every `x[k]&` breaks -- one that really is the address of one element stays
a `*T`. The compiler finds the rest, as errors at the callee that indexes past
`[0]`.

## Suggested order

All five steps are done.

1. **Done.** `[]T` as `slice<T>`, checked `s[i]`, array to slice conversion,
   `[]T` to `[]const T` widening, `s[a..b]`, and constant-index errors on
   arrays. `std/slicecore.rin` holds the struct and imports nothing; the
   compiler imports it wherever `[]T` or a range is written. One limit: a
   checked slice access or a conversion reads its slice twice, so the slice
   must be a name, field or index (no calls) -- otherwise it is an error that
   says to store it in a local first.
2. **Done.** `^T` is a flag on the pointer type, the same C pointer, so it
   never changes the generated C. rinbind emits it for every C pointer except
   `void *` and a pointer to an incomplete type: an opaque handle cannot be
   indexed or moved, so it stays `*T`. A string literal is `^c8`, and C
   strings are `^const c8` throughout std and njinn.
3. **Done.** `RIN_INDEX` / `RIN_RANGE` in `core.h`, off under NDEBUG; njinn's
   Debug build carries 714 index checks and its selftests pass with them on.
4. **Done.** The rules shipped as warnings with a note at each `*` to change:
   992 sites across std, njinn and the tests, almost all a handful of
   declarations -- container `.data` fields, C strings, lookup tables, token
   arrays. Each such declaration became `^T` (generated C byte-identical),
   and the pointer-plus-length APIs became slices: `fmt_to`, `fmt_cat1`-`5`,
   `fmt_expand_i64`, `cstr_copy`, `cstr_append` in std; in njinn the
   settings-path writers, `fxops_copy_cstr`, the JSON and CSV token readers,
   `gops_count_lines`, `gops_find_build_segment`,
   `pacops_collect_fx_desc_names`, and cryptops' header parser and
   little-endian readers -- `cryptops_read_u64_le(raw[24..32])`.
5. **Done: errors.** Indexing a `*T` past `[0]`, a range of one, arithmetic on
   one, and a `^T` becoming a `*T` without naming the element are all
   errors. Covered by `builtin_slices`, `bounds_runtime`, `constant_bounds` and
   `pointer_rules`.

## See also

- [`slices.md`](slices.md) -- `slice<T>`, the convention it enables, and the
  njinn conversion this builds on.
- [`safety.md`](safety.md) -- bounds checks among the other runtime bugs, and
  the stance section 5 reverses.
- [`operators-and-conversions.md`](operators-and-conversions.md) -- postfix
  `x[0]` and `x&`, no auto-deref, and why every pointer is not a slice.
- [`strings.md`](strings.md) -- `string8slice` and the `[]u8` question.
- [`type-safety.md`](type-safety.md) -- the compile-time half of safety.
