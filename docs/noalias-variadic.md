# noalias and variadic procs

> **Implemented** (2026-10-27), as designed below, with the portability macros
> and corrections noted in 1.7 and 1.8. Proposed 2026-10-26. Unless a number
> says otherwise it was measured on a Linux build of rin, gcc 13.3 and clang
> 18.1, on a 2.8 GHz Xeon. The kernels come from rin-dod, the data-oriented
> lessons, and c-dod, the same lessons in plain C.

| part | topic | decision |
|---|---|---|
| 1 | `noalias` | qualifier on pointer and slice parameters: `restrict` on `*T` and `[*]T`, an inlined helper for `[]T`, overlap checks in Debug |
| 2 | variadic procs | `noalias` on `*T` and `[*]T` allowed; on `[]T` an error until rin has built-in `...`, then a `va_list` helper |

---

# Part 1. noalias

`noalias` is a type qualifier for pointer and slice parameters. It promises
that for the duration of one call, memory reached through that parameter is
not reached through anything else.

    add_into: proc(dst: noalias []f32, src: []const f32, k: f32) -> void = { ... }
    m4_mul: proc(out: noalias *mat4, a: *const mat4, b: *const mat4) -> void = { ... }
    copy_raw: proc(dst: noalias [*]u8, src: [*]const u8, n: u64) -> void = { ... }

On `*T` and `[*]T` it lowers to C's `restrict`, nothing more. On `[]T` the
compiler moves the proc's body into a helper so that `restrict` lands on a
real C parameter, without changing the proc's signature. Debug builds check
the promise wherever the parameter's extent is known.

## 1.1 Why

**The compiler cannot prove two pointers are apart, so it plans for the
worst.** The movement kernel from rin-dod lesson 1, positions and velocities
split into six `f32` columns, 8192 particles, in L2:

| build | ns per particle | what happened |
|---|---|---|
| gcc `-O3`, plain loop | 3.20 | not vectorized |
| clang `-O2`, plain loop | 0.57 | vectorized behind runtime overlap checks |
| SSE intrinsics, either compiler | 0.82 | |
| AVX2 intrinsics, either compiler | 0.53 | |

The same loop through `[]f32` slices instead of `[*]f32` gives identical
assembly and timings, because with `NDEBUG` a slice index is a plain pointer
index. gcc gives up because six columns need more runtime overlap checks than
its default allows; `--param vect-max-version-for-alias-checks=30` makes it
vectorize, so the checks are the whole obstacle.

**The wider the kernel, the more it matters.** Without the promise, clang
pays with code and eventually gives up too:

| columns in one kernel | gcc `-O3`, today | clang `-O2`, today | with the promise |
|---|---|---|---|
| 4 | vectorized behind checks | vectorized behind checks | vectorized, no checks |
| 16 | not vectorized | 1,485 lines of assembly, mostly checks | vectorized, 188 to 252 lines |
| 32 | not vectorized | not vectorized | vectorized, 374 to 463 lines |

With the two to six columns most systems take, clang 18 on Linux already
vectorizes. It is large under gcc and for wide kernels, and it is nothing
once data streams from RAM, where every version runs at memory bandwidth.

