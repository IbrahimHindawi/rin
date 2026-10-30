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

A region is introduced by a block, and ends with it:

    region level {
        world: *world in level = load(level);
        while (running) {
            region frame {
                cmds: []command in frame = gather(frame, world);
                region scratch of frame {
                    sorted: []command in scratch = sort(scratch, cmds);
                    apply(world, sorted);
                }                             // frame rewound to where scratch began
            }                                 // frame cleared; its memory is kept
        }
    }                                         // level cleared

  * `region name { }` makes an arena called `name`, usable in the block as a
    value of type `*memops_arena` wherever an arena is wanted. At the closing
    brace the arena is cleared. The memory stays reserved, so a `region` in a
    loop body is today's `memops_arena_clear` at the end of each iteration.
  * `region name of parent { }` is a mark on `parent`'s arena: today's
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
added later without changing anything above.

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
  * **`vec` growth under a pointer.** Allowed, as above.
  * **Data races.** Regions say where memory lives, not who may touch it. A
    `thread` region (pointers from a thread-local arena may not be stored in a
    shared struct) is the same rule one level up and is a later design.
  * **Memory growth within a region.** An arena holds everything until it ends.
    Programs with unbounded churn inside one region use pools and free lists
    over indices, which is already how rin-dod's entity index works.
  * **Everything in [safety.md](safety.md)'s list that is not memory:**
    integer overflow, division by zero, lossy conversions. Those are separate,
    smaller changes: trap in debug, defined in release, `+%` for wrapping.

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
  * **Should `vec` growth under a pointer be rejected?** Leave it as a logic
    bug (this document's position), or add the local rule.
  * **The name.** `region` reads well in the block form and the parameter
    form. `arena` is what the thing is. One word for both, or keep `arena` for
    the value and `region` for the type?

## As implemented

The `regions` branch builds all of the above except dynamic regions and the
debug guard pages, and adds a few things the design left open. Everything
here is tested by `tests/run_rin_safety.py`: four programs that must check,
compile and print their `.expected`, and thirty that must be rejected with
the message in their first line. The same thirty check clean with
`--unsafe`, except the two about the shape of a region block, which hold
either way.

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
`_temp_end`, `_realloc_`) are `[unsafe]`; the rest of std that touches raw
memory is `[trusted]`, 286 procs in all, including every generic one, since
a generic body is only checked per instance.

**Regions.** `region level > frame > scratch;` once, at top level, longest
first; `permanent` is implied before them. `region frame { }` opens one,
`region scratch of frame { }` marks the parent's arena. Pointer and slice
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
borrowed pointer, the address of a local, cannot be kept or returned, and a
safe proc's returned pointer names its region. Type compatibility carries
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
beyond `region` blocks and parameters: a 150-line entity world with a slot
map, columns, a frame of commands and handles with generations has eleven
`in` annotations, all on fields or returns.

**Not done.** Dynamic regions; guard pages in debug builds; narrowing of a
field path (`if (a*.next) { a*.next*.v }` needs a local); `stable<T>` and
`handle<T>` in std; `?*T` on std's find procs, which still return `*T` and
null, trusted; integer overflow, division and shifts, which are
[safety.md](safety.md)'s list; data races; and rin-dod itself, which checks
only with `--unsafe`: its first lesson alone has 63 errors in safe mode, 38
of them direct `printf` calls, 13 writes to std fields with no region. The
course was written for C's model and porting it is the real measure of the
annotation cost; the counts above are from programs written for this one.
