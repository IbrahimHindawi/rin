# Why safe rin has no memory errors

This is the argument behind the claim, written so it can be attacked. It is not a formal
proof. Each section says what is claimed, which rule holds it up, and what that rule assumes.
To break the language, break an assumption or find a path the table misses, then see
[attacks/README.md](../tests/rin-safety/attacks/README.md).

## What safe rin is for

Safe rin does not try to make any program safe. It makes one way of programming safe, the way
working arena code is already written, and anything outside it is `unsafe` or cannot be written.
The patterns come from a shipped engine, and a feature earns its place by a pattern needing it:

| Pattern | What makes it safe |
|---|---|
| Memory lives as long as its arena | The region is part of a pointer's type |
| Scratch work | A block region, which ends on every way out |
| A whole lifetime ends at once | A region's end is one event, in one place |
| Arrays sized once | A fixed length, checked on every access; they never move |
| Things refer to each other by index | Indexes are checked against their table |
| Pointers used briefly | Locals and parameters may not outlive what they point at |
| GPU buffers and files | Tied to a region, and released when it ends; there is no release call to forget or misuse |

There are no destructors, no ownership, no borrow checker and no methods: nothing runs that the
program does not show, and a region's end is written where it happens.

The checker fails closed. When it cannot work something out it adds a check at run time or refuses
the program; it never takes not knowing for nothing to check.

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
   passed to. It only travels.

## Each error, and what stops it

| Error | What stops it | Where | Assumes | Tested by |
|---|---|---|---|---|
| Pointer outlives its region | A value holds pointers only into regions at least as long as its own | `region_flow_rejects`, `inner_flow_rejects` | Regions are a known order; a block ends where it is written | `reject/`, `attacks/r1-*`, `r2-*` |
| Pointer laundered through a generic, alias or `**T` | Places reached through a pointer are invariant | `type_flow_same` | Generic bodies are checked per instance | `attacks/r2-*`, `r4-*` |
| Region re-entered while open | The checker tracks what each proc opens | `opens_collect_*` | Calls through pointers are refused a region | `reject/`, attack probes |
| Local's address escapes | The stack is the innermost region | Region rules, `borrow_*` | Locals do not outlive the frame | `reject/` |
| Stale pointer after a move | Borrow tracking kills it; the old buffer also stays valid until the region ends | `borrow_*`, `compute_proc_moves` | Arenas never free one object | `reject/`, `attacks/r3-*` |
| Null dereference | `option<*T>` must be tested; narrowing ends at anything that could change it | `narrow_*` | A field changes in any callee and through any path | `reject/`, `accept/010` |
| A zero in a non-null pointer | `{}`, `new` and `new_slice` refuse a type holding a `*T` | `safety_check_zeroed` | Reflection and generics see through to the field | `reject/`, attack probes |
| Out of bounds | `[]T` and arrays are checked in every build; a slice that can change is read whole | `RIN_INDEX`, `rin_slice_at`, `rin_slice_range` | Slices are `{ data, length }`; `length` is not written by safe code | `poison/008`, `poison/012`, `accept/026` |
| Size wraps an allocation | Counts go through `memops_bytes`; the arena subtracts to compare | `memops_arena_carve_` | Every counted allocation in std uses it | `poison/005` to `007` |
| Read of uninitialised memory | Every declaration has a value; `= ?` is `unsafe`; `push_zero` zeroes | Parser, `Stmt_Var` check | Plain `push` is only reached from `unsafe` | `reject/`, attack probes |
| Wrong type through bytes | A cast to or from a pointer is `unsafe`; unions hold plain data only | `type_mentions_pointer`, carrier rules | Plain data has no invalid bit patterns | `attacks/r1-11*`, `r4-78`, `r4-79` |
| Use after a file closes | Handles carry a generation | `std/file.rin` | The table is the only way to reach a stream | `attacks/r4-*` |

Three rules that are easy to miss, because they hold up the others:

- **Names.** The emitted C spells types and the runtime by name in the scope of the code around them. A
  local may not be named like a type or a runtime helper, nor a proc or global like the C library
  (`safety_check_local_name`, `safety_check_program_names`).
- **Directives.** A preprocessor line applies to the text of the C, under every rule here, so safe code has
  none: a constant is `N: const i32 = 4`, and C is handed its value. Any preprocessor line, above a field
  or an enum item as well, needs `// unsafe: why` (`safety_check_directive_lines`). A `cinclude` is C, and
  trusted as C is.
- **Generics.** An instance is named without regions, so each generic body is checked again with the
  regions its type arguments really have (`region_instance_note`).

C strings have a rule of their own. `cstr_of` finds a terminator, which is only good while nothing can
overwrite it. For an array it hands back a private copy, which nothing else can reach. For a slice it may
only be read by std, or by a proc that touches no global and has nothing to write through, with no writing
call beside it (`safety_check_open_cstr`).

## What the emitted C assumes

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
| `restrict` broken | A `noalias` proc is safe to call only if it takes plain data, touches no writable global, and calls only procs like itself, so its parameters are the only way in; overlap is checked on entry in every build. Any other needs `unsafe` | `compute_proc_noalias`, `RIN_NOALIAS` |
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

| Check | What it would catch |
|---|---|
| `tests/run_rin_safety.py` | A rule that stopped holding |
| `tests/run_rin_attacks.py` | An earlier hole coming back |
| `tests/run_rin_poison.py` | A stale pointer or overrun from `unsafe`, under ASan; a check gone from a Release build |
| `tests/run_rin_vm.py` | Attack programs that check clean, run in a VM with no way back to the host |
| `tests/gen_rin_safe.py` with `run_rin_vm.py --compare` | Random safe programs: a sanitizer report, a fault, or `-O0` and `-O2` disagreeing |

None of these prove the claim. They only look for counterexamples, and how hard they have looked is
the honest measure of how much to believe it.
