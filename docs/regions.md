# Regions: compile-time memory safety for arena programs

> **Implemented on the `regions` branch; see [As implemented](#as-implemented)
> at the end for what was built, where it differs from this design, and what
> is left.** This is the proposal for making rin memory-safe at compile time
> without a garbage collector and without a borrow checker, by giving the type
> system the one thing an arena program already has: a small number of named
> arenas with a known order of lifetimes. It reverses the "parked" verdict in
> [safety.md](safety.md) for the temporal half of that list. The spatial half
> (bounds, null, initialisation) is covered by
> [pointers-and-slices.md](pointers-and-slices.md) and the small items at the
> end of this document.
>
> Companions: [arrays.md](arrays.md) for why arrays stay C arrays;
> [clib-removal.md](clib-removal.md) for the std functions a safe build still
> needs; [pointer-syntax.md](pointer-syntax.md) for `^T`, which becomes the
> unsafe pointer.

## The claim

An arena program frees memory only by ending an arena: clearing it, or
rewinding it to a mark. Nothing is freed on its own. So the only way a pointer
can dangle is to outlive the arena it points into. If the compiler knows which
arena each pointer points into, and arenas end at known places, it can reject
every dangling pointer at compile time with one rule:

> A value may only hold pointers into arenas that live at least as long as the
> arena it lives in.

That is the whole design. Everything below is how the compiler learns which
arena a pointer points into, and how arenas get known ends.

The theory is Tofte and Talpin's region calculus (1994), which is proven
sound. It failed in practice because the ML Kit inferred the regions and put
them where no one could read them, and Cyclone (2002) failed because it
annotated every pointer and tried to be safe for all of C, individual `free`
included. rin's arenas are already explicit, already few, and already nested,
so nothing is inferred and almost nothing is annotated.

## What can go wrong in an arena program

Every memory error in a program that uses only arenas, and where each lands:

| error | today | with regions |
|---|---|---|
| index out of bounds | checked at run time on `[]T` and arrays | same |
| null dereference | silent | compile error: `?*T` must be tested first |
| read of uninitialised memory | silent, unless `push_zero` | compile error: every push zeroes unless `[uninit]` |
| wrong type through a cast | silent | compile error outside `[unsafe]` |
| pointer stored in something longer-lived than its arena | silent | **compile error: the rule** |
| pointer used after its arena is cleared or rewound | silent | **compile error: arenas end at block ends** |
| address of a local stored or returned | silent | compile error: the stack is the innermost region |
| pointer into a `vec` after the `vec` grew | silent | allowed; see [Containers](#containers) |
| data race | silent | not addressed; see [Not covered](#not-covered) |

The two bold rows are the ones no amount of runtime checking fixes cheaply,
and they are the two that regions make into compile errors for free.

## Regions are blocks

A region is introduced by a block, and ends with it. It is declared like
everything else in rin, `name: kind = value`, the way `done: label = { }`
is:

    level: region = {
        world: *world in level = load(level);
        while (running) {
            frame: region = {
                cmds: []command in frame = gather(frame, world);
                scratch: region of frame = {
                    sorted: []command in scratch = sort(scratch, cmds);
                    apply(world, sorted);
                }                             // frame rewound to where scratch began
            }                                 // frame cleared; its memory is kept
        }
    }                                         // level cleared

  * `name: region = { }` makes an arena called `name`, usable in the block as
    a value of type `region name` wherever a region is wanted. At the closing
    brace the arena is cleared. The memory stays reserved, so a region in a
    loop body is today's `memops_arena_clear` at the end of each iteration.
  * `name: region of parent = { }` is a mark on `parent`'s arena: today's
    `memops_arena_temp_begin` and `_end`, as a block. Pushes inside go to
    `parent`'s memory and are gone at the brace.
  * `permanent` is the region of the whole program. Globals live in it, and so
    does anything pushed into an arena that is never cleared.
  * The stack is a region too: each block of a proc is a region shorter than
    everything around it. `x&` for a local `x` is a pointer into the block that
    declared `x`.

Regions nest, so their lifetimes are totally ordered by nesting:
`permanent > level > frame > scratch > stack`. The order is the block
structure; nothing is declared.

`memops_arena_clear`, `_set_pos`, `_pop` and `_temp_end` called directly still
exist, in `[unsafe]` procs. Structured regions are the safe spelling of them.

## Pointer types carry a region

A pointer type may say which region it points into:

    p: *entity in level;
    xs: []command in frame;
    q: *entity;               // no region: borrowed

| pointer | may be read and written through | may be stored in a struct | may be returned | may be passed down |
|---|---|---|---|---|
| `*T in R` | yes | into a struct in a region ≤ R | yes, as `*T in R` | yes |
| `*T` (borrowed) | yes | **no** | **no** | yes |
| `^T` | in `[unsafe]` only | in `[unsafe]` only | in `[unsafe]` only | yes |

**Borrowed is the default and the common case.** A parameter `e: *entity` says
"I will use this for the duration of the call and not keep it," which is what
most parameters mean. It needs no annotation and the rule enforces it: a
borrowed pointer has no region, so no struct's region is ≤ it, so it cannot be
stored. Nor can it be returned, since the caller would not know how long it
lives.

A pointer into a known region converts to borrowed freely, `*T in R` → `*T`,
the way `*T` converts to `^T` today. The other direction needs a region, and
there is no cast for it outside `[unsafe]`.

`[]T in R` is a slice whose elements live in R, with the same rules. `?*T` is a
pointer that may be null and must be tested before use; it takes a region the
same way.

## Structs

A struct that holds pointers says the region it lives in; the rule then applies
to its fields:

    enemy: struct[in level] = {
        target: *entity in level;     // ok: level ≥ level
        path: []vec2 in frame;        // error: frame < level
        name: ^const c8;              // ok: ^T is unchecked and unsafe to use
    }

A struct with no `[in R]` is region-free: it holds no checked pointers, and it
may live anywhere. Most component types are region-free, which is what makes
them columns.

A `struct[in R]` may only be allocated in a region ≤ R, so its own pointers
outlive it. `new<enemy>(frame)` is an error; `new<enemy>(level)` is fine.

## Procs

A proc's parameters are borrowed unless they say a region. Returning a pointer
needs a region, and the region usually belongs to the caller, so procs that
allocate take a **region parameter**:

    gather: proc<R>(r: region R, w: *world) -> []command in R = {
        cmds: []command in R = new_slice<command>(r, 64);
        ...
        return cmds;
    }

`region R` is both a type-level name and the arena value, so `new_slice(r, n)`
is typed `[]command in R` from its first argument. This is the one piece of
lifetime-like syntax, and it is substitution only: at the call
`gather(frame, world)`, `R` is `frame`, and the result is `[]command in frame`.
No inference, no elision rules, nothing to work out across procs.

A proc without a region parameter may still return `*T in permanent`, or a
pointer it was given as `*T in R` for a named R. It may never return a borrowed
pointer or the address of a local.

Proc pointers carry the same signatures, so a dispatch table of
`*proc(c: *ctx) -> void` is all borrowed parameters and nothing to check.

## Allocation

Two generic procs replace `cast(memops_arena_push_zero(...), *T)`:

    new<T>:       proc<T, R>(r: region R) -> *T in R
    new_slice<T>: proc<T, R>(r: region R, n: u64) -> []T in R

Both zero the memory. `[uninit]` variants exist for the cases where zeroing is
measured to matter, and they are `[unsafe]`. rin-dod has 32 `cast(memops_arena_
push` sites today; all of them become one of these two calls with no cast.

`memops_arena_push` itself keeps returning `^void` and moves to `[unsafe]`,
along with `_realloc_`. The containers in std are its only callers.

## Containers

`vec<T>` holds the arena it grows into, so it has a region: `vec<T> in R`. Its
elements are in R, so `vec_at(v, i)` returns `*T in R`, and a `T` with pointer
fields must satisfy `T`'s region ≥ R, the struct rule again. `hashmap`, `deque`
and the rest are the same shape. A region-free `T` goes in a `vec` of any
region, which covers every column in rin-dod.

**Growth.** When a `vec<T> in R` grows, its old storage is not freed; it is
abandoned inside R, still mapped, still `T`s, until R ends. A `*T in R` taken
before the growth still points at valid, correctly typed memory. Writing
through it is lost and reading it is stale, which is a logic bug, not a memory
error. The dojo's block 8 in rin-dod demonstrates exactly this. Regions do not
reject it, on purpose: rejecting it is a borrow checker, and the data-oriented
answer is already in the course, which is to hold indices into packed storage
and pointers only into storage that does not move. std gets both:

  * `vec<T>`: packed, moves on growth, addressed by index;
  * `stable<T>`: chunked, never moves, hands out `*T in R` that stay right;
  * `handle<T>`: an index with a generation, checked at the container, as the
    dojo's block 1 and 3 build by hand.

If experience says the stale case must be rejected, a local rule does it with
no lifetimes: a `*T` obtained from a container is dead after any call that
takes that container as `*vec<T>`. That rule is intra-procedural and can be
added later without changing anything above. *It was added; see
[As implemented](#as-implemented).*

## Unsafe

`[unsafe]` is an attribute on a proc. Inside it, and only there:

  * `^T` may be dereferenced, indexed and stepped;
  * `cast` may change a pointer's type or give it a region;
  * `memops_arena_clear`, `_set_pos`, `_pop`, `_temp_end`, `_push` and
    `_realloc_` may be called;
  * `[external]` procs may be called with pointer arguments;
  * `[uninit]` allocation may be used.

An `[unsafe]` proc has an ordinary signature, and its callers are checked
against that signature like any other. So the obligation is on the author of
the `[unsafe]` proc: what it returns must be what its type says. That is the
same contract Rust puts on `unsafe fn`, and it puts std under the same audit:
`vec`, `hashmap`, `memops`, the reflection casts and the OS layer are the
unsafe surface, and they are small.

Safe code never sees `^T` except as an opaque value to hand back to C.

## Dynamic regions

Some arena lifetimes are not lexical: an arena per request handed to another
thread, an arena stored in a struct and cleared by a different system, a cache
cleared on a timer. Rust meets the same wall with `Rc` and `RefCell`: give up
the static check locally and check at run time, loudly.

    cache: region[dynamic] = region_make();
    e: *entry in cache = new<entry>(cache);
    region_clear(cache);
    e*.hits += 1;                 // trap: stale generation

A `region[dynamic]` has no block. Pointers into it are fat, address plus the
arena's generation, and every dereference compares generations. `region_clear`
bumps the generation, so a stale pointer traps at the exact instruction instead
of reading garbage. Pointers into a dynamic region may be stored anywhere,
since the check moves to the use. The cost is 16-byte pointers and a compare
per dereference, paid only where chosen, the way `RefCell` costs only where it
is used.

The dojo's block 1 is this mechanism for handles. Dynamic regions apply it to
pointers.

## Debug builds

Even with the static rules, two things help while developing:

  * **Guard pages.** On a region's end, the freed pages are `mprotect`ed to
    no access until the next push into them. Any `[unsafe]` bug that leaves a
    stale pointer faults at the use, in the debugger, at zero cost per access.
    Arenas are already reserved from the OS in `memops_os`, so this is page
    rounding on rewind, in debug only.
  * **`noalias` checked.** Two slices entering a `noalias` proc are compared
    for overlap, two instructions per call. In debug only, or always; it is
    cheap enough for either.

## What it costs

Measured on rin-dod before writing any of it, as the test corpus:

| | today | with regions |
|---|---|---|
| `cast(memops_arena_push...` | 32 | 0 |
| `memops_arena_clear` and `_temp_begin`/`_end` | 5 | 5 `region` blocks |
| struct types holding checked pointers | ? | each gets `[in R]`; to be counted |
| procs that allocate for the caller | ? | each gets `region R`; to be counted |
| borrowed parameters | most | unchanged, no annotation |

The two question marks are the real measure of the design, and the first
piece of work is to count them by hand over rin-dod's 35 programs and 13 dojo
blocks. If each is a handful, the design is right. If struct annotations are
everywhere, the defaults are wrong and that should be learned before the type
checker is touched.

## Not covered

  * **Logic bugs through indices.** A stale index reads the wrong row. That is
    the model's trade: memory errors become logic errors, which are contained
    and debuggable. `handle<T>` turns the common ones into detected errors.
  * **`vec` growth under a pointer.** Rejected after all, by the local rule
    above; see [As implemented](#as-implemented).
  * **Data races.** Regions say where memory lives, not who may touch it. A
    `thread` region (pointers from a thread-local arena may not be stored in a
    shared struct) is the same rule one level up and is a later design.
  * **Memory growth within a region.** An arena holds everything until it ends.
    Programs with unbounded churn inside one region use pools and free lists
    over indices, which is already how rin-dod's entity index works.
  * **Everything in [safety.md](safety.md)'s list that is not memory:**
    integer overflow, division by zero, lossy conversions. Those are separate,
    smaller changes: trap in debug, defined in release, `+%` for wrapping.
    Overflow, shifts and division are done; see
    [As implemented](#as-implemented).

## Why this is not Cyclone again

Cyclone put a region on every pointer, inferred some of them, and kept
`free`. The complaints in its papers are about the annotation count and about
`free` making the regions complicated. Here:

  * **Borrowed is the default**, so parameters, which are most pointers, say
    nothing.
  * **Structs carry the region, not their fields**, so a struct with five
    pointers has one annotation.
  * **There is no `free`**, so a region's contents have exactly one lifetime,
    the region's.
  * **Regions are the blocks the program already has.** The frame loop and
    the level load are regions in every arena program ever written; the
    keyword names what was already there.

And why it is not Rust: Rust gives every object a lifetime and infers how they
relate; this gives every arena a lifetime and reads how they nest. Rust's
guarantee covers any program; this covers programs that end memory by ending
regions, and declines the rest rather than fighting them.

## Order of work

1. **Count.** Annotate rin-dod on paper: which structs need `[in R]`, which
   procs need `region R`. A day. Decides whether the defaults are right.
2. **Typed allocation and `[unsafe]`.** `new`, `new_slice`, the attribute, and
   `^T`/casts rejected outside it. No regions yet. Removes every cast in
   rin-dod and finds the unsafe surface of std.
3. **`?*T`.** Null as a type. Small.
4. **Regions.** `region` blocks, `in R` on pointer types, `[in R]` on structs,
   `region R` parameters, the rule in the checker. Port rin-dod; it must
   compile with only the annotations counted in step 1.
5. **Try to break it.** A week of writing safe code that dangles. Every success
   is a bug in the design, found before anyone else looks.
6. **Dynamic regions, guard pages, `stable<T>`.**
7. **Write it up** against the region calculus, so the soundness argument is
   "this is an instance of a proven system" and not "we tried hard."

## Open

  * **Generics and regions together.** `vec<T> in R` with `T = *U in R2`
    needs `R2 ≥ R` checked at instantiation. rin monomorphises, so the
    checker sees the concrete types; the question is the error message.
  * **Region parameters on struct types.** `list<T> in R` is a region
    parameter on a type. Needed for std containers; the syntax should match
    procs.
  * **Globals.** `permanent` by default. A global `*T in frame` is an error.
    A global arena is a dynamic region unless it is only ever cleared at
    program end.
  * **Callbacks from C.** An `[external]` proc that calls back into rin with a
    pointer: the pointer is `^T`, so the callback is `[unsafe]`. Acceptable,
    or does a `[trusted]` attribute belong on the binding?
  * **Should `vec` growth under a pointer be rejected?** Resolved: the local
    rule is in, see [As implemented](#as-implemented).
  * **The name.** `region` reads well in the block form and the parameter
    form. `arena` is what the thing is. One word for both, or keep `arena` for
    the value and `region` for the type? Settled for now: `region` is the
    kind, as in `frame: region = { }` and `r: region R`.

## As implemented

The `regions` branch builds all of the above except dynamic regions and the
debug guard pages, and adds a few things the design left open. Everything
here is tested by `tests/run_rin_safety.py`: seven programs that must check,
compile and print their `.expected`, and sixty-eight that must be rejected
with the message in their first line. The same sixty-eight check clean with
`--unsafe`, except the five about the shape of a region block and its
syntax, which hold either way. The seven programs also run clean under AddressSanitizer and
UndefinedBehaviorSanitizer with the same output.

**Safe by default, `--unsafe` to turn it off.** Every proc body is checked
unless its proc is `[trusted]` or `[unsafe]`. `--unsafe` on the command
line, or `RIN_UNSAFE=1` in the environment, turns the rules off for a
program written before them; `--safe` turns them back on. The two torture
suites and all of rin-dod run unchanged that way.

**The unsafe boundary.** In a safe body these are errors: dereferencing or
indexing a `^T`; a `cast` to or from a pointer; calling an `[unsafe]` proc,
or a C proc that takes or returns a pointer; `= ?`; and holding a *carrier*
by value. A carrier is a struct with a raw pointer, or a pointer or slice
that names no region, anywhere inside it: `vec`, `hashmap`, `memops_arena`,
`string8`. Safe code uses one only through a pointer into a region,
`*vec<T> in R`, made by a trusted proc. The arena's raw operations
(`memops_arena_push`, `_push_zero`, `_pop`, `_set_pos`, `_clear`,
`_temp_end`, `_realloc_`) are `[unsafe]`, and so are the three procs that
make a slice or string from a bare pointer and length
(`slice<T>from_parts`, `string8slice_from_parts`, `string8slice_from_cstr`),
since the length is a claim about memory the compiler cannot see; the rest of
std that touches raw memory is `[trusted]`, 314 procs in all, including every
generic one, since a generic body is only checked per instance. A region may
be handed to a `region R` parameter only: passing it where a trusted proc
takes a bare `*memops_arena` is an error in safe code, so safe code cannot
reach the arena's raw operations through a trusted wrapper by accident.

**Regions.** Declared at top level, longest-lived first, each inside the one
before: `level: region;`, `frame: region in level;`, `scratch: region in
frame;`; `permanent` is implied before them. `frame: region = { }` opens
one, `scratch: region of frame = { }` marks the parent's arena. Pointer and slice
types take `in R`, binding to the innermost pointer or slice just parsed:
`[]*node in level in frame` is a frame slice of level pointers. `?*T` may be
null. A proc takes a region as `r: region R`, a variable bound at each call;
as `r: region R in level`, a variable that lives no longer than level, so
the proc may put level pointers in it; or as `r: region level`, the declared
region itself. `new<T>(r)` zeroes, `new<T>(r, value)` copies a whole value
in, `new_slice<T>(r, n)` zeroes n of them. A region handle lives only in a
parameter: not in a field, a local or a return type.

**The rules, as checked.** A pointer kept in memory names its region, and
points into one that lives at least as long as the memory holding it,
through struct fields, array elements, generic arguments and pointees; a
borrowed pointer, the address of a local, cannot be kept, and a safe proc
returns a pointer with no region only when it points into what the caller
handed it (below). Type compatibility carries
the rule, with a message that names the two regions. A field whose pointer
names no region is set only by the struct's trusted procs. A region block
opens a declared region, not one already open, inside only longer-lived
ones, with no `goto` or label in it; `return`, `break` and `continue` emit
its cleanup. A `*T` that is zero would be null, so a zeroed value, a `new`
without a value, or an initializer that leaves such a field out is an error;
`?*T` is tested with `if (p)`, `if (p != null)`, `p && ...`, `p ? ... : ...`
or an `if (!p) { return; }`, which narrows the name, and an assignment to
the name undoes the narrowing. Null compares with any pointer.

**Two things the design missed, found by trying to break it.** A proc that
opens `region frame` would clear the arena from under a caller that has
frame open, so each proc's opened regions are computed over the whole
program through every by-name call, a call is rejected while one of them is
open or while a shorter-lived one is, and such a proc cannot be used as a
proc pointer, since a call through a pointer could not be checked. And a
`region R` value kept in memory would allocate into a cleared arena, hence
the rule above.

**What the counts say.** `new` and `new_slice` replace every arena cast in
safe code. The four safe programs annotate the struct fields that hold
pointers, which is where the regions have to be written, and nothing else
beyond region blocks and parameters: a 150-line entity world with a slot
map, columns, a frame of commands and handles with generations has eleven
`in` annotations, all on fields or returns.

**Growth under a pointer, rejected after all.** The design above left a
pointer into a `vec` valid but stale after a push, as a logic bug. The
"local rule" it mentioned is in, because it turned out to need no lifetimes
and to cost nothing to write: every checked pointer or slice that names no
region, `p: *i32 = vec<i32>at(v, 0)` or `items: []i32 = vec<i32>items(v)`,
is a *borrow* and remembers the path it was taken from, `v` or `w*.hp`. A
call that passes an overlapping path where the callee may move things, a
non-const `*vec<T>` or `[]T`, kills the borrow; so does a write to the path
or to a field of it. Using a dead borrow is the error `'a' may be stale:
'w*.hp', which it was taken from, has been changed since; take it again`.
Passing the borrow and its source in one call is rejected too, since the
callee could move the storage before it reads the pointer. A pointer taken
from a borrow inherits its source. The rule is per proc, over names: a
borrow is dead after any loop body that could kill it, it is joined across
the two arms of an `if`, and `a = vec<i32>at(...)` brings it back. Readers
in std (`at`, `last`, `items`, `get`, `has`, `find`, `scan`, `first`,
`next`, `length`, `is_empty`) now take `*const` so two reads may live
together and a read kills nothing; `push`, `set`, `slot`, `remove`, `clear`
and the rest take `*T` and kill. The reject programs 040 to 043 are the push,
the loop, the passed-together call and the field write; accept program 005
shows the four idioms that stay legal.

**Integers.** Signed overflow wraps in every build but Debug, where it traps:
the generated CMake passes `-fwrapv` to gcc and clang outside Debug, and in
Debug `-fsanitize=signed-integer-overflow,shift-exponent,integer-divide-by-zero`
with `-fno-sanitize-recover`, which stops at the first one with the rin source
line. The two cannot be combined, since `-fwrapv` defines the overflow away
and silences the check. A shift by the width or more is defined: the compiler
emits the amount masked to the width, `x << (n & 31)` for a 32-bit `x`, so
the result is what Rust's release build gives rather than C's undefined
value; the Debug check on the exponent therefore never fires on rin's own
shifts and only guards trusted C. Division by zero traps in Debug and is
whatever the machine does otherwise, as in Rust's release build with
overflow checks off. MSVC gets no flags: it wraps in practice and takes
neither spelling. Lossy conversions are unchanged: `cast` is explicit
already.

**The std audit.** Every std proc a safe program can call was read against
its signature, because a `[trusted]` proc is a promise: a lie in its
signature is a hole in safe code. Twenty-one procs that returned `*T` and
null on a miss now return `?*T`: `hashmap get`, `hashset`, `dlist`, `list`,
`map`, `queue`, `stack`, and `reflect`'s `find_field`, `find_value_by_name`
and `find_value_by_value`; their callers in std were fixed to test the result.
`vec<T>push` aborts rather than writing through a null arena, which a zeroed
`vec` in trusted code could reach. `hashmap<K, V>make(r)` and
`hashmap<K, V>set(m, key, value)` were added, so a hashmap can be owned by a
region the way a `vec` is; accept program 006 is a word count in a region.
`list`, `map`, `queue` and `stack` took an arena directly and had no
`make(r)`; they now have one, and arena-free `push`, `pop`, `find` and the
rest beside their old procs, so safe code can own any std container.

**Threads.** There is nothing to check because there is nothing to call: std
has no thread, atomic or lock API, and a safe body cannot call a C proc that
takes a pointer, so a safe program is single-threaded by construction. The
`thread` region in [Not covered](#not-covered) is still the design for the
day std gets one; until then a program that needs threads does it in
`[unsafe]` code and owns the result.

**Returning a borrowed pointer: Rust's elision.** `get_position(w:
*world, e: entity) -> ?*vec2` returns `w*.ps[i]&` or null, and the rule
above would have demanded a region on it. Rust lets `fn get(&self) -> &T`
leave the lifetime off because it can only be the parameter's, and the same
holds here: a safe proc may return a pointer that names no region if every
`return` hands back null, a pointer into a region, or a place reached through
a pointer, slice or array parameter, directly or through a call on such
parameters; the address of a local or of a by-value parameter is refused
with "the address of a local is gone when the caller looks". The caller
already treats such a result as a borrow of every pointer it passed, so it
cannot keep it past a change to them. This closed the one thing that stopped
rin-dod's entity lessons from checking (below).

**The second attack.** With the rules above in place, sixty-two programs
were written to break them, each trying to get a pointer past the checker
and then faulting under AddressSanitizer. Seventeen did, in six families, and
each family is now a rule and a reject test (045 to 062):

  * *Locals outlive the region that filled them.* `q: ?*i32 in frame`
    declared before `frame: region = { q = new<i32>(frame); }` and read after.
    Now a local whose type names a declared region can only be declared
    where that region is open: inside its block, or in a proc whose
    parameters already carry a pointer into it, since such a proc is only
    ever called while it is open. A borrowed local assigned inside the block
    from region memory is caught the other way: every borrow remembers the
    regions whose handles went into making it, and the block's end kills
    them, "may be dangling: it was made in region 'frame', whose block has
    ended".
  * *Locals outlive the block of the value they borrow.* `q = x&` inside an
    `if`, read after it. A borrow's place path has a root; at the end of any
    block, the borrows rooted at a *value* declared in it, a struct, array or
    scalar whose storage is gone, are dead. A pointer local going out of
    scope kills nothing, since what it points at does not go with it.
  * *A borrow written into memory through a pointer.* `out* = x&` with `out:
    *?*i32`. The pointee type names no region, and the stored-type rule only
    looked at types, not at writes. Now an assignment to any place other than
    a bare local, whose type is a pointer or slice with no region, is an
    error: nothing says how long what is written there lives.
  * *Gaps in the staleness tracking.* Through an alias (`al: *inner in level
    = w*.inr; push(al*.v, ..)` did not kill what was taken from
    `w*.inr*.v`), in an array of borrows, through a subslice of a borrow, and
    through a `break` that left a loop right after the kill. Every pointer
    local is now tracked for the places it aliases, so a path through one
    expands to the places behind it; arrays and unions holding borrows are
    carriers like structs; a subslice inherits its source; and `break`,
    `continue`, the case ends of a `switch` and the state before a loop all
    join into the state after it, dead on any path being dead. A name
    assigned anywhere in a loop body is also unnarrowed before the body, and
    a name whose address is taken anywhere in the proc is never narrowed.
  * *A trusted struct's invariant, written from safe code.* `v*.length =
    1000000`, `keep*.data = tmp*.data`. The fields of a carrier are read-only
    in a safe body, "set only by its own procs"; `vec<T>pop` and `clear` do
    what the two lines in accept program 004 did by hand. A generic's type
    arguments are also checked on their own, so `vec<[]i32>` cannot smuggle a
    region-less slice into a region.
  * *Three direct bypasses.* A proc pointer to an `[unsafe]` proc, or to a C
    proc with pointers in its signature, cannot be taken in a safe body; a
    positional array initializer shorter than the array is refused when the
    element holds a non-null pointer; and `printfmt` refuses a pointer
    argument instead of generating C that does not link.

Also from that pass: a signed left shift is emitted through the unsigned type,
`(i32)((u32)x << n)`, since shifting into the sign bit is undefined in C even
with the amount masked; and Debug builds add `float-cast-overflow` to the
sanitizers. A second pass of sixty-eight programs, aimed at the new rules
themselves, got six more through, now reject tests 063 to 068: a ternary as
the source of a borrow (`q = c ? x& : y&` took no places); the address of a
carrier field (`p = v*.length&; p* = 1000000`), so taking one, or a slice of
one, is now refused like the write; a kill in a `for` step, which runs after
the body and so after its re-take; a call through a proc pointer named
directly, which had no kill hook; and a `*const w` parameter whose struct
holds a non-const `*vec`, through which a checked callee may push. A trusted
proc's `*const T` is a promise, so it still kills nothing; a checked proc's
is shallow, so it kills when T holds a pointer anywhere inside. Ten correct
programs written to provoke false positives all passed.

**The third attack: std itself.** The checker trusts 314 signatures, so
seventy-three programs called the trusted procs from safe code with the
worst arguments their signatures allow. The bodies held: every bad index,
empty pop, zero capacity, huge allocation and misaligned type ended in a
`rin runtime:` message or a correct result. Fourteen programs got through,
and every one was a signature that lied, in five shapes, now reject tests
072 to 077:

  * `node.next`, `list.head`, `queue.head` and `.tail`, `stack.head` and
    dlist's links were `*node<T>` and null at the tail or when empty; safe
    code read them as borrows and dereferenced. They are `?*node<T>` now,
    and the checker demands the test.
  * A slice's `length` and `data` were plain fields, so `xs.length =
    100000000` forged the bounds check. They are read-only in a safe body,
    like a carrier's.
  * A `*T` converted to `^void`, and `*c8` to `^const char`, so a trusted
    proc taking a raw pointer took the address of anything: a `memmove` at
    an offset into an `i32`, `print_cstr` reading a `[3]c8` past its end. A
    checked pointer no longer becomes a raw one in safe code at all: raw
    pointers there come from literals or trusted procs.
  * `reflect_field_at`, `reflect_value_at`, `reflect_field_info` and
    `reflect_find_field_info` returned `*const` and null out of range. They
    return `?*const` now, like the `find` procs before them.

Still trusted-only after this pass, and said so rather than fixed: `hashset`
and `dlist` have no `make(r)`; `string8`, `string8slice` and the builder are
carriers with no region constructor; a hashmap cannot be iterated from safe
code, since `keys` is a raw pointer. A `vec<[3]u64>` element taken by
pointer is a C-level compile error, since the emitter has no pointer-to-array
declarator; the C compiler catches it, so it is a gap, not a hole.

**rin-dod, ported: all of it.** The real measure. Every program in rin-dod,
the fifteen ECS chapters, the seven layout lessons with their shared
library, and the dojo's thirteen blocks with their harnesses, katas, levels
and playgrounds, exists in the safe subset: 100 programs, 23,039 lines,
1,444 procs, each printing its original `.expected` byte for byte and
running clean under both sanitizers, with `tools/check.py` running the two
versions side by side. 134 procs are `[trusted]`, none `[unsafe]`. Of the
trusted ones, nearly all are one thing repeated per lesson: the type-erased
byte column that an archetype table is built on, allocated with a size and
an alignment and viewed as `[]T`, which is unsafe by nature in any language
and is the `unsafe` block inside every Rust ECS crate too. The rest are
reading `argv`, `strcmp`, a struct made of bytes for the reflection
inspector, and the shared library's allocation core. The four entity lessons
needed two `?` and a format spec; the table lessons needed `level: region;`,
a `world_make(r: region level)` and `*vec<T> in level` on the world's
fields; the systems lessons made the frame arena a `frame: region in level;`
opened once per frame, so no command can outlive its frame by construction.
The ported course is in rin-dod's `safe` branch, each `safe` folder with a
README on what changed.

Porting it found five rules that were right but too blunt, now fixed: a
`printfmt` in a loop killed the borrows it printed, since it was an unknown
callee; `exit(1)` did not end a branch for narrowing; a checked reader
taking `*const world` killed every borrow in its caller, because the struct
holds pointers a body could push through, which marked twenty library procs
trusted for saying "I only read" until the moves analysis replaced the rule
(each checked proc now carries the set of parameters its body reaches a
moving call with, a fixpoint over the program; a trusted proc's set is its
signature; raw pointers and value parameters reach nothing); and a proc
pointer was treated as a pointer into memory, so a struct holding one was a
carrier and a proc could not be returned as a value.

**Where this leaves it against Rust.** Rust's safe subset has three things
this does not: a type system that proves every pointer, every container and
every thread, where this one trusts 314 std procs and audits them by hand;
narrowing and borrows over field paths, where this one tracks names and the
paths they were taken from; and ten years of people trying to break it. What
it has that Rust does not is the price: a region on each field that holds a
pointer, `?*T` on what may be null, and nothing else, with no lifetimes, no
ownership and no borrow annotations anywhere in the 370 lines of
safe programs in the suite.

**Not done.** Dynamic regions; guard pages in debug builds; narrowing of a
field path (`if (a*.next) { a*.next*.v }` needs a local); `stable<T>` and
`handle<T>` in std; a string type safe code can hold by value, since
`string8slice` carries a raw pointer and is trusted-only, which leaves safe
code with `^const char` literals; `+%` and the other explicitly wrapping
operators, which are not needed now that wrapping is the default;
`INT_MIN / -1`, which traps on x86 and wraps on ARM in release, and a float
to integer cast out of range, which is undefined in C and only caught in
Debug; `make(r)` for `hashset` and `dlist`, and an iterator for `hashmap`;
pointers to array elements in generated C; a mechanical check that a
`[trusted]` signature is honest, which stays a review, though three attack
rounds are the next best thing; a brace escape in `printfmt`; and a C name
collision when a struct is called `system`, which the emitter does not
mangle.
