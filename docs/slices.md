# Slices

> **Implemented and in use.** `std/slice.rin` ships, and njinn was converted to
> it -- 20 signatures and fields, 0 uses to 73. This document records what a
> slice is for, the API convention it enables, the ergonomic gaps found by
> actually doing the conversion, and the open question of whether it should
> become a language feature.
>
> Companion: [strings.md](strings.md), where `string8slice` is the same idea
> specialised to bytes.

## The type

    slice: struct<T> = {
        data: *T;
        length: u64;
    }

A pointer and a length, monomorphised per `T`. Nothing else. It owns nothing,
allocates nothing, and cannot grow.

The library around it is small and free-standing: `from_parts`, `from_array`,
`from_vec`, `at`, `get`, `first`, `last`, `sub`, `skip`, `take`, `equals`,
`is_empty`, `copy_to`, `to_array`. Calls are spelled `slice<T>at(s, i)`, not
`s.at(i)` -- generic procs are not methods.

## The convention

This is the rule the type exists to make expressible:

| the proc needs to | it takes |
|---|---|
| append, remove, resize | `*Vec<T>` or `*Array<T>` |
| read and write elements, no resizing | `slice<T>` |
| read only | `slice<const T>` |

It is not novel. Rust reaches `&mut Vec<T>` / `&mut [T]` / `&[T]` and Zig
reaches `*ArrayList(T)` / `[]T` / `[]const T`. Two communities converged on it
because a signature then states what the function does to the caller's data,
which an owner-struct pointer never can.

**`slice<const T>` is enforced.** Writing through one is a compile error:

    cannot assign to const target of type 'const ...'

That matters, because the convention is only worth following if the read-only
half is checked rather than documented.

## Three sharp edges

Found by converting njinn, all still present.

### `*const slice<T>` is not the read-only spelling

    f: proc(s: *const slice<i32>) -> void = { s[0].data[0] = 99; }   // accepted

`const` on the wrapper stops you reassigning `.data`; it says nothing about the
pointee. This is correct C semantics inherited honestly, and it is exactly the
mistake a reader will make. **`slice<const T>` is the one that bites.**

### Mutable does not convert to const

    sum(cast(s, slice<const ent>))    // rejected: distinct struct types

`slice<T>` and `slice<const T>` are unrelated monomorphisations, so widening a
mutable view to a read-only one means rebuilding it:

    sum(slice<const ent>from_parts(cast(s.data, *const ent), s.length))

Every language with slices treats mutable-to-const as a free widening. rin does
not, and the workaround is verbose enough that it discourages using the const
form at all -- which defeats the convention.

### A fixed array does not know its own length

There is no array-to-slice coercion, and no reflection path to the count:

    value<g_snd>.count       // parse error
    r := Type<[3]u32>; r.count   // parse error
    sizeof(g_snd) / sizeof(u32)  // works -- the C idiom

So constructing a slice over `[256]gops_projectile_debug` means writing
`sizeof(x) / sizeof(x[0])` by hand. It works and it is better than a literal,
but the compiler already knows the answer and is not sharing it.

(Enum counts *do* work, via a different spelling: `gops_character_kind<>.count`.)

## What the njinn conversion actually found

Twenty conversions: 3 accessor pairs returning a slice, 12 parameters, 3 struct
fields, 2 output buffers. Build clean, 11/11 selftests.

The unplanned win was larger than the planned one. Twelve hand-written length
literals disappeared, because constructing a slice put the length next to the
array it describes:

    gops_play_random_sound(g_gops_hero_melee_sounds[0].&, 3)
    fxed_dropdown_u32(..., g_fxed_effect_kind_labels, 6, ...)

Both correct on the day they were written, both silently wrong the moment
someone adds a fourth sound or a seventh label. That is the one place slices
fixed a live bug class rather than tidying types.

### What did not convert, and why

Worth recording, because each is a rule rather than a one-off:

  * **`struct[external]`** -- `guiops_atlas.glyphs + glyph_count` mirrors a C
    layout. Fields in an external struct are not ours to change. This also
    accounts for 50 of njinn's 56 pointer+count struct pairs, all in the cgltf
    bindings.
  * **Pointer + count + *capacity*** -- `guiops_drawlist` has `vtx`, `vtx_count`
    *and* `vtx_cap`. That is a Vec. Replacing it with a slice would delete the
    capacity and break appending. A count alone means a view; a count beside a
    capacity means a container.
  * **Unrelated arguments that look like a pair** --
    `fxops_particles_spawn(desc, source, count)` where `count` is how many
    particles to spawn, not the length of `source`. And
    `gin_ensure_instance_buffer(dx, count)`. Any regex over signatures will find
    these; only reading the body rules them out.
  * **`[external]` C functions** -- `memcpy`, `memmove`, `memcmp`, `fgets`,
    `jsmn_parse`. The C ABI owns those signatures.

