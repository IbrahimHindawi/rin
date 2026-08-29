# `rin build`

> **Working.** njinn and rin-playground both build through it. The older
> bunyan arrangement still exists and still works; this replaces the part of it
> that every project had to copy.

## The shape

    rin build [build.rin]

`build.rin` is read, not run. It is ordinary rin holding top-level globals with
known names, so it parses with the same parser as every other module, type-checks
like one, and an editor already highlights it. Executing it would need an
interpreter for no gain: a build is a handful of paths and flags, and a
declaration states them more plainly than a script.

Four steps, in order:

  1. `build.rin` is parsed and the known globals are read out of it.
  2. The entry module is transpiled to `<build_dir>/rin_gen/<stem>.c`.
  3. A `CMakeLists.txt` is generated into `<build_dir>/rin_gen`.
  4. cmake configures into `<build_dir>/cmake`, then builds.

The generated `CMakeLists.txt` is rewritten on every run. Editing it is editing
an output.

## What build.rin can say

| global | type | meaning |
|---|---|---|
| `build_name` | `*const char` | target name; **required** |
| `build_entry` | `*const char` | the module to transpile; **required** unless `build_entries` |
| `build_entries` | `[N]*const char` | several independent programs, one executable each |
| `build_dir` | `*const char` | output root, default `build` |
| `build_generator` | `*const char` | cmake generator, default `Ninja` |
| `build_compiler` | `*const char` | C compiler, default `clang-cl` |
| `build_type` | `*const char` | cmake build type, default `Debug` |
| `build_pch` | `*const char` | one precompiled header |
| `build_c_sources` | `[N]*const char` | C files compiled alongside the generated one |
| `build_include_dirs` | `[N]*const char` | extra include roots |
| `build_libraries` | `[N]*const char` | link libraries |
| `build_defines` | `[N]*const char` | preprocessor defines |
| `build_c_flags` | `[N]*const char` | flags passed to the C compiler |

Paths are relative to the directory holding `build.rin`. The compiler's own
directory and its `std` are added to the include path automatically, so a
project never spells out where `core.h` and `reflect.h` live.

## A whole engine

njinn's, in full:

    build_name:      *const char = "njinn";
    build_entry:     *const char = "src/gin_win32.rin";
    build_dir:       *const char = "build";
    build_generator: *const char = "Ninja";
    build_compiler:  *const char = "clang-cl";
    build_type:      *const char = "Debug";
    build_pch:       *const char = "src/pch.h";

    build_c_sources: [3]*const char = {
        "src/miniaudio_impl.c",
        "src/cgltf.c",
        "src/stb_image.c",
    };

    build_include_dirs: [3]*const char = {
        "src",
        "extern/cgltf",
        "extern/cglm/include",
    };

    build_libraries: [5]*const char = {
        "user32", "gdi32", "d3d11", "dxgi", "d3dcompiler",
    };

    build_defines: [1]*const char = { "_CRT_SECURE_NO_WARNINGS" };

    // Vendored headers trip these; keep the list narrow so real warnings show.
    build_c_flags: [2]*const char = {
        "-Wno-macro-redefined",
        "-Wno-parentheses-equality",
    };

That is the entire build description for 25,000 lines of rin, three vendored C
libraries and a D3D11 renderer.

## Several programs at once

rin-learn is nineteen lessons that share nothing and link to nothing. Naming
them as entries gives nineteen executables, each named after its file:

    build_name:    *const char = "rin-learn";
    build_entries: [19]*const char = {
        "src/lessons/00_hello_world.rin",
        "src/lessons/01_sizes.rin",
        // ...
    };

With `build_entry` the single target is named `build_name`; with
`build_entries` each target is named after its file stem.

## Compiling one file

Unchanged, and still the right thing when there is no project:

    rin compile src/main.rin -o build/main.c --no-header

`rin build` is that command plus the cmake invocation around it.

## Known limits

  * **One translation unit per entry.** Module mode -- a `.c` per module plus a
    shared monomorph unit -- is not wired through. Everything reachable from an
    entry becomes one `.c`.
  * **Entries share their settings.** `build_include_dirs`, `build_libraries`,
    `build_defines`, `build_c_flags`, `build_pch` and `build_c_sources` apply to
    every target. A project needing different flags per program needs more than
    this command currently offers.
  * **No pre-build hooks.** njinn generates bindings and cooks resources before
    compiling, and those still run from its own script. A build that needs to
    generate sources first cannot yet say so here.
  * **cmake and a generator must be on PATH.** `rin build` shells out; it does
    not vendor a build system.
  * **No incremental short-circuit.** The transpile runs every time. cmake and
    ninja still skip unchanged work, so the cost is one compiler pass.
