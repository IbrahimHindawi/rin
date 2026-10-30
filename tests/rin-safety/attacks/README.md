# The attack corpus

Every program here was written to break the safety rules: to get a pointer
past `rin check` with the rules on, and then fault under AddressSanitizer or
UndefinedBehaviorSanitizer. Three rounds, 203 programs plus the false-positive
probes, 37 holes found, 37 closed. Each hole became a rule in the checker and
a reject test in `../reject`; the full story is in
[docs/regions.md](../../../docs/regions.md), under "The second attack" and
"The third attack".

- `r1-*`: the first round, against the rules as first written. Seventeen got
  through: locals outliving their region, borrows outliving a block, borrows
  written through out-parameters, four staleness gaps, trusted-struct fields
  written from safe code, and three direct bypasses.
- `r2-*`: the second round, against the rules added after the first. Six got
  through: ternaries as borrow sources, the address of a carrier field, kills
  in a `for` step, calls through a named proc pointer, shallow `*const`.
- `r3-*`: the third round, against std's `[trusted]` procs rather than the
  checker. Fourteen got through, every one a signature that lied: null in a
  non-null field, writable slice headers, implicit checked-to-raw conversion,
  reflect lookups returning `*const` that could be null.
- `checks-clean.txt`: the programs that pass the checker on purpose. Some are
  correct programs written to provoke false positives; some end in a checked
  run-time abort with a `rin runtime:` message, which is not a hole.

`python tests/run_rin_attacks.py` confirms every program is still rejected,
or still clean if it is on the list; `--run` also builds and runs the clean
ones under the sanitizers.

## Add yours

Write a program that should be unsafe, drop it here, run the script. Three
outcomes:

1. It is rejected. Good; leave it in, it is one more thing that stays closed.
2. It checks clean and runs clean. Either it is a correct program, so add it
   to `checks-clean.txt`, or the sanitizers missed something, so look harder.
3. It checks clean and the sanitizer fires. You found a hole. Open an issue
   with the program; it becomes a rule and a reject test.

The one claim this corpus supports is narrow and worth keeping narrow: nobody
who has tried has got a pointer past the checker since these were closed. The
day someone does, the corpus grows by one and the claim is true again an hour
later. That is the whole point of keeping it in the repo.
