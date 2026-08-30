# Compiler Hardening

Working notes for the effort to make wrong-code bugs findable. Companion to
`docs/verification.md`, which covers the theory; this one records what has
actually been done and what is still open.

Background: a switch lowering emitted cases without `break`, so an enemy AI ran
its approach case, fell through into retreat, and negated its own movement
vector. It compiled, it ran, and the source read correctly. That is a
**wrong-code bug**, and the suite at the time could not have caught it — the
checks were about whether things compiled, not what they computed.

## Method

Every test targets a **hypothesised fault**, not a feature. "Does switch work"
would have passed the entire time the bug existed: `002-enum-values.rin` had a
switch and passed, because every case returned and fall-through was invisible.

So each case is chosen so that the plausible wrong lowering **changes the
answer**. `Mode.Run` hits `default`, which is last, so nothing falls into it and
the bug hides; `Mode.Idle` exposes it. Where a test cannot name the fault it
guards against, it is not earning its place.

Expected values are **hand-computed from first principles**, never blessed.
`--bless` records whatever the compiler currently does, which is exactly how a
wrong-code bug gets enshrined as correct.

The suite is validated by **mutation**: break the emitter deliberately and
confirm the suite goes red. Disabling the `break` emission makes all six
assertions in `008-switch-semantics.rin` fire. A suite that stays green against an
injected fault has a hole.

## Done

Execute-suite cases live in `tests/rin-torture/execute`, which compiles, links,
runs, and diffs against a recorded `.expected`.

| test | covers |
| --- | --- |
| `008-switch-semantics` | fall-through, default in the middle, `break` inside a loop, empty case, nested switch, sparse and negative labels |
| `009-evaluation-order` | `and`/`or` short-circuiting, one-arm ternary, compound assignment evaluating its target once, argument evaluation counts |
| `010-loop-semantics` | `continue` running the increment, `break` depth, loop-variable scoping, do-while running once, while running never |
| `011-aggregate-value-semantics` | struct assignment copying, deep nested copy through arrays, pass-by-value, return-by-value, struct-in-array, pointer aliasing |
| `012-precedence-and-parens` | explicit grouping surviving lowering, shift vs addition, the bitwise group vs comparison, logical vs conditional, the pack/unpack shape |
| `013-integer-conversion` | narrowing truncation, signed-to-unsigned modularity, unsigned wrap, logical vs arithmetic shift, sign-preserving widening |
| `014-names-and-emission-order` | mutual recursion, forward struct references, sibling-scope name reuse, locals shadowing globals |
| `015-initialization` | partial init zeroing the tail, designated initialisers, `= {}` through nested aggregates, arrays of structs |
| `016-type-surface` | union member aliasing and size, transparent aliases, `sizeof` on an array not decaying, empty `for`, else binding |
| `017-generics-and-reflection` | instantiation independence, repeated and nested instantiation, enum and field reflection, enum-sized arrays |

Negative checks in `tests/run_tests.py`: `switch_no_fallthrough`,
`reserved_c_identifier`, `mangle_collision`.

### Pass 3: crashes, and constants that were not

Found by probing rather than mutation, which is why none of it had surfaced
before: the fuzzer mutates a corpus, and a mutation cannot produce a thousand
repetitions of one token. Every shape below needs either repetition or a
specific pairing.

**Four constructs took the process down** with `STATUS_STACK_OVERFLOW` and no
diagnostic at all. The previous pass guarded `parse_expr`; types, statements and
postfix chains each recurse through their own function and none was counted.
Crash depths, bisected:

| construct | crashed at | limit now |
| --- | --- | --- |
| `*T`, `[N]T` | 1335 | `RIN_MAX_TYPE_DEPTH` 100 |
| `if`, `while` | 647 | `RIN_MAX_STMT_DEPTH` 200 |
| `for` | 1098 | same |
| `a.b.c`, `a[0][0]`, `---x` | 1045 | `RIN_MAX_EXPR_NODES` 800 |

The chain case is worth separating: `parse_postfix` does not recurse, so the
parser survives any length. It builds a left-leaning tree that the *type checker*
walks, which is exactly what the node limit already existed for — it simply was
not counting field links, index links or prefix operators.

Counting those needed one correction, and that is the useful part. Charging a
node on entry to `parse_unary` and `parse_postfix` looked right and was not:
both run for every operand in the program, so an ordinary `a + b + c` paid two
nodes per term and a legal 400-term chain — already in the suite — began
failing. They are counted only where the operator or link is actually present.

Headroom against real code: deepest type 2, deepest block 11, longest chain 8,
across njinn, std, rin-learn and rin-playground.

**Global initialisers were not required to be constant.** A file-scope
initialiser becomes a C initialiser, and C requires those to fold. Five shapes
were accepted here and rejected by clang:

    g_a: i32 = g_b;      // another global, declared later
    g_b: i32 = 5;        // ... or earlier; neither is constant in C
    g_a: i32 = g_a;      // itself
    g: i32 = f();        // a call
    n: i32 = 4;
    x: [n]i32 = {};      // a variable-length array at file scope

