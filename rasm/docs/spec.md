# rasm specification

> **Status.** Designed 2026-10-24; implemented 2026-10-25 as `rin asm`
> (`rin/src/asm.c`), with the examples in [../examples](../examples) building,
> running and debugging. The author's decisions, superseding earlier drafts:
>
> - **Purpose.** Play with D3D11 graphics and raw SIMD assembly to master
>   data-oriented design and ECS. Educational; rin keeps its C backend and
>   there is no rin-to-rasm backend. See [roadmap.md](roadmap.md).
> - **Everything stays manual.** rasm generates no prologue, epilogue,
>   shadow space, register saves, outgoing argument area or argument moves.
>   Calling-convention help is a user macro file, as vulkasm's `invoke` was.
> - **The debugging experience must be exactly ml64's**, or rasm is not
>   worth writing. It is: the author stepped cleanly through a `.rasm` in
>   Visual Studio on 2026-10-24 ([debug-proto](debug-proto),
>   [debugging.md](debugging.md)).
> - **Output: clang's assembler for the code, C for the data.** Code goes to
>   `.intel_syntax noprefix` assembly with a `.cv_loc` per statement, so the
>   line table names the `.rasm`. Data declarations and their types go to a C
>   file, written by rasm from rin's parse of them, so the debugger has every
>   struct type and field name. ml64 output was the plan for part of that day and is dropped:
>   ml64 cannot attribute a line to another file.
> - **References:** masm64-dx11 and masm64-directx11 for the lessons and the
>   style; vulkasm for the manual style's first proof.
> - **Implementation:** an asm mode of the rin compiler, reusing its front
>   end, with the x64 code and the data file's C writer in its own source file. rin the
>   language is not changed for rasm's sake.
> - **No custom mnemonics.** Every instruction is a real x64 instruction.
> - **`#` means what it means in rin:** C's preprocessor. An object-like
>   `#define` is rasm's compile-time constant; there is no raw-assembler
>   escape.
> - **`const` has storage**, in rasm as in rin and C.
> - **Assembler line syntax:** `;` starts a comment and a newline ends a
>   statement, as in every normal assembler. rin's declaration syntax
>   otherwise, so `.rin` files are imported unchanged.
>
> Ideas that were considered and rejected are at the end, so the next agent
> does not build them.

Level tags:

- **asm**: emits exactly the instruction or directive the source states, or
  a data item. No hidden instructions, ever.
- **check**: emits nothing; rejects something the assembler would have
  accepted or something that would break at run time.

## 0. Principles

1. The program is instructions, one per line, emitted one per line, in
   order. rasm never adds an instruction and has no mnemonic of its own.
2. Types and layout are rin's, which are C's. The data is compiled as C, so
   the layout is C's by construction, and rasm's own offsets are checked
   against it at build time (§9).
3. One source file is one program (unity build). `import` brings in another
   file once.
4. Data is data. rasm checks that operand sizes agree, never what the bits
   mean: an `f32` field takes `eax`.
5. A notation is allowed when it is data, or a different spelling of the
   one instruction it emits. Anything else is code generation and is out.
6. What is on the debugger's screen is the `.rasm`. The generated `.s` and
   `.c` are build artifacts.
7. **When in doubt, keep it simple to write and simple to assemble.** The
   author's tie-breaker, 2026-10-24: "keep whatever keeps the assembly
   writing simple without overanalyzing the code or overcomplicating the
   assembler." Between two options, take the one that adds no syntax and no
   analysis of the code. Cooler options are parked in
   [decisions.rasm](decisions.rasm), not lost.

## 1. Lexical

`asm`. rin's lexer, in an assembler mode for `.rasm` files:

- **`;` starts a comment** that runs to the end of the line, as in MASM.
  `//` and `/* */` are not comments in a `.rasm` file.
- **A newline ends a statement or a declaration.** One instruction per line;
  one field per line in a struct body; one statement per line in a proc or
  macro body. A newline does not end anything inside `(` `)`, `[` `]`, or an
  initialiser's `{` `}` (the braces after `=` in a data declaration), so a
  long `invoke(...)` or a table of vertices continues onto the next lines.
- A declaration copied from a `.rin` file keeps working: its trailing `;`
  starts an empty comment. A one-line struct (`{ x: f32; y: f32; }`) does
  not, because everything after the first `;` is a comment; write one field
  per line.
