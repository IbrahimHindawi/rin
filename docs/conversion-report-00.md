# Conversion Report 00

> **Measured 2026-10-21** on njinn after the literal, conversion, slice and
> pointer rules landed ([operators-and-conversions.md](operators-and-conversions.md),
> [pointers-and-slices.md](pointers-and-slices.md)), and after njinn's C enums
> were bound. The question it answers is step 7 of the operators doc: how many
> casts does real code write, and would `x'T` make them better?

All counts are over `njinn/src/*.rin` -- njinn's own code, 20,849 non-comment
lines -- and exclude the generated bindings in `src/bindings/`. A "cast" is one
`cast(x, T)` expression, counted token by token rather than by regex, so
nested parentheses in `x` do not confuse it.

## The rules added no casts

| | casts |
|---|---|
| before the literal and conversion rules | 1,298 |
| after them | 1,297 |

The conversion rule found 142 sites. 139 were `sizeof(...)` or an enum's
`<>.count` going into a D3D `u32` -- constants, which now adapt like literals.
The other 3 were one real bug-shaped site (a u64 length kept in an i32), fixed by
changing the variable's type, not by casting. The pointer rules (992 sites)
were all declarations becoming `[*]T` or slices, again with no casts.

## Binding the C enums

njinn declared its D3D, DXGI and miniaudio enums empty -- `DXGI_FORMAT:
enum[external] = {}` -- so every constant was written as a number and cast:
`cast(2, DXGI_FORMAT)`. Those 21 enums now carry their members, read out of the
Windows SDK and miniaudio headers by `rinbind` (cgltf's were already bound):

| | |
|---|---|
| enums given their members | 21 |
| members bound | 376 |
| `cast(N, Enum)` replaced by the member's name | 228 |

| enum | constants named |
|---|---|
| `DXGI_FORMAT` | 56 |
| `D3D11_INPUT_CLASSIFICATION` | 33 |
| `ma_result` | 21 |
| `D3D11_PRIMITIVE_TOPOLOGY` | 19 |
| `D3D11_USAGE` | 16 |
| `cgltf_result` | 14 |
| `D3D11_TEXTURE_ADDRESS_MODE`, `cgltf_attribute_type` | 12 each |
| `D3D11_BLEND` | 8 |
| `D3D11_MAP` | 7 |
| 11 others | 30 |

`cast(2, DXGI_FORMAT)` is now `DXGI_FORMAT.DXGI_FORMAT_R32G32B32A32_FLOAT`. The
generated C names the constant instead of casting a number; njinn builds, its
selftests pass, and the game runs.

## What is left

**1,065 casts**, one every 19.6 lines (was 16.1).

| target | casts | what they are |
|---|---|---|
| integer types | 541 | f32 to index, u64 lengths to u32 fields, signed/unsigned at APIs |
| float types | 241 | integers into maths: `cast(i, f32)` |
| pointers | 167 | `*void` from allocators (45), COM interface upcasts (`*ID3D11Resource`, 23), byte views |
| rin enums and C types | 116 | integers into njinn's own enums -- `gops_action_kind` 38, `gops_character_kind` 22, `gops_anim_kind` 13 |

Top target types: f32 173, u32 158, i32 134, u64 96, f64 60, `UINT` 47,
`*void` 45, i64 41, `gops_action_kind` 38. By file: gin 258, gops 160, fxops
150, fxed 102, cryptops 76, os 70.

29 casts still wrap a bare literal: character codes into `c8`/`char` (16), key
codes into `guiops_key` (6), two `HRESULT` constants, and a few numbers.

## What it says about `x'T`

- **About 780 casts are numeric** (to an integer or a float type). Those are
  the ones `x'T` would shorten: `cast(i, f32)` to `i'f32`. That is one every 27
  lines -- present, not dominant.
- **The other ~280 would not change.** Pointer casts are interop and want to
  stand out; enum casts are better served by typed helpers or by keeping the
  value typed as the enum in the first place.
- **The biggest win was not syntax.** Binding the enums removed 228 casts and
  replaced 228 magic numbers with names, which `x'T` could not have done.

Recommendation: keep `cast(x, T)`. Revisit `x'T` if the numeric casts in game
maths start to feel noisy in practice, and look first at the 116 enum casts --
njinn converting integers into its own enums usually means a value travelled as
an integer when it could have stayed typed.

## See also

- [`operators-and-conversions.md`](operators-and-conversions.md) -- step 7.
- [`pointers-and-slices.md`](pointers-and-slices.md) -- the pointer migration.
