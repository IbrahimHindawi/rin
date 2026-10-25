# Roadmap

> 2026-10-24. What rasm is for, in the author's words: "play with dx11
> graphics and simd assembly to master Data Oriented ECS Programming", and
> specifically "the raw simd experience instead of intrinsics". The author
> knows DoD can be learned in C with intrinsics and a profiler; writing the
> instructions by hand is the point.

## Decisions

- **SIMD:** SSE first (4 floats, `xmm`). Later, how a game engine supports
  several instruction sets: AVX2 (`ymm`, 8 floats), AVX-512 (`zmm`, 16
  floats, mask registers). The author's machine is a Ryzen 9 9950X3D (Zen 5,
  full-width AVX-512) with an RTX 5070 Ti.
- **Memory:** arena programming, with rasm's own arena, written in rasm,
  fields in `memops_arena`'s order (design.md §7).
- **Layout:** pure structure-of-arrays, written by hand. No SoA helper
  declaration for now; maybe later if writing arrays out starts to hurt.
- **Steps:** small ones. The archetype ECS may be overkill in rasm; it may
  be tried after the particle lesson.

## Bindings

Not a work item any more: `rinbind` on `windows.h`, `d3d11.h`, `dxgi.h` and
`d3dcompiler.h` gives complete structs, `*Vtbl` structs, enums, externals and
`#define` constants (design.md §7). What remains is choosing the
`--filter`s and hand-writing the few call-shaped constants (`IDC_ARROW`).

## Lessons, in order

1. **2.1 hello triangle** (`examples/2.1.hello_triangle`). **Done
   2026-10-25**: the first lesson through `rin asm`; draws and debugs.
2. **5.1 transformations** (`examples/5.1.transformations`, with
   `examples/includes/math.rasm`). SSE matrix maths: shuffles, dot products,
   x87 `fsin`, shadow space as scratch, no frames.
3. **Particles, two layouts.** N particles (10k, 1M, 10M) updated every
   frame with SSE, then drawn with one instanced draw (positions in a
   dynamic buffer, or a structured buffer the vertex shader reads):
   - **vec4 per entity**, kayan's layout (`Array_vec4s positions`): one
     entity per `xmm`, `w` wasted;
   - **true SoA**, `pos_x[]`, `pos_y[]`, `pos_z[]`: four entities per `xmm`,
     every lane used.

   The update must include maths that combines components (a length, a
   distance test, a branch as a mask), or the comparison flatters vec4: a
   plain `p += v` is nearly as good in both. Time both, at sizes inside and
   outside the cache (the 9950X3D's V-Cache makes the step visible later
   than usual).
4. **Choosing the instruction set at run time.** `cpuid` once at startup,
   then `update_sse`, `update_avx2` or `update_avx512` stored in a `u64`
   global; the frame does `call update_particles`, which the operand rule
   makes `call qword ptr update_particles`. Nothing new in rasm.
5. **Maybe: an archetype ECS.** The assembly-shaped version of kayan:
   - the archetype header holds `offsets: [64]u32` indexed by the
     component's bit number, instead of kayan's string-keyed map, so a
     column lookup is one `mov eax, [rbx + rcx*4 + offsetof(...)]`;
   - `tzcnt`/`bsf` walk a signature's bits; a query is `and` plus `cmp`.

   The same two changes apply to kayan in C. There the offset table is
   simplicity rather than speed (the lookup is per archetype, not per
   entity). kayan also has a bug to fix: B's and D's entity adds call
   `kayan_entity_add(&a.archetype_header.entity_count)`, so A's count grows
   and theirs stay 0.

## Generic data structures

Type-erased containers in rasm (element size as an argument), typed access
through macros that fold `sizeof(T)`, and rin's generic structs imported for
layouts (`positions: vec<vec4>` is free through rin's C backend). Generic
procs with instruction bodies (`vec<T>push: proc<T>`) are parked: they
would need a monomorphiser. All of it written out in decisions.rasm §5.

## What the author does not need rasm for

Recorded honestly: DoD and ECS are about memory (layout, access patterns,
cache lines, bandwidth, batching, branch-free loops) and can be learned in
C with intrinsics, the Visual Studio disassembly window and AMD uProf.
Spreading systems across cores is where ECS really pays and has nothing to
do with assembly. rasm is for writing the SIMD by hand, and plain ml64 in
masm64-dx11 is always available for that while rasm is being built. The
author's standing priority: the game comes first; rin and rasm are
time-boxed.
