# `^T` for C's pointer, `p*` to dereference

> **Implemented** (2026-10-28). Proposed the same day.
> - `p*` (section 3) on `*T` and `^T`, with a spacing rule added in review: a
>   `*` glued to its operand and followed by a space is a dereference, so
>   `p* - 1` needs no parentheses (3.1). njinn's 7,609 `x[0]` on a `*T` were
>   rewritten with `tools/deref_migrate.py`, its generated C unchanged.
> - `^T`, with `[*]T` an error at once rather than after a warning period
>   (step 4 of section 8 skipped by choice), the `raw_` mangle (3.4), and
>   `rinbind` writing `^T` for every C pointer (5).
> - Reading or indexing through a `void` pointer is an error. An external
>   struct declared without fields is **not**: rin cannot know whether C sees
>   it as complete (`FILE` is), so the opaque row of the table in 3 stays C's
>   call.
> Supersedes the deprecated [c-pointer-spelling.md](c-pointer-spelling.md),
> and changes two spellings in [pointers-and-slices.md](pointers-and-slices.md);
> everything else there stands.

| part | change |
|---|---|
| types | `[*]T` is spelled `^T`. `*T`, `[]T` and `[N]T` keep their meaning and spelling. |
| dereference | a postfix `p*` reads through `*T` and `^T`. `p[0]` keeps working on both, and `p[n]` with `n` other than a literal `0` stays a compile error on `*T`, as today. |

Nothing that compiles today stops compiling except the `[*]` spelling. `p*`
is new: it gives the dereference a spelling of its own, so code can say
"follow this pointer" instead of "element zero".

---

## 1. Today

    *T    one element        only p[0]
    [*]T  C's pointer        p[i] unchecked, arithmetic
    []T   slice              p[i] checked, pointer + length
    [N]T  array              p[i] checked

Two things read badly.