This is where "order does not matter" stops being true, and the reason is worth
stating precisely: reordering rescues none of them, because reading another
global is not a constant expression in C even when that global comes first. The
rule is constness, not order. Locals are untouched — a VLA inside a function is
legal C99 and works.

Two exemptions came from testing rather than reasoning, and both were wrong in
the first draft. `E<>.count` resolves to `E_reflect.count` and the record is a
global, so the obvious check rejected it — but it is const with a literal
initialiser and C folds the read, confirmed by compiling the equivalent C by
hand. And an array count is *meant* to be able to name a macro; rin's own
`#define` lowers to one, but defines are registered in the enclosing scope for
name resolution, so looking there rejected all of njinn. It reads
`prog->globals` now, where only real variable declarations live.

**A non-void proc with no `return` anywhere** emitted `i32 f(void) { }`. clang
warns under `-Wreturn-type` and compiles it, so the call returns whatever was in
the register. Only the decidable half is implemented: whether every *path*
returns needs reachability over the statement tree, and getting it wrong rejects
working code — `if` without `else`, terminal loops, `goto`. "Contains no return
at all" cannot be wrong, and measuring first showed it costs nothing: none of
887 non-void procs across all four projects trips it. An empty non-void body was
previously on the *accepted* list as "a proc that does nothing", which it is
not — it does nothing and then returns a value it never produced.

Each guard was verified to have a test that **fails when the guard is removed**,
which is a different claim from the test passing.

### Fixes made

**Switch cases no longer fall through.** A `case` takes a block, so it is
self-contained. The emitter appends `break` unless the body already ends in a
jump, which keeps returning cases free of unreachable code. Go, Rust and Zig
make the same call. *Ratified.*

**Bitwise operators now bind tighter than comparison.** C binds `&` `^` `|`
looser than `==` only because early C had no `&&`; by the time it did, changing
it would have broken existing code, and Ritchie wrote it up as a known mistake.
Rust, Zig, Go and Python all fixed it. rin does not consume C source, so it owed
nothing to that compatibility. Verified safe by regenerating the whole njinn
engine and diffing: **0 changed lines across 28,301** — nothing relied on the old
grouping, because people parenthesise `(a & b) == c` by reflex. *Ratified.*

**C keywords are rejected as identifiers.** `typedef: i32 = 1;` used to emit
`i32 typedef = 1;` and fail in the C compiler, pointing at generated code. Now a
diagnostic on the real source line. Currently enforced for local variables and
proc parameters.

### Verified already correct

Mangling collisions between enum members and globals are caught. Mutual
recursion and forward struct references emit correct declarations. Locals may
shadow globals. Initialisation zeroes unmentioned fields at every depth. Unions
alias their members and size to the largest. `sizeof` on an array does not decay
to a pointer. Generic instantiations are independent. The emitter fully
parenthesises subexpressions. The dangling-else ambiguity is unwritable, because
`if` requires a braced body.

## Pending

Open questions, each of which is a decision rather than a defect.

### Shadowing an enclosing local is rejected; shadowing a global is allowed

Sibling blocks may reuse a name, so scopes are properly nested, but an inner
block declaring a name that an enclosing block already declared is reported as a
duplicate local declaration.

    v: i32 = 1;
    if (v != 0) {
        v: i32 = 20;   // semantic error: duplicate local declaration
    }

C, Go, Rust and Zig all permit this. The asymmetry with globals — which *can* be
shadowed — suggests it may not be deliberate. Three options:

1. keep it, and document "no shadowing an enclosing local" as a rule
2. allow it, matching every other language in the family
3. keep it *and* forbid shadowing globals, for consistency

`014-names-and-emission-order.rin` currently pins only the settled part.

### Identifier restriction versus mangling

C keywords are currently **rejected**. The alternative is to mangle them on
emission so that any rin identifier is legal regardless of what C reserves.

Rejecting was chosen because it is reversible — relaxing a restriction later is
backward compatible, the reverse is not — and because mangling every identifier
would make the generated C harder to read, which matters given it is meant to be
read as a teaching artifact. But it does leak the backend into the language
surface, and "why can't I name a variable `auto`?" is a fair question from a
student.

The check also does not yet cover every declaration site. Locals and parameters
are checked; **globals, proc names, struct and union names, field names, enum
members and type aliases are not.** If the restriction stays, that coverage
should be completed.

### Emitted-parenthesis policy

The emitter parenthesises every subexpression, so `1 shl 2 + 3` becomes
`(1 << (2 + 3))`. With the precedence table now deliberately different from C's,
those parentheses are **load-bearing** rather than redundant — stripping them
would let C re-parse `6 & 4 == 4` back into the old grouping, silently.

