# Debugging generated assembly

> **Status, 2026-10-24: solved and verified.** The author's condition for
> writing rasm at all: "I would write rasm only if the debugging experience
> is exactly as masm64." It is. Code goes through clang's assembler with a
> `.cv_loc` per statement (route C below) and a raw `S_GPROC32` per proc;
> data and types go through rin's C backend, compiled with `/Z7`. The author
> "was able to cleanly step through the rasm" in Visual Studio, using
> [debug-proto](debug-proto). Routes A and B are kept as the analysis that
> led there; B is also the reference a direct backend would need.

The question this answered: hand-written ml64 assembled with `/Zi` steps
cleanly in Visual Studio, source line by source line. A generated file must
not lose that. Can the debugger step through the `.rasm` source itself?
Yes, by three routes; C is the one taken.

## How the debugger finds a line

ml64 `/Zi` writes CodeView into the `.obj`: a `.debug$S` section with a
`DEBUG_S_FILECHKSMS` subsection (one entry per source file: an offset into
the string table, a checksum kind, a checksum), a `DEBUG_S_STRINGTABLE`
subsection (the file names), and one `DEBUG_S_LINES` subsection per
function (a code range, then blocks of `{offset, line}` records pointing at a
file entry). `link /debug` merges those into the `.pdb`. Visual Studio maps
the instruction pointer to `(file, line)` through that table and opens the
file. Nothing in this chain knows what language the file is. A record that
says `hello_triangle.rasm:112` opens `hello_triangle.rasm` at line 112.

ml64 has no `#line` or `.loc`: every record it writes names the file it read
and the line it read it on, and a macro expansion gets its invocation line.
So the generated `.asm` is what the records name, unless something rewrites
them.

Verified 2026-10-24 on a hand-written lesson object:

    "C:\Program Files\LLVM\bin\llvm-pdbutil.exe" dump -l C:\devel\masm64-dx11\build\obj\hello_window.obj

    C:\devel\masm64-dx11\includes\cstr.asm (SHA-256: 6E60...)
      0000:00000000-000007DB, line/addr entries = 191
        14 00000000     13 00000000     15 00000008 ...
    C:\devel\masm64-dx11\includes\print.asm (SHA-256: 5079...)
      ...
    C:\devel\masm64-dx11\src\1.getting_started\1.1.hello_window\hello_window.asm (SHA-256: 63F8...)
      0000:00000000-000007DB, line/addr entries = 84

Two facts fall out. ml64 keeps **one table per source file, included files
too**, so a unity build already debugs across files. And the checksum kind it
writes is SHA-256 of the file, which the rewriter in route B must either
recompute for the `.rasm` or set to "none".

## Route A: line-aligned output. Zero tooling.

Emit the `.asm` so that **line N of the `.asm` is line N of the `.rasm`**.
Then the records are already right except for the file name, and the file
they name is a readable twin of the source.

What has to hold:

- A statement that expands to one instruction is emitted on its own line.
  That is every instruction, label, and data declaration: the bulk of a
  lesson.
- A statement that expands to several instructions (a typed call, a
  prologue, an epilogue before `ret`, a macro invocation) must still occupy
  one output line. The way that works: emit the expansion as **one MASM
  macro invocation**. rasm writes a per-call-shape macro at the top of the
  file and invokes it on the statement's line; ml64 attributes the whole
  expansion to that line, F10 steps over the call as one step, F11 enters
  the callee. The alternative, letting a call take several lines, shifts
  every line after it and breaks the alignment.
- Struct definitions, `externdef`s, the constant pool and anything with no
  source line go in a second generated file that the main one `include`s on
  its first line. That costs one line, absorbed by the source's own first
  line, which is always the comment header. Since ml64 keeps a line table per
  included file (verified above), the included file gets its own table and
  the main file's alignment is untouched.

