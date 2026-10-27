# C codegen: linkage, floats, SIMD, toolchain

> **Proposed** (2026-10-26). Only Part 6 is implemented (2026-10-27). What porting the
> rin-dod lessons to plain C (c-dod) showed about the C that rin emits.
> Measured on a Linux build of rin with gcc 13.3 and clang 18.1 on a 2.8 GHz
> Xeon unless a section says otherwise. Timings on that shared machine vary by
> about 20 percent between runs; the patterns quoted held in every run.

| part | topic | decision | status |
|---|---|---|---|
| 1 | linkage | `static` on module-level tunables now; the compiler infers internal linkage later | proposed |
| 2 | float contraction | rin never fuses `a * b + c` implicitly; `fma` is explicit | proposed |
| 3 | SIMD vector types | not decided; the constraints if they are added | open |
| 4 | toolchain | clang and clang-cl are the reference; no LLVM backend for now | proposed |
| 5 | `must_use` | no change: rin checks it, C never sees it | already right |
| 6 | printing `u64` on Linux | use `PRIu64`-style formats | implemented |

`noalias` and variadic procs are in [noalias-variadic.md](noalias-variadic.md).

---

# Part 1. Linkage of globals and procs

## 1.1 Finding

As far as the C compiler knows, a global with external linkage can be
changed by any store through a pointer of its type, since code in another
file may hold its address. So a loop that reads a global `f32` and writes
`f32` through a pointer reads the global again on every iteration. A global
with internal linkage whose address is never taken cannot be reached by any
pointer: the compiler reads it once, and folds it into a constant if nothing
writes it.

rin already has internal linkage, `x: static T` and `p: static proc`. Without
it, module-level globals get external linkage:

    gravity: f32 = -9.8;           ->  f32 gravity = -9.8f;
    gravity: static f32 = -9.8;    ->  static f32 gravity = -9.8f;

## 1.2 Measured

A two-column loop reading global `gravity` and `dt`, built at `-O2`:

| globals | gcc | clang |
|---|---|---|
| external | both read inside the loop every iteration | both read inside the loop, not vectorized |
| `static`, written elsewhere | read once before the loop | read once, loop vectorized |
| `static`, never written | folded into a constant | folded into a constant |
| `static`, address taken anywhere | read inside the loop again | read inside the loop again |

The last row matters for games: handing `&gravity` to a debug slider brings
the reloads back.

Timed:

| kernel | gcc `-O2` external | gcc `-O2` `static` | clang `-O2` external | clang `-O2` `static` |
|---|---|---|---|---|
| c-dod lesson 7, six `f32` columns, tunables read in the loop, 8192 particles, ns per particle | 4.48 | 3.20 | 1.10 | 1.14 |
| rin-dod lesson 1 `move_soa`, 262144 particles, ms | 25.9 | 26.4 | 37.4 | 37.5 |

gcc is 30 percent faster in cache. clang is unchanged: over four runs each,
external ranged 1.10 to 1.41 and `static` 1.14 to 1.33.
rin-dod lesson 1 was built through rin with `gravity` and `dt` made
`static`. Both compilers folded `gravity` into a constant, with no load of it
left in `move_soa`, and it made no measurable difference to that loop.

## 1.3 Proposal

**Now, no compiler change.** Document that module-level tunables and procs
private to a module should be `static`. It is never slower, sometimes faster,
and it states intent.

**Later, in the compiler: infer it.** rin sees the whole program, so it knows
which globals and procs no other translation unit refers to, and can emit
those `static` without the user writing it. Limits:

- **Only in builds that produce an executable.** A library built with
  `--header` exposes its non-`static` names to C, and inference would remove
  them from its public surface.
- **Not monomorphs in the module build.** [modules.md](modules.md) gives them
  external linkage on purpose: one definition in `i_monomorphs.c`, prototypes
  everywhere else.
- **C code linked into the same program** may call a rin proc by name, which
  rin cannot see. It needs a way to say so, such as an export attribute, or
  inference stays off for that build.
- **Unused warnings.** A `static` function nothing calls is a
  `-Wunused-function` warning on gcc and clang, and an error under `-Werror`.
  c-dod hit this with a helper used only inside an `assert`, which vanishes
  under `NDEBUG`. Inferred-`static` procs that nothing references are either
  not emitted or emitted with `__attribute__((unused))`.

## 1.4 Tests

- A global used only by its own module is emitted `static` in an executable
  build and not in a `--header` build.
- The six-column kernel reading a global tunable has no load of it inside the
  loop under gcc `-O2`.
- A program with an unreferenced private proc builds under
  `-Wall -Wextra -Werror` in Debug and with `NDEBUG`.

---

# Part 2. Floating-point contraction

## 2.1 Finding

