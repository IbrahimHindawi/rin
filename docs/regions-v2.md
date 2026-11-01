# Regions, second pass: one rule, on a small IR

This is the design for replacing the safety checker's pointer rules with one rule, checked on a
lowered form of each proc body. It is not a new language: programs keep the regions they write
today, `level`, `frame`, `scratch`, `permanent`, `r: region R`. It is a return to the rule
[regions.md](regions.md) wrote down, made total, with three ideas taken from Rust for the parts
that document left to the implementation. Nothing here is built yet. The point of the document is
to be read and judged before anything is.

## Why

Twenty attack rounds found 180 holes in safe code, and the fixes closed them all, and the rate did
not fall: every round that took a new angle found a new family. The reason is structural. The
checker is 27,000 lines, with 197 places that refuse a program and 48 walks that stop at a depth
and answer "fine". Each rule is keyed on a syntactic shape, so a closed hole reopens through the
next shape: a ternary, `unwrap`, a cast, a field, a generic, a proc pointer, a loop's second pass.
And the rules are partial models of one question, *what may this pointer point at*, so holes live
where two of them meet: raw pointers × `reset`, proc types × flow, `new` × raw, loops × paths.

[regions.md](regions.md) had the answer already, and the implementation drifted from it:

> The stack is a region too: each block of a proc is a region shorter than everything around it.
> `x&` for a local `x` is a pointer into the block that declared `x`.

> `*T` (borrowed): may be read and written through; may not be stored in a struct; may not be returned.

Neither held in the code. A bare `*T` local has no region in its type, so its lifetime is tracked
by *provenance*: paths it was taken from, `retaken`, `scope_killed`, loop seeding, back-edge checks,
own-address paths, `region_from_args`, `from_param`. That machinery is most of the 197 sites and
nearly all of the holes since round 13.

## The three ideas from Rust

**Every pointer has a region, always.** Rust's elision hides a lifetime's name, never the fact. In
rin, a bare `*T` is not "no region"; it is a region the checker works out:

| Where a bare `*T` appears | Its region |
|---|---|
| a parameter | that parameter's own region, `'p`: some region of the caller's, unknown, outliving this call |
| a local | inferred inside the body, from what flows into it |
| a result | the region of the single pointer parameter (elision), or it must be named |
| a field | the struct value's region (see below) |
| `x&` for a local `x` | the block that declared `x`, written `@b`, never spelled by the program |
| a raw `^T` from a call | this statement, `temp`: it can be passed on, and nothing can hold it |

Rust stops there and solves general constraints. Rin's regions nest, so the solving is a small
fixed point over sets, below.

**One rule.** A value may flow into a place only if every region the value may point into outlives
the place's region. Storing, returning, passing, resetting, leaving a block, loops, own addresses,
raw pointers, ternaries: instances, not rules.

**Check an IR, not the syntax.** Rust's borrow checker runs on MIR, where a ternary is two
assignments to a temporary and a loop is a back-edge. Rin lowers each body to five statement forms
before checking. There is then no shape left to route around.

And one idea from Rust's NLL for `reset` and loops: a region is a set of program points, and a
pointer's region must contain every point where that pointer is live.

## The lattice

A pointer value's abstract region is a set of atoms:

    permanent                     the whole program: globals, literals, `permanent` memory
    level > frame > ...           the program's named regions, in their nesting order
    R                             a region parameter of this proc: some caller region, outliving the call
    'p                            a pointer parameter's region: some caller region, outliving the call
    @b                            a block of this proc; inner blocks are shorter than outer ones
    temp                          this statement
    null                          points nowhere; flows anywhere nullable

`A outlives D` for atoms: `permanent` outlives everything; named regions by nesting; `R` and `'p`
outlive every `@b` and `temp` of this proc, and themselves, and a named region only when declared
so (`r: region R in level` today); `@outer` outlives `@inner`; `temp` outlives nothing. A set
outlives `D` when every atom in it does. This is Cyclone's and Tofte–Talpin's region order with
blocks added, which is what regions.md promised in "Order of work", step 7.

Regions inside a pointer are invariant, as today (`type_flow_same`): `**T in level` and
`**T in frame` are different types, and a proc type's signature is part of its type at any depth.

## The IR

