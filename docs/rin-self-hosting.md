# Self-hosting: what it would mean, and what it would cost

> **Not started, and not scheduled.** This exists so the question has a written
> answer instead of being re-argued. It records what self-hosting would actually
> require, what already works, and the reasons to be in no hurry.

## What the word means here

rin compiles to C. A self-hosted rin compiler would be **a compiler written in
rin that emits C** -- not a compiler that emits machine code. The backend does
not change; only the language the compiler itself is written in.

That distinction matters, because it removes the usual argument for
self-hosting. There is no "the compiler proves the language can do systems work
end to end", since the C compiler is still doing the code generation either way.
What is left is a narrower claim: the language is expressive enough to write a
16,000-line program in, and its authors have to live in it daily.

## Where things stand

| | |
|---|---|
| `src/main.c` | **16,810 lines of C** -- the whole compiler |
| `src/main.rin` | a demo program, ~60 lines. **Not a port.** |
| `src/std` | 3,040 lines of rin across 21 modules |
| full recompile of the compiler | ~700 ms |
| test suite | 329 checks, 27 of them whole programs |

Nothing has been ported. `src/main.rin` exercises arenas, generics and
reflection; it is a smoke test that happens to sit next to the compiler, and it
is occasionally mistaken for a beginning.

## The language is not the obstacle

Every construct the compiler leans on already works. Checked, not assumed:

| construct | rin |
|---|---|
| `union` | yes |
| recursion | yes |
| proc pointers in structs | yes |
| nested generics (`Vec<Vec<T>>`) | yes |
| varargs in a declaration | yes |
| tagged structs, enums, arrays of them | yes |
| `goto` and labels | **no** |
| reading varargs (`va_list` / `va_arg`) | **no** |

Both gaps are one use each in `main.c`, and both are avoidable -- the `goto` is a
single cleanup jump, and the `va_list` is one diagnostic wrapper that `printfmt`
already replaces. Neither is a reason to add a feature.

The containers are there too: `Vec`, `Array`, `Map`, `string8`, `slice<T>`, the
arena, and since the string work, `fmt` and `cstr`. `main.c` uses `Vec` heavily
and little else that std lacks.

So the honest position is that **rin could express this compiler today**. What
stops it is not capability.

## What actually stops it

**1. It is a rewrite of 16,810 lines with no user-visible benefit.**
Every line ported is a line that already works, tested by 329 checks that would
have to keep passing throughout. The result compiles the same programs to the
same C. A user cannot tell the difference.

**2. The bootstrap has to be maintained forever.**
Building the rin compiler would require a rin compiler. That means either
keeping the C compiler alive as stage zero -- two implementations to keep in
step -- or checking in generated C, which is the same thing with worse
ergonomics. Neither cost ever goes away.

**3. The compiler is the one program that cannot afford to be slow.**
A full rebuild is ~700 ms today and njinn transpiles in ~516 ms. Those numbers
are the budget. A self-hosted compiler that is twice as slow makes every project
that uses it twice as slow to build, and the arena discipline that makes rin
programs fast is not automatically what a compiler wants -- `main.c` allocates
per-token and per-node and never frees, which suits an arena well, but that is a
thing to verify rather than assume.

**4. Debugging gets one layer deeper.**
Today a compiler bug is debugged in C with a normal debugger. Self-hosted, it is
debugged in rin, through rin's own `#line` mapping, using the compiler that has
the bug. That is survivable -- the PDB line tables are good -- but it is worse
than what exists.

## What would make it worth doing

Not the usual reasons. Specifically:

  * **When the compiler stops changing shape.** Porting a design that is still
    moving means porting it twice. The error model, the module question and the
    `defer` decision are all still open.
  * **When rin has something C does not that the compiler wants.** The obvious
    candidate is reflection: the compiler hand-writes a great deal of table
    emission that `<>` describes natively. That is a real argument, and it is
    the only one here that gets stronger over time.
  * **When the std is load-bearing enough that the compiler using it keeps it
    honest.** Today njinn is that pressure. A self-hosted compiler would be a
    second, very different consumer -- and the one most likely to find the sharp
    edges in `Map`, `string8` and the arena.

## What to do instead, for now

If the goal is to prove rin can carry something the size of a compiler, njinn
already does: 25,180 lines of rin, a D3D11 renderer, zero `string.h`, and the
whole thing builds from a 25-line `build.rin`. That is the same claim at the
same scale, without a bootstrap to maintain.

If the goal is to make the compiler nicer to work on, the reflection argument
above is the one to pull on -- and it can be tested in a corner, by porting one
self-contained pass to rin and calling it from C, long before anything commits
to a full port.

## The one thing to avoid

Half a port. A compiler split across two languages, with the C half still
authoritative, is the worst of both: two build paths, two debugging stories, and
a boundary that has to be crossed by hand. If it is ever started, it should be
started with a plan to finish -- and this document exists mostly to argue that
the plan is not needed yet.
