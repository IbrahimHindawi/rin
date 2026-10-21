# Operators And Conversions

> **Implemented, except `x'T` (section 5), which waits for cast counts.**
> Decided in discussion, 2026-10-13 to 2026-10-21. Memory operators are postfix
> (`x[0]`, `x&`), logic is C's `&& || !`, literals take their type from context,
> and implicit conversions must be lossless -- an error, not a warning. The old
> spellings are errors that name the new form. Progress and what was measured
> are in the suggested order at the end.

Two threads that turned out to be one: making the syntax read in one direction,
and closing the integer-conversion bug class that [`shape.md`](shape.md) 2.1
parked. Each section records what rin does **today**, verified against the
compiler, and then the decision.

## 1. Places are postfix

**The principle.** Operators on *places* are postfix; operators on *values* are
prefix or infix. After this change the only prefix operators left are `-`, `!`
and `~`, and none of them touch memory.

    nodes[i]                  // index
    nodes[i].next             // member
    nodes[i].next[0]          // deref
    nodes[i].next[0].data&    // address
    cb.fn(x)                  // call

A chain reads once, left to right, one hop per step. C makes you start in the
middle and spiral: `&(*nodes[i].next).data`. Types already read this way --
`*[4]i32` is "pointer to array of 4 i32" -- so expressions and declarations end
up reading in the same direction.

### Today

Two spellings for each operation:

| operation | spellings | parser |
|---|---|---|
| deref | `p[0]`, `p.*` | `.*` builds the same `Expr_Index`-with-zero node as `[0]` (`src/main.c:2788`) |
| address | `&x`, `x.&` | both build `Expr_Addr` (`src/main.c:2797`, `2856`) |

`[0]` is already the canonical deref: std writes `[0].` 387 times and `.*` in
no code at all, and the compiler's own hint says "use '[0]' to access the
pointed value".

### Decision

- `x[i]` index, `x[0]` deref, `x.f` member, **`x&` address**, `f(x)` call.
- `.*`, `.&` and prefix `&` are removed. **`.` means member access and nothing
  else**; `T<>.name` is still a member access, just a compile-time one.
- **No auto-deref.** `p.x` does not reach through a pointer. Every hop is a
  visible `[0]`, which is the point.
- **Hard break**, not a deprecation period. Prefix `&x` becomes an error that
  says "prefix '&' was removed; write 'x&'".

### Postfix `&` beside binary `&`

Prefix `&` and binary `&` coexist trivially -- position decides, as in C. A
*postfix* `&` sits where a binary operator would, so it needs a rule. One token
of lookahead is enough:

> `&` is address-of when the next token **cannot start an operand**, and
> bitwise AND otherwise.

| written | next token | means |
|---|---|---|
| `foo(buf[i]&)` | `)` | address |
| `f(a&, b)` | `,` | address |
| `p& == q` | `==` | address |
| `x&[0]` | `[` | address -- no rin expression starts with `[` |
| `p& && ok` | `&&` | address |
| `flags & mask` | identifier | AND |
| `x & -1` | `-` | AND |
| `x & ~m` | `~` | AND |
| `x & (m)` | `(` | AND |

No whitespace sensitivity. The one thing the rule cannot say is "address of
`p`, minus one", and rin already spells that `p[-1]&`.

**One lexer change.** `p&==q` lexes greedily as `&=`, `=`. Don't form `&=` when
the next character is `=`.

### Rejected spellings

- **`[&]`** (`x[0][&]`) -- consistent ("brackets are memory") but reads as an
  index on first sight.
- **`.0` / `.N`** -- `p.0.0` lexes as `p`, `.`, `0.0`, the bug Rust had to patch
  for tuples; and `.N` competes with field names.
- **`@`** (`buf[i]@`) -- free in expressions and conflict-free, but `&` is C's
  symbol and staying close to C won.

## 2. Logic, bitwise, shifts

**Today.** `and` / `or` are keywords, `!` is not, `& | ^ ~` are C's, and shifts
are `shl` / `shr`. Bitwise binds tighter than comparison, so `6 & 4 == 4` means
what it looks like, and the emitter parenthesises everything so C cannot
regroup it ([`shape.md`](shape.md) 4.3).

### Decision

