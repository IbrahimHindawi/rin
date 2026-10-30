# How the safe subset was built: the log

The decisions behind `docs/regions.md`, in the order they were made, for
whoever picks this up next. Each entry is what was decided and the reason,
not the code; the code is on branch `regions` and the rules are in the doc.

## 1. The premise

rin is an arena language: programs already have a few named arenas with a
known order of lifetimes. The question was whether that order, made visible
to the type system, is enough to prove memory safety without a borrow
checker or a garbage collector. The bet: give the arenas names (`regions`),
make every pointer kept in memory say which one it points into (`*T in R`),
and refuse any pointer that could outlive its region. The temporal half of
memory safety becomes a lifetime-order check; the spatial half (bounds,
null, initialisation) is run-time checks and `?*T`.

## 2. The first build

Branch `regions`. Safe by default, `--unsafe` for old code, so the whole
existing course and both torture suites kept running unchanged. Two proc
attributes mark the boundary: `[unsafe]` (dangerous to call) and `[trusted]`
(unchecked body, honest signature). std was annotated by an error-driven
script: every proc the checker rejected became `[trusted]`, 286 at first.
Carriers were invented early: a struct holding a raw pointer or a
region-less pointer is held only through `*T in R`, never by value, because
its author's invariant (a `data` and a `length`) is what makes it safe.

Two things the design had missed, found by trying to break it: a proc that
opens a region cannot be called while that region is open (a fixpoint over
the call graph computes what each proc opens), and a region handle cannot be
stored in memory.

## 3. The second round: staleness and integers

Growth under a pointer was first left as a logic bug on purpose, since in an
arena the old storage stays valid. The rule was added anyway, because it was
cheap and the bug is expensive: a region-less pointer taken from a container
is a *borrow* that remembers its source path and dies when that path is
passed to a proc that may move it. Integers: wrap in release (`-fwrapv`),
trap in Debug (sanitizers), shifts masked to the width. A std audit made 21
find procs return `?*T`, readers take `*const`, and the three
pointer-and-length constructors `[unsafe]`.

## 4. Three attack rounds

An adversary agent wrote programs that should be unsafe, compiled the ones
the checker accepted with ASan and UBSan, and ran them. Round one, 62
programs, 17 holes in six families; round two, 68 programs against the new
rules, 6 holes; round three, 73 programs against std's trusted procs, 14
holes, every one a lying signature. All 37 became rules and reject tests;
the corpus is in `tests/rin-safety/attacks`. The lesson that repeated: every
hole was a place where a *name* stood in for a *fact*: a region-less local
outliving its block, a `*const` that was shallow, a `*T` field that could be
null, a `[]T` whose length was writable. The fix each time was to make the
fact the thing the checker tracks.

## 5. Syntax

The first spelling was `region frame { }` and `region level > frame;`, which
reintroduced C's keyword-first declarations into a language whose one
declaration shape is `name: kind = value`. Changed to `frame: region = { }`,
`level: region;`, `frame: region in level;`, matching `done: label = { }`.
The parameter form `r: region R` stayed; the declared-region form
`r: region level` is the odd one out and the proposed fix is
`level: region` as the parameter itself.

## 6. The course port

All of rin-dod, 100 programs, 23,039 lines, by four parallel agents with a
shared conventions file and a per-file checker, after a survey that found
the common blockers and fixed them once in std (`fmt_make`, hashmap walking,
hashset `make`). Result: 134 `[trusted]` procs of 1,444, almost all the same
type-erased byte column repeated per lesson. The port found five rules that
were right but too blunt and fixed them: `printfmt` as an unknown callee,
`exit()` not ending a branch, shallow `*const` (replaced by the moves
analysis: each checked proc carries the set of parameters its body reaches a
moving call with), proc pointers treated as memory pointers, and by-value
arguments of nested calls counted as roots.

## 7. Where it stands against Rust

Same shape of guarantee: a program with no `[trusted]` or `[unsafe]` of its
own is memory safe at compile time, provided std is correct. Smaller and
younger trusted base; names and paths rather than field-level reasoning; no
threads at all; and a fraction of the annotation cost, with no lifetimes,
ownership or borrow annotations. The honest gap is time and strangers: the
rules have been attacked by their author, not yet by anyone else.

## 8. What was deliberately not done

Dynamic regions (a region whose end is a run-time decision would take away
the one fact the checker stands on); destructors (a region-end cleanup list
is the data-oriented version, and it waits for the engine to show a need);
exempting raw-pointer structs from the carrier rule (a `data` plus a
`length` is exactly the invariant that must not be writable).