Result: F10 and F11 in Visual Studio step through `hello_triangle.asm`, which
reads like the `.rasm` with the sugar expanded. To make VS open the `.rasm`
instead, patch the file name in the `.obj` (the string table entry) and set
the checksum kind to none. That is a 50-line script, but it is route B's
little brother, so if it is written, write route B.

Cost: the emitter is constrained for life by the alignment rule. A one-line
rule, but every feature has to respect it.

## Route B: rewrite the CodeView line table. The real fix.

rasm emits the `.asm` freely and writes a **map**: for each output line, the
`.rasm` file and line it came from (several output lines may map to one
source line: a call's expansion all maps to the call). After ml64, a
post-pass opens the `.obj`, finds `.debug$S`, and:

1. in `DEBUG_S_STRINGTABLE`, adds the `.rasm` path,
2. in `DEBUG_S_FILECHKSMS`, adds an entry for it (checksum kind MD5 of the
   `.rasm`, or none),
3. in every `DEBUG_S_LINES` block, rewrites each record's file index to the
   new entry and its line number through the map,
4. fixes the subsection lengths and section size, and writes the `.obj`
   back.

`link /debug` then produces a `.pdb` whose lines point at the `.rasm`. Visual
Studio opens the `.rasm`, F10 steps a statement (a typed call is one step,
since all its instructions share a line), F11 enters callees, breakpoints set
in the `.rasm` land on the first instruction of the statement, and the
Disassembly window shows the generated instructions interleaved with `.rasm`
source. This is exactly what `#line` buys rin, done after the assembler
instead of before.

Format reference: `cvinfo.h` and `cvdump` in Microsoft's `microsoft-pdb`
repository, and LLVM's `llvm/include/llvm/DebugInfo/CodeView/Line.h` and
`CodeViewTypes.def`. The line record is a 32-bit code offset followed by a
32-bit word of `line:24, delta_end:7, is_statement:1`. Verify a rewritten
`.obj` with `llvm-pdbutil dump -l foo.obj` or `cvdump -l foo.obj` before
linking; both print the file and line table.

Size: a few hundred lines in Python or in rasm's own C, operating on one
section. The map file is a text file of `out_line in_file in_line`. The
risk is the format details; the verification tools above make a wrong
rewrite visible immediately.

A side benefit: the `.asm` remains a perfectly good secondary source. If
the `.pdb` points at the `.rasm` and a reader wants to see what expanded,
the Disassembly window shows it, and the `.asm` is beside it.

## Route C: emit for LLVM's assembler with `.cv_loc`. Verified.

LLVM's MC layer accepts CodeView directives in assembly source. **Tested
2026-10-24 with the clang in `C:\Program Files\LLVM`**; the file is
[cv_loc_test.s](cv_loc_test.s) beside this document:

    .intel_syntax noprefix
    .text
    .globl h
    h:
    .cv_file 1 "C:/devel/rasm/docs/sample.rasm"
    .cv_func_id 0
    .cv_loc 0 1 20 0
        mov eax, 1
    .cv_loc 0 1 21 0
        add eax, 2
        ret
    .Lh_end:
    .section .debug$S,"dr"
    .p2align 2
    .long 4
    .cv_linetable 0, h, .Lh_end
    .cv_filechecksums
    .cv_stringtable

    clang --target=x86_64-pc-windows-msvc -c cv_loc_test.s -o t.obj
    llvm-pdbutil dump -l t.obj

    C:/devel/rasm/docs/sample.rasm (no checksum)
      0000:00000000-00000009, line/addr entries = 2
        20 00000000 !   21 00000005 !

The object's line table names the `.rasm` and its lines, with no
post-processing. `link /debug` consumes it like any other object. This is
`#line` for assembly, native. Details learned: the `.debug$S` section must
be opened explicitly with its `.long 4` signature before `.cv_linetable`;
the `.cv_file` path takes forward slashes (a backslash is an escape); the
`!` marks `is_statement`.

The cost is that the input is GNU-syntax Intel assembly
(`.intel_syntax noprefix`, `.text`, `.globl`, `dword ptr [rip + x]`,
no `proc`/`endp`, no `struct`, no `local`), not MASM. rasm would emit that
instead of ml64 text. Since rasm computes layouts and frames itself, it
never needed MASM's `struct` or `local`; the output just stops looking like
a masm64-dx11 lesson, and `#` passthrough lines would be GAS lines.

**`llvm-ml64` is ruled out.** Tested the same day: it ignores `/Zi` ("warning:
ignoring unsupported 'Zi' option"), rejects every `.cv_*` directive as an
invalid mnemonic, and does not support `option casemap`. It cannot produce
line information at all, so it is not a target for any route.

## Decision and verification

**Route C for the lines, C for the types.** What ml64 `/Zi` gives Visual
Studio, read from masm64-dx11's `hello_triangle.obj` with `llvm-pdbutil dump
-symbols -types`, and how rasm matches it:

| ml64 `/Zi` | rasm |
|---|---|
| line table per source file | `.cv_loc` per statement naming the `.rasm` |
| `S_GPROC32` per proc (call stack, function breakpoints) | the same record, written as raw `.short`/`.long` in `.debug$S` |
| typed data symbols, `LF_STRUCTURE` types with field lists (Watch by field name) | the data compiled as C with `/Z7`: real types, real field names (ml64's would be MASM's renamed `_Width`) |
| `S_REGREL32` for `local`s and homed parameters (Locals window) | none yet: rasm has no locals. The parked frame struct (decisions.rasm §1) could emit `S_REGREL32` later |

The prototype, [debug-proto](debug-proto): `proto.rasm` (the source shown),
`code.s` (what rasm would emit for it), `data.c` (what rin's C backend would
emit for its data), `build.ps1`. Verified:

- by the author in **Visual Studio**: breakpoint on a `.rasm` line, F10
  through it cleanly;
- with **lldb** (LLVM 22, PDB reader `native`, `cmds.lldb`): `b
  proto.rasm:6` binds to `main + 4`; `next` steps 6, 7, 8, 9 with the source
  shown; `target variable desc` prints `{ count = 1000, gravity = (x = 0, y
  = -9.80000019, z = 0) }` and then `count = 1001` after the store; `bt`
  shows `main at proto.rasm:12`.

Notes from the run: the Windows SDK's `cdb` on this machine is broken (a
2016 `dbgeng.dll` beside a newer `dbghelp.dll`, exit `0xC0000138`); LLVM's
lldb needs a `python311.dll` on `PATH` and `PYTHONHOME` (borrowed from an
installed application for the test). Neither matters for Visual Studio.

Visual Studio did not need the `S_OBJNAME` and `S_COMPILE3` records clang
writes for C. If a later tool insists, clang's templates (from `clang -S -g
-gcodeview` on a C file) are:

    .short .Ltmp5-.Ltmp4          # S_OBJNAME
    .Ltmp4:
    .short 4353
    .long 0                       # signature
    .asciz "path\\to\\object"
    .p2align 2
    .Ltmp5:
    .short .Ltmp7-.Ltmp6          # S_COMPILE3
    .Ltmp6:
    .short 4412
    .long 0                       # flags and language (0 = C; 3 = MASM)
    .short 208                    # CPU: x64
    .short 22, 1, 6, 0            # frontend version
    .short 22016, 0, 0, 0         # backend version
    .asciz "compiler name"
    .p2align 2
    .Ltmp7:

## What to check, each time

1. `rin asm` (or `debug-proto/build.ps1`), then `devenv /debugexe x.exe`,
   open the `.rasm`, F9, F5. The file that opens on a break is the one the
   `.pdb` names.
2. F10 over an `invoke(...)`: one step for the whole expansion. F11 into a
   `call`: lands on the callee's first `.rasm` line.
3. Watch a struct global: fields by their header names.
4. `llvm-pdbutil dump -l build\obj\x.obj` lists every file and line record;
   `dump -symbols` shows the `S_GPROC32`s.
5. The Registers, Memory and Disassembly windows read the process, not the
   line table; they always work.
