# Open questions

> 2026-10-24. All closed. Kept for the record, with where each was decided.
> The tie-breaker for anything new is spec principle 7: simple to write,
> simple to assemble.

## Closed

**A. Frames, homing, `naked`.** No frames of any kind. Spec R1, R2.

**B. `= {}` on locals.** There are no locals. Spec R6.

**D. Call-argument hazards.** No typed calls; the user macro moves
arguments, the clobber is visible in its body. Spec R4, §7.

**E. Unwind directives.** Not generated. Spec R8.

**F. Interface call sugar.** Not generated, and the `interface` kind is
gone: `*Vtbl` structs. Spec R5, §2.4.

**H. Implementation language.** An asm mode of the rin compiler, x64 code
in its own file. design.md §2.

**J. Binding format.** `.rin` files shared with rin, complete layouts,
`rinbind` to emit them, `*Vtbl` structs included.

**K. Checks across `#` passthrough.** Moot: there is no passthrough. Spec R12.

**L. Macro power for the calling convention.** Overloading by parameter
count: `invoke` and `vcall` are single names, no control flow. Spec §3.3, R10.

**P. Compile-time constants.** Object-like `#define`; `const` has storage.
Enum members rejected as ugly; rin untouched. Spec §3.5, R11.

**Output target.** Code through clang's assembler with `.cv_loc`, data
through rin's C backend. ml64 was the plan for part of the day and was
dropped (spec R7).

**Debugging route.** Solved and verified by the author in Visual Studio
2026-10-24: stepping on `.rasm` lines, proc names, typed globals with field
names. debugging.md, debug-proto/.

**Reserved names.** Moot: the `.s` quotes every symbol (`[rip + "offset"]`),
the C file uses rin's C names. (Under ml64 this was a real problem.)

**Custom mnemonics.** None. Spec R9.

**M. Naming stack slots.** Raw offsets with comments; no new syntax. The
frame struct (`[rsp].FRAME.f`, already legal) is liked and parked, the first
to come back. `at` slots parked. decisions.rasm §1.

**N. Check-only passes.** None in the first version: the assembler does not
analyse code. Argument-order clobber, register audit (`proc[saves(...)]`)
and stack-alignment hint parked. decisions.rasm §2.

**O. Header constants.** A generated `win32_consts.rasm` of evaluated
`#define`s; the C data file guards each with `#ifndef`. The per-value
`_Static_assert` parked. decisions.rasm §3.

**R. Float folding.** Whatever C allows in a static initialiser; rasm passes
it to C. decisions.rasm §4.

**C, G.** Settled by default: plain `again:` labels; operand size from
the type.

**I. Pointer spelling.** `*T` is the only pointer in `.rasm` files (C's
`T*`); `^T` and `[]T` from imported `.rin` files are read as layouts. The
author's point: one-element pointers and slices mean nothing to an
assembler that never indexes through a type. A deliberate divergence from
rin, whose pointer kinds stay as they are (the `c-pointer-spelling.md`
rewrite was dropped). Spec §2.

**Line syntax.** `;` comments, newline-terminated statements, in `.rasm`
files; `.rin` imports keep rin's. Sharing `;`/`//` with rin for one LSP was
considered and judged not worth it: the shared part is declaration files,
which keep rin's syntax anyway.

## Open

Nothing open as of 2026-10-24. New questions go here; options the author
liked but parked are in [decisions.rasm](decisions.rasm).
