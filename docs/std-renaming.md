# std renaming: length versus capacity

> **Proposed** (2026-10-26). Nothing renamed yet.

std's containers use the same words for different things. `reserve` means
"make n elements" on `array` and "make room for n" on `vec`, and `vec`'s
`resize` means "double the room" where every other language means "set the
length". This note fixes the names so each word means one thing everywhere,
the meaning C++, Zig and Odin give it.

## What went wrong

    items: vec<i32> = vec<i32>reserve(arena&, 128);
    for (i: u64 = 0; i < 128; i += 1) { items.data[i] = cast(i, i32); }
    // items.length is 0: the vec is empty

`reserve` allocated 128 slots and set the capacity (`border`), but not the
length. The writes land in valid, zeroed memory, so nothing crashes, but the
vec still says it is empty: `items()`, `at`, `last` and the debugger's natvis
view show nothing, and the next `append` overwrites element 0. Reading the
code, expecting `length == 128` was reasonable:

- the parameter is called `length`
- `array<T>reserve(arena, length)`, one file over, *does* set the length
- `vec<T>resize` exists, and in C++ that is the call that sets the length

## The words today

| call | what it does | length | capacity |
| --- | --- | --- | --- |
| `array<T>reserve(arena, length)` | allocates n zeroed elements | n | (fixed) |
| `vec<T>reserve(arena, length)` | allocates room for n | 0 | n |
| `string8_reserve(arena, capacity)` | allocates room for n bytes | 0 | n |
| `vec<T>resize(arena, v)` | doubles the room; called by `append` | unchanged | ×2 |
| `deque<T>grow(arena, d)` | doubles the room; called by `push_*` | unchanged | ×2 |
| `string8_grow` | grows the room | unchanged | grows |
| `heap<T>make(less)` | an empty heap with a comparison | 0 | 0 |

`deque` and `string8` already call the doubling step `grow`; `vec` is the odd
one out. `heap` already uses `make` for a constructor.

## How others spell it

| | room only | set the length | double the room |
| --- | --- | --- | --- |
| C++ `std::vector` | `reserve(n)` | `resize(n)`, `vector(n)` | internal |
| Zig `ArrayList` | `ensureTotalCapacity(n)` | `resize(n)` | internal |
| Odin `[dynamic]T` | `reserve(&a, n)` | `resize(&a, n)`, `make([dynamic]T, len, cap)` | internal |

All three agree: `reserve` is room, `resize` is length, and growth is an
implementation detail of append.

## Proposal

**vec**

| call | change | meaning |
| --- | --- | --- |
| `vec<T>reserve(arena, capacity)` | parameter renamed only | room for n; length stays 0 |
| `vec<T>resize(arena, v, length)` | **new meaning** | length becomes n: new elements are zeroed, the room grows if needed; shrinking keeps the room |
| `vec<T>grow(arena, v)` | **renamed** from today's `resize` | doubles the room, as `deque<T>grow` does |

So the example above becomes either of the two C++ patterns:

    items: vec<i32> = vec<i32>reserve(arena&, 128);    // room, then append
    for (...) { vec<i32>append(arena&, items&, value); }

    items: vec<i32> = {};
    vec<i32>resize(arena&, items&, 128);              // 128 elements, then assign
    for (i: u64 = 0; i < 128; i += 1) { items.data[i] = cast(i, i32); }

**array**

| call | change | meaning |
| --- | --- | --- |
| `array<T>make(arena, length)` | **renamed** from `array<T>reserve` | n zeroed elements; an array never grows, so this is its constructor, like `heap<T>make` |

After this, `reserve` means room and nothing else, in `vec` and `string8`.

## Who calls what

Measured 2026-10-26 across rin, njinn, rin-learn, rin-dx11 and rasm-dx11.

| call | callers outside std | effect of the change |
| --- | --- | --- |
| `vec<T>reserve` | njinn `gops.rin` 1, `sops.rin` 2; tests 2 | none: all of them reserve and then `append`, which is the intended use |
| `vec<T>resize` | none; only `vec<T>append` inside `vec.rin` | rename that one call to `grow` |
| `array<T>reserve` | njinn 52 (`sops.rin` 45, `gops.rin` 6, `gin.rin` 1); std `slice.rin` 1; torture and fuzz seeds 6; `src/main.rin` 2 (and `main.old.rin` 2); LSP fixtures 2 calls, a copy of the definition and a name check | mechanical rename to `make` |

The `vec` half breaks nothing: no caller uses today's `resize`, and the new
`resize` is a name no one calls yet. Only the `array` rename touches njinn.

## Order

1. `vec`: rename `resize` to `grow`, add the new `resize`, rename `reserve`'s
   parameter. Tests: `resize` up sets the length and zeroes; `resize` down
   keeps the room; `append` after `resize` adds after the last element.
2. `array`: add `array<T>make`. Keep `array<T>reserve` as a one-line call to
   `make` for a release, so njinn moves when it suits the game rather than on
   the compiler's schedule.
3. Rename the callers in rin's own tree (std, tests, torture, fuzz seeds, LSP
   fixtures), then njinn's 52, then remove `array<T>reserve`.

## Open questions

- **Does `resize` take a fill value?** C++ has `resize(n, x)`. Zeroing matches
  everything else in std (`memops_arena_push_zero`, `= {}`); a fill variant can
  come later if something needs it.
- **A constructor with a length?** `vec<T>make(arena, length)` would be C++'s
  `vector(n)`. It is `reserve` plus `resize` in one call; worth adding only if
  that pair turns up often.
- **The same audit for the rest of std.** `deque`, `list`, `map` and `hashmap`
  were checked only for `reserve`, `resize` and `grow`. Other words (`clear`
  versus `reset`, `at` versus `get`) may disagree in the same way.
