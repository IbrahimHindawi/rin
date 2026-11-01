# Known holes

Any program `rin check` accepts that commits a memory error is a soundness bug in rin, however contrived it is. This
page lists what is known to be open, so that the claim in [soundness.md](soundness.md) is read with it. It is not a
proof that nothing else is open: attack rounds 25 to 35 found about forty causes in about 1,600 programs, nearly every
round found new ones, the last of them in the newest change, and none was run by anyone outside this project.

An entry leaves this page when it is closed and a test in `tests/rin-safety` holds it.

## Memory errors

None known to be open. Every program that has faulted in an attack round is closed, and is in `tests/rin-safety/attacks`.

That is a statement about what has been found. Rounds keep finding more, so expect this section to fill.

## Order of evaluation still left to C

A call (its callee first, then its arguments), binary operators and assignments run left to right, in
`emit_expr_in_order` and the assignment case of `emit_stmt_inner`. An assignment works out its place, then its value;
a compound assignment reads the old value after that, so `x += f()` sees what `f` left in `x`, as Rust does for
numbers. These are not ordered yet, and rest on a checker rule against a call that could write beside a read:

| Where | The rule it rests on | Why it matters |
|---|---|---|
| An index against its base, `a()[b()]` | `safety_check_open_cstr_beside`, `narrow_guard_unsequenced`, "read twice by its bounds check" | Those rules have leaked before |
| The members of an initializer, `{ a(), b() }` | The same | The same |
| A `for` step's assignment, which has no block to take a temporary | The same | The same |
| An operand with a temporary in the way: `S{...}&`, `cstr_of`'s copy, a range, a field of a call, a cast or a ternary | The same | A nested `({ })` would end it while a value still points at it. Only an array field needs this, and a scalar field is held back with it, so `add((c ? a : b).n, bump())` is left to C |
| An assignment to a packed place, or an array copy into one | The same | A pointer to the field would lose its packed access |
| `printfmt` with an enum or a print of the program's | No rule: it is written out piece by piece | `printfmt("{} {}", e, noisy())` prints `e` before `noisy` runs, and the single `printf` form runs every argument first |

## The checker and the emitter disagree, and C refuses

These pass `rin check` and are then refused by the C compiler. They cannot fault, because nothing is built, but the
checker has signed off on something the emitter cannot write:

- A plain proc named like a generated instance, such as `hashmap_i32_i32_find` or `swap_S`: a redefinition.
- `printfmt` of a generic's `T` when there is no print for it, and `printfmt` of an integer or a C string in a program
  that does not import `std/print.rin`: an undeclared `print_*`.
- A file using `fprintfmt` with no `std/file.rin`: an undeclared `rin_file_printf`.
- `case N` beside a local `N`.
- A struct named `key` as a generic's argument: `key key` in C.
- `reflect<>&`, `reflect_field<>&`, `reflect_variant<>&` and `FILE<>&`: an undeclared table.
- A proc returning a proc pointer, and an array of them written without an alias: emitted as `i32 * pick(...)` and
  `i32 * fns[2]`.
- An alias of an enum, whose typedef comes before the enum.
- `sizeof(q[0])` on a slice.
- An enum named `rin`, whose table takes the name of rin's own.
- A field, a struct or an alias named like one of C's own: `__LINE__`, `__auto_type`, `__typeof__`, `__int128`.
- A proc returning a pointer to an array, `*[3]i32`.
- A proc pointer alias with an array parameter, whose typedef has a pointer to the wrong thing.
- `_ = { 1, 2 };`
- A generic proc and a specialization for the same type, `f: proc<T>` and `f: proc<u32>`, both called: the generic's
  body is checked for that type too, and the C defines `f_u32` twice, which the C compiler refuses. Nothing runs.
- A local named like an object-like macro of a system header, which is then a syntax error in C. Most are listed and keep
  the suffix; the headers cannot be read, so the list cannot be complete. Nothing is hidden: C refuses the declaration.
- A float cast to `long`.

## A lost write, not a memory error

`vec<i32>at(v, 0)* = grow(v);` writes through the place worked out first, into the old storage, so the write is lost.
The old storage stays valid, so it is no memory error. The same statement through a local is refused as stale; written
inline it is not. The same goes for a stale pointer passed beside a call that grows.

## Not attacked yet

- Anything since round 35, which attacked plain local names. Its fixes have been through the suites, and not a round.
- Threads: the claim is single-threaded.
- `gcc` on the engine and on Windows. In the VM, gcc 16 builds and runs the generated programs, the clean-by-design
  attacks and the accept programs at -O0 with sanitizers and at -O2, with no reports, and prints what clang prints on all
  300 generated programs. One control is not caught under gcc: a read after a mark rewound by hand in `unsafe`
  (`poison/003`), which is a weaker check and not a hole. Clang on Windows, where arguments used to run right to left, is
  only covered by the engine and the course.
- Real play of the engine under the sanitizers. Its own code has run under ASan and UBSan with no reports: every level
  from 0 to 50 briefly, one level for about 14,000 ticks, and the scripted playtest, which goes through the upgrade menu
  and from level 0 to level 1. That is one level change, not a run through the game, and the input was random or scripted.
  The editors were not run, and the vendor libraries were not instrumented.

## Outside the claim

- A macro in a program's own `cinclude` that names a variable, `#define LOG(x) (counter += (x))`, reaches a local
  `counter` where the suffix has come off. That is what such a macro means in C.

- A program's `unsafe { }`, `[external]` and `cinclude`, and the C it links. The engine's Win32 and D3D11 boundary is
  in this group, and is checked only by reading it.
- Running a compiler over source you do not trust. rin is not a sandbox, and the build runs C compilers on what it
  writes.
- Leaks, running out of memory and stack overflow, which stop a program or waste memory without corrupting it.
