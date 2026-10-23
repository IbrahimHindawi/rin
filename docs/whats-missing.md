# What Is Missing

A survey of the language and compiler, measured against njinn rather than
against a wish list. Everything here was probed against the compiler as it
stands; the numbers come from njinn's source.

## Where it already stands up

Worth stating first, because it changes what the gaps cost.

- **318 discriminating checks**, mutation-tested. The suite fails when a
  lowering is deliberately broken, which is the only property that means
  anything.
- **481 ms to compile njinn** (measured at 28k lines) -- now 36k lines of rin
  into 52k lines of C.
- **Order-independent declarations.** A type or proc may be used before it is
  declared. No forward declarations, no header discipline.
- **Nominal typing where it counts.** Structs do not interconvert, pointers do
  not interconvert without a cast, `const` is tracked through the type and
  through argument passing, arity is checked, and two different enums cannot be
  compared.
- **Reflection with verified layout** -- 952 `_Static_assert`s comparing
  declared external records against the real C ones.
- **Diagnostics report more than the first error** and map back to the `.rin`
  line through `#line`.

## 1. Error handling, and discarded results

### The measurement

    268 procs return b32
    call sites that use the result:  665
    call sites that discard it:      145   (18%)

Including `dx11ops_create_bloom_targets(dx);` -- a GPU resource creation whose
failure signal goes nowhere. Some of those 145 are deliberate fire-and-forget.
Some are bugs. Nothing distinguishes them, and nothing will without a **must-use**
notion: a proc marked such that discarding its value is a diagnostic.

**Done: `enum[must_use]`.** The marker goes on the status type, not the proc, so
one attribute covers every proc that returns it. A bare call whose result is
such an enum is a type error, and `_ = f();` discards it on purpose (lowered to
`(void)(f());`). See [attributes.md](attributes.md). It covers status enums,
not `b32`: those 145 sites are only checked once they return a status type, which is
the direction §1 already chose.

### Error handling: decided

**A status enum per domain, the value through an out-parameter, the message from
reflection.** `result<T>` is not the direction.

    mesh_status: enum[must_use] = { success, file_not_found, bad_magic, truncated, }

    mesh_load: proc(mesh: *geo_mesh) -> mesh_status { ... }

    e: mesh_status = mesh_load(mesh&);
    if (e != mesh_status.success) {
        printfmt("load failed: {}\n", e);   // prints the member name
        return -1;
    }

What it gets right:

- **The enums are nominally typed.** `mesh_status` and `file_status` cannot be
  compared -- the compiler rejects it. Better than C's `int` codes and better
  than the `b32` used 303 times in njinn, where every failure looks alike.
- **The message is derived, not written.** Reflection produces the member name,
  so adding `truncated` makes `"truncated"` print with nothing else to update.
  Every C codebase has a hand-written `switch (err)` returning strings that
  goes stale; this one cannot.
- **It costs nothing.** It is an int, and it compiles to an int.
- **The out-parameter avoids a copy.** `result<geo_mesh, E>` returns the mesh by
  value, and rin has no moves, so that is a copy of everything in it.

The pattern lives in the language rather than in each project:

- **`{}` on an enum prints its member name.** It goes through the reflection
  record every enum already has (`print_enum` in `std/print.rin`), and a value
  that is not a member prints as `mesh_status(7)`. A hand-written `print<E>`
  still takes priority. Covered by `printfmt_enum`.
- **For a string rather than output,** `reflect_name_from_value_or(e<>&,
  cast(e, i32), fallback)` is in `std/reflect.rin`. `<>` works on a value, so
  the status variable names its own record. Covered by `status_enum_errors` and
  `reflect_of_value`.

### Why not `result<T, E>`

Worth writing down, because the obvious argument for it does not survive
contact.

`result` does *not* remove cross-domain conversion. Rust's `?` requires
`impl From<FileError> for MeshError` -- you still write the mapping, `?` just
calls it. The gain is that it is declared once per pair rather than spelled at
each call site. Real, but modest.

What actually makes errors compose in Rust is **payload-carrying enums**
(`enum MeshError { File(FileError), BadMagic }`), which is a separate feature
from `result`. And rin can already express that by hand -- a struct with a
tag field and a payload:

    mesh_status: struct = {
        kind: mesh_kind;
        file: file_status;   // meaningful when kind == from_file
    }

