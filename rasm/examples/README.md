# rasm examples

Built with `rin asm`, the rasm assembler inside the rin compiler (rin's package
must be on `PATH`, or call `C:\devel\rin-windows-x64\rin.exe`). Each command is
run from the example's own directory and writes `build/<name>.exe` with its
`.pdb` beside it; the generated `.s` and data `.c` are in `build/rasm/`.

| example | build | what it shows |
|---|---|---|
| `0.proto/proto.rasm` | `rin asm proto.rasm` | the smallest program: one struct, one global, one proc |
| `0.proto/calls_test.rasm` | `rin asm calls_test.rasm` | `import`, `invoke`, a macro with its own label, pooled strings and floats, `x&`, `#define`, an enum; prints three lines |
| `0.proto/bind_test.rasm` | `rin asm bind_test.rasm` | the generated Win32 and D3D11 binding: real struct offsets, `*Vtbl` offsets, header constants, all asserted against the SDK |
| `2.1.hello_triangle/hello_triangle.rasm` | `rin asm hello_triangle.rasm -l d3d11 -l d3dcompiler -l dxguid` | LearnOpenGL's hello triangle on D3D11: a triangle and an indexed rectangle; Tab toggles wireframe, Escape quits |
| `5.1.transformations/transformations.rasm` | not built | the first draft, kept for `includes/math.rasm`; the finished lesson, with every other LearnOpenGL lesson, is in [rasm-dx11](https://github.com/IbrahimHindawi/rasm-dx11) |

## Debugging

Open the exe in Visual Studio (`devenv /debugexe build\hello_triangle.exe`),
open the `.rasm`, set a breakpoint and press F5. F10 steps a statement (a whole
`invoke(...)` is one step), F11 enters a `call`, and the Watch window shows any
global by its type and field names. lldb works too:
`lldb build\hello_triangle.exe`, then `b hello_triangle.rasm:216`.

## The includes

| file | |
|---|---|
| `includes/calls.rasm` | `invoke` and `vcall`, the Win64 convention as plain macros. Stack arguments go through r11, so rax can be passed anywhere |
| `includes/print.rasm` | `print(format, up to three values)`: wsprintfA to stdout and the debugger |
| `includes/learn/filesystem.rasm` | `filesystem_path` (a file next to the lesson, found from the exe), `filesystem_read` |
| `includes/math.rasm` | SSE vectors and matrices for 5.1 |
| `includes/d3d11.rin` | the binding: `cinclude`s for the C side, the generated declarations, a few externs |
| `includes/gen/gen_bindings.py` | regenerates `gen/win32_d3d11.rin` with rinbind, one header per run |