Each proc body lowers to a graph of basic blocks over these statements:

    place = value          value: a local, a field, an index, an address, a call result, a literal, null
    call f(args)           a call for its effects, including procs that reset or open regions
    branch cond -> B1, B2  and `goto B`
    enter @b / leave @b    a block's start and end; `break`, `continue`, `return` and `goto` leave every block they cross
    open R / close R / reset R
    return value

`c ? a : b` is a branch and two assignments to a temporary. `unwrap(p)` is a check and a copy. A
field or index chain is a place. A `for` is init, a header block, the body, the step, and a
back-edge. There are no expressions with control flow left, so "the step runs after the body" and
"an assignment below the return reaches it next pass" are facts of the graph, not rules.

## The analysis

Two standard dataflows over the graph, then five checks.

1. **May-point-into**, forward. For each pointer-typed local and temporary, the set of atoms its
   value may be in, at each point. Assignment replaces the set; a join unions; a call result's set
   is the callee's declared result region under the call's bindings, or by elision the argument's
   set; `x&` is `{@b}` for `x` in block `@b`, or `{permanent}` for a global; a raw call result is
   `{temp}`; a parameter starts as `{'p}` or its named region.
2. **Liveness**, backward. Whether a local may still be read on some path from a point before
   being assigned.

Checks:

1. **Into a place with a declared region.** `g = v`, `s*.f = v`, `new<T>(r, {v})`, `push(c, v)`,
   an argument to a `*T in R` parameter, a result `-> *T in R`: `set(v)` outlives `R`. A global's
   place is `permanent`; a field's is its struct value's region; a slice element's is the slice's.
2. **Leaving a block.** On every edge that leaves `@b`, including the back-edge of a loop body and
   every jump, no local live on that edge may have `@b` in its set. This is `q = x&` surviving the
   block that owns `x`, in all its forms: `break`, `continue`, the loop's next pass, a `for`-init
   local, a pointer local's own address, a struct local that holds one.
3. **A reset.** At `reset R`, and at a call to any proc whose transitive `resets` include `R`, no
   local or parameter live after the point may have `R` or anything inside `R` in its set. A global
   that may hold `R` is emptied at run time, as today, and so must be `option`.
