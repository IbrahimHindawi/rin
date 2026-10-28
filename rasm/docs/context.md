# Context

What an agent needs to know before touching rasm, gathered from the sibling
repositories on 2026-10-24. The decisions themselves are at the top of
spec.md; where this file and spec.md disagree, spec.md wins. Paths are absolute so they can be opened directly.

## 1. The sibling repositories

### rin (`C:\devel\rin`)

A systems language that transpiles to C. One 19k-line `src/main.c` holds lexer,
parser, checker, monomorphiser and emitter; `src/rinbind.c` is a libclang
binder that turns C headers into rin declarations; `src/std/` is 29 `.rin`
files copied beside the executable. `python bunyan.py build debug` builds and
packages into `C:\devel\rin-windows-x64`. `python tests/run_tests.py` is the
suite and passed in full on 2026-10-24.

Syntax rin and rasm share:

    name: type = value;                   every declaration
    x: i32 = {};  y: [64]u8 = ?;          zeroed; deliberately not initialised
    vec3: struct = { x: f32; y: f32; z: f32; }
    Color: enum = { Red, Green, Blue }
    f: proc(a: i32, b: *vec3) -> u64 = { ... }
    f: proc[external](...) -> i32 = {}    declared, defined elsewhere
    T: struct[external] = { ... }         a type the C header already defines
    x&                                    postfix address-of
    p*                                    the value at p (p[0] also allowed on *T)
    ^T                                    raw pointer, C's T*
    []T                                   slice (pointer + length)
    [N]T                                  array
    // and /* */                          comments
    # ...                                 C preprocessor passthrough, verbatim (rasm reads object-like #define)
    done: label = { ... }                 labels take a block

Pointer spelling: [`docs/pointers-and-slices.md`](../../docs/pointers-and-slices.md) (implemented
2026-10-21) has `*T` as one element, `^T` as the raw pointer and `[]T` as a
slice. That stays: the proposal in `c-pointer-spelling.md` to swap to `*T`
raw and `&T` one element was dropped by the author on 2026-10-24. rasm uses
`*T` alone for any pointer (spec §2), a deliberate divergence; imported
`.rin` files keep rin's spellings.

rasm will be an asm mode of this compiler (design.md §2). Reusable pieces in
`src/main.c`, by line as of commit f704f43:

| what | where |
|---|---|
| `TokenKind` | 385 |
| `parse_type` | 2339 |
| `parse_decl_attributes` (the `[external, WINCALL]` slot) | 3996 |
| `parse_program` | 4485 |
| layout `static_assert` emission (rin computes no layouts itself; C does) | `emit_struct_layout_check`, about 16425 |

Design notes worth reading in rin's [`docs/`](../../docs): `rin-soul.md` (what makes
C C; what rin keeps), `whats-missing.md` (decided against: methods, `for in`,
multiple returns, `result<T>`), `build-command.md` (`rin build` reads a
`build.rin` of known globals; `build_c_sources` exists and a
`build_asm_sources` would be the natural extension), `attributes.md`.

### rin-dx11 (`C:\devel\rin-dx11`)

LearnOpenGL rewritten for Win32 and D3D11 in rin. 38 self-contained lessons,
16.8k lines of rin in `src/`, 1.6k in `includes/`. `includes/win32.rin` is a
**hand-written** partial binding: 219 `external` declarations, only the fields
the lessons touch, layout trusted to the C header. Built with `rin build`
(cmake, Ninja, clang-cl). Shaders are shared unchanged with masm64-dx11, all
72 files byte-identical.

### masm64-dx11 (`C:\devel\masm64-dx11`)

The same 38 lessons in ml64 with the MASM64 SDK macros. 8.0k lines in `src/`,
4.7k hand-written in `includes/`, plus `includes/d3d11.inc`, 6.8k lines
**generated** from the Windows SDK by `tools/gen_d3d11.py` (libclang): every
struct at clang's offsets with explicit padding, every enum and integer
`#define`, vtable method offsets as `IFoo_Method equ N`, GUIDs. Members whose
names MASM reserves get an underscore (`_Width`, `_Flags`). Built by
`scripts\build.bat`: one `ml64` and one `link` per lesson, unity build,
`/subsystem:console`, `/entry:main`. `--shot out.png` renders headless for
checking. This is the main reference: the lessons a rasm port translates and the
debugging experience rasm must match.