- **`&& || !` replace `and` / `or`.** `#if` lines already use them, so code and
  directives finally agree. Truthiness is unchanged ([`shape.md`](shape.md) 4.1).
  `&&` is one token and cannot start an operand, so it works with the postfix
  `&` rule above.
- **`& | ^ ~` stay.**
- **`shl` / `shr` stay words.** `>>` would close generics: `Vec<Option<T>>`. That
  is the one deliberate departure from C's symbols, and it has a reason. The
  parser recognises C's `>>`, `<<`, `>>=` and `<<=` (two touching angles in
  operator position) and points to the word spelling.
- **`shr` is defined by type**: arithmetic on signed operands, logical on
  unsigned. That is what `013-integer-conversion.rin` already pins (`logical_shr`,
  `arith_shr`) and what clang and MSVC do; it closes [`shape.md`](shape.md) 4.2.
  It is also where C23 and C++20 landed.
- **Rotate, popcount, clz, ctz are std procs**, lowered to compiler intrinsics.
  No new operators.

For reference:

| | C | Rust | x86 / ARM |
|---|---|---|---|
| logic | `&& \|\| !` | `&& \|\| !`, operands must be `bool` | -- |
| bitwise | `& \| ^ ~` | `& \| ^ !` | `and or xor not` / `and orr eor mvn` |
| clear bits | `x & ~m` | `x & !m` | `andn` / `bic` |
| shifts | `<< >>`, signed `>>` impl-defined | `<< >>`, by type | `shl shr sar` / `lsl lsr asr` |
| rotate | none | `rotate_left` | `rol ror` |

## 3. Literals

**Today.** The checker types every integer literal `i32` and every float literal
`f32` (`src/main.c:9701`), with a hidden exception: a number spelled with a
trailing `u64` is `u64`. The float case is quietly wrong -- rin emits `1.0`,
which C evaluates as a `double`, so `1.0 / 3.0` is computed in double while rin
believes it is float.

### Decision

- **A literal takes its type from context**: the other operand (`len > 0`), or
  the target of an assignment, argument or return.
- **Defaults with no context: `i32` and `f64`.** The float default now matches
  what C computes.
- **A literal must fit.** `300'u8` is an error; so is `-1` where a `u64` is
  wanted.
