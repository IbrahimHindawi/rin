# Why safe rin has no memory errors

This is the argument behind the claim, written so it can be attacked. It is not a formal
proof. Each section says what is claimed, which rule holds it up, and what that rule assumes.
To break the language, break an assumption or find a path the table misses, then see
[attacks/README.md](../tests/rin-safety/attacks/README.md).

## What is promised

The promise is Rust's, and it is a definition: **any program `rin check` accepts, with no `unsafe`, `[external]` or
`cinclude` of its own, that commits a memory error is a bug in rin**, however contrived the program is. Nothing is out
of scope for being hostile.

That says what counts as a bug. It does not say none are left:

- What is known to be open is in [known-holes.md](known-holes.md). What could be hit by code written in good faith is
  fixed first, and what takes a deliberate attack is fixed or listed there.
- This is young and has not been tested by anyone outside the project. Every attack round so far has been run from
  inside it, and most have found something. Build with the sanitizers while developing.
- rin is not a sandbox. Do not compile source you do not trust.
- It covers the code outside `unsafe`. An engine's boundary with C and the C it links are checked only by reading them.

## What safe rin is for

Safe rin does not try to make any program safe. It makes one way of programming safe, the way
working arena code is already written, and anything outside it is `unsafe` or cannot be written.
The patterns come from a shipped engine, and a feature earns its place by a pattern needing it:

| Pattern | What makes it safe | In the language |
|---|---|---|
| Memory lives as long as its arena | The region is part of a pointer's type, worked out from what is stored | `*T in level`, or nothing on a field |
| Scratch work | A block region, which ends on every way out | `scratch { }` |
| A level's state in globals, ended in one place | The end empties every global holding that memory, and no local that may point there can be read after | `level: region global;`, `reset level;` |
| Arrays sized once | A length known at compile time, checked on every access; they never move | `[N]T`, `new_slice<T>(r, n)` |
| Things refer to each other by index | An index fits one table, and finds nothing once its slot is reused | `table<T>`, `index<T>` |
| Pointers used briefly | Locals and parameters may not outlive what they point at | `*T` with no region |
| GPU buffers and files | Tied to a region, and released when it ends; there is no release call to forget or misuse | `memops_arena_on_end`, `file_open_in` |

Std's containers in the safe subset are `vec`, `table` and `hashmap`. `list`, `dlist`, `map`, `hashset`, `deque`,
`stack`, `queue`, `heap`, `dsu`, `array` and the node types are `proc[unsafe]`: no safe program relies on them, so
their bodies are not part of what the claim trusts. `python tests/rin_trusted_lines.py` counts what it does trust:
the lines of `unsafe` that safe code can reach, following calls by name, now 353 as the block layout counts them. What is left is what cannot be
written otherwise: the arena, the allocations of the containers, and the calls to C. Strings, formatting,
reflection and the map's probing are checked code, through a few views.

A proc can be `proc[static]`, like a static global: it belongs to the file that declares it, and a body anywhere
else may not name it. It is how a file keeps a helper that would break an invariant if anyone could call it: the
map's views of its arrays reach slots nothing wrote, so they are `hashmap`'s alone. An `unsafe { }` block may still
call one, and is then the author's.

The generated C is GNU C, for clang and gcc: `__typeof__`, an empty struct and a zero-length array mean what
those compilers define them to, and standard C does not cover them.

The pointer rules are the second pass of [regions-v2.md](regions-v2.md): one outlives rule, checked on a lowered
form of each body. The rules below that are not about lifetimes are unchanged by it.

What safe rin cannot express, on purpose: lifetimes that overlap without nesting, freeing one object for
its memory back, data that churns forever in a region that never ends, long-lived data pointing at
shorter-lived data, threads, closures and coroutines, and type-erased or raw memory. Each of those is an
index into a table, a copy, or a small `unsafe` core.

There are no destructors, no ownership, no borrow checker and no methods: nothing runs that the
program does not show, and a region's end is written where it happens.

The checker fails closed. When it cannot work something out it adds a check at run time or refuses
the program; it never takes not knowing for nothing to check. A constant is computed by rin and handed to C
as a number, so the two cannot disagree. Every expression and statement the emitter writes must have been
through the checker, or the build stops.

## The claim

