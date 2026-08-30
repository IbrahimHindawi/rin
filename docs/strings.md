# Strings

> **Partly implemented.** `string8_builder` is in and tested. The plan to get C
> strings out of rin is measured but parked. This document merges what was
> previously split across `string.md` and `strings-and-slices.md`: what each
> string type is for, the size of the C string problem, and the decisions still
> open.
>
> Companions: [slices.md](slices.md) for the general `slice<T>` design, which
> settles one of the questions this document used to leave open;
> [clib-removal.md](clib-removal.md) for the libc dependency inside
> `string8.rin` itself.

## Four shapes, not two

| shape | owns | grows | NUL-terminated | job |
|---|---|---|---|---|
| `string8` | arena bytes | machinery exists | yes, in practice | owned, finished string |
| `string8slice` | nothing | no | no | a view of bytes |
| `string8_builder` | the arena head | by pushing | on `finish` | constructing |
| `[64]c8` in a struct | the struct does | no | caller's business | long-lived mutable |

The fourth is not a library type. It is a fixed array declared inline, and it is
how both engines answer "a string that lives as long as this struct and changes
occasionally" -- gin has 140, njinn 143. It costs no allocation and asks no
lifetime question.

## What `string8` is actually for

`string8` looks like a growable buffer. It is not used as one. Outside its own
implementation in `std/string8.rin`, the growth machinery is untouched:

| | `string8_append*` | `string8_grow` | `string8_reserve` |
|---|---|---|---|
| njinn | 0 | 0 | 4 |
| rin-learn | 0 | 0 | 0 |
| rin-playground | 0 | 0 | 0 |

njinn holds six `string8` values. Every one is produced by a proc that returns a
finished string -- `res_path`, `res_read_file` -- then read. None is ever
extended. Against 23 `string8slice` uses.

So the capacity field and the doubling path serve a use that is not happening.
What `string8` genuinely provides is something else: **it owns arena bytes and
it is NUL-terminated.** `string8_reserve` allocates `capacity + 1`, and every
append writes `data[length] = 0`. That is why a `string8` can be handed to a C
`%s` with no conversion, and it is worth keeping.

## The model

  * **`string8slice`** -- a view. Owns nothing, guarantees nothing about a
    terminator, and sub-slicing is free. This is the default for anything that
    reads.
  * **`string8`** -- an owned, finished, NUL-terminated string over arena bytes.
    Treat it as immutable once made.
  * **`string8_builder`** -- the way to make one. Zero-copy.

`string8_grow` and `string8_append_*` stay, but they are the fallback for the
case the builder cannot serve, not the normal path. Reaching for them is a
signal that a builder was wanted.

## Why the builder replaced growing

`string8_grow` calls `memops_arena_realloc_`, which always pushes a fresh block
and memcpys into it. Building an N-byte string by appending therefore copies
about N bytes through the doubling chain and abandons every earlier block.

In a scratch arena the abandoned bytes cost nothing, because the whole arena is
reset. The copying is real.

The builder avoids both. If nothing else allocates while a build is open, the
string *is* the arena head, so there is no difference between the string's
capacity and the free space in front of the cursor. Appending is a push:

    scratch: grew 11 bytes for an 11 byte string

Eleven bytes of arena for an eleven-byte string. `string8_append` would have
reported sixteen or more, having doubled. No capacity field, no realloc, no
copy. The single copy left is `string8_builder_commit`, moving the finished
string from scratch into permanent -- which is the copy the two-arena pattern
wanted anyway.

### The one rule

**One *open* builder per arena.** Finished strings coexist in a scratch arena
without trouble: build one, take its slice, build the next, and they sit side by
side. What breaks is appending to a builder after a second one opened, because
the push lands past the other's bytes and the length then spans memory the
builder does not own:

    SEQUENTIAL -- finish one, then start the next:
      a = 'hello world' (len 11)
      c = 'second' (len 6)

    INTERLEAVED -- append to one after another started:
      d = 'AAAB' (len 4)        <- picked up e's first byte
      e = 'BBB' (len 3)

