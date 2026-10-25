# `*T` for C's Pointer

> **Deprecated 2026-10-25.** rin keeps its current pointer spelling (`*T`, `[*]T`, `[]T`);
> this rewrite was dropped. Only rasm uses a plain `*T`, and that is rasm's own rule.
> Kept as a record of the discussion.

*Status: a proposal, discussed 2026-10-23. Nothing here is implemented.*

Today rin spells its pointers as Zig does (see
[pointers-and-slices.md](pointers-and-slices.md)):

    *T    one element      only [0]
    [*]T  raw pointer      unchecked, C's T*
    []T   slice            checked, pointer + length

The proposal is to give `*` back to C:

    *T    raw pointer      unchecked, exactly C's T*
    &T    one element      only [0]
    []T   slice            checked, pointer + length

and to retire `[*]T`.

## Why

The goal is to stay as close to C as possible and make interop trivial: every C
signature should be writable in rin as it reads, cursed parts included.

- **LeetCode's C signatures** carry over as written. `int* nums, int numsSize`
  is `nums: *i32, numsSize: i32`, not `nums: [*]i32`.
- **Every C header** does the same. `rinbind` maps `T*` to `*T` directly, and
  hand-written `[external]` declarations read like the header they copy.
- **`&T` pairs with postfix `&`.** `x&` takes an address, and its type is `&T`:
  the same symbol makes a reference and names its type.

## Against

- **The short spelling becomes the unchecked one.** People write the shortest
  form by default. Today that is `*T`, where indexing past `[0]` is an error;
  after the swap it is the raw pointer, with no checks. That is honest C
  behaviour, but rin loses its nudge towards the checked form.
- **Churn.** Every `arena: *memops_arena`, every `m: *hashmap<K, V>`, every
  out-parameter in std, njinn and the lessons becomes `&`, right after the last
  pointer migration (about 900 njinn sites).
- **`&T` invites C++ and Rust expectations**: automatic dereference and
  non-null. rin's would be neither. It is a plain pointer, dereferenced with
  `[0]` like everything else, and nullable as in C. That has to be said up
  front.
- **`&&T` lexes as `&&`.** A pointer to a pointer collides with the logical-and
  token. Rust has the same collision and splits the token in type position, so
  it is solvable.

## `&T` or `^T`

`&T`. `^` is already XOR in rin, and `^T` reads as Pascal or Odin rather than
C. `&` already means "address of" in rin and "reference" to nearly everyone.

## Migrating without silent changes

Swapping meanings is dangerous in one specific way: old code does not break, it
quietly changes meaning, and every unconverted `*T` becomes a raw pointer with
weaker checks. So it goes in three steps, with the compiler as the checklist:

1. Add `&T` as the one-element pointer, and make bare `*T` an error whose
   message says "write `&T` for one element or `[*]T` for C's pointer".
2. Convert std, njinn, the lessons, rin-leet and the site until everything
   compiles.
3. Only then let `*T` mean the raw pointer, and turn `[*]T` into an error that
   says "write `*T`".

At no point does a program compile with a meaning its author didn't choose.

## Raw at the boundary, `&T` and `[]T` inside

The second half of the idea: raw pointers live only where rin meets C, and
everything else uses `&T` and `[]T`, the way Rust confines raw pointers to
`unsafe`.

Most of this already exists. The conversions out of a raw pointer are explicit
today and need no new operators:

    p[0]&        // raw -> one element: say which element
    p[0..n]      // raw -> slice: vouch for the length

A LeetCode solution is already written this way: it takes `nums: *i32`, makes
`xs: []i32 = nums[0..numsSize]` on its first line, and works on the slice from
then on.

What would make it a rule rather than a habit is deciding where raw pointers
may be used freely, meaning indexed past `[0]`, offset with arithmetic, or cast:

- **Anywhere** (C's rule, today's rule). Nothing to build.
- **Only in boundary code**: `[external]` declarations, plus procs marked as
  touching C, for example `proc[c]`. Using a raw pointer elsewhere is a
  diagnostic. This is the Rust `unsafe` shape.

A boundary marker only changes what the checker accepts, not how anything is
lowered. That bends the rule in [attributes.md](attributes.md) that an attribute
may change lowering but never meaning. Whether a checking-only attribute counts
as changing meaning would need settling first.

## Recommendation

**Do the swap**, if C interop is the priority, and it is: `*T` for C's pointer,
`&T` for one element, `[]T` for many, `[*]T` retired. Do it only through the
three-step migration above, so nothing changes meaning silently. And do it
soon, before more rin code exists to convert.

**Keep raw pointers usable anywhere, and make "convert at the edge" a
convention first.** A hard Rust-style wall works against "express all the
cursed C in rin". The explicit conversions (`p[0]&`, `p[0..n]`) already make the
edge visible. If the convention turns out to need teeth, add an opt-in warning
for raw-pointer indexing and arithmetic outside `[external]` and boundary-marked
procs: off by default, so rin stays C, and on for code that wants to stay
`&T`/`[]T` pure, such as std.

**Keep the rest as it is**: dereference stays `[0]` with no auto-deref, and
`&T` stays nullable, like every C pointer.