Its `docs/0_masm64.md` is the trap list. After the author's decision that
the assembler generates nothing, the traps split into those rasm removes
(they are MASM surface) and those that stay the programmer's (they are the
convention, and the convention is manual):

| trap | rasm |
|---|---|
| `invoke` loads arguments in order, so `invoke f, rdx, rcx` reads a clobbered `rcx` | stays: the user's call macro has the same property; it is visible in its body |
| 5th+ argument travels through `rax`; a later `rax` argument reads the wrong value | stays, same reason |
| float arguments: `movf xmm3, 1.0` before the call, a placeholder in the list | stays manual: `movss xmm1, ...` then `call`, written out (5.1's `m4_rotate`); the literal is pooled |
| `[rbx].model.vertices` through a register needs `qword ptr` | removed: field type gives the size |
| float literals only exist as data: `movf`, `storef`, `floats(...)` macros | removed: a float literal is an operand, pooled in `.rdata` |
| reserved member names (`size`, `type`, `length`, `offset`, `width`, `mask`, `name`, ...) | removed: fields become numbers in the code and keep their header names in the C data file; every symbol in the `.s` is quoted (`[rip + "offset"]`), so no global name is reserved |
| labels in a proc share a namespace with globals | removed: labels are proc-scoped |
| `table[rbx * 4]` on a global is a 32-bit absolute address, refused by the linker | made an error: `lea rsi, table`, then `[rsi + rbx*4]`. `table[i]` with a constant counts elements |
| `push` inside the frame lands where the next call's stack arguments go | stays: the frame is the programmer's |
| `vcall` keeps object and vtable in `r10`/`r11` because `invoke` only touches `rax` and the argument registers | stays, as the user's `vcall` macro (object in `rcx`, vtable in `r10`); the method offset is a field of a `*Vtbl` struct, `ID3D11DeviceVtbl.CreateBuffer` |
| MASM struct padding differs from C's; `gen_d3d11.py` places padding by hand | removed: C layout is rasm's |
| `proc`/`endp`, `option casemap`, `OFFSET`/`ADDR`, `equ` vs `=` vs `TEXTEQU`, `%` text macros, `proc`'s hidden `uses`/`local` behaviour | removed: none of it exists in rasm |

Its `includes/macros.asm` is the complete macro vocabulary the lessons needed:
`AssertEq`, `AssertNotEq`, `AssertHr`, `vcall`, `release`, `movf`, `storef`,
`floats`, `print`. Nine macros for 38 programs. rasm should need fewer:
`movf`, `storef` and `floats` disappear into pooled literals and `const`
data (`movf` loads a pooled float into an xmm register; `storef` copies one
into memory through `eax`, because x86 has no float immediates and MASM only
converts float literals in data definitions).

### masm64-directx11 (`C:\devel\masm64-directx11`)

A copy of CMalcheski's CodeProject series "DirectX and Pure Assembly
Language: Doing What Can't be Done": a spinning D3D11 triangle in ml64,
4.4k lines in `source/`, built by `go.bat` (rc, ml64, link with
`/largeaddressaware:no`). The style is the most manual of the references:

- arguments are loaded into `rcx`, `rdx`, `r8`, `r9` **by hand** before each
  call; the `WinCall` macro (`source/macros.asm`) only reserves the stack,
  aligns `rsp` dynamically through a `holder` local, copies the 5th and
  later arguments through `rax`, and restores `rsp`;
- COM method offsets are `textequ`s over `rbx`
  (`ID3D11Device_CreateBuffer textequ <qword ptr [rbx+24]>`), with
  `mov rbx, [rcx]` written before each call;
- data in `.data` across many include files (`structures.asm`,
  `variables.asm`, `vTables.asm`, ...); `Save_Registers` /
  `Restore_Registers` macros push every general register.

Its `/largeaddressaware:no` is what lets it use 32-bit absolute addresses of
globals, the trap rasm turns into an error.

### kayan (github.com/ibrahimhindawi/kayan)

The author's type-erased ECS in C: archetypes as structs of a header
(`signature` bitmask, `entity_count`, a string-keyed `Map_u64` of column
offsets) followed by `Array_vec4s` / `Array_f32` columns from an arena;
queries match `(signature & wanted) == wanted`, then look columns up by name.
Components are `vec4s` per entity. roadmap.md compares this layout with true
structure-of-arrays and sketches the assembly-shaped archetype header.

### vulkasm (`C:\devel\vulkasm`)

A Vulkan triangle in ml64, 2.6k lines, the first proof that the minimal set
works. Primitives: the SDK's `invoke`; `AssertEq`/`AssertNotEq`;
`SaveRegisters`/`RestoreRegisters`/`procPrologue`/`procEpilogue`; the `saha`
arena (`arenaGetPos`, `arenaPushArray`, `arenaPushArrayZero`); `DebugPrint`;
header guards; one module per Vulkan step with `Module_var` and
`Module_Execute` naming; every variable in `.data`, no stack locals. The
Vulkan headers were translated with libclang (`tools/vulkan_gen.bat`).

`docs/bugz.md` lists four defects found by static review. Three are frame or
convention bugs, and they are the strongest argument for rasm:

| bug | cause | rasm, after the "everything manual" decision |
|---|---|---|
| saved registers overwritten by `invoke` stack args and callee shadow space | `procPrologue` pushed `rbp, rbx, rsi, rdi, r12-r15` *below* the frame the SDK already reserved, so `[rsp+0..38h]` held saved registers exactly where arguments go | stays the programmer's. The frame is visible in the source, one instruction per line, with no SDK prologue hiding under it, which is what made this one hard to see |
| `rsi`/`rdi` clobbered in `saha.asm` without saving | nothing checked | stays the programmer's; a check-only register audit is parked (decisions.rasm §2) |
| shader buffer allocated as 1 byte | `arenaPushArrayZero` assumed its count was already in `rax`; `rax` held `GetFileSizeEx`'s `TRUE` | a rasm macro takes its count as an argument; nothing is assumed to be in a register |
| arena `used` runs away on growth | arithmetic bug | not rasm's business; fixed in masm64-dx11's `saha.asm` |

These were, in the first draft of this design, the argument for generated
frames and a register audit. The author weighed that and chose the manual
style anyway: the point of rasm is to write the convention by hand and see
it, with the MASM surface out of the way.

## 2. The Win64 ABI, as rasm must implement it

- Integer and pointer arguments 1 to 4 in `rcx`, `rdx`, `r8`, `r9`; 5th onward
  at `[rsp+32]`, `[rsp+40]`, ... at the call instruction.
- A floating-point argument goes in `xmm0` to `xmm3` **by position**: the third
  argument is `xmm2` whatever the first two were. Its integer slot is left
  unused. From the 5th on it goes on the stack like anything else.
- The caller reserves 32 bytes of shadow space below the stack arguments,
  always, even for a call with no arguments. The callee may spill its register
  arguments there (`[rsp+8]..[rsp+32]` relative to the return address).
- `rsp` is 16-byte aligned at the `call` instruction, so inside the callee,
  at entry, `rsp mod 16 == 8`.
- Return in `rax`, or `xmm0` for a float or double. A struct of 1, 2, 4 or 8
  bytes returns in `rax`; a larger struct is returned through a hidden first
  argument pointer the caller supplies.
- A struct argument of 1, 2, 4 or 8 bytes passes by value in the register; any
  other size passes as a pointer to a caller-made copy. rasm should reject
  by-value structs in calls other than those four sizes, or reject them all.
- Varargs: a float vararg must also be copied into the corresponding integer
  register. Only relevant if rasm ever calls `printf`; `wsprintfA` has no `%f`.
- Volatile (caller-saved): `rax`, `rcx`, `rdx`, `r8` to `r11`, `xmm0` to
  `xmm5`. Non-volatile (callee-saved): `rbx`, `rbp`, `rsi`, `rdi`, `r12` to
  `r15`, `xmm6` to `xmm15`. The upper halves of `ymm` are volatile.
- Non-leaf procs need unwind information (`.pdata`/`.xdata`) for exceptions
  and for the debugger's stack walk. ml64's `proc frame` with `.pushreg`,
  `.setframe`, `.allocstack`, `.endprolog` emits it. masm64-dx11 does not use
  `frame` and the debugger still steps fine; stack walks through such frames
  rely on `rbp` chains. rasm emits no `frame` directives (spec R8).

## 3. ml64 facts

rasm does not emit ml64 (spec R7). These facts matter because every
reference lesson is ml64, and they explain the traps above.

- `name proc uses rbx rsi, a:qword, b:qword` with `local x:dword` is native
  MASM and generates prologue, epilogue, register saves and `rbp`-relative
  names. The SDK's `STACKFRAME` (run by `masm64rt.inc`, so vulkasm has it
  too) installs `OPTION PROLOGUE` macros that generate a frame for any proc
  with parameters or locals and reserve outgoing argument space, which is why
  the SDK code never adjusts `rsp` around calls. rasm emits bare `name proc`
  with no parameters, locals or `uses`, so ml64 generates nothing.