`d` came out a plausible string of the wrong contents rather than crashing,
which is why every append checks `start + length == cursor` and aborts:

    rin runtime: string8 builder lost the arena head (interleaved allocation)

When two builds genuinely have to overlap, give the inner one its own
`memops_arena_temp` scope. A silent fallback to copying was considered and
rejected: it would keep working while quietly costing exactly what the builder
exists to avoid, and nobody would ever find out.

## The C string problem

The good types exist. The codebase does not use them. In njinn:

| | count |
|---|---|
| `*c8` / `*const c8` in signatures and locals (spelled `char` in the source) | 536 |
| `string8` / `string8slice` mentions | 91 |
| `strcmp` | 167 |
| `snprintf` | 51 |
| `strlen` | 26 |
| `strncpy` | 17 |
| `strtok` | 11 |
| `strcpy` | 6 |
| `strncmp` | 6 |
| `strchr` / `strrchr` / `strstr` | 8 |
| `sscanf` | 5 |

Six to one against.

### Why it stayed that way

`string8.data` is `*u8`. Every C string function wants `*const char`. The two are
different types and rin correctly refuses to interconvert them without a cast
(see [`type-safety.md`](type-safety.md)), so every crossing costs a `cast` --
and once a value has been cast to `*const c8` to call `strcmp`, keeping it that
way is the path of least resistance. The friction points the wrong way.

`strcmp` at 167 uses is not 167 hard problems; it is one easy problem 167 times.
`string8slice_equals` already exists and already does the job. What is missing is
that the values being compared are `*const c8` by the time they reach it.

### The target

1. **`string8slice` is the parameter type.** Anything that takes a string and
   does not own it takes a slice. This is where the 167 `strcmp`s go.
2. **`string8` is the owned type.** Anything that builds a string builds one.
3. **`*const c8` survives only at the C boundary**, in `external` declarations,
   with explicit one-directional-per-call conversion.
4. **A literal is a `string8slice`.** Right now a string literal is a
   `*const c8`, which is what makes step 1 awkward -- every call site would need
   a wrapper. Changing what a literal *is* makes the rest fall out, and it is
   also the one that touches everything, which is why this is parked rather than
   half-done.
5. **`snprintf` is replaced, not wrapped.** 51 uses, and the one that most wants
   a real answer.

### Sequencing

The literal type (step 4) gates everything else and has a very wide blast radius
across njinn. Nothing before it is wasted, though -- migrating `strcmp` to
`string8slice_equals` at sites that already hold a slice is independently
correct and shrinks the eventual diff.

## The bounded writer

`std/fmt.rin` answers step 5. It is a writer, not a format string:

    fmt: struct = { data: *c8; capacity: u64; length: u64; truncated: b32; }

njinn's 16 remaining `out: *char, out_cap: usize` procs are the migration
target. Notably njinn already calls the *convenience wrappers* -- `fmt_cat3`
appears in `fxops.rin` -- while never using the writer as a value that
accumulates. Converting those 16 collapses three things at once: the capacity
parameter, the caller's separate buffer, and the `b32` return that means "did it
fit", which `fmt` already carries as `.truncated`.

**`fmt` should be rebuilt on top of `slice<c8>`.** The struct above is a pointer
and a capacity, which is a slice with the parts written out longhand. The honest
decomposition is a destination slice plus a cursor plus a truncation flag, which
makes `fmt_to` take one argument instead of two and removes the chance that a
caller's pointer and capacity disagree at construction. The matching change on
the way out is `fmt_done` returning `slice<const c8>` -- the exact bytes written,
no `strlen` -- rather than a NUL-terminated pointer. See [slices.md](slices.md).

This also settles a sequencing question: **the 16 procs should not get a slice
pass first.** Once they take a `*fmt`, the slice is inside it. Going straight to
`fmt` skips a step that would have been thrown away.

