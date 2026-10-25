# rasm design

> **Status.** Implemented 2026-10-25 in [`src/asm.c`](../../src/asm.c) (about
> 3,000 lines, included by `main.c`, reached only through `rin asm`), tests in
> [`tests/run_rasm.py`](../../tests/run_rasm.py). Follows the decisions at the top of
> [spec.md](spec.md): everything manual, code through clang's assembler with
> `.cv_loc`, data as C, an asm mode of the rin compiler, educational scope.
> Where the implementation differs from the first draft of this document, the
> text below says so.

## 1. Pipeline

    .rasm / .rin  --lex-->  tokens  --parse-->  declarations + proc bodies
        --check-->  layouts, constants, operand sizes, names, labels, macro expansion
        --emit-->   foo.s       code, .cv_loc per statement, S_GPROC32 per proc
                    foo_data.c  types and data, written from rin's parse, plus layout asserts
        --clang -c foo.s, clang -c -g -gcodeview foo_data.c-->  two .obj
        --clang -fuse-ld=lld -g-->  .exe + .pdb

Four passes, no IR, no code generation. A proc body is a list of statements
that are already instructions; checking resolves names to offsets and sizes;
emission prints each statement once, preceded by its `.cv_loc`. Declarations
take the path rin already has for them.

## 2. Where it lives: an asm mode of the rin compiler

The author's direction: reuse the rin compiler. The constraint that comes
with it is rin's own rule, no platform knowledge in the compiler. So:

- the x64 code (instruction statements, registers, operand rewriting, the
  `.s` emitter, the CodeView records) lives in its own source file,
  `src/asm.c` or similar, reached through its own entry (`rin asm`,
  provisional), and the C backend never calls into it;
- it reuses, unchanged: the lexer, `parse_type`, `parse_decl_attributes`,
  struct/enum/alias/proc declaration parsing, the designated-initialiser
  parser, the expression parser, and `import` with its path resolution.
  **Not** rin's C backend, as first planned: rin's checker, which the
  backend relies on, rejects rasm's data rules (one pointer type, `#define`
  names in global initialisers, `window_proc&` for a function's address).
  rasm writes the data's C itself from rin's syntax tree, about 400 lines;
- rin the language does not change for rasm. When rin changes, rasm's
  parser follows for free. One deliberate divergence: rasm's own files use
  `*T` for every pointer, where rin has `*T`, `[*]T` and `[]T` (spec §2).
  All of them lower to C's `T*` in the data file, so the C is identical.

Reusable pieces in `src/main.c` (19,164 lines), by line as of
commit f704f43: `TokenKind` 385, `parse_type` 2339, `parse_decl_attributes`
3996, `parse_program` 4485, `emit_struct_layout_check` about 16425.

**New code the asm mode needs:**

1. **A layout pass.** rin never computes a struct layout; it emits
   `static_assert`s and lets the C compiler decide. The code side needs
   size, alignment and offset for every struct operand, by C's rules. It is
   checked, not trusted: every offset it hands the code is asserted in the
   data file (§4.2).
2. **A constant folder** for integer expressions, `sizeof`, `offsetof`,
   enum members and `#define` constants in instructions. (In data
   initialisers C folds them.)
3. **Object-like `#define` evaluation**: rin passes `#` lines through
   verbatim; the code side must read `#define NAME expr`.
4. **The assembler line syntax** for `.rasm` files: `;` comments, newlines
   ending statements (not inside `()`, `[]` or initialiser braces), and the
   instruction statement parser for proc bodies, registers as keywords,
   labels as `name:`. `.rin` imports keep rin's lexing.
5. **Macros**: definitions, overloading by parameter count, substitution,
   hygienic labels, invocation line kept for every expanded instruction.
6. **The `.s` emitter**, with the CodeView proc records.
7. **The split**: each `.rasm` file becomes, line for line, rin text of its
   declarations (proc bodies blanked, a proc's signature kept as a
   `proc[external]` declaration, `;` added where a statement ends), which
   rin's parser reads with the `.rasm` as its source path, so its errors
   name `.rasm` lines. Procs and macros go to the assembler.
8. **The C writer** for the data file (§4.2).

A parse-divergence test keeps the modes honest: every `.rin` file the
lessons import parses identically in both.

## 3. Checker

- **Layout** of every struct (§2, item 1).
- **Constants**: fold every constant expression in code; reject a `const`
  variable used where C needs a constant expression.
- **Operand resolution**: each name to `[rip + name + off]` or
  `[reg + off]` with a size; each enum member, `#define`, `sizeof`,
  `offsetof` to a number; each float or string literal to a pooled label.
- **Address constants**: `x&` only as the source of `mov r64, ...` or in an
  initialiser.