C lets a compiler fuse `a * b + c` into one fused multiply-add, which rounds
once instead of twice and so changes the last bits. Whether it happens
depends on the compiler, the flags, the target and the optimisation level.
Checked in the generated assembly for `p + v * dt`:

| compiler and flags | target | fused |
|---|---|---|
| clang-cl `/Od` or `/O2`, no `/arch` | x64 | no |
| clang-cl `/arch:AVX2`, `/Od` | x64 | **yes** |
| clang-cl `/arch:AVX2`, `/O2` | x64 | **yes** |
| clang `-mfma` or `-march=native`, `-O0` | x64 | **yes** |
| clang, no flags, `-O0` | ARM64 | **yes** |
| gcc default mode (`gnu17`), `-mfma -O2` | x64 | **yes** |
| gcc default mode, `-mfma -O0` | x64 | no |
| gcc `-std=c11`, `-mfma -O2` | x64 | no |
| clang-cl `/arch:AVX2 /O2` with `#pragma STDC FP_CONTRACT OFF` | x64 | no |
| clang `-mfma -O2` with the pragma | x64 | no |

The clang-cl rows were compiled with clang 18 in cl mode targeting
`x86_64-pc-windows-msvc`, and the ARM64 row with `-target aarch64-linux-gnu`;
those were inspected, not run.

So the same rin program gives different floats on x64 and ARM64, with and
without `/arch:AVX2`, and under gcc in Debug and Release. rin-dod lesson 1,
built through rin:

| build | `checksum aos` |
|---|---|
| clang, no flags | 38572460.505 (the `.expected` value) |
| clang `-mfma`, `-O0` | 38572460.519 |
| clang `-march=native`, `-O0` | 38572460.519 |
| gcc `-std=c11 -O2 -mfma` | 38572460.505 |

Its byte-for-byte output check fails under the clang builds. In a game the
same drift breaks replays, lockstep networking and golden-output tests.

## 2.2 Proposal

rin floats never fuse implicitly. Fusing is something code asks for.

- Every generated `.c` begins with

      #if defined(__clang__)
      #pragma STDC FP_CONTRACT OFF
      #endif

  The pragma is standard C and clang and clang-cl honour it. The guard is for
  gcc, which ignores it and warns under `-Wall`
  (`ignoring '#pragma STDC FP_CONTRACT' [-Wunknown-pragmas]`), which fails a
  `-Werror` build.
- When the C compiler is gcc, `rin build` passes `-ffp-contract=off`.
- `rin build` never passes `-ffast-math` or `/fp:fast`, which allow far more
  than fusing.
- std gets an explicit `fma(a, b, c)` lowering to C's `fmaf` and `fma`: one
  rounding, on every target, where the code wants it. On a target without FMA
  hardware it is a slow library call.

With this, rin-dod lesson 1 passes under `-march=native` with no change to
the lesson.

**Cost.** Kernels that gained from implicit fusing lose it until they use
`fma`. Not measured.

## 2.3 Tests

- A program printing `a * b + c` at full precision gives the same output
  under clang `-O0` and `-O2`, clang `-mfma`, clang-cl `/arch:AVX2`, gcc in
  its default mode at `-O2 -mfma`, and an ARM64 build where one is available.
- rin-dod's checks pass with clang `-march=native`.
- The generated C builds under gcc `-Wall -Werror`, so the guard is there.

---

# Part 3. SIMD vector types

**Not decided.** These are the numbers and the constraints, for whenever the
question comes back.

## 3.1 What c-dod measured

c-dod lesson 8, the movement update, nanoseconds per particle. "avx2" is
`-mavx2 -mfma`.

| kernel | in cache, clang `-O2` | in cache, gcc `-O2` | in cache, gcc `-O3` | in cache, clang avx2 | from RAM, clang `-O2` |
|---|---|---|---|---|---|
| `vec3` columns, plain loop | 2.28 | 1.54 | 2.83 | 1.44 | 3.21 |
| `vec4` columns, plain loop | 1.50 | 1.51 | 2.07 | 1.32 | 3.31 |
| `f32` columns, plain loop | 0.81 | 2.44 | 0.62 | 0.59 | 2.06 |
| `f32` columns, SSE intrinsics | 0.81 | 0.82 | 0.81 | 1.04 | 2.07 |
| `f32` columns, 8-wide vector types | 0.59 | 1.12 | 1.13 | 0.46 | 2.06 |
| `f32` columns, AVX2 intrinsics | | | | 0.60 | |

- **Layout comes first.** `f32` columns beat `vec3` three to five times in
  cache. From RAM every `f32`-column version runs at about 2.1 and only the
  layout matters. Padding to `vec4` does not pay.
- **A plain loop's speed belongs to the C compiler.** The same `f32`-column
  loop ranged from 0.48 to 2.44 ns across compilers and flags; SSE intrinsics
  were 0.81 to 0.82 on every compiler without AVX. A vector type is the only
  way rin could promise SIMD whatever compiles its output.