**On Windows clang it matters at six columns too.** The same six-column
kernel through the implementation, clang `-O2 -DNDEBUG` targeting
x86_64-pc-windows-msvc, 8192 particles, the kernel kept out of line with
`-fno-inline` (which still honours the helper's `always_inline`):

| build | code | ns per particle |
|---|---|---|
| plain slices | scalar `mulss`, not vectorized, no overlap checks tried | 0.96 |
| `noalias` slices | 4-wide `mulps`/`addps`, no overlap checks | 0.15 |

Inlined into a caller that can see the six arrays are distinct globals, the
plain version vectorizes as well and both run at 0.15, so the promise pays
where the compiler cannot see the arguments: a kernel in its own function,
which is how systems are written.

**On Windows a single slice needs it too.** Windows x64 passes a 16-byte
slice by reference, as a pointer to a copy, and clang-cl compiles with
`-relaxed-aliasing` by default to match MSVC. So without the promise, a float
written through the slice might be the slice's own `data` or `length`, and
the loop reads both again after every element. rin-dod lesson 2's
`age_system(lives: []f32)` on clang-cl `/O2` did one float per pass,
reloading `lives.data` and `lives.length` each time; with
`lives: noalias []f32` it does eight floats per pass. `/clang:-fstrict-aliasing`
does the same for the whole build, but makes type punning anywhere undefined
behaviour. The rule: `noalias` on every slice a loop writes, even when it is
the only one.

**Hot code keeps its slices.** [pointers-and-slices.md](pointers-and-slices.md)
moved rin's pointer-plus-length APIs to slices because "each one replaces a
pointer that trusted its caller with a slice that knows its length". A
`noalias` limited to raw pointers would push hot kernels back to pointer plus
length. With it on slices, a kernel keeps a clean signature and its Debug
bounds checks, which in rin-dod caught a real off-by-one in a loop over
component tables.

**It states a contract a caller can break.** `m4_mul(out, a, b)` called with
`out` equal to `a` overwrites its input halfway through. With
`out: noalias *mat4`, a Debug build stops on the first such call. Unlike the
speed gain, this check has not yet caught a real bug.

Elsewhere: standard C++ has no `restrict`, but MSVC, gcc and clang all accept
`__restrict`. Fortran assumes it for every argument. Rust implies it for
every `&mut`, which its borrow checker proves. Zig has `noalias` on
parameters. None of them check it at run time; rin can, because `[]T` and
`*T` know their extent.

## 1.2 Syntax

A prefix qualifier, parsed in `parse_type_inner` beside `const` and
`volatile`, which sets `is_noalias` on the type it precedes:

    dst: noalias []f32
    out: noalias *mat4
    src: noalias []const f32
    raw: noalias [*]u8

It qualifies the pointer or slice itself, not what it points at, and reads
left to right like every other rin type: `noalias *const T` is a no-alias
pointer to a const `T`, which is `const T *restrict` in C.

**Not an attribute.** [attributes.md](attributes.md) rules out parameter
attributes and names `restrict` as a qualifier that belongs to the type.

**No proc attribute either.** Nothing outside the proc changes (1.7), so
there is nothing for the caller to be warned about.

**Not spelled `restrict`.** `restrict` is a C keyword, which rin renames
when it is used as an identifier, so a field or variable may be called
`restrict`; and it names the mechanism. `noalias` names the promise, and is
the word Zig and LLVM use.

**It reserves a word.** No identifier named `noalias` exists in rin's `src`
and tests, rin-dx11 or rin-dod as of this writing.

## 1.3 Where it may appear

Only at the top of a parameter's type, in a proc declaration or a proc-pointer
type, on `*T`, `[*]T` or `[]T`. Generic parameters are fine:
`proc<T>(dst: noalias []T, src: []const T)`.

| written | error |
|---|---|
| `x: noalias f32` | `'noalias' qualifies a pointer or slice; 'f32' is neither` |
| `x: noalias [4]f32` | `'noalias' cannot qualify an array; pass a slice, '[]f32'` |
| a local, global or struct field | `'noalias' applies to proc parameters only` |
| a return type | `'noalias' applies to proc parameters only` |
| `x: []noalias *T`, `x: *noalias *T` | `'noalias' qualifies a parameter's own pointer, not one it points to` |
| `x: noalias noalias []T` | `'noalias' is already given` |
| a `noalias []T` parameter on a variadic proc | see Part 2 |

On an `[external]` proc it is accepted and changes nothing: the C header
decides, and there is no rin body to check it in. On `[external_emit]` it is
emitted in the prototype. C does not check it against the header's: a
qualifier on a parameter is ignored when two declarations of a function are
compared, so a mismatch is accepted silently either way. A slice parameter
on an external proc gets no helper, since there is no body to move.

It is accepted on the parameters of a proc-pointer type, where it changes
nothing, since such a type has no body to check and is compared without it
(1.5).

## 1.4 Meaning

For the duration of a call: if memory reached through a `noalias` parameter
is modified during the call, that memory is accessed during the call only
through that parameter or through pointers derived from it. Indexing,
sub-slicing, `x[i]&` and arithmetic on a `[*]T` all derive. Overlap that is
only read is allowed.

That is C's rule for a `restrict` parameter, scoped to one call. Two
`noalias` slices may point into the same allocation, such as two columns
pushed from one arena block, as long as their ranges do not overlap.

Breaking the rule is undefined behaviour in a build without the Debug checks.
An add loop called with `dst` one element past `src`, both promised apart,
first 8 results:

| build | result |
|---|---|
| gcc or clang `-O0` | `1 2 3 4 5 6 7 8` |
| gcc `-O3` | `1 2 2 2 2 3 2 2` |
| clang `-O3` | `1 2 2 2 2 2 2 2` |
| gcc `-O3`, promise removed | `1 2 3 4 5 6 7 8` |

The answer depends on the optimisation level and the compiler, and nothing
crashes. That is the case for checking it.

## 1.5 Types

`noalias` is not part of type identity.

- A `[]f32` argument passes to a `noalias []f32` parameter, and inside the
  body the parameter behaves as a plain `[]f32`.
- Proc-pointer types compare without it: `*proc(dst: noalias []f32) -> void`
  and `*proc(dst: []f32) -> void` are the same type, as C ignores parameter
  qualifiers when comparing function types. 1.7 keeps every proc's C
  signature exactly what it would be without `noalias`, which is what makes
  this hold for slices too.
- It does not create a separate generic instance. Reflection is unchanged,
  since parameters are not reflected.

## 1.6 Debug checks

At the start of the proc, one line per proc, compiled out under `NDEBUG` like
`RIN_INDEX`:

    RIN_NOALIAS(2, dst.data, dst.length * sizeof(f32), "dst",
                   src.data, src.length * sizeof(f32), "src");

**Which pairs.** Every `noalias` parameter against every other pointer or
slice parameter, `noalias` or not, unless both point at const. A proc with k
such parameters has at most k(k-1)/2 pairs, tested in a loop inside the check,
so the generated C stays one line however many there are. A proc with fewer
than two pointer or slice parameters has nothing to compare, and no check.

**Stricter than 1.4.** The check cannot know whether the body writes, so it
treats every non-const parameter as written: an overlap that 1.4 allows,
because neither side is written in this call, still stops a Debug build.
Writing the parameter `const` says it is not written.

Each entry is the pointer, its extent in bytes, the name, and two flags,
`RIN_NOALIAS_PARAM` and `RIN_NOALIAS_WRITE`, all from `core.h`:

    RIN_NOALIAS(2, dst.data, (u64)dst.length * sizeof(f32), "dst", 3,
                   src.data, (u64)src.length * sizeof(const f32), "src", 0);

**Extents.**

| kind | extent | what is caught |
|---|---|---|
| `[]T` | `length * sizeof(T)` | any overlap |
| `*T` | `sizeof(T)` | any overlap |
| `[*]T` | unknown | equal addresses only |

A zero-length slice overlaps nothing. A failure prints
`file:line: noalias parameters 'dst' and 'src' overlap` and calls `abort()`,
like a failed bounds check. The compiler emits a `#line` for the proc's
declaration before the check, so `file:line` names the `.rin` source.

**Not checked:**

- globals reached from the body;
- pointers inside struct parameters, such as the columns of a `*kayan_world`;
- anything reached through pointers stored elsewhere;
- offset overlap of a `[*]T`;
- other threads;
- `[external]` procs, which have no rin body;
- a write through a pointer whose `const` was dropped with `cast`.

## 1.7 Lowering to C

**`*T` and `[*]T`:** `restrict` on the C parameter, nothing else.

    out: noalias *mat4         ->  mat4 *restrict out
    src: noalias *const mat4   ->  const mat4 *restrict src

**`[]T`:** a slice is a struct, and C's `restrict` only works on a pointer
parameter, so the compiler gives the body one. Whenever a proc has a
`noalias` slice parameter, it is emitted as two functions:

- a helper, `static inline` and `always_inline`, named `rin_noalias_` plus the
  proc's mangled name. It takes each `noalias` slice as a `restrict` pointer
  and a length, rebuilds the slice on its first line, and holds the body
  unchanged.
- the public proc, with exactly the signature it would have without
  `noalias`. It runs the Debug check and calls the helper.

The rin source:

    add_into: proc(dst: noalias []f32, src: []const f32, k: f32) -> void = {
        for (i: u64 = 0; i < dst.length; i += 1) {
            dst[i] += src[i] * k;
        }
    }

What rin emits today, without `noalias`:

    void add_into(slice_f32 dst, slice_const_f32 src, f32 k) {
        for (u64 i = 0; i < dst.length; i += 1) {
            dst.data[RIN_INDEX(i, dst.length)] += (src.data[RIN_INDEX(i, src.length)] * k);
        }
    }

What it emits with it:

    RIN_ALWAYS_INLINE void rin_noalias_add_into(f32 * RIN_RESTRICT dst_rin_data, u64 dst_rin_length,
                                                slice_const_f32 src, f32 k) {
        slice_f32 dst = { dst_rin_data, dst_rin_length };
        for (u64 i = 0; i < dst.length; i += 1) {
            dst.data[RIN_INDEX(i, dst.length)] += (src.data[RIN_INDEX(i, src.length)] * k);
        }
    }

    void add_into(slice_f32 dst, slice_const_f32 src, f32 k) {
        RIN_NOALIAS(2, dst.data, (u64)dst.length * sizeof(f32), "dst", 3,
                       src.data, (u64)src.length * sizeof(const f32), "src", 0);
        rin_noalias_add_into(dst.data, dst.length, src, k);
    }

Built and run: Release assembly for `add_into` drops from 86 to 58 lines
under gcc `-O3` and from 76 to 49 under clang `-O2`, the difference being the
runtime overlap checks and the fallback loop. The Debug build stops on an
overlapping call with the message from 1.6.

**Why a helper.** Every simpler lowering was measured on a four-slice kernel:

| lowering of a `noalias` slice | gcc `-O3` | clang `-O2` | outside the proc |
|---|---|---|---|
| none, as today | 157 lines, overlap checks | 152 lines, overlap checks | unchanged |
| `restrict` locals copied from `.data` | 157 lines | 151 lines | unchanged |
| `restrict` inside the slice struct | 87 lines | 152 lines | new slice types |
| pointer and length split in the proc's own parameters | 87 lines | 58 lines | signature, call sites and proc pointers change |
| helper with `restrict` parameters | 87 lines | 58 lines | unchanged |

Both compilers ignore `restrict` on locals, and clang ignores it on struct
fields, so a pointer parameter is the only place it works on both. Splitting
the proc's own parameters works too, but changes the proc's C signature, so
the change spreads to every call site, the generated header and proc-pointer
types. The helper keeps it inside the proc.

clang's `__builtin_assume_separate_storage` was also tried, and rejected:
see 1.10.

**Why `always_inline`.**

| helper marked | separate function at `-O0` | at `-O2` |
|---|---|---|
| `static inline` | yes, gcc and clang | no |
| `static inline` + `always_inline` | no | no |

With it, no build ever has an extra function or an extra call-stack frame,
and Release code is identical either way. The promise survives inlining, so
the inlined body keeps its `restrict` benefit.

**Details.**

- Generic procs get one helper per instance, as monomorphisation already does.
- Calling-convention attributes such as `proc[WINCALL]` stay on the public
  function. The helper is internal and uses the default.
- A recursive call in the body goes through the public function, so each
  level is checked.
- `restrict` parameters and `noalias` slices can be mixed: the helper takes
  every parameter, and only the `noalias` slices are split.
- The helper's split parameters are named `<param>_rin_data` and
  `<param>_rin_length`; the first line of its body rebuilds `<param>`.
- Variadic procs: Part 2.

**Every compiler.** The generated C names no compiler-specific spelling. It
writes two macros, defined once in `core.h`:

    #define RIN_RESTRICT __restrict
    #if defined(_MSC_VER) && !defined(__clang__)
    #define RIN_ALWAYS_INLINE static __forceinline
    #else
    #define RIN_ALWAYS_INLINE static inline __attribute__((always_inline))
    #endif

`__restrict` is accepted in C by gcc, clang, clang-cl and MSVC's `cl`
whatever the standard flag, where `restrict` needs C99 mode and `cl` only
takes it under `/std:c11`. clang-cl defines `_MSC_VER` as well and takes the
attribute. One difference remains: `cl` ignores `__forceinline` in a Debug
build (`/Od`), so there the helper is a real function and an extra frame in
the call stack. Release code, and everything under gcc, clang and clang-cl,
is as described above.

## 1.8 Tests

Each one can fail:

1. **Emission.**
   - `*T` and `[*]T` put `restrict` on the C parameter;
     `noalias *const T` emits `const T *restrict`.
   - A `noalias` slice produces the helper and an unchanged public signature.
   - At `-O0`, no `rin_noalias_` symbol is left in the object file.
2. **Errors.** Every row of the table in 1.3, and the variadic one in 2.2.
3. **Debug traps.**
   - Overlapping slices abort with the message.
   - Disjoint slices pass, and so do const-const overlaps.
   - Two disjoint slices of one arena block pass.
   - Equal `[*]T` pointers abort.
   - `m4_mul(m&, m&, b&)` aborts.
   - The same program built with `NDEBUG` runs without the checks.
4. **Proc pointers.** A proc with `noalias` slice parameters is stored in a
   plain proc-pointer type and called through it.
5. **Codegen.** The six-column movement loop vectorizes with `noalias`
   slices and does not without. This is the test the feature exists to pass.
   It is checked by hand rather than in the suite, since vectorizer output
   is not stable across compiler versions: see the Windows table in 1.1.

As built: 1 to 4 are `noalias` in `tests/run_tests.py`, which checks the
emitted C, every error, and the traps in a Debug and an `NDEBUG` build. 1 and
4 also run as `tests/rin-torture/execute/021-noalias.rin`, under clang, clang
`--single-file` and MSVC `cl`, which is what proves `RIN_RESTRICT` and
`RIN_ALWAYS_INLINE` on every compiler. The `-O0` symbol check in 1 is not
automated.

## 1.9 Open

- Also check at the call site, so calls into `[external]` procs with `noalias`
  parameters are covered.
- Whether rinbind should emit `noalias [*]T` for C parameters declared
  `restrict`, which today it drops.
- A checked-release mode that keeps the overlap checks with optimisation on.

## 1.10 Rejected

- **On every parameter by default.** Rust can, because its borrow checker
  proves it. rin has none, so every in-place call would become undefined
  behaviour.
- **As a parameter attribute, `dst: []f32[noalias]`.** Parameters take no
  attributes.
- **A proc attribute marking procs whose lowering differs.** Considered while
  the lowering changed signatures. The helper changes nothing a caller sees,
  so the attribute would mark nothing.
- **Only on `*T` and `[*]T`, as in C.** It would push hot kernels back to
  pointer plus length, against the direction of
  [pointers-and-slices.md](pointers-and-slices.md).
- **`restrict` locals, `restrict` struct fields, split parameters.** Measured
  in 1.7.
- **clang's `__builtin_assume_separate_storage(a, b)` instead of the helper.**
  It works mechanically: on four slices passed as structs, one call per pair
  of `.data` pointers removed the overlap checks (104 instructions down to 48
  under clang 18 `-O2`, still vectorized) with no helper and no signature
  change. But it promises a different thing. clang's documentation: the
  arguments point into separately allocated storage, meaning "the outermost
  enclosing allocation", so "it's never correct to call this function passing
  the addresses of ... elements of the same array". Columns pushed from one
  arena block are one allocation, so the builtin would be undefined behaviour
  in exactly the case `noalias` is for. It is also clang-only.
- **Calling it `hot`.** gcc and clang already have a `hot` attribute meaning
  "optimise harder", so a reader would take it for a speed hint.
- **`[*]T` tied to a length, `[*]f32[noalias(count)]`.** It duplicates what a
  slice already is.

---

# Part 2. Variadic procs

## 2.1 What C allows

- `restrict` works on the named parameters of a variadic function. The C
  library does it: `int printf(const char *restrict format, ...);`.
- The `...` arguments cannot be `restrict`. They have no declared type to
  qualify.
- `va_start` only works in the function that declares `...`. gcc and clang
  both reject it anywhere else: "'va_start' used in function with fixed
  arguments".

A body that reads its `...` arguments does so today through C's `va_start`,
bound by hand as an external, so the body has to stay in the variadic
function itself.

## 2.2 The rule today

- `noalias` on `*T` and `[*]T` is allowed on variadic procs. It lowers to a
  qualifier on the parameter and needs no helper, as `printf`'s
  `const char *restrict format, ...` does in C.
- `noalias` on `[]T` is an error on a variadic proc:

      'noalias' on a slice cannot be used in a variadic proc; only the proc
      that declares '...' can read its arguments

  The slice lowering moves the body into a helper (1.7), and a helper cannot
  call `va_start`.

The same shape can be written by hand today: a thin variadic proc that reads
its arguments and calls a non-variadic proc that takes the `noalias` slices.

## 2.3 Once rin has built-in access to `...`

The error is temporary. With a compiler-owned argument list, for example
read with `va_arg<T>(args)`, the compiler rather than the body calls
`va_start` and `va_end`. The public proc then starts the list and hands it to
the helper as an ordinary `va_list` parameter, the split C already uses
between `printf` and `vprintf`:

    static inline __attribute__((always_inline))
    void rin_noalias_scale_all(f32 *restrict dst_data, u64 dst_length,
                               slice_const_f32 src, i32 count, va_list args) {
        slice_f32 dst = { dst_data, dst_length };
        /* the body, reading its arguments from `args` */
    }

    void scale_all(slice_f32 dst, slice_const_f32 src, i32 count, ...) {
        RIN_NOALIAS(2, dst.data, dst.length * sizeof(f32), "dst",
                       src.data, src.length * sizeof(f32), "src");
        va_list args;
        va_start(args, count);
        rin_noalias_scale_all(dst.data, dst.length, src, count, args);
        va_end(args);
    }

Built with gcc and clang at `-O0` and `-O2`: no warnings under
`-Wall -Wextra`, the same results in every build, no helper symbol left, and
the loop still vectorized.

Built-in `...` is worth having for typed varargs on its own; lifting this
error is a side effect.

---

# Open questions

- Built-in access to `...`, which lifts the error in 2.2 (2.3).
- Call-site checks for `[external]` procs, rinbind and C's `restrict`, a
  checked-release mode (1.9).

# See also

- [codegen-findings.md](codegen-findings.md): linkage, float contraction,
  SIMD types and the toolchain, from the same work.
- [pointers-and-slices.md](pointers-and-slices.md): the pointer kinds `noalias`
  builds on, and the bounds checks it copies.
- [attributes.md](attributes.md): why parameters take qualifiers, not
  attributes.
- [safety.md](safety.md): runtime checks and what they cover.
- [external-vs-external_emit.md](external-vs-external_emit.md): when C sees a
  prototype rin emits.
