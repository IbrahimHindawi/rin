# rasm

A rin-shaped assembler for x64 Windows, for playing with D3D11 graphics and raw
SIMD assembly. rin's declaration syntax and types, x64 instructions written as
they are, one per line. The assembler generates nothing you did not write: frames,
shadow space, saved registers and argument moves are yours, or your macros'.

The code is assembled by clang with a `.cv_loc` per statement and the data is
compiled as C, so Visual Studio steps through the `.rasm` source exactly as it
does through MASM source.

**Status:** implemented as `rin asm`, in the rin compiler
([`src/asm.c`](../src/asm.c), tested by [`tests/run_rasm.py`](../tests/run_rasm.py)).
This folder is rasm's own section of the rin repository: everything about
rasm that is not the assembler itself.

    rin asm hello_triangle.rasm -l d3d11 -l d3dcompiler -l dxguid      -> build/hello_triangle.exe + .pdb

- [docs/](docs/README.md): the spec, design, roadmap, decisions, and the
  debugging prototype that proved it ([docs/debug-proto](docs/debug-proto))
- [examples/](examples/README.md): the smallest programs, the call macros, the
  generated Win32/D3D11 binding, and LearnOpenGL's hello triangle
- [editors/](editors): syntax colouring for Neovim
  ([editors/nvim](editors/nvim/README.md)) and Visual Studio
  ([editors/visualstudio](editors/visualstudio/README.md), a TextMate grammar
  VS Code reads too), and a language server
  ([editors/lsp](editors/lsp/README.md)) that answers from the assembler
  itself: diagnostics, field offsets and sizes on hover, the register each
  argument of a Windows call goes in, instruction notes, definitions,
  completion and signature help
- [rasm-dx11](https://github.com/IbrahimHindawi/rasm-dx11): all 38 of
  LearnOpenGL's lessons, from the first window to image based PBR, in rasm

Related: [rin](../README.md) (the language whose front end rasm reuses).
