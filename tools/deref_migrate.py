"""Rewrite `x[0]` to `x*` where x is a one-element pointer `*T`.

    python tools/deref_migrate.py ROOT RIN_EXE ENTRY.rin... [--skip FILE.rin]... [--dry-run]

Unlike rin_migrate.py this needs types, so the compiler decides: `rin compile
--report-derefs` lists every literal `[0]` with its base kind, once per generic
instance, and a site is rewritten only if every report for it says the base is
a `*T`. Only files under ROOT are touched, never std. Where the next token could
start an operand, so `*` would read as multiplication, the operand is
parenthesised: `(w*) - 1`. Entries that do not compile beforehand are skipped
and named.

Afterwards every entry is compiled again, and its C must be identical to before,
apart from source columns in comments, since `x*` is the same expression as
`x[0]`. njinn's 7,609 went through it this way. See docs/pointer-syntax.md.
"""
import collections, os, pathlib, re, subprocess, sys

args = sys.argv[1:]
dry = "--dry-run" in args
args = [a for a in args if a != "--dry-run"]
skip = set()
while "--skip" in args:
    i = args.index("--skip")
    skip.add(os.path.normcase(os.path.abspath(args[i + 1])))
    del args[i:i + 2]
root, rin, entries = pathlib.Path(args[0]).resolve(), args[1], args[2:]
work = pathlib.Path(__file__).resolve().parent.parent / "build" / "deref_migrate"
work.mkdir(parents=True, exist_ok=True)

broken = set()

def compile_all(tag, report=False):
    out, reports = {}, []
    for e in entries:
        if e in broken: continue
        c = work / f"{tag}_{pathlib.Path(e).stem}.c"
        cmd = [rin, "compile", e, "-o", str(c), "--no-header"] + (["--report-derefs"] if report else [])
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        if r.returncode != 0:
            if tag == "before":
                # Already broken before the rewrite: leave it out, and say so.
                print(f"  skipped {e}: does not compile before the rewrite either")
                broken.add(e)
                continue
            sys.exit(f"compile failed ({tag}) {e}:\n{r.stdout[-3000:]}")
        out[e] = c.read_bytes()
        reports += [l.split("\t") for l in r.stdout.splitlines() if l.startswith("RIN_DEREF\t")]
    return out, reports

before, reports = compile_all("before", report=True)

kinds = collections.defaultdict(set)
start = {}
for _, path, line, col, sline, scol, kind in reports:
    key = (os.path.normcase(os.path.abspath(os.path.join(root, path))), int(line), int(col))
    kinds[key].add(kind)
    start[key] = (int(sline), int(scol))
inside = os.path.normcase(str(root)) + os.sep
# Only the project's own files: imports from std (the compiler's copy) and
# anything outside the root are reported too, and are not ours to edit.
sites = [k for k, v in kinds.items() if v == {"one"} and k[0] not in skip and k[0].startswith(inside)]
print(f"{len(kinds)} literal [0] sites, {len(sites)} on a *T in every instance")

OPERAND = re.compile(rb"[A-Za-z0-9_\"'({~]|-(?![=>])|!(?!=)")

def next_token_starts_operand(text, pos):
    i = pos
    while i < len(text):
        if text[i:i + 1] in (b" ", b"\t", b"\r", b"\n"):
            i += 1
        elif text.startswith(b"//", i):
            i = text.find(b"\n", i)
            if i < 0: return False
        elif text.startswith(b"/*", i):
            i = text.find(b"*/", i) + 2
        else:
            return bool(OPERAND.match(text, i))
    return False

by_file = collections.defaultdict(list)
for k in sites:
    by_file[k[0]].append(k)

changed, wrapped, missing = 0, 0, 0
for path, keys in by_file.items():
    text = pathlib.Path(path).read_bytes()
    lines = text.split(b"\n")
    offsets = [0]
    for l in lines:
        offsets.append(offsets[-1] + len(l) + 1)
    def off(line, col): return offsets[line - 1] + col - 1
    edits = []  # (offset, length to replace, replacement)
    for _, line, col in keys:
        o = off(line, col)
        m = re.match(rb"\[\s*0\s*\]", text[o:])
        if not m:
            missing += 1
            print(f"  no [0] at {path}:{line}:{col}: {text[o:o+12]!r}")
            continue
        if next_token_starts_operand(text, o + m.end()):
            sline, scol = start[(path, line, col)]
            edits.append((off(sline, scol), 0, b"("))
            edits.append((o, m.end(), b"*)"))
            wrapped += 1
        else:
            edits.append((o, m.end(), b"*"))
        changed += 1
    # Right to left, and at one offset a replacement before an insertion, so
    # `(` lands in front of what is already there.
    for o, n, rep in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        text = text[:o] + rep + text[o + n:]
    if not dry:
        pathlib.Path(path).write_bytes(text)
print(f"rewrote {changed} ({wrapped} parenthesised) in {len(by_file)} files; {missing} not found")

if dry:
    sys.exit(0)
after, _ = compile_all("after")
# Comments name source columns ("instantiated at f.rin:12:9"), and `*` is
# two characters shorter than `[0]`, so columns in comments may move.
norm = lambda b: re.sub(rb"(\.rin:\d+):\d+", rb"\1", b)
bad = [e for e in entries if e not in broken and norm(before[e]) != norm(after[e])]
if bad:
    sys.exit(f"generated C differs for: {bad}")
print(f"generated C identical for all {len(entries) - len(broken)} entries that compiled")