An earlier proposal to emit unparenthesised C for legibility is therefore
withdrawn for binary operators. A narrower version remains possible: omit
parentheses only where rin's precedence agrees with C's, keeping them where the
tables now differ. That is cheap to state and fiddly to maintain, and it would
need `012-precedence-and-parens.rin` extended to cover every operator pair.

### Arithmetic right shift is implementation-defined

`013-integer-conversion.rin` records that right-shifting a negative signed value
is arithmetic. C leaves this implementation-defined. The test documents the
current behaviour rather than a decision; it should be confirmed as deliberate
and written into the language docs, or changed.

### Documentation drift

`docs/verification.md` and `docs/rin-soul.md` both describe rin as following C's
operator precedence. That is now false and should be corrected before it
misleads a reader — including a student.

## Not yet covered

The largest gap is not a construct but a resolution rule: **a call to a name
nothing declares is not checked**, so it reaches clang and reports there. Three
instances were hit in a single sitting of ordinary porting. Measured and written
up separately in `name-resolution.md`, together with the proc-shadowing case,
which is the same shape.

Constructs with no discriminating test, roughly in the order worth doing:

- **field and enum-member name collisions** with C keywords, and the remaining
  declaration sites listed above
- **varargs** — forwarding, and the promotion rules at the call boundary
- **pointer arithmetic** — scaling by element size, differences, comparison
- **`goto` across scopes** — jumping backward over an initialisation, jumping
  out of a nested block; `003-goto-labels.rin` covers the ordinary cases only
- **associativity** beyond the arithmetic cases, especially the right-associative
  conditional: `a ? b : c ? d : e`
- **index expressions with side effects** in multidimensional access
- **`volatile`** — does the qualifier survive lowering, or does the C compiler
  optimise away an access it should keep?
- **`static`** — internal linkage, and statics inside procs if they exist
- **recursion depth and large aggregates** returned by value
- **string and character literals** — escapes, embedded nulls, concatenation

## Still accepted, then rejected by clang

Found by probing rather than by a bug report, so none of these had bitten yet.
All are the same shape this document is about: rin says the program is fine
and the C compiler disagrees, pointing at generated code. Two are fixed; one
turned out to be a deliberate inheritance rather than a defect, which is why
the entry is still here.

### ~~A type containing itself by value~~ *(fixed)*

> **Fixed.** One walk over type references, rooted at every struct and every
> alias, reporting the path it found:
>
>     error: type contains itself by value, so it has no size;
>            use a pointer to break the cycle 'B -> A'
>
> It turned out to be six shapes rather than the two that were obvious, all one
> root cause -- direct, through an array, mutual, through an alias, through an
> anonymous member, and generic. The walk follows whatever needs a *complete*
> type and stops at whatever does not.
>
> The half that matters as much: a self-pointer, a generic self-pointer, a
> nested generic argument (`Pair<Pair<i32, i32>, i32>`) and a type shared by two
> others all stay legal. A check that rejected a self-pointer would be worse
> than no check, since linked lists are the common case. njinn's 165 records and
> 24 aliases build unchanged.
>
> **Deliberate false negative:** generic type *arguments* are not walked, only
> the base name. Walking them would be unsound without substituting them into
> the generic's fields -- `P: struct = { items: Vec<P>; }` is legal whenever
> `Vec` holds its elements behind a pointer, and rejecting it would be a false
> positive on correct code. A generic that does hold its argument by value is
> still missed, which is where this already was.
>
> Covered by `type_cycles`, which asserts the accepted list as well as the
> rejected one, and checks the diagnostic names the path. Mutation-tested by
> disabling the call.

### A non-void proc with no return *(not a defect: decided)*

    f: proc() -> i32 = { }
    f: proc(n: i32) -> i32 = { if (n > 0) { return 1; } }
    f: proc(n: i32) -> i32 = { switch (n) { case 0: { return 1; } } }

All three are accepted, and stay accepted. This was briefly implemented as an
error and reverted: rin keeps C's rule here, like the rest of
[`safety.md`](safety.md). clang warns by default with `-Wreturn-type` and
`#line` puts the warning on the real source line, which is the whole bargain --
C does the checking rin has deliberately not taken on.

Listed here only so it is not "found" a third time.

### ~~A zero-length array~~ *(fixed)*

> **Fixed.** `xs: [0]i32` is now *array length must be greater than zero*. Zero
> uses across njinn, `std` and the test suite, so nothing had to change; the C
> trick it exists for, a flexible array member, has its own spelling.
>
> The related case stands: guarding every field of a struct out with `#ifdef`
> leaves an empty struct, which ISO C also forbids and clang sizes at 4 here
> without a word. That one needs a program to go looking for it.

## Coverage criteria

Two, and the second is the one that means anything.

**Enumerative.** Every one of the 13 statement kinds and 17 expression kinds in
`main.c` has at least one discriminating test. This is a checklist that can be
ticked off against the compiler's own enums.

**Mutation.** Break a lowering deliberately and confirm the suite fails. If it
stays green, that construct has no discriminating coverage regardless of how
many tests mention it.
