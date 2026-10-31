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

    python tests/run_rin_safety.py            # accept and reject programs; all must pass
    python tests/run_rin_safety.py --unsafe   # the rejects must check clean with the rules off
    python tests/run_rin_attacks.py           # the attack corpus: rejected, or clean by design
    python tests/run_rin_poison.py            # stale memory dies under ASan; accepts stay clean
    RIN_UNSAFE=1 python tests/run_rin_torture.py --cc clang   # 9/9, the old compiler suite
    RIN_UNSAFE=1 python tests/run_rin_execute.py --cc clang   # 23/23
    # in rin-dod, with RIN pointing at the fresh build:
    RIN=<path>/rin RIN_UNSAFE=1 CC=gcc python tools/check.py  # 70 lessons, 26 dojo blocks
    RIN=<path>/rin CC=gcc python tools/check.py --sanitize safe

The accept programs and the ported lessons must also run clean under
`-fsanitize=address,undefined`. On Windows, clang's sanitizers work (`CC=clang`);
tools/check.py finds the runtime.

Do not run a new attack program on this machine: `python tests/run_rin_vm.py [program.rin ...]`
runs it in the `archlinux` WSL2 distro, with no window (the guest needs clang, compiler-rt, python,
a user with uid 1500, and an `/etc/wsl.conf` turning off automount and interop). `--sandbox` runs it
in a Windows Sandbox instead, which opens a window; ask before using that.
`rin check` never executes anything. One of them, opening a terabyte arena per
level of a recursion, crashed Windows twice (bugcheck 0x1A, subtype 0x3453, in
`MiDeleteFinalPageTables`), which is why a local region now reserves 4 GiB. Keep
agents to a few at a time, and commit often.

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
`unsafe { }` core, kept small with honest signatures; every other proc is
unannotated and checked. Report every remaining `unsafe { }` and why.

## State of play

Done: the safe compiler, including `unsafe { }` in place of `[trusted]`, local regions
(`tmp: region of r = { }`), field-path narrowing, `str`, `must(p)`, `std/file.rin`,
invariance through pointers, and four attack rounds; the whole course ported; the
engine ported on its `regions` branch (see njinn's `docs/rin-safe-port.md` for where
that stands).

Next, in the order that pays:

1. Narrow std's `unsafe { }` bodies: the migration wrapped each old `[trusted]` body
   whole, and 264 still are, most of them generic. The count of lines inside them is the honest measure of the trusted base.
2. A stranger's attack round, in a VM, and the soundness argument written out:
   regions and their invariance, bounds, null, the sealed raw boundary. Staleness is
   a lint on top of that: memory in a region stays valid when a `vec` moves.
3. `stable<T>` and `handle<T>` in std; `std/file.rin` is the pattern for the second.
4. A region-end cleanup hook for GPU buffers and files, only if the engine shows the
   manual version going wrong. A copy of a COM pointer can go stale after Release.
5. Known gaps: programs `rin check` accepts and C then refuses, so no binary exists (a `main` with a wrong
   signature, a negative array length, returning an array: `run_rin_vm.py` lists them), a `printfmt` brace escape, a C name collision on a struct named
   `system`, pointer-to-array element codegen, threads, which safe code has no way to
   start.

The claim, and exactly what it excludes, is in `rin/docs/soundness.md`. Seven attack rounds have run. Rounds five to
seven each found dozens of holes in safe code and all are closed, but they have not slowed down, so expect more.
Before calling it safe: a fresh round per new rule, `tests/gen_rin_safe.py` with `run_rin_vm.py --compare`, and strangers.