- An explicit vector kernel does not depend on the vectorizer, so it needs no
  `noalias` to be vectorized. `noalias`
  ([noalias-variadic.md](noalias-variadic.md)) is for the plain loops.

## 3.2 Constraints, if added

Each of these came up in c-dod:

1. **Lower to `vector_size`**, which gcc, clang and clang-cl support. Plain
   MSVC `cl.exe` has no vector types and would need intrinsics.
2. **Default to the target's native width.** Without AVX an 8-wide vector is
   two 4-wide halves, and gcc made a poor job of it: 1.12 ns against 0.82 for
   4-wide SSE. clang was fine. Either `f32x4` and `f32x8` with the width
   chosen to match the build, or one type whose width the target decides.
3. **No vector values passed or returned by value across calls that are not
   inlined.** The calling convention for a 32-byte vector depends on whether
   AVX is enabled, and clang warns about it (`-Wpsabi`). Pass by pointer,
   force inlining, or require one set of target flags for the whole program.
4. **Load and store through `memcpy`** from `[*]f32` or `[]f32`, never by
   casting a `*f32` to a vector pointer, which breaks C's aliasing rules.
   `memcpy` compiles to a single vector move and needs no alignment.
5. **Runtime dispatch,** if wanted, maps to `__attribute__((target("avx2")))`
   on a proc plus `__builtin_cpu_supports("avx2")`. Not tried in rin; it would
   be a proc attribute.

---

# Part 4. Toolchain

- **clang and clang-cl are the reference C compilers.** `rin build` already
  defaults to clang-cl. Everything in this document was checked on clang;
  gcc is noted where it differs.
- **If gcc builds are supported, Release uses `-O3`.** gcc 13 at `-O2`
  vectorizes far less: the plain `f32`-column loop is 2.44 ns at `-O2` and
  0.62 at `-O3`, and the `restrict` kernel of c-dod lesson 7 is 2.51 against
  0.77.
- **No LLVM backend for now.** Emitting LLVM IR would give `noalias` and
  alias scopes on any access, exact control of contraction, and vector types
  on every target. It would cost the C ABI, which LLVM leaves to the front end
  (Windows x64 and System V struct passing for every D3D11 and Win32 call),
  header generation, `#line` debugging, and an LLVM dependency whose API
  changes every release. What rin needs today is reachable from clang's C:
  `restrict`, the contraction pragma, `vector_size`, `target` attributes.
  Revisit for things C cannot say: rin's own calling convention, per-operation
  overflow rules, alias scopes on arbitrary accesses, compile speed.

---

# Part 5. must_use: no change

rin checks `enum[must_use]` itself, as a type error, with `_ = f();` as the
discard, and emits no C attribute. Keep it that way. The C spellings disagree:
gcc's `__attribute__((warn_unused_result))` warns even through a `(void)`
cast, clang's does not, and C23's `[[nodiscard]]` is silenced by `(void)` on
both. Emitting any of them would make rin's discard rule depend on the C
compiler.

---

# Part 6. Printing u64 on Linux

`std/core.h` defines `u64` as `uint64_t`, which is `unsigned long` on Linux
and macOS and `unsigned long long` on Windows. `printfmt` and std's print use
`%llu` for `u64`, and the same for `uintptr` and `uintmax`, so every print of
one is a `-Wformat` warning outside Windows. rin-dod's `check.py` passes
`-Wno-format` on non-Windows targets to get past it.

**Fix:** emit the `<inttypes.h>` formats, `"%" PRIu64` for `u64` and
`"%" PRId64` for `i64`, and `PRIuPTR` and `PRIuMAX` for the others. Do not
change the typedefs instead: a `*u64` has to stay compatible with C's
`uint64_t *`.

**Test:** rin-dod builds on Linux without `-Wno-format`.

**Implemented** (2026-10-27): `printfmt` emits the `<inttypes.h>` formats for
`i64`, `u64`, `b64`, `intptr`, `uintptr`, `intmax` and `uintmax`, and `%td`
for `ptrdiff`. `print<u64>` and `print<i64>` call `rin_print_u64` and
`rin_print_i64` in `core.h`, since rin source cannot name a format macro.
Checked on Windows only so far.

---

# Open questions

- How C code in the same program marks a rin proc it calls, once linkage is
  inferred (1.3).
- Contraction under plain MSVC `cl.exe`, which nothing here tested.
- Whether to add vector types, and how their width is chosen (Part 3).

# See also

- [noalias-variadic.md](noalias-variadic.md): the other half of this work.
- [modules.md](modules.md): why monomorphs have external linkage.
- c-dod, lessons 7 and 8: the C programs behind Parts 1 to 3.