## Where slices do not help, and why

This is the part worth remembering, because it is counter-intuitive and it was
measured rather than guessed.

njinn is 1077 procs. **76% touch no global state at all** -- they are already
plain functions over values. Of the rest, 138 reach a global world struct
directly and **3** take a world-ish parameter. `g_world.` appears 682 times,
`g_fx_world.` 411.

So there was never a world-pointer parameter for slices to replace. And the read
sites -- 199 indexed accesses through `.data[i]` / `.at(i)` -- almost all happen
in the same scope as the container they index. A slice earns its place in two
situations only:

  1. data crosses a boundary without its length attached, or
  2. you take a sub-window of a container

njinn does the first about thirty times. It does the second **zero** times --
there is no pointer arithmetic on any container `.data` field anywhere in the
codebase.

**Containers that live in globals never get passed, so they never lose their
length.** That is the whole reason the conversion was twenty edits and not two
hundred.

### The refactor deliberately not done

The obvious next step -- give the 87 procs that touch exactly one global field a
parameter instead -- was considered and rejected. The distribution is why:

| global fields a proc touches | procs |
|---|---|
| 1 | 87 |
| 2 | 42 |
| 3-6 | 51 |
| 11-30 | 16 |

Passing state only beats reaching for it when the parameter is *smaller* than
the global. At one or two fields it is. At thirty it is not, and a `*world`
parameter is then a global with paperwork -- the same coupling, more typing, and
a wider call-site blast radius every time the proc needs one more field.

The genuine benefit of passing state is testability, and njinn's whole test
surface is 12 selftests. The trigger to revisit is **a job system**: the day
skinning or particle simulation moves to worker threads, globals stop being a
style question, and the systems that would parallelise are exactly the
single-field leaves. Doing the leaf conversion then is forced anyway; doing it
now is paying for a benefit that has not arrived.

## Should slices become a language feature?

Probably, eventually -- but **`[]T` syntax is the smaller half of the win, and
the wrong half to do first.**

Nothing in the njinn conversion hurt because of angle brackets. It hurt because
of this:

    slice<const jsmntok_t>from_parts(cast(tok[0].&, *const jsmntok_t), cast(tok_count, u64))

Three separate ceremonies in one expression -- a pointer cast for constness, an
integer cast for the length, and an explicit constructor for something the
compiler can see. The three sharp edges above are all of that.

So the recommended order is:

1. **Fixed array to slice coercion.** The compiler knows `[256]u32` has 256
   elements; making the programmer write `sizeof(x) / sizeof(x[0])` is
   withholding information it already has.
2. **`slice<T>` widening to `slice<const T>`.** Free in every other language
   with slices, and its absence actively discourages the const form.
3. **Subrange syntax**, `s[a..b]` for `slice<T>sub(s, a, b)`.

Items 1 and 2 need **no new syntax at all**. They are compiler-known conversions
on the existing generic type, and they remove most of the verbosity. `[]T` sugar
can follow later, decided with real usage behind it rather than in the abstract.

### The question an intrinsic forces

**Bounds checking.** The generic version ducks it, because `s.data[i]` is
visibly raw pointer indexing and nobody expects it to be checked. Make `[]T` a
language feature with `s[i]` and people will reasonably expect a check --
and "checked sometimes" is worse than either answer.

That is a bigger question for rin's identity than the syntax is, and it should
be settled before any of this starts. It is the same question `safety.md`
circles: rin is C with the ergonomics fixed, and a bounds check is the first
place the generated C would do something the source does not literally say.

## Relationship to `string8slice`

`string8slice` predates `slice<T>` and is the same shape specialised to bytes:

    string8slice: struct = { data: *u8; length: u64; }

It is deliberately *not* being replaced by `slice<u8>`, because it carries
string-specific operations that a generic slice should not. What it should adopt
is the const distinction, now that `slice<const T>` proves the shape works. See
[strings.md](strings.md), open decision 1.

## See also

- [`strings.md`](strings.md) -- `string8slice`, and why `fmt` should be built on
  a `slice<c8>`.
- [`safety.md`](safety.md) -- the bounds-checking question an intrinsic forces.
- [`stranger-with-generics.md`](stranger-with-generics.md) -- how `slice<T>`
  monomorphises, and the call syntax for generic procs.
