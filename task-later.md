# Later

Left over from the 2026-10-28 pointer work (`docs/pointer-syntax.md`). None of
it blocks anything.

## Commit and push

- [x] Committed and pushed 2026-10-28, except the old rasm repo (see below)
      and rin-leet's untracked `docs/debugging.md`, which predates this work.
      What it covered, `rin/scripts` (the LSP's keywords) before rin:

      | repo | files |
      |---|---|
      | rin | 81 |
      | rin/scripts | 1 |
      | psite | 98 |
      | rin-dx11 | 42 |
      | njinn | 41 |
      | rin-dod | 41 |
      | rasm (old repo) | 21, mostly earlier uncommitted work |
      | rin-learn | 19 |
      | rin-leet | 3, includes the in-progress `solution.rin` |
      | rin-playground | 2 |
      | rasm-dx11 | 1 |

- [ ] Run njinn the usual way. Its generated C is unchanged apart from 40
      generic instances renamed `ptr_` to `raw_`, but the game itself has not
      been started since.
- [ ] Deploy psite (`scripts/deploy.sh`, see its `AGENTS.md`). `site-static/`
      has the rebuilt rin-dx11 section, lessons page and rin-dod page, and
      `site-static/rin/lessons.pdf` is deleted.

## Language and std

- [ ] Debug null check on a dereference: `p*` and `p[0]` on a `*T` abort with
      the rin line when `p` is null, instead of crashing in the generated C.
      Gone under `NDEBUG`, like the bounds checks.
- [ ] Debug alignment check on `cast(raw, *T)`: the address is a multiple of
      `alignof(T)`.
- [ ] Use-after-reset in arenas: in Debug, memops fills popped or reset
      memory with a pattern, or marks it for AddressSanitizer
      (`__asan_poison_memory_region`), so `-fsanitize=address` catches stale
      pointers into an arena.
- [ ] `^void` for memory in the other repos' own declarations, as std now
      does: rin-dx11 has 130 `*void`, njinn 116, rin-dod 6. `*void` and
      `^void` convert both ways, so the generated C does not change.

## Docs and repos

- [ ] `docs/codegen-findings.md` Part 2 still proposes that rin never fuses
      `a * b + c`. The decision was to leave fusing on, and to add a build
      setting only if replays or lockstep ever need identical floats.
- [ ] The old `C:\devel\rasm` repo is superseded by `rin/rasm` and rasm-dx11,
      but has uncommitted work from before 2026-10-28: archive, commit, or
      leave it.