The risk is worth stating plainly. Almost all 16 are path builders, so an
off-by-one does not fail to compile -- it produces a path that does not resolve,
surfacing as a missing asset at runtime. None of njinn's 12 selftests exercises
`resio_path_join`, `cryptops_normalize_path` or `fxops_resolve_runtime_path`
directly. **Write those tests against the current signatures first**, then
convert underneath them, one file at a time.

## `c8` signedness *(resolved)*

The type is C's `char`, and its signedness is the platform's:

    c: c8 = 200;    // -56 here; clang warns about the conversion

Rather than pick signed or unsigned and defend it at every C boundary, rin
declines to have an opinion and names the type accurately: **`c8` is whatever
the C compiler's `char` is on this target**, width and signedness included. The
name also stops implying "a character", which it never was. `char` remains a
legal spelling and normalises to `c8`, so the two are one type. See `shape.md`
2.5.

That resolves the *naming*, not the hazard: byte arithmetic on a `c8` is still
subject to the platform's signedness. Which is why the rest of this plan matters
more than the rename -- `string8` stores `*u8`, and that choice was already made
correctly. The rule stands: `c8` exists for the C boundary, `u8` is the byte
type, and code that is not talking to C should not touch `c8` at all.

## `usize` only

There is a `usize` and there is deliberately no `isize`. A size is not negative,
a length is not negative, and an index into a buffer is not negative; the cases
that genuinely want a signed offset want `i64` and can say so. `isize` exists in
other languages mostly to paper over pointer subtraction, which here already has
a type: `p - q` yields `long`. Now that `ptrdiff` exists as a primitive, that is
arguably the honest name for the result -- worth revisiting, but a separate
change from adding the name.

## Open decisions

### 1. Const views *(direction now set)*

    string8slice: struct = { data: *u8; length: u64; }

`*u8`, not `*const u8`, and there is no const variant. Every slice is a
mutable-capable view; in practice none of the sixteen `string8slice_*` procs
writes through one, so it is immutable by convention only.

The general answer arrived with `slice<T>`, where **`slice<const T>` is real and
the compiler enforces it** -- writing through one is rejected. `string8slice`
should follow the same shape rather than inventing a second convention. The open
part is only whether that means `string8slice` gains a const sibling or is
eventually replaced by `slice<const u8>` outright. The latter is tempting and
would delete a type, but `string8slice` carries string-specific procs that a
generic slice should not, so this is not a pure rename. See
[slices.md](slices.md).

### 2. Long-lived mutable strings *(resolved: nothing to do)*

Both engines already answer this the same way, and it is the C answer: an inline
fixed array. gin has 140, njinn 143 -- `path: [512]char`, `key: [64]char`,
`status_line: [128]char`. The port preserved the pattern rather than abandoning
it.

It is worth stating only because it rules something out: a free list is only
worth building for many strings of unpredictable size with genuinely independent
lifetimes, and neither engine has that population. What this does mean is that
`[N]c8` is a fourth string shape in practice, and the bounded copy into it --
`strncpy` today, 17 calls in njinn -- is a std gap rather than a design question.

### 3. The C library

Covered in [clib-removal.md](clib-removal.md). Independent of everything above.

## What none of this changes

The split itself is right, and it is the same one Zig arrives at -- `[]const u8`
and `ArrayList(u8)`. Nothing here argues for a different string representation.
The confusion was never in the types; it was that `string8` looks like a growable
buffer and is used as an immutable owned one. The builder removed the reason to
grow, and this document is the part that says so out loud.

## See also

- [`slices.md`](slices.md) -- the general slice design; several decisions here
  are special cases of it.
- [`safety.md`](safety.md) -- unterminated buffers and out-of-bounds reads are
  the same failure this is trying to remove.
- [`type-safety.md`](type-safety.md) -- variadics are unchecked, which is half of
  why `snprintf` is dangerous.
- [`clib-removal.md`](clib-removal.md) -- the libc dependency in `string8.rin`.
