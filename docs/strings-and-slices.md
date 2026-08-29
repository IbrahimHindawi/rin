# Strings and slices: which type is for what

> **Partly implemented.** `string8_builder` is in and tested. The rest of this
> records what each type is actually for, the measurements behind that, and the
> three decisions still open. Companions: [string.md](string.md) for the plan to
> reduce C string usage, [clib-removal.md](clib-removal.md) for the libc
> dependency in `string8.rin` itself.

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

Read the three types this way:

  * **`string8slice`** -- a view. Owns nothing, guarantees nothing about a
    terminator, and sub-slicing is free. This is the default for anything that
    reads.
  * **`string8`** -- an owned, finished, NUL-terminated string over arena bytes.
    Treat it as immutable once made.
  * **`string8_builder`** -- the way to make one. Zero-copy.

`string8_grow` and `string8_append_*` stay, but they are the fallback for the
case the builder cannot serve (see below), not the normal path. Reaching for
them is a signal that a builder was wanted.

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

## Open decisions

### 1. Mutable versus immutable slices

There is no such distinction today:

    string8slice: struct = { data: *u8; length: u64; }

`*u8`, not `*const u8`, and there is no const variant. Every slice is a
mutable-capable view. In practice none of the sixteen `string8slice_*` procs
writes through one, so it is immutable by convention only.

If `string8` is to be read as "immutable once made", the matching move is
`*const u8` in the slice, with a separate mutable view added only when something
needs it. Cheap now; annoying once more code exists.

### 2. Long-lived mutable strings

Both engines already answer this the same way, and it is the C answer: an inline
fixed array. gin has 140 of them, njinn has 143 -- `path: [512]char`,
`key: [64]char`, `status_line: [128]char`. The port preserved the pattern rather
than abandoning it.

So there is nothing to fix here. It is worth stating only because it rules
something out: a free list is only worth building for many strings of
unpredictable size with genuinely independent lifetimes, and neither engine has
that population. Long-lived mutable strings already live in the struct that owns
them, allocate nothing, and ask no lifetime question. gin shipped that way and
njinn kept it.

What this does mean is that `[N]char` is a fourth string shape in practice, and
the bounded copy into it -- `strncpy` today, 17 calls in njinn -- is a std gap
rather than a design question.

### 3. The C library

Covered in [clib-removal.md](clib-removal.md). Independent of everything above.

## What this does not change

The split itself is right, and it is the same one Zig arrives at -- `[]const u8`
and `ArrayList(u8)`. Nothing here argues for a different string representation.
The confusion was never in the types; it was that `string8` looks like a
growable buffer and is used as an immutable owned one. The builder removed the
reason to grow, and this document is the part that says so out loud.