- A typed memory operand (`desc.ByteWidth` where `desc` is a `local` or a
  `.data` symbol with a struct type) carries its size; `[rbx].T.field` does
  not and needs `qword ptr`.
- Float literals are only accepted as data initialisers (`real4 1.0`), never
  as instruction operands.
- `.const`, `.data` and `.code` can be switched any number of times; a macro
  that emits a `.const` item and switches back to `.code` is how `movf` works.
- `LOCAL` inside a macro makes a unique label. Code labels inside
  `proc`/`endp` are local to the proc, but they still collide with global
  *data* symbol names, which is the trap recorded above.
- `option casemap:none` makes names case-sensitive, which the Windows imports
  need.
- `externdef sym:proc` or `sym proto` declares an external function;
  `externdef __imp_Foo:PPROC` plus `Foo equ <__imp_Foo>` is how a DLL import
  is reached without an SDK `.inc`, as masm64-dx11's `window.asm` does for
  `SetProcessDPIAware`.
- `/Zi` gives CodeView line numbers per source line of the file ml64 read. A
  macro expansion gets the line of its invocation. There is no `#line`
  equivalent. See debugging.md.
- MASM reserves a long list of ordinary words as member or symbol names. The
  authoritative list used by the generator is `RESERVED` in
  `C:\devel\masm64-dx11\tools\gen_d3d11.py`. `option nokeyword:<scale width
  length>` un-reserves words (verified with ml64 14.51); words the emitter
  itself uses cannot be.