- `.rin` files imported by a `.rasm` program are read with rin's own rules
  (`;` terminators, `//` comments), unchanged.

- Registers are keywords: `rax`..`r15`, `eax`..`r15d`, `ax`..`r15w`,
  `al`..`r15b`, `ah`..`bh`, `xmm0`..`xmm15`, `rip`; `ymm` and `zmm` when the
  roadmap gets there. They cannot be declared as names.
- Mnemonics are identifiers. A statement whose first token is an identifier
  that is not a declared macro, followed by operands, is an instruction.
- Integer literals: decimal, `0x` hex, `0b` binary, `'c'`, with C's suffixes
  (`U`, `L`, `UL`, `LL`, `ULL`) accepted, so header constants copy over.
  A character literal is one character, maybe escaped (`'\\'`, `'\n'`);
  `'ab'` is an error, not MASM's two bytes: write `0x6261`.
- Float literals: `1.0`, `-0.5`. A literal without `.` is an integer.
- String literals: `"text"` with rin's escapes.
- Comments are carried into the `.s` as `#` comments.
- `#` lines: see §3.5. A `#define` line takes no `;` comment, because C
  would read the `;` as part of the value; rasm rejects it.

## 2. Types

`asm` for layout and sizes; `check` for names.

    u8 u16 u32 u64  i8 i16 i32 i64  f32 f64  b32  usize
    *T              a pointer: 8 bytes holding an address. The only pointer type
    [N]T            array; N a constant expression
    name            a struct, enum or alias
    proc(...) -> T  a proc type, for declarations and function pointers

**One pointer type.** rin distinguishes `*T` (one element), `^T` (many)
and `[]T` (a slice) so its compiler can check indexing. rasm never indexes
through a pointer type: a pointer is used only after `mov rax, p`, and
`[rax + rcx*4]` or `[rax].vec4.x` name their own type. So `*T` is the only
pointer in a `.rasm` file, and it means C's `T*`. This diverges from rin on
purpose: in rin `*T` is one element, and stays so. An imported `.rin` file
keeps rin's spellings; rasm reads `*T` and `^T` alike as a pointer and
`[]T` as its 16-byte layout (pointer, length), with no checks. In the data
file every pointer kind lowers to C's `T*`, so the C is the same either way.

Sizes as C on x64. The size of a type is the size of a memory operand that
names a symbol or field of that type, so `dword ptr` is not written when the
operand is typed. An explicit `T ptr` always wins (`movups xmmword ptr [rcx],
xmm0` stores four floats into an `f32` field on purpose).

### 2.1 struct

    D3D11_BUFFER_DESC: struct = {
        ByteWidth: UINT
        Usage: D3D11_USAGE
        BindFlags: UINT
        CPUAccessFlags: UINT
        MiscFlags: UINT
        StructureByteStride: UINT
    }

C layout on x64: natural alignment per field, padded to the largest,
nested structs, arrays, anonymous `union`/`struct` members as in rin. No
bitfields in a first version. `[align(N)]` on a field or variable as rin
spells it: `mask: [4]const u32[align(16)] = {...}`.

The code never names a field: rasm computes the offset and emits a number,
with the field path as a comment:

    mov dword ptr [rip + buffer_desc + 8], ecx     # buffer_desc.BindFlags

The C file declares the struct with the header's own field names, so the
Watch window shows `Width` and `sType`, not MASM's `_Width` and `_sType`.

`sizeof(T)` and `offsetof(T.field)` are constants.

A header's anonymous members are looked through, as C does: rinbind writes
them as fields named `_anon0`, `_anon1`, ... of a named type, which C does
not have, so `rtv_desc.Texture2DArray.MipSlice` finds `Texture2DArray` in
`D3D11_RENDER_TARGET_VIEW_DESC`'s anonymous union, and the layout assert
asks C for `offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Texture2DArray.MipSlice)`.

### 2.2 enum

    D3D11_USAGE: enum = { D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE, ... }

4-byte integers, referenced qualified as in rin
(`D3D11_USAGE.D3D11_USAGE_IMMUTABLE`), emitted as the number.

### 2.3 alias

    HWND: alias = *void
    UINT: alias = u32

### 2.4 COM vtables are structs

