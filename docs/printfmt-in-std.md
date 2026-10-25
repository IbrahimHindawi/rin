# printfmt in std

*Status: an idea, parked. Nothing here is implemented.*

`printfmt` is a compiler builtin. The aim would be to make it an ordinary std
proc, so formatting changes stop being compiler changes.

## Recommendation

**Leave the builtin alone for now.** It is one rewrite pass that works, it
already hands the formatting itself to std's `print` specialisations, and it
checks the `{}` count at compile time. Replacing it costs more machinery than it
removes.

**When it does move, use a tagged array, with the reflection table as the
tag**, not a generic argument pack. Concretely: an `any` that pairs a pointer
with the type's reflection table, one general `..any` parameter rule that packs
the arguments, a std formatter that walks them, and a lint that keeps counting
`{}` in literal format strings so the compile-time check survives as a warning.
Skip the generic pack entirely: it is the C++/Zig road and the least C-like.

**The trigger is a second user of the packing rule**, such as a string builder
or a logger that wants the same `{}` syntax. At that point `..any` pays for
itself, and moving `printfmt` onto it is the cheap part.

## What the builtin does today

`printfmt` is statement-only. A pass before emission splits the format literal
at each `{}` and rewrites the statement into a run of calls:

    printfmt("hp {} name {}\n", health, name);

becomes

    print_cstr("hp ");
    print<i32>(health);
    print_cstr(" name ");
    print<[*]const char>(name);
    print_cstr("\n");

So the formatting itself already lives in std, in the `print` specialisations
of `print.rin`. What the compiler owns is the part a library can't do without
new language machinery: taking any number of arguments of any types, and
choosing a `print` for each one by its static type. It also counts the `{}`
against the arguments at compile time.

That is a small builtin. Moving it out buys something only if the machinery that
replaces it is useful beyond printing.

## Two ways a library could take the arguments

**A generic argument pack.** The types stay known at compile time, and the
formatter is specialised for each combination of argument types, as C++
variadic templates or Zig's comptime tuples do. This is the most compiler
machinery, generates the most code, and is the least C-like. Not this.

**A tagged array.** Each argument is converted to one common type carrying a
tag, and one shared formatter loops over them and switches on the tag. The
dispatch happens at run time, which costs nothing that matters next to I/O.
This is the simpler starting point.

## The tagged array, with reflection as the tag

rin already generates a reflection table for every type, so the tag needn't be
a new enum. It can be a pointer to that table, the way Odin's `any` works:

    any: struct = {
        data: *void;
        type: *const reflect;
    }

The formatter switches on the reflected kind: integer, float, string, enum,
struct, slice, pointer. That gives more than today's builtin:

- **Structs, enums and slices print with no code.** Fields come from the
  reflection table, enum member names too.
- **Custom formatting is one field away.** If a type defines a `format`
  specialisation, the compiler can store its address in that type's reflection
  table, and the formatter calls it when present. The tables are already
  generated at compile time, so this is an extra field rather than a new
  mechanism.

## The one language rule it needs

Something still has to turn `printfmt("{} {}", health, name)` into an array of
`any`. That would be a typed variadic parameter, for example a final
`args: ..any`: each remaining argument is stored in a temporary, and the callee
receives `{ temp&, T<> }` for each.

That rule is not about formatting, so it would serve logging, string builders,
`assert` messages and the like as well. It needs its own spelling, because
`...` already means C varargs on external procs, and it should stay as plain as
possible: no comptime syntax.

## What it costs

- **Format checking moves to run time.** Today a wrong number of `{}` is a
  compile error. A library formatter can only report it while running. A lint
  can keep the safety net: when the format argument is a string literal, the
  compiler counts `{}` and warns. That is a diagnostic, not code generation, so
  it does not pull formatting back into the compiler.
- **Every argument becomes an addressable temporary.** Invisible at the call
  site, and free in practice.
- **Every printed type needs a reflection table.** That is the case today; it
  would matter only if reflection ever became opt-in per type.

## If it goes ahead

1. `any`, and a std formatter that walks one through its reflection table.
2. The `..any` packing rule.
3. `printfmt` as an ordinary std proc, and the builtin deleted (the rewrite
   pass, `call_name_is_builtin`, `printfmt_spec_for_type`).
4. The literal-format lint.
5. Later, the `format` field in the reflection table.

## Why it is parked

It is more machinery than the builtin it replaces: a new type, a new parameter
form, a lint, and a reflection-table field, against one rewrite pass that works.
It becomes worth doing when a second user of the packing rule shows up, such as
a string builder or a logger that wants the same `{}` syntax.