- **A `'` suffix for the no-context cases**, replacing the hidden `u64` check:

      printfmt("{}", 1024'u64);
      makeg<u8>(0xFF'u8);
      printfmt("{}", 1.0'f32);

  Nim spells it the same way. The tick keeps the type from blending into the
  digits -- in Rust, `0x1f32` is the hex number `0x1f32`, not `0x1f` as `f32`.
  Directly after a number `'` can never begin a char literal, so the suffix needs
  no lexer context.
- `_` as a digit separator if wanted: `1_000_000'u64`. `'` is taken, so C++14's
  `1'000'000` is not available.
- **C's suffixes are removed** (decided 2026-10-21): `1.0f`, `10u`, `10u64` and
  the rest are errors that name the replacement -- usually nothing, because the
  context supplies the type, otherwise `'`. One spelling per literal, the same
  rule that removed `.&`.

## 4. Conversions

**Today.** Everything converts silently -- narrowing, sign change, float to int
([`shape.md`](shape.md) 2.1). That was parked as "deliberately dirty". This
reopens it, because contextual literals remove the main cost of being strict.

### Decision

**Implicit only when lossless, plus int to float.**

    a: u8  = 200;
    b: u32 = a;            // ok: every u8 fits in u32
    c: i64 = b;            // ok: every u32 fits in i64
    d: u32 = c;            // error: i64 -> u32 can lose bits
    d: u32 = cast(c, u32); // ok: said so

    i: i32 = -1;
    n: u64 = i;            // error: sign change
    len: u64 = 10;
    if (i < len) { }       // error: mixed-sign comparison
    if (len > 0) { }       // ok: the literal becomes u64

    x: u16 = 1;
    y: u32 = 2;
    z: u32 = x + y;        // ok: x widens, result is u32
    w: u16 = x + y;        // error: u32 into u16

    f: f32 = i;            // ok: int -> float is the deliberate exception
    g: i32 = f;            // error: float -> int
    h: f32 = 1.0;          // ok: the literal adapts
    k: f32 = some_f64;     // error: f64 -> f32 narrows

- **Widening is free.** This is Zig's rule, not Rust's: Rust makes even
  `u8 -> u32` explicit, and most of its cast noise comes from that.
- **int -> float is implicit on purpose.** It loses precision gradually above
  2^24 but never wraps or flips sign, which are the bugs being hunted. Requiring
  casts here would flood game code: every ratio, every pixel coordinate, every
  loop index used in math.
- **Lenient where strictness catches nothing:** an index may be any integer type
  (Rust's `usize`-only indexing is its most complained-about rule), a shift
  amount may be any integer type, and truthiness stays.

### What it does not catch

It closes "the compiler silently changed my number". It does not touch "the
arithmetic itself went wrong":

- signed overflow -- still UB, inherited from C
- unsigned wraparound -- `for (i: u64 = 0; i < len - 1; ...)` with `len == 0`
- a wrong explicit cast -- `cast(-1, u64)` compiles because you asked
- shifting past the width, division by zero

Those are runtime questions; see [`safety.md`](safety.md).

### The emitter has to agree

C promotes anything narrower than `int` before doing arithmetic. If rin says
`u8 + u8` is a wrapping `u8`, the generated C must say so too:

    a: u8 = 200; b: u8 = 100; c: u8 = 44;
    if (a + b == c) { }    // rin: 300 wraps to 44 -> true
                           // C:   300 == 44      -> false

So a sub-`int` result is cast back where its value is used: `(u8)(a + b) == c`.
`013-integer-conversion.rin` is where to pin it.

### Rollout: warning first

Shipped as a **warning**. Compile njinn, count the sites, then decide. If it is
dozens, mostly at the D3D boundary (`UINT` meeting `size_t`), make it an error
and add the casts -- those are exactly the places a negative count becomes four
billion. If it is thousands, the count shows where the friction is before
anything is committed.

## 5. Cast spelling

**Decision for now: `cast(x, T)` stays.** It needs no migration and no lexer
rules, and it already reads value first, type second. `'` ships on **literals
only** (section 3).

**`x'T` is the candidate generalisation**, once the literal suffix has been
lived with:

    count: i32 = strlen(name)'i32;
    desc.ByteWidth = (sizeof(Vertex) * n)'u32;

It is postfix, it would merge typed literals and casts into one feature (a
constant source must fit, a runtime value converts), and it avoids Rust's
`x as usize < y` problem. It costs two rules and some tooling:

- **Lexing.** `'` is a cast tick after an identifier, number, `)` or `]`, and a
  char literal anywhere else -- the way JavaScript tells `/` from `/regex/`.
  After `&` it is ambiguous (`buf&'*u8` vs `flags & 'a'`), so there a complete
  char literal (`'a'`, `'\n'`) wins.
- **Generic cast types need parentheses.** In `n'u32 < max` the `<` must be a
  comparison, and the parser cannot know which names are generic, so
  `p'(*Node<T>)`.
- **Highlighting.** `nvim/syntax/rin.vim:99` opens a char region at every `'` and
  needs a cast-tick match ahead of it. Naive highlighters elsewhere will get it
  wrong; LSP semantic tokens come from the compiler and will not.

The open question is not the parser but visibility: a cast at the end of a long
chain is one light character. Decide after the warning pass shows how many casts
njinn really needs -- if it is few, `cast()` is fine and the question goes away.

**`cast<T>(x)` rejected.** It matches generic-call syntax but puts the type
first, against the postfix direction, and adds brackets without removing
anything.

## 6. Considered and dropped: every pointer a slice

> **Succeeded by [pointers-and-slices.md](pointers-and-slices.md):** `*T` stays
> one word but points at exactly one element, `[]T` is the checked slice, and
> `[*]T` is the raw pointer.

Making `*T` a pointer plus a length would make `[0]` literally true and every
index checkable. It breaks too much of [`rin-soul.md`](rin-soul.md):

- C's ABI takes thin pointers, so `[external]` procs would need them anyway --
  it would never be *every* pointer.
- `next: *Node` doubles from 8 to 16 bytes; linked structures and handles pay
  everywhere.
- njinn measured about 30 places where data crossed a boundary without its
  length, and **zero** subrange uses ([`slices.md`](slices.md)).

Slices stay a separate type, in the order `slices.md` already gives: array to
slice coercion, `slice<T>` to `slice<const T>` widening, then `s[a..b]` -- which
joins the postfix family.

## 7. Migration cost

Counted in this repo; njinn is outside it.

| change | std | elsewhere |
|---|---|---|
| `.&` -> `&` | 60 | 30 in `.rin` fixtures, about 100 in `tests/run_tests.py`, 24 in `tests/run_lsp_tests.py` |
| `.*` -> `[0]` | 0 in code | -- |
| prefix `&x` -> `x&` | 2 (`string8.rin:580`, `599`) | a few fixtures |
| `and` / `or` -> `&&` / `\|\|` | about 40 / 75 lines | 59 / 12 in test fixtures, plus inline sources |

Tooling: completion inserts `{name}.&` and matches `endswith(".&")` in
`scripts/rin_lsp.py` (2730, 2762, 4164 -- a submodule); `nvim/syntax/rin.vim:71`
matches `\.&`.

**njinn.** `.&`, `.*`, `and` and `or` are mechanical -- they are tokens, so a
token-level replace cannot hit an identifier. Prefix `&` cannot be told from
binary `&` by a regex; follow `rin check --diagnostics=json`, which will list
every site.

## Suggested order

1. **Done.** Postfix `&`, and remove `.*`, `.&` and prefix `&`.
2. **Done.** `&& || !` replace `and` / `or`.

   Both landed together. `tools/rin_migrate.py` rewrote std, the fixtures, the
   test runners' inline programs and njinn (3,270 `.&`, 412 `and`, 956 `or`);
   the generated C for every fixture and all of njinn came out byte-identical
   apart from column numbers inside `/* instantiated at */` comments. Covered by
   `postfix_addr` and `old_pointer_spellings`. Two fixes came with it: the
   emitter parenthesises an address used as a base, so `x&[0]` is `(&(x))[0]`
   and not C's `&((x)[0])` (the old `x.&[0]` had the same bug); and a statement
   that reports an error but still parses to its own `;` no longer swallows the
   statement after it, so one file reports every old spelling at once.
3. **Done.** `shr` is by type -- arithmetic on signed, logical on unsigned --
   recorded in [`shape.md`](shape.md) 4.2 and pinned by
   `013-integer-conversion.rin`.
4. **Done.** Contextual literal typing, the `f64` default, must-fit and `'`.
   A literal remembers the type its context gave it and is emitted as that
   type's C literal: `0.5` into an f32 is `0.5f`, and constant arithmetic that C
   would do in int gets a suffix -- `1 shl 40` into a u64 is `1ULL << 40`,
   `1 / 3` into an f32 is `1.0f / 3.0f`. C's suffixes are errors;
   `tools/rin_migrate.py` removed 3,226 of them (`f` and `u` dropped, `ULL` and
   friends kept as `'u64`). Covered by `contextual_literals`,
   `old_literal_suffixes` and `literal_fit`.
5. **Done.** The conversion rule, and the emitter casts for sub-`int` results:
   `a + b` on two u8 is emitted `((u8)(a + b))`. Counted on njinn: **142**
   sites. 139 of them were `sizeof(...)` or an enum's `<>.count` landing in a
   D3D `u32` -- constants C knows and rin does not. Those now adapt like
   literals (Zig treats its compile-time integers the same way), with the
   emitter writing the cast so C agrees: `((u32)sizeof(V))`. Char literals
   adapt too. That left one real site, `fmt_cat3`'s u64 length stored in an
   i32, now fixed. Covered by `lossy_conversions` and `narrow_arithmetic`.
6. **Done: an error.** One site is well under "dozens", so the rule is an
   error. The warning machinery stays for the next rule that needs counting.
7. Revisit `x'T`. Measured in [`conversion-report-00.md`](conversion-report-00.md):
   the strict rules added no casts, binding njinn's C enums removed 228, and
   the 1,065 left are about 780 numeric and 280 pointer or enum casts.
   Recommendation for now: keep `cast(x, T)`.

## See also

- [`shape.md`](shape.md) -- 2.1 conversions, 4.1 truthiness, 4.2 right shift.
- [`type-safety.md`](type-safety.md) and [`safety.md`](safety.md) -- the static
  and runtime halves of the safety bargain.
- [`slices.md`](slices.md) -- why slices stay a separate type.
- [`rin-soul.md`](rin-soul.md) -- thin pointers, predictable layout, C as the ABI.
- [`verification.md`](verification.md) -- the lowering table, including signed
  `shr`.