- **Sizes**: both operands agree, or one is explicit. Never meaning.
- **Labels**: defined once per proc; every jump target defined.
- **Names**: no symbol may be a register; a macro name may not be a known
  mnemonic.
- **Macro expansion** before resolution, so expanded statements are checked
  like written ones.

Not computed: values at run time, flow, liveness, registers, frames, calls.
No code analysis in the first version (spec principle 7); the parked checks
are in decisions.rasm §2.

## 4. Emitters

### 4.1 The code: `.s` for clang

Verified 2026-10-24 with clang 22 ([debug-proto/code.s](debug-proto/code.s)
is the template; the author stepped through its `.rasm` in Visual Studio):

- `.intel_syntax noprefix` first; `.cv_file N "path"` for every source
  file, forward slashes (a backslash is an escape).
- `.section .rdata,"dr"`: the constant pool, `.Lk_f32_1: .long 0x3f800000`
  or `.float 1.0`, `.Lk_str_3: .asciz "..."`, each `.p2align`ed.
- `.text`; per proc: `.globl name` (unless `static`), `.p2align 4`,
  `name:`, `.cv_func_id K`, then per statement `.cv_loc K F L 0` before its
  first instruction (one per macro expansion: the invocation's line), and
  `.Lname_end:` after the body.
- One instruction per statement, in source order, mnemonic and registers
  preserved; names rewritten to `[rip + "sym" + off]`, sizes added, a
  `# path` comment on every rewritten field; comments carried over as `#`.
- Every symbol is emitted **quoted**: `[rip + "offset"]`. A bare `offset`
  is a parse error in clang's Intel syntax (it is the operator); quoted, it
  assembles and keeps its real name (verified). Quoting always means no
  reserved-word list and no renaming.
- Address constants as `movabs r64, offset sym + off` (an ADDR64 fixup; a
  plain `mov r64, offset` would be 32-bit).
- Labels as `.Lproc_label`; macro-local labels as `.Lproc_label_N`.
- `.section .debug$S,"dr"`, `.p2align 2`, `.long 4`; a symbol subsection
  (`.long 241`, its length) holding per proc an `S_GPROC32` (kind `0x1110`:
  parent, end, next = 0; code length; debug start 0, debug end = length;
  type 0; `.secrel32 name`; `.secidx name`; flags 0; `.asciz "name"`;
  aligned to 4) and an `S_END` (`.short 2`, `.short 6`); then per proc
  `.cv_linetable K, name, .Lname_end`; then `.cv_filechecksums` and
  `.cv_stringtable`.

Visual Studio did not need `S_OBJNAME` or `S_COMPILE3` records in the test.
If a later VS or tool insists, clang's own templates for them are in
debugging.md.

What the emitter never does: add an instruction, reorder, choose a
register, touch `rsp`.

### 4.2 The data: C written by rasm

From rin's syntax tree of the program's declarations, rasm writes: the
`cinclude`d headers; every `#define` inside an `#ifndef` guard; structs,
enums and aliases that are not `[external]`; every global with its
initialiser, `const` data as
C `const`, the imported files' C lines (`cinclude`, `#define`), and a
prototype for each rasm proc whose address appears in an initialiser. Then
it appends, for every distinct offset and size the code side used:

    _Static_assert(offsetof(D3D11_BUFFER_DESC, BindFlags) == 8, "rasm layout: D3D11_BUFFER_DESC.BindFlags");
    _Static_assert(sizeof(mat4) == 64, "rasm layout: sizeof(mat4)");

so the build fails, naming the field, the moment rasm's layout pass and C
disagree. With a complete binding imported over the real header
(`struct[external]`), the asserts check rasm against the Windows SDK
itself. Compiled with `clang-cl /c /Z7`, the object carries the CodeView
types and typed data symbols that give the Watch window its field names.

Symbol names are the same in both files: the `.s` quotes every symbol and
the C uses the name as written. A declaration from an imported `.rin` file
is C's when it is `[external]`, so a binding marks everything external
(`gen_bindings.py` adds `[external]` to every alias rinbind writes).

## 5. Driver

`rin asm foo.rasm` writes both files and, as `rin build` does for rin
today, runs clang, clang-cl and the linker. Later an `asm` entry kind in
`build.rin`, next to `build_c_sources`. A rasm program can link rin objects
and the reverse with no change to either.

## 6. Macros

Textual, hygienic, no control flow; several definitions may share a name and
the parameter count picks one. The calling convention lives in macros, by
the author's decision, in `examples/includes/calls.rasm`:

    invoke: macro(f, a1, a2, a3, a4, a5) = {
        mov r11, a5
        mov qword ptr [rsp + 32], r11
        mov rcx, a1
        mov rdx, a2
        mov r8, a3
        mov r9, a4
        call f
    }

    vcall: macro(obj, method, a1, a2) = {
        mov rcx, obj
        mov rdx, a1
        mov r8, a2
        mov r10, [rcx]
        call [r10].method
    }

21 definitions in all, `invoke` for 1 to 12 arguments and `vcall` for 0 to 8,
about 300 lines, each line one the reader can see. A zero-argument call is
just `call f`.

What the substitution must support, learned from the lessons:

- a parameter used as a field path: `[r10].method` with `method` =
  `ID3D11DeviceVtbl.CreateBuffer`;
- a parameter inside a constant expression: `[rdx].mat4.m[row * 4 + 1]`;
- not: arithmetic on an operand. `v3load` takes a base register because
  `[rdx]` cannot be turned into `[rdx + 8]` by substitution.

## 7. Sharing with rin

Both sides produce COFF objects with the Win64 ABI and C layout; rasm's data
is literally rin-compiled C. The intended use for the ECS work: bookkeeping
in rin or C (kayan), hot SIMD kernels in rasm, linked together.

- **Declarations.** A `.rin` file imported by rasm means the same layout.
  Its procs with bodies become external declarations (spec §3.4), so
  `import "std/memops.rin"` works.
- **Constants.** `#define` is the compile-time constant in both; `const` has
  storage in both. No rin change.
- **The boundary.** A rasm proc called from rin must follow Win64 fully: save
  `rbx`, `rbp`, `rsi`, `rdi`, `r12`-`r15`, `xmm6`-`xmm15` if it uses them.
  The private conventions inside a rasm program (math.rasm's `sinf`
  touching only `xmm0`) do not hold across it. Nothing checks this; it is
  where the parked register audit (decisions.rasm §2) would earn its keep.
- **Names.** No assembler reserved words reach anything: the `.s` quotes
  every symbol (`[rip + "offset"]`, verified), and the data's C file uses
  rin's C names. Every global keeps its own name, exported as written.
- **Bindings: rinbind already does it.** Run directly on the SDK headers
  (verified 2026-10-24 on `d3d11.h`, 4,277 lines), `rinbind` writes every
  struct complete and in order, with clang's offset of each field as a
  comment (`// rinbind: field_offset Usage:32`, in bits: a ready reference
  for testing rasm's layout pass); all the `*Vtbl` structs; enums with
  values; `proc[external]` declarations; and object-like macros as
  `#define NAME value` with casts and `L` suffixes stripped, which is the
  constants file the header-constants decision asked for. The C data file
  guards each `#define` with `#ifndef`. Gap: a macro whose value is a call
  (`IDC_ARROW` is `MAKEINTRESOURCEA(32512)`) comes out as its tokens, not a
  number; a handful, written as the number by hand. The hand-curated
  bindings in rin-dx11 and njinn (`njinn/src/bindings/win32.rin`) are
  partial on purpose, under `no_layout_check`, and are not what rasm
  imports.
- **The arena.** rasm's own arena keeps `memops_arena`'s fields in order
  (`base`, `cursor`, `previous`, `used`, `pagesize`, `npages`, `reserved`),
  so a rin program can hand its arena to a rasm kernel.

## 8. Non-goals

A compiler backend for rin. Any code generation. An optimiser. Portability
beyond Win64. Replacing MASM's macro language. A raw-assembler escape. ml64
output.

## 9. Later

- **Locals in the debugger**: if the parked frame struct (decisions.rasm §1) is
  added, an `S_REGREL32` record per slot would put them in the Locals
  window, the one piece of ml64's debugging rasm does not match yet.
- **A direct backend**: an x64 encoder, COFF writer, CodeView lines and
  types, behind the same command, tested byte for byte against clang's
  objects. Educational; not needed for anything above.

## 10. First milestone: reached 2026-10-25

`examples/2.1.hello_triangle/hello_triangle.rasm` through `rin asm`, clang
and lld, drawing the same picture as masm64-dx11's 2.1 and stepping at
`.rasm` lines (lldb: breakpoint in `create_buffer`, call stack through
`main at hello_triangle.rasm:298`, `buffer_desc` watched with its D3D11
field names and `Usage = D3D11_USAGE_IMMUTABLE`). That exercises data and
designated initialisers through the C backend, a complete binding with
`*Vtbl` structs, procs as labels, pooled strings and floats, address
constants, proc-scoped labels, overloaded macros, `import`, `.cv_loc` and
the layout asserts. Then 5.1 with `math.rasm` (SSE), then the particle lesson
([roadmap.md](roadmap.md)).