- `mov rdx, offset x` is a 10-byte `mov` with an `IMAGE_REL_AMD64_ADDR64`
  fixup; `mov eax, dword ptr x+8` on a global is RIP-relative (both verified).
- MASM's `x[8]` is `x + 8` in bytes, with no scaling; `[rsi].T.f` and
  `[rsi.T.f]` are both field access through a register.

## 4. How the lessons are actually written

Representative idioms, from `C:\devel\masm64-dx11`, that the sample and the
spec must be able to express without becoming higher level than this:

    ; a leaf proc with register arguments and no frame
    ; rcx: key -> rax: 1 if it was pressed since the last call
    key_pressed proc
        and ecx, 0FFh
        lea rdx, keys_pressed
        movzx eax, byte ptr [rdx + rcx]
        mov byte ptr [rdx + rcx], 0
        ret
    key_pressed endp

    ; a proc with named arguments, locals, saved registers, COM calls
    create_buffer proc bind:qword, data:qword, data_size:qword
        local desc:D3D11_BUFFER_DESC, init:D3D11_SUBRESOURCE_DATA, buffer:qword
        invoke mem_zero, ADDR desc, sizeof D3D11_BUFFER_DESC
        mov eax, dword ptr data_size
        mov desc.ByteWidth, eax
        ...
        vcall device, ID3D11Device_CreateBuffer, ADDR desc, ADDR init, ADDR buffer
        mov rax, buffer
        ret
    create_buffer endp

    ; a float argument, by position
    movf xmm3, 1.0
    vcall context, ID3D11DeviceContext_ClearDepthStencilView, depth_dsv, D3D11_CLEAR_DEPTH, 0, 0

    ; a constant array's address as an argument
    vcall context, ID3D11DeviceContext_ClearRenderTargetView, backbuffer_rtv, floats(0.2, 0.3, 0.3, 1.0)

    ; a struct mirroring a cbuffer, padded by hand
    transforms struct
        model mat4 <>
        normal_matrix mat4 <>
        view mat4 <>
        projection mat4 <>
        view_position vec3 <>
        pad real4 ?
    transforms ends

    ; SSE maths, four floats to a register, caller-owned xmm6-15 untouched
    ; see includes/math.asm

The whole of `includes/` in masm64-dx11 (window, shader, gpu, cube, camera,
transforms, model, image, srgb, shadow, wall, pbr, math, cstr, print, saha) is
the corpus a rasm port would translate, and the corpus to test the instruction
grammar against: if any line in it cannot be written in rasm without raising
the level, the grammar is wrong, not the line.
