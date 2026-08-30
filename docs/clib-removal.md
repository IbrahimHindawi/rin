# Removing the C library from strings

> **Measured, not implemented.** This records what the dependency actually is,
> what it costs to replace, and the one thing that cannot be replaced by
> writing rin. Companion to [strings.md](strings.md), which covers the string
> types themselves.

## What the dependency actually is

`src/std/string8.rin` is 609 lines and pulls in three C headers. The calls,
counted:

| function | calls | what it is |
|---|---|---|
| `printf` | 9 | the abort path |
| `memcmp` | 7 | a string primitive |
| `exit` | 7 | the abort path |
| `strlen` | 5 | a string primitive |
| `memcpy` | 5 | a string primitive |
| `fclose` | 2 | file I/O |
| `memset` | 1 | a string primitive |
| `fread` | 1 | file I/O |

`memops.rin` adds `exit` (3), `printf` (2), `memset` and `memcpy` (1 each).

Sorted by kind, the picture is smaller than the header list suggests:

  * **Four primitives** -- `strlen`, `memcpy`, `memset`, `memcmp`. These are the
    only entries that are string work.
  * **The abort path** -- `printf` + `exit`, on allocation failure and bounds
    violations. Not string work; it is how std reports a fatal.
  * **File I/O** -- `fread`/`fclose`, reached only by `string8_read_file`. Not
    string work either; it is in the wrong module.

## The four primitives are five lines each

Written in rin and verified working:

    rin_len:  proc(s: *const c8) -> u64 = {
        n: u64 = 0;
        while (s[n] != 0) { n += 1; }
        return n;
    }

    rin_copy: proc(dst: *u8, src: *const u8, n: u64) -> void = {
        for (i: u64 = 0; i < n; i += 1) { dst[i] = src[i]; }
    }

`rin_set` and `rin_cmp` are the same shape. A test copying `"hello world"`
through them reported `len=11 copy='hello world' cmp_same=0 cmp_diff=-1`.

There is no performance argument against this at string sizes. Compiled at
`-O2`, clang inlined and vectorised the copy loop -- the generated assembly
contains no call at all for it.

## The thing that cannot be replaced

Source-level purity does not give link-level purity. An object built from rin
source containing **zero** `memset` calls still reports:

    U memset

Clang's loop-idiom pass recognises a byte-fill loop and emits `call memset`.
It does the same for struct assignment and array zeroing. This is not a rin
problem: it is why freestanding C environments must supply `memcpy`, `memset`,
`memmove` and `memcmp` regardless of what the source says. Any language with a
C backend inherits it.

So those symbols cannot be avoided. They can be **owned**: define them in rin
under those names and the linker resolves against your code, not libc.

The obvious fear is self-recursion -- a `memset` whose own fill loop is turned
into a call to `memset`. Checked, and clang does not do it:

    === does our own memset call itself? ===
      (no calls)

Both with and without `-fno-builtin`. MSVC's `#pragma function(memset)` exists
for the same purpose if it ever becomes necessary there.

## The plan

1. `std/mem.rin` -- `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` in rin,
   exported under those C names so backend-generated calls resolve to them.
2. `string8.rin` drops `cinclude "string.h"` and calls those.
3. `string8_read_file` moves to an os/file module. It is the only reason
   `stdio.h` appears in a string module for a reason other than the abort path.
4. The abort path routes through `memops_os`, which already abstracts the
   platform. `stdio.h` and `stdlib.h` then drop out too.

That leaves `string8.rin` with no `cinclude` at all, and the four symbols the
backend assumes exist are ones we wrote.

## Known limits

  * **`memmove`** needs the overlap direction check, and a simple version is
    meaningfully slower than a tuned libc one on large blocks. Strings rarely
    move large overlapping regions. Write the simple correct one; revisit only
    if a profile says so.
  * **This does not make rin freestanding.** It removes libc from *strings*.
    `memops_os` still calls the platform for virtual memory, and that is the
    right place for it.
  * The abort path has to print somewhere. Routing through `memops_os` moves
    the dependency rather than deleting it -- which is the point, since one
    platform seam is easier to reason about than `stdio.h` in every module.