**`p[0]` says "array" when there is none.** On a `*T` the only legal index is
the literal `0`, so `w[0].count` dresses a dereference up as element zero of
an array that does not exist. It is also the most common operator in rin
code: njinn has 8,711 `x[0]`, 7,332 of them field accesses.

    for (i: u32 = 0; i < w[0].count; i += 1) {
        if (w[0].has_position[i] && w[0].has_velocity[i]) {
            w[0].positions[i].x += w[0].velocities[i].x * dt;

**`[*]` is heavy for what it is.** C's pointer is the type every external
declaration and every `rinbind` header writes, and a pointer to pointers is
`[*][*]T`. That is rare (njinn has 9, mostly `argv` and C out-parameters), but
`[*]` itself is written about 1,400 times across std, njinn, rin-dx11 and
rin-dod.

## 2. Types

    *T    one element        p*  or  p[0]       p[n], n != 0: compile error
    ^T    C's pointer        p*  or  p[i]       unchecked, arithmetic
    []T   slice              p[i]               checked
    [N]T  array              p[i]               checked

- **`^T` is today's `[*]T` exactly**: the same C pointer, the same unchecked
  indexing and arithmetic, the same conversions to and from slices. Only the
  spelling changes.
- **`*T` is unchanged**: the checked, one-element pointer stays the short
  spelling, so the form people reach for first is still the safe one. That is
  what the deprecated proposal gave up, and why it was dropped.
- Types compose as they do today, prefix by prefix, read left to right:
  `^^c8` (`argv`), `^*ID3D11Buffer` (an array of interface pointers),
  `*^T`, `^const T`, `const ^T`.
- `^` in a type cannot collide with xor, which is only an expression operator,
  and `^^` is not a token, so it lexes as two.

## 3. Dereference

A postfix `*`:

    w*.count            field through a *T or ^T
    w* = v              write the whole value
    x: T = w*;          read it
    p*&                 back to the pointer
    pp**                through two levels (3.3)

Lowers to `(*w)` in C, which is what `w[0]` lowers to today.

| on | `p*` | `p[0]` | `p[i]` |
|---|---|---|---|
| `*T` | yes | yes | compile error, as today: `'*T' points at one element` |
| `^T` | yes | yes | yes, unchecked |
| `[]T`, `[N]T` | error: `'[]T' has many values; index it` | yes | yes, checked |
| `^void`, `^Opaque` | error: no value to read | error | error: `'void' has no size` |

`p*` and `p[0]` are the same operation on a pointer, as `*p` and `p[0]` are
in C. Which one to write is style; `p*` says what happens. The last row is
new only in that rin reports it: C already rejects reading or indexing
through a `void *` or a pointer to an incomplete type.

### 3.1 Parsing

`*` is also multiplication, so the parser decides by the next token:

- **a postfix dereference** when the next token cannot start an expression:
  `.` `)` `]` `;` `,` `=` `*`, or the end of the statement;
- **multiplication** otherwise.

rin accepts no leading-dot number (`.5` is a parse error), so a `.` after `*`
is never the start of an operand, and has no prefix `*`, so `* *` is never a
multiplication by a dereference.

### 3.2 Why a misparse is never silent

Every spelling the rule reads the wrong way comes out as a pointer multiplied
by something, and **multiplying a pointer is always a type error**: C rejects
it, and so does rin. The type checker turns each one into an error that names
the fix, so a program never compiles with a meaning its author did not write:

| written | parsed as | result |
|---|---|---|
| `w*.count` | deref, field | ✓ |
| `w* = v` | deref, assign | ✓ |
| `a* * b` | `(a*) * b` | ✓ |
| `w* - 1` | `w * (-1)` | error: `cannot multiply a pointer; to dereference, write '(w*) - 1'` |
| `x = w* + 1` | `w * (+1)` | the same error, `'(w*) + 1'` |
| `w*= v` | `w *= v` | error: `'*=' on a pointer; to assign through it, write 'w* = v'` |
| `cast(p, u64) * 2` | multiplication | ✓: an address as a number, written down |

Generic code is covered too: a body is type-checked per instance, so an
instance whose `T` is a pointer gets the same error at its own call.

**Future syntax to keep in mind.** If rin ever takes leading-dot enum literals
(`.red`, as Zig and Odin have), `w * .red` would start to look like a field
access. Such a literal would have to be written `Color.red` after a `*`, or the
feature would have to pick another spelling.

### 3.3 `**`

`pp**` dereferences twice. `**` is not a token (there is no power operator),
so it lexes as two `*`, and the rule above reads each in turn: the first is
followed by `*`, so it is a dereference, and so on. `**T` in a type is
unchanged.

### 3.4 Generics

Most of it falls out of what rin does already, because a generic body is
type-checked once per instance, with the concrete `T` substituted (as
`T<>` and `noalias` rely on):

- **Type arguments take the new spelling as they are**: `vec<^u8>`,
  `vec<*Node>`, `map<^const c8>`. `<^` and `^>` are not tokens, so nothing
  new lexes differently.
- **`x*` on a value of type `T`** is legal in the instances where `T` is a
  pointer and an error in the others, at the call that made the instance, the
  same as any other operation a type does not support. rin has no constraints
  on type parameters, so this is the C++-template model it already uses.
- **The parsing rule meets angle brackets safely.** `>` and `<` cannot start an
  expression, so `p* > q` is `(p*) > q`, which is what it means.

One change is needed. **`*T` and `^T` mangle to the same name today**:
`vec<*u8>` and `vec<[*]u8>` are both `vec_ptr_u8`, and `first<*u8>` and
`first<[*]u8>` are one instance, `first_ptr_u8` (measured). The two allow
different operations, `p[i]` and `p + 1` on `^T` only, so a shared instance
is type-checked once, against whichever argument created it, and a body like
`get: proc<T>(p: T, i: u64) -> u8 = { return p[i]; }` passes or fails
depending on which call the compiler saw first. This is a latent bug today,
with `[*]T`; the proposal is a good moment to fix it.

So **`^T` gets its own mangle**, `raw_u8` beside `ptr_u8`: `vec<^u8>` is
`vec_raw_u8`. Each instance is then checked against exactly what its caller
wrote. The C types are identical structs under two names, which matches rin
treating them as two types, and costs a few duplicate instances in a program
that uses both. It also changes names a user can see: `vec_raw_u8_reflect`,
natvis type names, and any C that names a generated instance.

## 4. Lowering to C

Nothing new reaches C:

| rin | C |
|---|---|
| `^T`, `^const T` | `T *`, `const T *` |
| `p*` | `(*p)` |
| `noalias ^T` | `T * RIN_RESTRICT`, as `noalias [*]T` does today |

The bounds checks, `RIN_INDEX` on slices and arrays and the compile-time
`[0]`-only rule on `*T`, keep their current behaviour. A Debug null check on
dereference, discussed alongside this, would go on `p*` and on `p[0]` of a
`*T`, the two ways a single pointer is read through; it is a separate
proposal.

## 5. C interop

- **`rinbind`** maps every C `T *` to `^T`, including `void *` and pointers
  to opaque types. Today it writes `[*]T`, except that those two bind as `*T`,
  because `*T` was the nearest way to say "cannot be indexed". C already
  rejects indexing or reading through them, and rin reports it too (the last
  row of the table in 3), so the exception buys nothing, and "a C pointer is
  `^T`" is one rule without cases. Passing still works the convenient way:
  `*T` converts to `^T` implicitly, as it converts to `[*]T` today, so `x&`
  passes to a `^void` parameter unchanged. The other direction, a `^void`
  from C into rin's `*T`, is explicit, as raw to single is today.
- **Why `^T` for all of them** (decided 2026-10-28): a C type cannot say one
  or many. `float *out_value` and `float *data` are the same type, so any rule
  that binds some C pointers as `*T` is a guess. Single-element out-parameters
  (`UINT *pNumViewports`) would stay `^T` regardless, giving an API that looks
  partly checked, and a header change that completes a struct would flip its
  pointers on the next regeneration. `^T` is the definition of a C pointer, so
  the binding says exactly what the header says. Code that knows a pointer is
  one thing says so where it holds it: a `*T` local, or `p[0]&` once at the
  boundary.
- **Hand-written `[external]` declarations** use `^T` for C pointers:
  `strlen: proc[external](s: ^const c8) -> usize = {}`.
- **The boundary rule** from the deprecated proposal carries over unchanged:
  raw pointers live where rin meets C, and rin code inside uses `*T` and `[]T`.
  `^T` makes that boundary easy to see.

## 6. What it gives up

- **C lookalikes.** C's `T*` is `^T` in rin, so an external declaration no
  longer reads exactly like its header. The deprecated proposal wanted that,
  and paid for it with an unchecked default; this design keeps the checked
  default instead.
- **Odin's convention, reversed.** In Odin `^T` is the single pointer and
  `[^]T` the many-pointer. A reader from Odin will guess wrong once; this doc
  and pointers-and-slices.md say it in their first paragraph.
- **Two spellings of a dereference**: `p*` and `p[0]` both work on `*T` and
  `^T`, as `*p` and `p[0]` do in C. Keeping `p[0]` means none of njinn's 8,711
  has to change; making `p*` the only spelling on `*T` was considered and
  dropped for that reason.

## 7. Rejected

- **Swapping meanings: `*T` for C's pointer, `^T` (or `&T`) for one
  element.** It makes the unchecked pointer the short default, and every
  unconverted `*T` silently loses its checks. Deprecated once already
  ([c-pointer-spelling.md](c-pointer-spelling.md)).
- **Auto-dereference on `.`** (`w.count`, as Go, Zig and Odin do). The
  shortest, but it hides the indirection, which a teaching language should
  show.
- **`w.*`** (Zig). Unambiguous everywhere, but a Zig spelling, and `p*` reads
  as the C programmer's `*p`.
- **`w*.` only, and `[0]` for whole values.** Covers field access only, and
  needs `[0]` for the rest; `p*` with the rule in 3.1 covers both.
- **`p*` as the only dereference on `*T`, `p[0]` an error.** Cleaner, since
  brackets would then always mean many, but it rewrites up to 8,711 njinn
  sites for a spelling, and needs a type-aware `--fix` to do it.
- **`rinbind` binding `void *` and opaque pointers as `*T`.** Today's rule;
  see 5.
- **A Debug-only fat `*T`** (pointer plus a length of 1). The length is
  always 1, so it checks nothing a compile-time rule does not; and it would
  change struct sizes between Debug and Release and break C layout.
- **`[**]T`** for a pointer to pointers. A special token for one combination,
  where types otherwise compose prefix by prefix.
- **A backtick** (``w` ``). Markdown, which every doc and chat uses, eats it.

## 8. Migration

No step changes what existing code means, and none blocks njinn.

1. **`^T` as a second spelling of `[*]T`.** Additive. `[*]T` keeps working,
   with a warning: `'[*]T' is now spelled '^T'`. Because the meaning is
   identical, the rename is a safe textual replacement of `[*]` in type
   position, and the compiler's own diagnostics list every site.
2. **`p*`, the `^T` mangle, and the `^void`/opaque errors.** `p*` is
   additive. The new mangle renames generated instances that hold a raw
   pointer (`vec_ptr_u8` becomes `vec_raw_u8` where the argument was `[*]u8`),
   which matters only to C that names them.
3. **Convert** std, the tests, njinn, the lessons, rin-leet, the site, and
   regenerate the `rinbind` output, at whatever pace suits each. Code using
   `p[0]` needs no change; moving it to `p*` is optional, a style pass for
   whenever it suits.
4. **Make the `[*]T` warning an error.**

Measured 2026-10-28, `[*]` in source and docs:

| where | `[*]` |
|---|---|
| rin std | 175 |
| rin tests | 152 |
| rin docs | 75 |
| njinn | 1,029 |
| rin-dod | 338 |
| rin-dx11 | 147 |
| rin-learn | 52 |
| psite (rin pages) | 31 |
| rin-leet | 9 |
| rasm and rasm-dx11 | 3,606 and 7,597, almost all `rinbind` output, regenerated rather than edited |

`p[0]` stays legal, so njinn's 8,711 need nothing.

Also to update: the LSP (`scripts/rin_lsp.py`, which treats `[*]` as `*` for
completion), the Neovim and TextMate grammars, natvis (unchanged C, so
nothing), `docs/pointers-and-slices.md`, `docs/noalias-variadic.md`, and the
site's rin page.

## 9. Tests

1. `^T` emits the same C as `[*]T` today, in parameters, fields, globals,
   casts, generics and `noalias`.
2. `p*` reads, writes, takes a field, an address and a second level, and
   lowers to `(*p)`.
3. Every row of the table in 3.2: the ✓ rows compile and run, the error rows
   fail with the message given.
4. The table in 3: `p*` and `p[0]` give the same C on `*T` and `^T`; `p[1]`
   and `p[i]` on `*T` stay errors; `p*` on `[]T` and `[N]T` is an error;
   reading or indexing `^void` and `^Opaque` is an error.
5. `[*]T` warns, then errors.
6. `rinbind` writes `^T` for every C pointer, `void *` and opaque included.
7. Generics: `vec<^u8>` and `vec<*u8>` are distinct instances; `p[i]` in a
   generic body passes for `^T` and fails for `*T` whichever is instantiated
   first; `x*` on a non-pointer `T` fails at the instantiating call.

## 10. Open

- **The warning period.** How long `[*]T` stays a warning before it becomes
  an error; probably until njinn is converted. The rename is a textual
  replacement of `[*]` in type position, so no type-aware tool is needed.
- **Precedence of `p*` among postfix operators**: the same as `[i]` and `.f`,
  so `p*.f[i]*` reads left to right. Confirm nothing else expects otherwise.
