# Debugger integration

How rin programs look in a debugger, what is done, and what the deeper levels
would cost.

## What the debugger sees

rin compiles to C, and the C compiler writes the debug information. `#line`
directives make every machine instruction map back to a `.rin` file and line,
so Visual Studio, VS Code and WinDbg step through rin source. That is the only
part of the PDB rin influences. Everything else in it comes from the C
compiler:

- **types** are the emitted C structs: `wave: vec<Enemy>` is a `vec_Enemy`
  with fields `data`, `length` and `border`, and a slice `[]f32` is a
  `slice_f32` holding a pointer and a count
- **procedures** have their C names: `vec<Enemy>append` is `vec_Enemy_append`
  in the call stack
- **the frame's language** is C, so the Watch window parses C expressions:
  `wave.data[1]` and `p[0]` work as in rin, `x&` has to be written `&x`

None of this depends on which file is being stepped through. A variable
hovered in a `.rin` file is looked up as its C type.

## Three levels

### 1. C names plus natvis (done)

Natvis files tell the Visual Studio debugger how to show a type. Matching is
by the type's name, so a visualizer for `vec_Enemy` applies to `wave` while
stepping through rin. Natvis wildcards only work inside C++ template brackets
(`std::vector<*>`), and generic instances are plain C names, so no fixed file
can cover "every `vec_*`". The compiler writes one visualizer per instance the
program uses, because it is the only thing that knows the list.

- `rin compile --natvis <file>` writes the visualizers beside the C.
- `rin build` does that for every entry and links the file into the PDB with
  `/NATVIS:` (both `link.exe` and `lld-link` accept it, so this holds for
  `cl` and `clang-cl`). The debugger finds it with no setup.

What each std container shows:

| type | value column | expands to |
| --- | --- | --- |
| `slice<T>`, `array<T>` | `slice<f32> { length=3 }` | `[length]`, the elements |
| `vec<T>`, `heap<T>` | `vec<Enemy> { length=2 }` | `[length]`, `[capacity]`, the elements |
| `deque<T>` | `deque<u8> { length=4 }` | the elements in order, from `head` around the ring |
| `list<T>`, `stack<T>`, `queue<T>`, `dlist<T>` | `list<i32> { length=5 }` | the nodes' `data`, following `next` |
| `map<T>` | `map<i32> { length=1 }` | one item per occupied slot, named by its key |
| `hashmap<K, V>` | `hashmap<i32, f32> { length=1 }` | one item per occupied slot, named by its key |
| `option<T>` | `option<i32> some(42)` or `option<i32> none` | `value`, when there is one |
| `result<T>` | `result<i32> ok(7)` or `result<i32> err(3)` | `value` or `error` |
| `string8`, `string8slice` | the text | `[length]`, `[capacity]`; the text visualizer works too |

A container is recognised by its std name and its fields together, so a user
struct that happens to be called `vec` is left alone. `hashset<K>` needs no
entry of its own: it is a struct holding a `hashmap<K, u8>`, which expands
with the visualizer above. Enums already show their member names, since they
are C enums.

The limit at this level is the type column, which always shows the C name
(`vec_Enemy`), because C identifiers cannot contain `<>`. The summary in the
value column says `vec<Enemy>`, so it reads as rin where people look.

Which debuggers read natvis:

| debugger | natvis |
| --- | --- |
| Visual Studio | yes, from the PDB |
| VS Code with `cppvsdbg` (the Microsoft C/C++ extension) | yes, the same engine as Visual Studio |
| WinDbg | yes |
| lldb (VS Code's CodeLLDB), gdb | no; they need Python formatters instead |

On Windows the first three cover the debuggers in practice. lldb and gdb would
need the same information written as a Python script; nothing needs that yet.

### 2. rin names in the debug information

The type column would say `vec<Enemy>` and the call stack
`vec<Enemy>append`. Two ways there:

- **Rewrite the names after the C compiler.** The type and function names
  live in CodeView records in each object file's debug sections. A pass
  between compiling and linking could rename `vec_Enemy` to `vec<Enemy>`.
  natvis could then use `vec<*>` wildcards as it does for C++ templates, and
  the generated per-program file would become one fixed file in std. This is
  an idea, not measured: record lengths change with the names, so the
  records have to be re-serialised, and the type hashes the linker uses to
  merge types have to stay consistent.
- **Write the debug information directly.** That means rin generating its own
  code rather than going through C. `rin asm` (rasm) already emits its own
  line information for `.rasm` files, so part of this exists, but not for
  rin itself.

Rust, Zig and Odin on Windows sit at this level: their own names, emitted
through LLVM, plus natvis for the display.

### 3. A rin expression evaluator

The Watch window parsing rin, with rin's rules for `x&`, slices and casts.
That is a debugger-specific plugin: a Concord extension for Visual Studio, a
plugin for lldb. It is the one level that needs separate work per debugger,
and none of the languages above have done it. Not planned.

## Trying it

```
rin build
```

then open the executable in Visual Studio (File > Open > Project/Solution,
pick the `.exe`), set a breakpoint in a `.rin` file and start debugging.
Hovering a `vec`, slice or `option` shows the summary and the elements.

For a single file:

```
rin compile app.rin -o app.c --natvis app.natvis
clang-cl /Zi app.c /link /DEBUG /NATVIS:app.natvis
```

## Verification

- `tests/run_tests.py` checks that `--natvis` writes well-formed XML with a
  visualizer for each std container used, summarised by its rin name, and none
  for user structs; and that `rin build` links the file with `/NATVIS:`.
- A program declaring every std container produces visualizers that validate
  against the `natvis.xsd` schema Visual Studio ships, and
  `llvm-pdbutil dump --named-streams` shows the file embedded in the PDB.
- What the schema cannot check is whether the debugger evaluates every
  expression as intended. That needs a person in Visual Studio: its engine
  (`vsdbg`) only talks to Microsoft's own clients, so it cannot be scripted,
  and it is not automated. Visual Studio reports a visualizer it rejects in
  the Output window when *Tools > Options > Debugging > Output Window >
  Natvis diagnostic messages* is set to *Error* or *Warning*.