More verbose, and nothing checks that `file` is read only when `kind` says so,
but it works today with no new feature.

So `result` would buy three things, and two of them are not `result`: value and
error travelling together (needs exhaustive matching to enforce), composition in
expressions (needs a propagation form), and forced handling (needs must-use).
Without those it is a struct whose value can be read after a failure -- which is
what `std/result.rin` is, and why it has three uses.

### What would strengthen the decided design

**Enum exhaustiveness in `switch`** -- done (`switch_exhaustive`). Handle a `mesh_status` with a
`switch`, add `truncated` to the enum, and every handler that does not cover it
fails to compile. That is the property people actually want from `result` plus
`match`, on a plain enum, with no generics, no propagation operator and no
copies.

## 2. No `defer`

**Delayed** (2026-10-23). Not rejected; parked behind must-use.

Manual memory, 199 abort sites, and no scoped cleanup. Arenas carry most of the
weight in njinn, which is why this has not bitten harder -- but every early
return that owns a resource is a hand-written unwind.

This is the single most commonly cited C ergonomic fix and both Go and Zig have
it. It is also cheap: `defer` lowers to statements emitted at each scope exit,
and rin already computes those points for `break`/`continue` checking.

## 3. Naming: the module prefix tax

**1036 of njinn's 1130 top-level procs (91%) carry a hand-written module
prefix**, across 46 distinct prefixes -- `gin_` (205), `gops_` (201), `fxops_`
(106), `fxed_` (71), `guiops_` (59), `resops_` (52).

That is C-era bookkeeping the language could own, enforced by nothing but
habit.

**Methods are permanently out of scope.** Decided, not deferred: rin will not
grow `P.get: proc(self: *P)` or any other form of a proc bound to a type. This
is recorded so it is not proposed again.

Which leaves **module namespaces** (§10 of `shape.md`, parked) as the only
lever on that 91%.

**Delayed** (2026-10-23), along with multi-file C output. A namespace turns
`mem_arena` into `mem.arena`: a `.` instead of a `_`, which is not worth a
module system on its own.

## 4. Loop and return forms: decided against

- **No `for (v in xs)`.** It hides the index, and nearly every loop in njinn
  uses it. The index-style loop stays the only loop.
- **No multiple return values.** A value plus a status already has its shape
  (the out-parameter, §1), and a genuine pair is a small named struct. Tuples
  would also break the `name: kind = value` declaration shape.

## 5. Cheap correctness checks that do not exist

Each of these is accepted today, and each is a known bug source:

| accepted | note |
|---|---|
| ~~`switch` over an enum missing cases~~ | done: an error unless there is a `default` |
| an unused local | clang warns at `-Wall`; rin says nothing |
| ~~a discarded return value~~ | done for status types: `enum[must_use]` |
| assigning to a parameter | shadows the caller's intent silently |
| unreachable code after `return` | |
| `sizeof` on an opaque `external` | rin accepts, clang then reports the name as undeclared |

The remaining rows are cheap, and each catches a real class of bug.

## 6. Diagnostics cascade

Three undeclared types produce six errors -- each bad declaration also reports
a follow-on initializer mismatch. Reporting all errors is right; reporting
consequences of an error already reported is noise.

## 7. Compiler shape

`src/main.c` is **19,135 lines** in one file. Not a language problem, and not
urgent while one person works on it, but it is the reason a change like the
preprocessor rework touches lexer, parser, semantics and emitter in the same
file with no boundary between them.

There is also no self-hosting path, and there cannot be one until `std` grows an
OS layer -- see [`rin-build-story.md`](rin-build-story.md), which measures how far
off that is.

## Suggested order

1. **Done:** enum exhaustiveness in `switch`.
2. **Done:** must-use, as `enum[must_use]`.
3. **Next:** the cheap checks in §5, and the diagnostics cascade (§6).
4. **Delayed:** `defer`, module namespaces, multi-file C output.

**Decided, not deferred:** no methods, in any form; no `for (v in xs)`; no
multiple return values.

**Decided:** errors are a status enum per domain, the value comes back through
an out-parameter, and the message comes from reflection. `result<T>` is not the
direction, and neither is a propagation form. Reasoning in §1.