There is no `interface` kind. A COM interface's vtable is a struct of proc
pointers, as C's `CINTERFACE` headers declare it, and a method offset is a
field offset:

    ID3D11DeviceVtbl: struct = {
        QueryInterface: *proc(This: *ID3D11Device, riid: *const GUID, out: **void) -> HRESULT
        AddRef: *proc(This: *ID3D11Device) -> ULONG
        Release: *proc(This: *ID3D11Device) -> ULONG
        CreateBuffer: *proc(This: *ID3D11Device, desc: *const D3D11_BUFFER_DESC, init: *const D3D11_SUBRESOURCE_DATA, out: **ID3D11Buffer) -> HRESULT
        ...
    }

    mov rcx, device
    mov r10, [rcx]
    call [r10].ID3D11DeviceVtbl.CreateBuffer    ; call qword ptr [r10 + 24]

This is the ordinary `[reg].T.f` operand of §5. `rinbind` already emits the
`*Vtbl` structs when run on the SDK headers (verified 2026-10-24: 41 of them
from `d3d11.h`, `ID3D11DeviceVtbl.CreateBuffer` at byte 24). The hand-curated
bindings in rin-dx11 and njinn keep COM types opaque, which suits rin but not
rasm; rasm imports rinbind's full output instead.

### 2.5 external types

`struct[external]`, `alias[external]`, `enum[external]` parse so a rin
binding imports. rasm computes offsets from the declaration, so a shared
binding must be complete, every field in order; the C file then checks
those offsets against the real header (§9).

## 3. Declarations

`asm`. `name: kind = value`, one per line.

### 3.1 data

    pos_x: [MAX_ENTITIES]f32[align(16)] = {}    ; zero-initialised (.bss)
    scratch: [1024]u8 = ?                       ; uninitialised (.bss)
    running: b32 = 1                            ; initialised (.data)
    scr_width: const u32 = SCR_WIDTH            ; const: storage, addressable (.rdata)
    clear_color: [4]const f32 = { 0.2, 0.3, 0.3, 1.0 }
    wc: WNDCLASSA = {
        .lpfnWndProc = window_proc&,            ; an initialiser continues across lines
        .lpszClassName = "LearnD3D11",
    }

Every data declaration goes to the C file, exactly as rin would compile it,
so it is a typed global the debugger can show. Designated and positional
initialisers as in rin, nested. This is where state lives: there is no local
variable machinery (§4.4).

An initialiser is a **C constant expression**, because C compiles it:
integer and float literals, `#define` constants, enum members, `sizeof`,
`offsetof`, `cast(<constant>, T)`, string literals, and address constants
`x&` (`window_proc&`, `c.transform&`). A `const` variable is **not** a
constant expression in C: `rect: RECT = { .right = scr_width }` is an error;
`{ .right = SCR_WIDTH }` is right.

### 3.2 proc

    create_buffer: proc(bind: u32, data: *const void, size: u32) -> *ID3D11Buffer = {
        ...
        ret
    }

A proc is a code label and a block of statements. **rasm emits the label,
the statements and nothing else**, plus the debug record that names the proc
for the call stack. `ret` is an instruction the programmer writes. The
parameter list is documentation and a declaration: it is what a shared
`.rin` file states, what the C file's prototype says (so `window_proc&` can
initialise data), and it shows the convention at a glance. The body uses
`rcx`, `rdx`, `r8`, `r9`, `xmm0`.. and the stack directly. A parameter name
is not an operand.

- `proc[external](...)`: declared here, defined elsewhere.
- `proc[static]` keeps the label local; the default is `.globl`.
- `proc[callconv(...)]` is accepted from rin files and ignored.

### 3.3 macro

    v3load: macro(reg, base) = {
        movsd reg, qword ptr [base]
        insertps reg, dword ptr [base + 8], 0x20
    }

Textual substitution of token sequences. Labels defined in a body are
renamed per expansion. A body holds instructions, labels and other macro
invocations (bounded depth; recursion is an error). Every instruction of an
expansion carries the invocation's line, so F10 steps over a whole
`invoke`, as it did in ml64.

**Overloading by parameter count** is the one thing a macro does beyond
substitution: several definitions may share a name, and an invocation picks
the one whose count matches. That is what lets `invoke` and `vcall` be single
names (§7). No `if`, no loops, no varargs, no string operations: MASM's
`for row, <0, 16, 32, 48>` becomes a macro taking `row`, invoked four times.