Take a single-threaded program with no `unsafe { }`, no `[unsafe]` proc, no line marked `// unsafe`, no
`[external]` or `[external_emit]` declaration or `cinclude` of its own, and no global declared without a value (which is C's),
checked by `rin check` and built without `RIN_NO_BOUNDS_CHECKS`. If everything under
[What is trusted](#what-is-trusted) is correct, then when it runs:

- no read or write touches memory outside a live object of the expected type, and
- it reaches no undefined behaviour in the C it compiles to.

It may still stop on a checked failure (out of bounds, a `must` on null, division by zero,
overlap at a `noalias` entry, an arena out of room, a stack overflow at the guard page). It may
also have logic bugs. Neither is a memory error.

## What is trusted

The argument rests on these and checks none of them:

| Trusted | What goes wrong if it is wrong | How much there is |
|---|---|---|
| The bodies of `unsafe { }` in std | Every safe program inherits the bug | Count them: `tests/run_rin_vm.py` runs the attacks against them |
| `proc[no_move]` in std | It says a trusted body moves nothing, so pointers taken before the call stay good. Only std may say it | Read `vec<T>at`, `last` and `items` |
| A raw C string safe code holds is terminated | `cstr_view` measures it by reading to the terminator. It is a literal, a `str`, or what `cstr_of` found a terminator in. Of an array, `cstr_of` hands over a private copy; of a slice, only std's readers may read it, with nothing that could write beside it | `cstr_view`, in `slicecore.rin`, and `safety_check_open_cstr` |
| A `reflect` is as the compiler emitted it | The slices in `reflect_field_slice` and `reflect_value_slice` take their length from its count. Safe code cannot make, hold by value or change one | Those two procs |
| C the program links, and calls from `unsafe` | Anything | Per program |
| What a program says of C: `proc[external]`, `proc[external_emit]`, `struct[external]`, a global with no value, `cinclude` | No checker can see past a declaration. `free: proc[external](p: u64)` is callable from safe code, and is a lie | Per program; they are excluded from the claim above |
| The checker and emitter in `src/main.c` | A rule below does not hold | About 26,000 lines |
| The C compiler, the OS and the hardware | Anything | Out of scope |
| The stack guard page | A deep recursion could reach other memory | Windows probes by default; `rin build` asks for `-fstack-clash-protection` elsewhere |

Safe code cannot start threads, so data races are outside the claim. Nor can it reach the memory
of another thread started by C it links; if such a thread touches rin memory, the claim does not
cover the program.

## What holds at every point in a run

The rules below keep these true. The argument is an induction over execution: each is true when
`main` starts, and no safe operation, and no call to a correct std proc, makes one false.

1. **Pointers.** A `*T in R` points at a live `T`, in `R` or something that outlives `R`. A `option<*T>`
   is that or null. A pointer with no region named is a parameter or local that cannot outlive
   what it was made from.
2. **Slices.** A `[]T` has `length` live `T`s at `data`, or a length of 0. A slice's fields cannot be written, or stated in a literal: only an array, a subrange or `new_slice` makes one.
3. **Regions.** Memory in a region stays valid until the block that opened the region ends, and
   that block's end is the only thing that takes it back. No region is open twice.
4. **Values.** Every local, field and element has been given a value of its type. Every non-null
   pointer in memory is non-null.
5. **Raw pointers.** A `^T` in safe code is never read through, stepped or kept past the call it was
   passed to. It only travels. A raw parameter is not stored in memory, however it gets there.

## Each error, and what stops it

| Error | What stops it | Where | Assumes | Tested by |
|---|---|---|---|---|
| Pointer outlives its region | One rule on a lowered body: a value may flow into a place only if every region it may point into outlives the place's. Blocks, parameters and a statement's temporaries are regions too | `shadow_check_proc`, check 1 (`sh_check_outlives`) | Regions are a known order; a block ends where it is written | `reject/`, `attacks/r1-*` on |
| Pointer laundered through a generic, alias or `**T` | Places reached through a pointer are invariant | `type_flow_same` | Generic bodies are checked per instance | `attacks/r2-*`, `r4-*` |
| A region result left unsaid | A result names its region, or is the caller's memory through a pointer parameter (elision), or lasts for good; by value, its fields' regions hold what they name | check 4 (`sh_check_return`) | - | `reject/270`, `279` to `281`, `attacks/r13-*`, `r14-*` |
| A region handle kept or changed | A region is a parameter and nothing else: no fields, aliases, arrays, generics, casts, addresses or assignment | `parse_type`, `region_type_ok` | - | `attacks/r13-*` |
| A loop's next pass | The body is a graph with a back-edge; liveness and the forward fixed point see the next pass as a path | the lowering, checks 2 and 3 | - | `attacks/r14-*`, `r15a-*`, `r16-*` |
| A variable's own address | `x&` is the block that declared `x`; a local whose address is taken holds only what outlives its block, and is live wherever a local holding its address is | `sh_place_memory`, address edges in liveness | - | `reject/287` to `289`, `294`, `attacks/r15a-*`, `r16-*` |
| Region reset under a pointer | No local or parameter live after a reset, or after a call to a proc that resets, may point into the region or anything inside it; a parameter's memory may be anywhere, so it counts; a global holding it is emptied, and so must be `option` | check 3 (`sh_check_reset_at`), `emit_region_forget` | Every call that can reset is found by name; calls through pointers are refused one | `reject/237` on, `attacks/r12-*`, `r19-*` |
| Region re-entered while open | The checker tracks what each proc opens | `opens_collect_*` | Calls through pointers are refused a region | `reject/`, attack probes |
| Local's address escapes | Leaving a block, by its end or a jump, nothing live may point into it or into a region it opened | check 2 (`sh_check_leave`) | - | `reject/`, `attacks/r15a-*`, `r16-*` |
| Stale pointer after a move | A lint, outside the claim: a pointer taken from a container dies when a call may move the container or it is written; the old buffer stays valid until the region ends | `borrow_*`, `compute_proc_moves` | Arenas never free one object | `reject/`, `attacks/r1-*` |
| Null dereference | `option<*T>` must be tested; narrowing ends at anything that could change it | `narrow_*` | A field changes in any callee and through any path | `reject/`, `accept/010` |
| A zero in a non-null pointer | `{}`, `new` and `new_slice` refuse a type holding a `*T` | `safety_check_zeroed` | Reflection and generics see through to the field | `reject/`, attack probes |
| Out of bounds | `[]T` and arrays are checked in every build; a slice that can change is read whole | `RIN_INDEX`, `rin_slice_at`, `rin_slice_range` | Slices are `{ data, length }`; `length` is not written by safe code | `poison/008`, `poison/012`, `accept/026` |
| Size wraps an allocation | Counts go through `memops_bytes`; the arena subtracts to compare | `memops_arena_carve_` | Every counted allocation in std uses it | `poison/005` to `007` |
| Read of uninitialised memory | Every declaration has a value; `= ?` is `unsafe`; `push_zero` zeroes | Parser, `Stmt_Var` check | Plain `push` is only reached from `unsafe` | `reject/`, attack probes |
| Wrong type through bytes | A cast to or from a pointer is `unsafe`; unions hold plain data only | `type_mentions_pointer`, carrier rules | Plain data has no invalid bit patterns | `attacks/r1-11*`, `r4-78`, `r4-79` |
| Use after a file closes | Handles carry a generation | `std/file.rin` | The table is the only way to reach a stream | `attacks/r4-*` |

Three rules that are easy to miss, because they hold up the others:

- **Names.** A local `x` keeps that name in C unless the emitted C uses it for something else, and is then
  `x$rin_local`, which rin cannot spell. So it hides nothing the emitted C spells by name. A local may still not be what rin itself reads by name: an enum or the C name of
  its member, `printfmt`, `unwrap` and the like. A proc or global may not be named like the C library
  (`safety_check_local_name`, `safety_check_program_names`).
- **Directives.** A preprocessor line applies to the text of the C, under every rule here, so safe code has
  none: a constant is `N: const i32 = 4`, and C is handed its value. Any preprocessor line, above a field
  or an enum item as well, needs `// unsafe: why` (`safety_check_directive_lines`). A `cinclude` is C, and
  trusted as C is.
- **Generics.** An instance is named without regions, so each generic body is checked again with the
  regions its type arguments really have (`region_instance_note`).

C strings have a rule of their own. `cstr_of` finds a terminator, which is only good while nothing can
overwrite it. For an array it hands back a private copy, which nothing else can reach. For a slice it may
only be read by std's readers, with no call that could write beside it in any place C gives no order to
(`safety_check_open_cstr`). A proc of the program's own takes a `[]const c8` instead, which has a length to stop
at. This used to ask whether such a proc could write the buffer, and that question had holes (round 25).

A local or parameter `x` is `x` in C if that name is used for nothing else in the C written for the program, and
`x$rin_local` if it is, since rin cannot spell a dollar. A local that is called keeps the suffix too, because `name(` is
what makes a function-like macro fire. So a local cannot hide anything the generated C says by name, and a debugger shows
an ordinary variable's plain name unless it clashes. `$` in a name is a GNU C extension, which clang and gcc take and
`-pedantic-errors` refuses. What the checker or the generated C reads by name, `printfmt`, `unwrap`,
`new`, `cstr_of`, `print_cstr`, `print_enum`, is not a program's to name.

What answers for a type operation is what was declared to: `hash: proc<ky>` answers for `hash<ky>`, and a plain
proc only spelled `hash_ky` does not, for `printfmt`'s `print` as well. A value with no designator may not follow
one with a designator.

## What the emitted C assumes

Arithmetic reaches C with both operands cast to the type rin gave the operation, and a shift's amount masked
to its width, so C's integer promotions and `int` arithmetic never decide a result rin typed narrower or
unsigned ([regions-v2.md](regions-v2.md), "Explicit C"). The checker refuses a proc its pointer pass cannot
follow within its limits, rather than accepting it unchecked.

C has undefined behaviour that the rules above cannot see, and an optimiser may use any of it to
remove a check. So the emitter never relies on a C flag to define these:

| C would be undefined | What rin does | Where |
|---|---|---|
| Signed `+ - *` overflow, `-MIN` | Through the unsigned type: it wraps | `emit_binary_expr`, `emit_defined_compound` |
| `u16 * u16`, computed in `int` | Through `u32` | same |
| Division or remainder by zero | Stops the program | `rin_idiv_*`, `rin_irem_*` |
| `MIN / -1`, `MIN % -1` | Wraps, and 0 | same |
| Shift by the width or more, or negative | The amount is masked | `emit_binary_expr`, `emit_defined_compound` |
| Signed left shift into the sign bit | Through the unsigned type | same |
| Float to integer out of range, NaN | Saturates, NaN is 0, as in Rust | `rin_f2i_*` |
| Reaching the end of a proc without a value | Stops the program | `rin_no_return` |
| `restrict` broken | `noalias` belongs on a `proc[unsafe]`, and the promise is the author's; overlap is still checked on entry in every build | `RIN_NOALIAS` |
| A variable-length array | A length is known at compile time, and the generated C makes one an error, in `unsafe` code as well | `resolve_array_count_constants`, `-Wvla` |
| Arithmetic C types its own way | Arithmetic is on numbers: an enum or a boolean is cast first. A ternary's arms are one type | `type_check_binary_op`, `type_check_ternary` |
| Unspecified order around a call | A slice is read whole; a subrange reads each bound once; a base read twice must be pure | `rin_slice_at`, `rin_slice_range`, `expr_is_pure` |
| Width only C knows (`long`, an enum, a generic's differing instances) | C picks the helper by type | `RIN_WRAP_OP`, `RIN_ANY_INT`, `RIN_ANY_NUM` |
| Overlapping `memcpy` | Array and slice copies use `memmove` | `Stmt_Assign`, `slice<T>copy` |
| Strict aliasing | Safe code has no cast that could break it | Cast rule |

| Writing to a const object | A pointer, slice or array parameter made from a const place must say const | `safety_check_place_for_view` |
| A misaligned pointer | No pointer or slice to a field of a packed struct | same |
| Reading and writing a stream with nothing between | std seeks or flushes, and flushes only output | `std/file.rin` |
| A printf conversion that does not fit | Arguments are cast to what was worked out; `%s` takes only a C string, once; no flags C leaves undefined | `printfmt_try_single_call` |

Four things remain assumed of the C compiler, and are worth knowing:

- A conversion from a wider integer to a narrower signed one wraps. C calls this
  implementation-defined rather than undefined, and every compiler rin targets wraps.
- A signed right shift is arithmetic, for the same reason.
- Floats follow IEEE 754 (C's Annex F): division by zero and out-of-range `f64` to `f32` give infinity.
- A loop with no side effects may be removed by a C compiler. That can change whether a program
  hangs, but touches no memory.

## What a checked program may still do

- Stop, on any failure listed above.
- Exhaust its stack or an arena, and stop.
- Compute the wrong answer, including through two pointers to the same object, which rin allows on
  purpose. Exclusivity would catch those, at the cost of a borrow checker; see [regions.md](regions.md).

## How the argument is tested

The VM runner judges a program by the sanitizers' own report, written to a file the program cannot reach
(`log_path`), and by the exit code; what the program prints decides nothing, and the poison controls prove
on every run that a real report arrives in the file.

| Check | What it would catch |
|---|---|
| `tests/run_rin_safety.py` | A rule that stopped holding |
| `tests/run_rin_attacks.py` | An earlier hole coming back |
| `tests/run_rin_poison.py` | A stale pointer or overrun from `unsafe`, under ASan; a check gone from a Release build |
| `tests/run_rin_vm.py` | Attack programs that check clean, run in a VM with no way back to the host |
| `tests/gen_rin_safe.py` with `run_rin_vm.py --compare` | Random safe programs: a sanitizer report, a fault, or `-O0` and `-O2` disagreeing |

None of these prove the claim. They only look for counterexamples, and how hard they have looked is
the honest measure of how much to believe it.
