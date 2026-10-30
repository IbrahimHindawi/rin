# Working on rin's safe subset: the operational notes

Copy this file to the directory that holds the repos (`/devel/CLAUDE.md`) so
a session opened there starts with it. It holds what the code does not say:
how things are built and verified, which branches are which, and where the
work stands. The design itself is in `rin/docs/regions.md`; read that second.

## The repos and their branches

- `rin/`: the compiler, one file, `src/main.c`, lexer to C emitter, with std
  in `src/std`. Branch `regions` is the safe compiler; `main` is the original
  unsafe rin and is never touched. Everything in this note is about `regions`.
- `rin-dod/`: the data-oriented design course in rin. Branch `regions` (and
  its twin `safe`) holds every program twice: the original under `ecs/src`,
  `src`, `dojo`, and the port to the safe subset under `ecs/safe`,
  `src/safe`, `includes/safe`, `dojo/safe`. `main` is the published course.
- the engine: gets its own `regions` branch when its port starts. Its `main`
  is not touched either.

A compiler change is not finished until rin-dod's course still passes on it.

## Building the compiler

rin resolves `std` as the `std` folder beside its own executable, so a built
compiler needs std next to it. On Windows the repo's own build does this. On
Linux or macOS, build `src/main.c` with gcc or clang and copy or symlink
`src/std` beside the binary; the test runners look for `rin/build/rin.exe`
and `rin/build/std`, so point those at the build (symlinks are fine). After
any std change, copy `src/std/*.rin` next to the binary again.

Flags: `rin check file.rin` checks with the safety rules on (the default);
`--unsafe` or `RIN_UNSAFE=1` turns them off, `--safe` turns them back on.
`rin compile file.rin -o out.c --header out.h` emits C. `RIN_DEBUG_MOVES=1`
prints each checked proc's moves mask (which parameters its body may move
storage through).

## Verifying, in this order

    python tests/run_rin_safety.py            # 7 accept, 68 reject; must be 75/75
    python tests/run_rin_safety.py --unsafe   # the rejects must check clean with the rules off
    python tests/run_rin_attacks.py           # 215 attack programs: rejected, or clean by design
    RIN_UNSAFE=1 python tests/run_rin_torture.py --cc clang   # 9/9, the old compiler suite
    RIN_UNSAFE=1 python tests/run_rin_execute.py --cc clang   # 23/23
    # in rin-dod, with RIN pointing at the fresh build:
    RIN=<path>/rin RIN_UNSAFE=1 CC=gcc python tools/check.py  # 70 lessons, 26 dojo blocks
    RIN=<path>/rin CC=gcc python tools/check.py --sanitize safe

The accept programs and the ported lessons must also run clean under
`-fsanitize=address,undefined`; on this machine's clang the ASan runtime was
missing, so sanitizer runs use gcc.

## Rules of the road

- Never put a model name in a commit, a comment or a file; commits carry the
  attribution trailers the session is given, nothing more.
- Commit as the repo owner (`-c user.name=... -c user.email=...` if needed).
- Do compiler changes in the main session, serially: each one needs the full
  pipeline above. Use subagents for what parallelises: porting independent
  modules, and adversarial attack rounds. Agents must not edit the compiler,
  std or `build.rin`; they report blockers and the main session fixes them.
- Survey before porting: run `rin check` on every file, classify the errors
  by message (strip names), fix the common blockers once in the compiler or
  std, then fan out. This is what made the course port take one pass.
- Permissive checker changes (accepting more) can ship while agents run;
  restrictive ones cannot, since their in-progress files would break.

## The port conventions

The agents' brief is `rin-dod`'s `ecs/safe/README.md` plus these:
ports mirror the original tree under a `safe` folder with the original
`.expected` copied beside them, unchanged; `printf` becomes `printfmt` with
`{:5.2}` widths; a world lives in `level: region;` and is made whole by a
`world_make(r: region level)`; fields are `*vec<T> in level`; lookups that
can miss return `?*T`; the frame arena is `frame: region in level;` opened
once per frame; the type-erased byte column is the one legitimate
`[trusted]` core, kept small with honest signatures; every other proc is
unannotated and checked. Report every remaining `[trusted]` and why.

## State of play

Done: the safe compiler (regions, `?*T`, borrows and staleness, carriers,
integer semantics, elision, the moves analysis, the `name: region = { }`
syntax), std annotated and attacked, 75 tests, 215 attack programs, and the
whole course ported: 100 programs, 23,039 lines, 134 `[trusted]` procs of
1,444, none `[unsafe]`.

Next, in the order that pays:

1. Replace `[trusted]` with Rust-style `unsafe { }` blocks inside otherwise
   checked procs, keeping `[unsafe]` for "dangerous to call". Counts trusted
   lines instead of trusted procs. Do this before the engine port.
2. `stable<T>` (chunked, never moves, hands out `*T in R`) and `handle<T>`
   (index plus generation over a region pool) in std.
3. Port the engine on a `regions` branch: survey first, platform layer stays
   trusted with honest signatures, everything else checked. The list of
   remaining `[trusted]` and of places the checker was wrong is the result.
4. A region-end cleanup hook for GPU buffers and files, only if the engine
   shows the manual version going wrong.
5. Known gaps: field-path narrowing, a string type safe code can hold by
   value, `hashset` and `dlist` iteration, a `printfmt` brace escape, a C
   name collision on a struct named `system`, pointer-to-array element
   codegen (`vec<[3]u64>at(w, 1)*[2]` does not compile as C).

The claim that holds today: a rin program with no `[trusted]` or `[unsafe]`
of its own is memory safe at compile time, provided std is correct. What it
still needs is a stranger's attack round that finds nothing.