### 3.4 import

    import "../includes/d3d11.rin"
    import "../includes/calls.rasm"

Processed once per program; no guards. A `.rasm` file contributes
everything. A `.rin` file contributes `struct`, `enum`, `alias`, data and
proc declarations and object-like `#define`s, and:

- a rin **proc with a body** is taken as an external declaration: the
  linker finds the body in the object rin built. `import "std/memops.rin"`
  gives rasm the arena's layout and its functions;
- rin's C-only lines (`cinclude`, function-like `#define`, `#if`/`#ifdef`,
  `#include`) are skipped by the code side and kept by the C file, where
  they mean what they mean in rin.

### 3.5 `#define`

    #define MAX_ENTITIES 65536
    #define WINDOW_STYLE (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX))

An object-like `#define NAME <constant expression>` is a compile-time
constant: an immediate in an instruction, a value in an initialiser or an
array size. It occupies no storage. The same line means the same thing in
rin and in rasm's C file. Any other `#` line is an error in a `.rasm` file.

## 4. Statements inside a proc

### 4.1 instruction

`asm`. Any x64 mnemonic, operands in Intel order, rasm names as operands
(§5). rasm checks what it can see (operand sizes, undefined names, labels)
and leaves the rest to the assembler, whose error points at the `.rasm`
line through `.cv_loc`.

### 4.2 label

    again:
        ...
        jnz again

`asm`. Proc-scoped: emitted as `.Lproc_label`, so a label never collides
with a global or with another proc's label (the masm64-dx11 trap).

### 4.3 macro invocation

    v3load(xmm0, rdx)
    invoke(CreateWindowExA, 0, "LearnD3D11", ...)

`asm`: the chosen definition's statements, substituted, in place.

### 4.4 locals: none

