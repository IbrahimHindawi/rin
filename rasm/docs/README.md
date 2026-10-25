# rasm docs

rasm is a proposed assembler: rin's declaration syntax and types, x64
instructions written as they are, the code assembled by clang with a
`.cv_loc` per statement and the data compiled as C through rin's backend, so
Visual Studio steps through the `.rasm` exactly as through ml64 source
(verified by the author, [debug-proto](debug-proto)). It is for
playing with D3D11 graphics and **raw SIMD assembly** to master
data-oriented design and ECS. **The assembler generates nothing**: frames,
shadow space, saved registers and argument moves are the programmer's, or
the programmer's macros', as in vulkasm and masm64-directx11.

**Implemented 2026-10-25** as `rin asm`, in [`src/asm.c`](../../src/asm.c), with
tests in [`tests/run_rasm.py`](../../tests/run_rasm.py). The four steps of the first plan work:
`examples/0.proto` steps in a debugger at `.rasm` lines with typed watches;
macros, `import`, pooled constants and address constants work; the
generated Win32/D3D11 binding passes every layout assert against the SDK;
and `examples/2.1.hello_triangle` draws LearnOpenGL's hello triangle and
debugs at its `.rasm` lines. All 38 LearnOpenGL lessons are in
[rasm-dx11](https://github.com/IbrahimHindawi/rasm-dx11).
How to build each example: [../examples/README.md](../examples/README.md).

The design was settled in discussion on 2026-10-24. The decisions are at the top of
[spec.md](spec.md); ideas considered and rejected are at its end, so that
the next agent does not build them. rin keeps its C backend; there is no
rin-to-rasm backend.

## Read in this order

0. [decisions.rasm](decisions.rasm): the last four questions as code,
   decided by the simplicity rule, with the options the author liked parked
   there, written out, for later.
1. [roadmap.md](roadmap.md): what rasm is for, the decisions about SIMD,
   memory and layout, and the lessons in order (hello triangle,
   transformations, particles in two layouts, run-time ISA dispatch, maybe
   an archetype ECS).
2. [spec.md](spec.md): the language. Decisions first; then types, data,
   procs, macros (overloaded by parameter count), `#define` constants,
   operands, calls, the output (`.s` + C data file); **Considered and
   rejected** last.
3. [design.md](design.md): an asm mode of the rin compiler, what it reuses
   (the front end) and what it adds (a layout pass checked by C asserts, a
   constant folder, its own C writer for the data), the checker, the `.s` emitter with its
   CodeView records, macros, sharing with rin, the first milestone.
4. [context.md](context.md): the sibling repositories, the traps and bugs
   they hit, the Win64 ABI, ml64 facts.
5. [open-questions.md](open-questions.md): everything that was open and
   how it closed. Nothing is open now.
6. [debugging.md](debugging.md): how the `.rasm` debugs exactly like ml64
   source in Visual Studio, verified with [debug-proto](debug-proto); the
   routes that were analysed on the way.

## The examples

| file | what it shows |
|---|---|
| `examples/2.1.hello_triangle/hello_triangle.rasm` | the port of masm64-dx11's 2.1: state in `.data`, designated initialisers, manual frames, `invoke`/`vcall`, a tail `jmp` into `DefWindowProcA`, an argument-order hazard resolved by hand |
| `examples/5.1.transformations/transformations.rasm` | per-frame matrices, a float argument by position (`xmm1`), `andps` with a typed mask |
| `examples/includes/calls.rasm` | the Win64 convention as 21 plain macros |
| `examples/includes/math.rasm` | SSE: `v3load`/`v3store`, `m4_mul` unrolled by macro, `m4_rotate` with no locals, `sinf` using the shadow space; `// INTERESTING:` comments mark what the style teaches |

None of them assembles yet: they import a complete binding (`d3d11.rin`) and
helpers (`print`, `filesystem`, `learn/window`, ...) that do not exist.

## Related repositories

| path | what | relation |
|---|---|---|
| `C:\devel\rin` | the rin compiler, transpiles to C | rasm will be an asm mode of it, reusing its front end; shares `.rin` declaration files |
| `C:\devel\masm64-dx11` | LearnOpenGL's lessons in ml64 on D3D11 | the main reference; the corpus a rasm port translates |
| `C:\devel\masm64-directx11` | CMalcheski's DirectX-in-assembly tutorial | the other reference: arguments loaded by hand, `WinCall` handling only the stack |
| `C:\devel\rin-dx11` | the same lessons in rin | the rin twin; its `win32.rin` is a partial binding |
| `C:\devel\vulkasm` | a Vulkan triangle in ml64 | the manual style's first proof; its bug list |
| `C:\devel\rin-windows-x64` | packaged `rin.exe`, `rinbind.exe`, `std\` | what `RIN_HOME` points at |
| github.com/ibrahimhindawi/kayan | the author's type-erased ECS in C | what the ECS lessons compare against |

Conventions the author holds across these, so an agent does not rediscover
them: C behaviour is the default; no platform knowledge in the rin compiler,
and rin is not changed for rasm's sake; the standard library is arena-only,
no malloc; the game project comes first and rin and rasm are time-boxed; a
casual idea gets a discussion before a plan; and for rasm, nothing the
assembler emits may be an instruction the source did not state, and there
are no custom mnemonics.
