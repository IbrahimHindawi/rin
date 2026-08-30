# `build.rin`: declaration or orchestrator?

> **Decided: it stays a declaration. bunyan keeps the orchestration, for now.**
> This records the question, the measurement behind the answer, and the exact
> conditions under which it should be revisited -- so the next person to have
> the idea does not start from scratch.
>
> Companion: [build-command.md](build-command.md) for what `rin build` does
> today and every global `build.rin` understands. This document is only about
> where the *line* sits.

## The question

`build.rin` currently describes a build. `bunyan.py` currently runs one. The
proposal was to collapse the second into the first -- let `build.rin` drive the
whole pipeline and delete the per-project Python.

It is a reasonable instinct. Every project carrying its own `bunyan.py` with its
own hooks is exactly the fragmentation `build.rin` was introduced to end.

## What bunyan is actually doing

Measured on njinn, which is the most demanding of the three projects. Three
distinguishable jobs:

  1. **Orchestration.** Run `res_meta_gen.py`, then `bindgen_cgltf.py` (which
     itself shells out to `rinbind.exe`), then configure, then build. This is
     sequencing: a list of commands with inputs and outputs and a staleness
     rule.
  2. **The asset pipeline.** `stage.py` and `resources_inject.py` -- cooking
     textures, resizing images against `--max-tex-width`, staging 1,230 files
     and injecting 1,520. Real image processing.
  3. **Invocation.** The cmake and ninja calls. `build.rin` already owns this.

Only the first is a candidate. The third is done. The second is not a build
config problem wearing a disguise; it is an image pipeline, and Python is
legitimately good at it.

## The decision

**Keep bunyan. `build.rin` stays declarative.**

Two reasons, in order of weight.

**Build configs that gain control flow become build programs.** The moment
`build.rin` needs an `if`, or a loop, or a variable derived from another
variable, it has stopped describing a build and started being one -- and every
build language in history has become the worst-loved part of its ecosystem. The
value of `build.rin` today is that you can read one in fifteen seconds and know
what it does. njinn's is 25 lines. That property is worth more than deleting a
Python file.

**The interesting half would not move anyway.** Even a fully orchestrating
`build.rin` would still shell out to `stage.py` and `resources_inject.py`,
because rewriting image cooking in rin is a project with no payoff. So the
realistic end state is "`build.rin` names some scripts" -- which is a smaller
change than it sounds, and does not justify making the format executable.

## What would move, if this is revisited

Not "make `build.rin` a program". Add one declarative feature: **pre-build
steps**, as data.

    build_steps: [N]build_step = { ... }   // command, inputs, outputs

A command, the files it reads, the files it writes. The build runner skips a
step whose outputs are newer than its inputs. That is enough to express
everything bunyan's orchestration does for njinn, and it stays a declaration --
the runner has the logic, the config has the facts.

That would delete per-project `bunyan.py` without giving `build.rin` an
interpreter. If this is ever built, that is the shape.

## The line

**`build.rin` describes. Scripts do work.**

If a project needs conditional logic, that logic goes in a script that
`build.rin` names. One indirection, and the config stays readable. The test is
simple: if you cannot answer "what does this build?" by reading the file top to
bottom without evaluating anything, the line has been crossed.

## A note on an earlier position

[`rin-build-story.md`](rin-build-story.md) contains a section arguing the
opposite -- "Why the build script should be a program, not a manifest". That was
written before `rin build` existed, when the alternative to a program was a
hand-copied CMake contract that lived nowhere. The manifest turned out to be
sufficient for all three projects, so the argument is superseded rather than
wrong. Leaving both on record because the reasoning there still applies if the
declarative form ever runs out of room.

## One thing that surprised us, worth knowing here

The compiler emits **one `.c` and one `.h` per program**, not per module. There
is no path through `rin` that splits `gops.rin` and `gin.rin` into separate
translation units; `--no-header` (which njinn sets) only suppresses the single
companion header. Everything else written to disk is a monomorphised generic
header.

This matters for build config because it removes a whole category of question --
there is no object-file graph to describe, no per-module dependency tracking, no
incremental C compilation to arrange. A rin build is: transpile once, hand one
`.c` to the C compiler. That simplicity is part of why the declarative form has
been sufficient.

## See also

- [`build-command.md`](build-command.md) -- the command, and every `build.rin`
  global.
- [`rin-build-story.md`](rin-build-story.md) -- how the build got here, and the
  superseded argument for an executable build script.
- [`rin-self-hosting.md`](rin-self-hosting.md) -- what the toolchain is expected
  to do on its own.