There is no `local` and no named slot. Stack space is `sub rsp, N` and
`[rsp + K]`, written out with a comment; or state lives in data. A callee
may use the 32 bytes of shadow space above its return address as scratch
(math.rasm's `sinf` does). A struct laid over the frame (`[rsp].FRAME.f`) is
legal through the ordinary field operand and is the first parked idea to
bring back when raw offsets hurt ([decisions.rasm](decisions.rasm) §1);
with it, rasm could also write the records that fill VS's Locals window.

## 5. Operands

`asm`. The only thing rasm does to an instruction is rewrite names into
numbers and sizes.

| operand | meaning | emitted as |
|---|---|---|
| `rax`, `xmm0` | register | itself |
| `42`, `0x40`, `MAX_ENTITIES`, `D3D11_USAGE.D3D11_USAGE_IMMUTABLE`, `sizeof(T)`, `offsetof(T.f)`, `row * 4 + 1` | immediate; integer expressions fold | the number |
| `name` | a global | `T ptr [rip + "name"]`, `T` from its type; every symbol is quoted, so no name is reserved |
| `name.f.g` | a field of a struct-typed global | `T ptr [rip + name + off]`, comment `# name.f.g` |
| `name[i]`, constant `i` | an element (elements, not MASM's bytes) | `T ptr [rip + name + i*size]` |
| `name&`, `name.f&` | the address of a global, a link-time constant | `movabs r64, offset name + off`: a 64-bit immediate with an ADDR64 fixup (verified with clang). Only as the source of a `mov r64, ...` and in initialisers. Elsewhere (`cmp`, `push`, a 32-bit register) it would be a 32-bit absolute address: a `check` error |
| `lea r, name` | the address, RIP-relative | `lea r, [rip + name]` |
| `[reg + ...]` | memory through registers | itself; size from the other operand or an explicit `T ptr` |
| `[reg].T.f`, `[reg].T.f[i]` | a field through a pointer in a register | `S ptr [reg + off]`, comment `# T.f[i]` |
| `T ptr <mem>` | explicit size | itself |
| `1.0` | a pooled `f32` in `.rdata`, so a **memory** operand | `dword ptr [rip + .Lk_f32_1]` |
| `"text"` | the address of a pooled zero-terminated string, a constant like `x&` | `movabs r64, offset .Lk_str_3` |

`name[reg]` is an error: an indexed global cannot be RIP-relative and would
need a 32-bit absolute address (`/LARGEADDRESSAWARE:NO`). Write `lea rsi,
name` and index `[rsi + rcx*4]`.

`mov eax, 1.0` is legal and is a load from the pool, not an immediate; the
bits as an immediate are written as bits, `mov eax, 0x3F800000`.

## 6. Constant pool

A float or string literal used as an operand is an `.rdata` item in the `.s`
and a reference, once per distinct literal, aligned to its type. This
replaces masm64-dx11's `movf`, `storef` and `floats()` macros. A constant
array is declared as `const` data and passed as `x&`.

## 7. Calls

`asm`. `call name`, `call reg`, `call [mem]` are instructions.
`examples/includes/calls.rasm` holds the convention as two macro names,
one definition per argument count:

    invoke(f, a1, ..., an)          f: a proc, an import, or a u64 function pointer global
    vcall(obj, method, a1, ...)     method: Vtbl.Method

`invoke(f, a, b, c, d, e)` expands to seven lines: `mov r11, e`, `mov qword
ptr [rsp + 32], r11`, `mov rcx, a`, `mov rdx, b`, `mov r8, c`, `mov r9, d`,
`call f`. What stays the caller's, exactly as with vulkasm's `invoke`:

- rsp 16-aligned at the call, with 32 bytes of shadow space and 8 per
  argument past the fourth, reserved by the caller's own `sub rsp`;
- arguments move in order, stack arguments first through `r11`, so `r11` is
  not an argument to a call with more than four (r11 and not rax, so that
  rax, which holds every return value, can be passed anywhere; the first
  real program, calls_test, fell into the rax version), and a register is not
  passed after the register it would be moved into
  (`invoke(D3DCompile, rdx, r8, rcx, ...)` passes the source as the path);
- values move as 64 bits: anything passed by value lives in a 64-bit
  register or global;
- floats go in `xmm0`..`xmm3` by position and are not handled: write the
  `movss` and the `call` (5.1's `m4_rotate`).

The assembler does not check for the order clobber, or anything else about
registers: no code analysis in the first version (principle 7). The parked
checks are in [decisions.rasm](decisions.rasm) §2.

## 8. What is not in the language

Expressions in instructions beyond folding integer constants. Implicit
moves, loads or stores. `if`, `while`, `for`, `switch`. Register
allocation. Prologues, epilogues, frames, `uses`, `local`. Typed calls.
Generics, reflection, bounds checks, slices. Struct assignment. Methods. A
raw-assembler escape. A standard library beyond what is written in rasm.

## 9. Output

A program compiles to two files, which `rin asm` then assembles, compiles
and links:

    rin asm hello_triangle.rasm -l d3d11 -l d3dcompiler -l dxguid [-o out.exe] [--windows] [--no-link] [--verbose]
        -> build/rasm/hello_triangle.s        the code
        -> build/rasm/hello_triangle_data.c   the data and its types
        clang --target=x86_64-pc-windows-msvc -c hello_triangle.s
        clang --target=x86_64-pc-windows-msvc -c -g -gcodeview hello_triangle_data.c   (-I rin's root and std)
        clang -fuse-ld=lld -g ... -lkernel32 -luser32 -l...   -> build/hello_triangle.exe + .pdb

kernel32 and user32 are always linked; the C runtime's startup calls `main`
as an ordinary function, so `main` is a proc like any other. `--windows`
links for the GUI subsystem.

**The `.s`** holds: `.intel_syntax noprefix`; `.cv_file` for every source
file (forward slashes); `.rdata` with the constant pool; `.text` with each
proc as a `.globl` label, `.cv_func_id`, a `.cv_loc` before every
statement's first instruction, and an end label; a `.debug$S` section with
an `S_GPROC32` record and `S_END` per proc (raw `.short`/`.long`, as clang
writes for C), a `.cv_linetable` per proc, `.cv_filechecksums`,
`.cv_stringtable`. Recipe and a working example:
[debug-proto/code.s](debug-proto/code.s).

**The C file** is written by rasm from rin's parse of the program's type and
data declarations. (Not by rin's own backend: rin's checker enforces rin's
rules, its pointer kinds, `#define` names in initialisers, `proc` rather
than `proc&`, and rasm does not share them.) It holds the structs, enums and aliases, every global with its
initialiser, prototypes for the procs the data refers to (`window_proc&`),
the imported `.rin` files' C lines, and every `#define` the program uses
inside an `#ifndef NAME` guard, so a constant that `windows.h` already
defines keeps the header's definition. It also carries a
`_Static_assert(offsetof(T, f) == N, ...)` for **every offset the code
used**, and `sizeof` checks for every typed operand, so if rasm's layout and
C's ever disagree the build fails, with the field named. The same mechanism
rin already uses for external layouts.

What the debugger gets, verified in Visual Studio and lldb: breakpoints and
stepping on `.rasm` lines; proc names in the call stack; every global
watchable by name with its struct type and field names; registers, memory
and disassembly as always. What it does not get yet: a Locals window,
because rasm has no locals (§4.4).

---

## Considered and rejected

Recorded 2026-10-24 with the reason, so that the thinking is kept. **Do not
implement these.** The reason for most is the same: the assembler stays
manual, and the output contains only instructions the source states.

### R1. Generated frames

Every proc gets `push rbp`, `mov rbp, rsp`, `sub rsp, N` and an epilogue, `N`
computed from locals, saves and the largest call (which would have fixed
vulkasm's bug 3). Rejected: the programmer writes the frame.

### R2. Parameter homing

`rcx`..`r9` copied to their shadow slots so a parameter is a named memory
operand. Rejected with R1. A parameter name is documentation.

### R3. `uses` and the register-write audit as a generator

`uses rbx, rsi` making rasm save and restore them. Rejected. A check-only
audit is parked in decisions.rasm §2.

### R4. Typed calls

`f(a, b, 1.0, desc&)` expanding to argument moves with checks. Rejected:
argument placement is the user macro's job.

### R5. Interface call sugar, and the `interface` kind

`device.CreateBuffer(...)` expanding to the vtable load and call. Rejected
with R4. The `interface` declaration kind that gave method offsets by name
was dropped too: a `*Vtbl` struct gives the same offset through the
ordinary field rule (§2.4).

### R6. `= {}` on locals, and MASM's `LOCAL`

Rejected: there are no locals. MASM's `local x:T` is rejected for what it
does, not for naming a slot: it makes ml64 insert `push rbp`, `mov rbp,
rsp`, `sub rsp, N` and a `leave` before every `ret`, and it chooses the
offsets. Naming a stack slot without generating anything is open-questions
M; a struct laid over the frame is the deferred idea recorded there.

### R7. ml64 as the output

Chosen for part of 2026-10-24 ("for now we can just compile to masm64 and
later brute force our way into getting the debugger to work"), because the
author is at home in MASM and its references. Dropped the same day when the
author made ml64-grade debugging the condition for writing rasm at all: ml64
has no `#line`, so its line table can only name the `.asm`; reaching the
`.rasm` would need a post-pass rewriting every object (debugging.md route
B), and its Watch window would show MASM's renamed fields. clang with
`.cv_loc` plus C data does both with no post-processing.

### R8. Unwind directives

`.pdata`/`.xdata` for exact stack walks. Frame generation by another name;
out with R1.

### R9. A custom mnemonic for argument loads

`ld reg, value`, picking `lea`, `mov r32` or `mov r64` by operand. Proposed
to make `invoke` handle `ADDR` and 32-bit values. Rejected by the author:
no custom mnemonics. `x&` as a link-time constant (a real `mov`) and 64-bit
values for anything passed by value cover it.

### R10. Variadic macros with a loop

`invoke: macro(f, args...)` with `for a, i in args` and an `if i < 4`.
Rejected: that is MASM's macro language in rin syntax. Overloading by
parameter count (§3.3) gives one name with no control flow.

### R11. Enum members as the compile-time constant

`Limits: enum = { MAX_ENTITIES = 65536 }`, because a C `const` is not a
constant expression. Rejected by the author as ugly; in C only a `#define`
works there, so rasm reads object-like `#define`s and rin is not touched.

### R12. `#` as a raw-assembler escape, and rin's `;` terminators

`#` took rin's meaning (§3.5); nothing in the lessons needed the escape.
rin's `;` statement terminator and `//` comments gave way to assembler line
syntax by the author's decision (§1).

### R13. Debug types written by rasm itself

rasm could write the CodeView type records (`.debug$T`) and typed data
symbols as raw bytes, as clang does. Not needed: compiling the data as C
gives the same records, written by a real compiler, plus a free layout
check. Kept in mind for a direct machine-code backend, which would need it.
