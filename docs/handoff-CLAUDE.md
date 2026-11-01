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
can miss return `option<*T>`; the frame arena is `frame: region in level;` opened
once per frame; the type-erased byte column is the one legitimate
`unsafe { }` core, kept small with honest signatures; every other proc is
unannotated and checked. Report every remaining `unsafe { }` and why.

## State of play

Safe rin targets one way of programming, the arena patterns of working engine code, and anything else is
`unsafe` or cannot be written; `rin/docs/soundness.md` says which patterns, and `rin-safe.md` shows the syntax.
`gin` is the evidence for the patterns, not a spec: judge a feature by whether a pattern needs it, and prefer
removing it to defending it. No RAII, destructors, ownership, borrow checker or methods.

Done: fail-closed checking (no type is an error outside a program's own `cinclude`, and the emitter takes only what
the checker signed off), constants computed by rin, `option<*T>` and `unwrap`, `level: region global;` and
`reset level;`, `scratch { }`, field regions worked out from what is stored, `table<T>` and `index<T>`, region-end
cleanup, no `#define` numbers, no VLAs, no program unions, `noalias` only in `unsafe`. The engine runs on a real
level region, and its level meshes are released by the reset.

Next, in the order that pays:

1. Narrow std's `unsafe { }` bodies. `python tests/rin_trusted_lines.py` gives the honest measure: the lines of it safe code can reach.
2. Strangers, in a VM. The crash-containment test is off the list, by the user's decision.
3. Threads, last.

Twenty attack rounds have run. From round five on each found dozens of holes in safe code, and all are closed; the rounds are not slowing down.
Run a fresh round per new rule, and `tests/gen_rin_safe.py` with `run_rin_vm.py --compare`. Verify with a script
that fails on a missing line or a skipped program: a filtered log hid a failing suite here twice.