4. **A return.** The result's set outlives the declared result region, which is a named region,
   `R`, or by elision the one pointer parameter's `'p`. A set containing `@b` or `temp` never does,
   so the address of a local and `cstr_of`'s copy cannot leave.
5. **Raw values.** `temp` outlives nothing, so a raw call result is an argument and nothing else.
   A raw parameter's set is `{'p}`, which outlives no place in a struct or a global, so it is passed
   on or read, never kept. `^void` is bytes: it converts to nothing checked.

Everything a region names at run time keeps its run-time half: arenas are poisoned when they end,
bounds checks stay on, `reset` zeroes the globals that may point into it.

## What this closes, by family

Every hole family from rounds 13 to 20, and which part of the design covers it:

| Round | Family | Covered by |
|---|---|---|
| 13 | region handle stored, cast, assigned | regions are not values: unchanged rule |
| 13 | bare result from a region through a parameter | check 4: the result's set names the region |
| 13 | views on assignment and `unwrap` | the IR: an assignment is an assignment, whatever it was written as |
| 14 | region handle not counted in a result's region | elision computes the set from all arguments, handles included |
| 14 | ternary takes its first arm's region | the IR: two assignments to a temporary, each checked |
| 14 | a return in a loop ignores assignments below it | liveness and the back-edge |
| 14 | the walk reads only a pointer's outer region | regions inside a pointer are part of the type |
| 15 | loop seeding, one-pass borrow state, `for`-init locals | check 2 on the back-edge; nothing is seeded |
| 15 | a parameter's or local's own address as caller memory | `x&` is `{@b}` for every `x`, pointer or not |
| 15 | pointer locals skipped at scope end | check 2 does not care what type the local has |
| 15 | hashmap's slice fields open to safe code | trusted base: unchanged, kept small |
| 15 | raw pointer as a generic argument; std given a comparator beside `cstr_of` | check 5; the `cstr_of` rule simplifies to `temp` |
| 16 | a jump does not end a block; `(q&)*`; seed cap; records by name; stale records; own address history; step before body | the IR and check 2: jumps are edges, `(q&)*` is a place, there are no records |
| 17 | emitted C: `--`, promotion, bitfields, float suffix, names, sizes, escapes | not a region matter: the emitter emits explicit C, below |
| 18 | trigraphs, `callconv`, std's path, file names, depth limits | front end: unchanged small rules; the depth limits go with the walks |
| 19 | nested proc types never compared | invariance at any depth in the type, once |
| 19 | `new` kept a raw value; `reset` missed raw parameters | checks 5 and 3 |
| 20 | raw void converts; ternary arms differ inside; raw read back through a call | check 5; the IR; may-point-into through a call's arguments |

Outside the claim and unchanged: vec staleness stays a lint, now a one-level one (a pointer taken
from a container is dead after a non-const call on that container); null narrowing; zeroing of
types holding `*T`; bytes and carriers; names; directives; bounds.

## The emitter

Round 17 was not a region problem. It was the emitter leaning on C's rules: promotion to `int`,
`--` formed from two minuses, a literal's type read from its digits. Rust never relies on the
target's conversions. The emitter rule becomes one: every operand of every arithmetic and shift is
cast to its rin type before C sees it, every conversion is written, and no two adjacent tokens are
emitted without a space. That replaces ten patches with one place.

## What a program sees

Nothing new to write. `level`, `frame`, `scratch`, `permanent`, `in R`, `r: region R` stay as they
are. Two things get easier:

    parser: struct = { src: []const c8; pos: u64; }
    p: parser = { buf[0..n], 0 };        // a local over a local buffer: fine, p's region is @b

    first: proc(b: *bag) -> *i32 = {     // elision: the result is in b's region, nothing to say
        return vec<i32>at(b*.items, 0);
    }

The first is the stack view the blind test wanted most. A struct value binds one region for all
its pointer fields, as regions.md's `struct[in R]` meant; a local binds its block, `new<T>(r, ..)`
binds `r`, a global binds `permanent`. A struct field that names a region keeps it.

What gets stricter is what the holes were: a pointer into a block cannot leave it by any route,
and a raw pointer from a call is an argument, never a local. The engine's 90 raw locals and the
course's 37 will say how many of those were already the bug.

## What it removes

The borrow records and everything on them (`paths`, `retaken`, `scope_killed`, `movable`), loop
seeding, the back-edge check, the later-assignment check, `return_root_ok`,
`expr_returns_caller_memory`, own-address paths, `region_from_args`, the `raw_*` walkers,
`reset_kills`'s record walking, most of `safety_check_open_cstr`, and the depth caps of the walks
they drove. By family counts that is roughly a quarter of the checker, replaced by a lowering, two
dataflows and five checks: by my estimate three thousand lines for six to eight thousand. The
estimate is mine and unmeasured; shadow mode measures it.

## Shadow mode, as measured

The pass exists: `RIN_SHADOW=1 rin check file.rin` runs it after the old checker and prints what it
would refuse as `shadow:` lines, changing no verdict. About 600 lines in `main.c`, from `shadow_check_proc`
back to the `Sh` types. Measured on the day it was written, against the old checker:

| Programs | Old accepts, shadow accepts | Both refuse | Shadow refuses, old accepts | Old refuses, shadow silent |
|---|---|---|---|---|
| 45 accept tests | 45 | 0 | 0 | 0 |
| 300 generated programs | 300 | 0 | 0 | 0 |
| 249 attack programs clean by design | 249 | 0 | 0 | 0 |
| the course, 101 files | 75 | 0 | 0 | 26, imports that are not programs |
| the engine | 1 | 0 | 0 | 0 |
| 140 holes, rounds 13 to 20 | 11, clean by design | 75 | 0 | 41, all under rules that stay |
| 1,123 refused attack programs, rounds 1 to 12 | - | 193 | - | 884, all under rules that stay, 46 at parse |

"Rules that stay" was checked by message: region handles as values, const memory, temporaries, packed
fields, zeroing, null narrowing, carriers, unions, directives, names, type invariance inside a pointer,
raw void conversions, the stale lint. No lifetime family in either corpus is caught by the old rules
alone.

Three things the measurement corrected in the design above, now part of it:

- **A raw result goes by elision**, like a checked one: it points into what the call was given, or lasts
  for good when the call was given nothing. The engine keeps thirty such names from lookups by index.
  Only `cstr_of` hands out a one-statement pointer, since the program can take its terminator. A raw
  pointer read back out of a local's storage through a pointer is unknown, so it is a temporary too.
- **An aggregate carries its fields' regions**, whoever holds it: a struct copied, passed or returned by
  value is checked against the regions its fields name, not against the memory it was read from.
- **A local whose address is taken is memory others read through**, so what is assigned to it must
  outlive its block, as it must for a field.

## Plan

1. **This document.** Read, argue, decide. Done: the decision is to go.
2. **Shadow mode, one to two weeks.** Done in a day, above. The lowering and the analysis run beside the old checker on
   every program: the safety suite, the 1,538 attack programs, the course, the engine, the 300
   generated programs. Every disagreement is listed with its line. Acceptance: the new checker
   refuses every program the VM has shown to be a hole, and every program it refuses that the old
   one accepted is either a hole or a stack view the program must write differently, with the
   count known.
3. **Switch, one week.** Done in a day, above, up to the fresh rounds.
4. **Explicit C, two to three days,** independent of the above. Done, above.
5. **Later:** a checked build in which pointers carry their region's tag, checked on dereference,
   so a hole nobody has found is a stop and not corruption.

## The switch, as done

The pass refuses by default since the commit after the shadow measurement; `RIN_REGIONS=v1` turns that off.
The old lifetime rules then went one family at a time, with a gate after each: the safety suite with its
expectations moved to the new messages, the 1,548 attack programs, the course, the engine. An attack program
that became accepted named what the family still held, and the pass was extended there, never by putting
the rule back.

| Family deleted | What its deletion exposed, now in the pass |
|---|---|
| loop seeding, the back-edge check, assignments below a return in a loop | nothing: the graph already had it |
| own-address paths, the address-of-borrow refusal, `region_from_args`, the old return rule | a struct returned by value with a temporary in a raw field; an aggregate global read as a value |
| the raw-pointer walkers | a compound assignment's right side in liveness; array locals and array fields as views; aggregate places with only raw fields; statics; raw values read through parameter memory |
| `reset_kills` | nothing |
| the lifetime half of the borrow records | nothing; the stale lint stays, and now sees a struct local made from borrowed members |
| the region order inside `type_compatible` and `region_flow_rejects` | each member of an initializer list against its own field's region; a read-only view may see a longer region as a shorter one, never the reverse (kept as a type rule); an array parameter is the caller's memory |

Every attack program from twenty rounds is refused as before, the course and the engine pass, and `main.c` is
about 500 lines shorter net, with the pass about 900 lines of that. What remains of the old records is the
stale lint and a few dead fields, to be cleaned when nothing else is moving.

## The first rounds against it

Round 21, two attackers, the day of the switch. One aimed at the pass's model and found six holes from two
causes, all confirmed in the VM and closed: `reset` did not end what a pointer typed in a region *variable*
pointed into (`*T in R` with `R` bound to the reset region); and the pass's limits, on atoms and tracked
locals and on the fixed point settling, accepted a proc they could not follow. They refuse now: a proc past
them is split by its author, not waved through. The other attacker read every std proc safe code can reach
against what elision makes a caller believe about its result, built about twenty-five programs, and found
nothing. It noted three promises that rest on an invariant rather than on a body alone: a container header's
arena is the region the header lives in (only `make` sets it, and safe code cannot write it); `fmt_text`
points into `fmt`'s own region because `fmt_make` is the one safe constructor; and `cstr_of` is known by
name. Each is written down here so that a new constructor is checked against it.

Round 22's first attacker found the pass treating an `unsafe { }` block inside a safe body as more trusted
than checked code: what it assigned to a bare pointer became permanent, and nothing inside it was checked,
so `unsafe { reset level; }` silenced the reset check around it. Six programs, all faulting in the VM. The
pass now checks every statement, inside `unsafe` as outside, with these as the author's word: a cast speaks
for its declared type's region; a raw pointer, however it got there, lasts as long as the author says it does;
what an external or unsafe proc returns is its signature's; a `return` inside the block is the signature's
promise. A checked pointer keeps its region inside the block as outside, and a reset or a block's end inside
it counts for everything around it. An unsafe block weakens nothing around it.

Round 22's second attacker wrote eleven realistic programs from the design document alone, each keeping a
pointer into `frame` in a global typed `option<*T in frame>` and reading it after the frame ended, through
every shape it could think of. All eleven ran clean in the VM: such a global must be `option` because the
region's end empties it, so the read is a checked stop. They are in the corpus as clean by design. Its
complaint that a table of proc pointers was refused as a carrier was right, and is fixed: code has no region.

Round 23's realistic attacker, told to stay away from that family, built forty programs across level reloads
through calls, bare results redirected through globals and statics, generics instantiated with two regions,
proc-pointer tables, array views and copies, loops of every shape, aggregates by value and names kept in
tables, and filed none: everything was refused, or safe by construction where it was accepted. Its friction
report is about spelling, not safety: `level: region global;`, `level: region;` opened by a block, and
`r: region R` are three things with overlapping syntax, and a bare struct field cannot take a region
parameter's region, so the field must say `*T in R` itself.

Round 23's attacker on the pass found one more slip in the unsafe model: a cast inside `unsafe` to a checked
pointer that names no region took the value for permanent, where a bare `*T` says nothing about the region
and the operand's stands. Three programs closed; the fourth launders through a raw pointer, which inside
`unsafe` is the author's word, and stays clean by design. Everything else it pushed on held: bare struct
fields, address edges at two levels, region variables bound to `scratch`, elision through std, array copies.

Round 24's realistic attacker went to domains no round had tried: a byte serializer, an interpreter with
closures, an LZ compressor, hash-consing, a gap buffer, a scheduler's heap, a packet ring, graph search, a
hand-rolled resource manager. Twenty-eight programs, none inside the claim. What each domain cannot write
in safe rin is the list this document started with: a closure that outlives the frame it captured, a
pointer into a growing table kept across the growth, a cursor as a pointer into a buffer that moves, a
pointer handed out of a frame; each becomes an index, a copy, or an allocation in the region that outlives
it, and a lying length field or a released handle is a checked stop. What stays open is wrong answers:
aliasing two views of one object, an index reused, defined integer wrap, a stale read of memory that is
still there.

Round 24's attacker on the runtime and the oracle found nothing inside the claim across container moves,
long containers of short elements, globals across resets in every option shape, region-variable resets,
scratch escapes, float casts and uninitialised reads, and two things worth more than a hole. The
quarantine's budget guard switched it off for regions above the budget, the large `level` it exists for;
it is relative to the reservation now, with the biggest regions first. And the VM oracle judged by text:
a sanitizer's words in the program's own output, which a program can print or drown. It now reads the
sanitizers' own log files, which the program cannot write, keeps the exit code as a second signal, and the
poison controls prove on every run that a report reaches the file; a program that merely prints the words
is "ok (prints sanitizer words)". Its claim scan also took a struct field at column zero for a valueless
global and so filed a safe program as outside the claim; it reads braces now.

## Explicit C

Round 17's emitter bugs were C's promotion rules deciding what rin had already decided. The emitter now
casts both operands of every operation whose type rin knows to that type before C sees them
(`emit_binary_expr`, `typed`), so `u8 | i8` is computed as `u8`, and a shift's left operand is its own
width. What C still decides: an operation on an enum or a C typedef, where rin has no width to name.

## The checked build

Defence in depth for what no round has found. The first plan here was a two-word pointer, address plus
region tag, in checked builds; the engine rules it out, since structs with pointer fields cross the C
boundary to Win32 and D3D11 as they are. What the sanitizer build already does is most of a checked build:
every arena is poisoned when it ends, so a pointer into an ended region stops the program with the pointer's
origin, and the stack is covered by use-after-return and use-after-scope. The gap is a region *reused* after
`reset`: the next allocations land where the old ones were, and a stale pointer reads fresh objects of
another type with no complaint.

The checked build closes that with a quarantine, in the arena and only under the sanitizer: a cleared
region does not hand out its old range again until the arena has moved on by a budget, so a stale pointer
into it meets poison for as long as the budget lasts, and the arena wraps to its base when the reservation
would run out. No type changes, no ABI change, release builds untouched. It is not a replacement for the
pass: the pass refuses at compile time, the checked build stops at run time, and the second is there for the
day the first is wrong.

## What this still is not

Not a proof. It is an instance of a known system, regions with a lexical order plus liveness,
which is what step 7 of the original order of work asked for and what the current checker cannot
be. Not Rust: no ownership, no moves, no destructors, no `&mut`, no lifetime parameters to write.
Not threads. And the trusted base, now 748 lines of std and the emitter, stays trusted by reading.
